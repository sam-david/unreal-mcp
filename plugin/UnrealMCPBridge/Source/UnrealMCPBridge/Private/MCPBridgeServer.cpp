#include "MCPBridgeServer.h"

#include "Common/TcpListener.h"
#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "MCPBridgeCommands.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UnrealMCPBridgeModule.h"

namespace
{
	/** Frame header: uint32 little-endian payload length. */
	constexpr int32 FrameHeaderSize = 4;

	/** Reject implausible lengths instead of allocating on a corrupt prefix. */
	constexpr uint32 MaxFrameSize = 16u * 1024u * 1024u;

	void DestroySocket(FSocket*& InSocket)
	{
		if (InSocket != nullptr)
		{
			InSocket->Close();
			if (ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				Subsystem->DestroySocket(InSocket);
			}
			InSocket = nullptr;
		}
	}
}

bool FMCPBridgeServer::Start(uint16 InPort)
{
	const FIPv4Endpoint Endpoint(FIPv4Address::InternalLoopback, InPort);

	// Build the listen socket here rather than letting FTcpListener create it:
	// the listener binds on its own thread, so a port clash would otherwise stay
	// invisible until much later. Owning it means we also destroy it in Stop().
	ListenSocket = FTcpSocketBuilder(TEXT("MCPBridgeListener"))
		.AsReusable()
		.BoundToEndpoint(Endpoint)
		.Listening(8);

	if (ListenSocket == nullptr)
	{
		UE_LOG(LogMCPBridge, Error,
			TEXT("Could not bind 127.0.0.1:%u - another editor instance is probably already ")
			TEXT("serving the bridge. Pass -MCPBridgePort=<n> to use a different port."), InPort);
		return false;
	}

	Listener = new FTcpListener(*ListenSocket, FTimespan::FromMilliseconds(200));
	Listener->OnConnectionAccepted().BindRaw(this, &FMCPBridgeServer::HandleConnectionAccepted);

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMCPBridgeServer::Tick), 0.0f);

	UE_LOG(LogMCPBridge, Log, TEXT("Bridge listening on 127.0.0.1:%u"), InPort);
	return true;
}

void FMCPBridgeServer::Stop()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// Delete the listener first so its thread stops handing us new sockets.
	if (Listener != nullptr)
	{
		delete Listener;
		Listener = nullptr;
	}
	DestroySocket(ListenSocket);

	FSocket* Orphan = nullptr;
	while (PendingSockets.Dequeue(Orphan))
	{
		DestroySocket(Orphan);
	}

	for (FMCPBridgeConnection& Connection : Connections)
	{
		DestroySocket(Connection.Socket);
	}
	Connections.Empty();

	UE_LOG(LogMCPBridge, Log, TEXT("Bridge stopped"));
}

bool FMCPBridgeServer::HandleConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& InEndpoint)
{
	// Listener thread: queue only. The game thread owns the socket from here.
	PendingSockets.Enqueue(InSocket);
	return true;
}

bool FMCPBridgeServer::Tick(float DeltaTime)
{
	FSocket* Adopted = nullptr;
	while (PendingSockets.Dequeue(Adopted))
	{
		Adopted->SetNonBlocking(true);
		Connections.Add(FMCPBridgeConnection{ Adopted, TArray<uint8>() });
		UE_LOG(LogMCPBridge, Verbose, TEXT("Client connected (%d open)"), Connections.Num());
	}

	for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
	{
		FMCPBridgeConnection& Connection = Connections[Index];

		if (Connection.Socket->GetConnectionState() == SCS_ConnectionError)
		{
			CloseConnection(Connection);
			Connections.RemoveAt(Index);
			continue;
		}

		ReceiveInto(Connection);
		if (!ExtractFrames(Connection))
		{
			CloseConnection(Connection);
			Connections.RemoveAt(Index);
		}
	}

	return true;
}

void FMCPBridgeServer::ReceiveInto(FMCPBridgeConnection& Connection)
{
	uint32 PendingSize = 0;
	while (Connection.Socket->HasPendingData(PendingSize) && PendingSize > 0)
	{
		const int32 Offset = Connection.Buffer.Num();
		Connection.Buffer.AddUninitialized(static_cast<int32>(PendingSize));

		int32 BytesRead = 0;
		if (!Connection.Socket->Recv(Connection.Buffer.GetData() + Offset,
				static_cast<int32>(PendingSize), BytesRead) || BytesRead <= 0)
		{
			Connection.Buffer.SetNum(Offset);
			break;
		}

		Connection.Buffer.SetNum(Offset + BytesRead);
	}
}

bool FMCPBridgeServer::ExtractFrames(FMCPBridgeConnection& Connection)
{
	while (Connection.Buffer.Num() >= FrameHeaderSize)
	{
		// Read the prefix explicitly little-endian rather than memcpy-ing a
		// uint32, so the wire format does not depend on host byte order.
		const uint32 FrameSize =
			  static_cast<uint32>(Connection.Buffer[0])
			| (static_cast<uint32>(Connection.Buffer[1]) << 8)
			| (static_cast<uint32>(Connection.Buffer[2]) << 16)
			| (static_cast<uint32>(Connection.Buffer[3]) << 24);

		if (FrameSize > MaxFrameSize)
		{
			UE_LOG(LogMCPBridge, Error,
				TEXT("Frame length %u exceeds the %u byte cap; dropping client."),
				FrameSize, MaxFrameSize);
			return false;
		}

		if (static_cast<uint32>(Connection.Buffer.Num()) < static_cast<uint32>(FrameHeaderSize) + FrameSize)
		{
			break; // Frame still in flight.
		}

		const FUTF8ToTCHAR Decoded(
			reinterpret_cast<const ANSICHAR*>(Connection.Buffer.GetData() + FrameHeaderSize),
			static_cast<int32>(FrameSize));
		const FString Payload(Decoded.Length(), Decoded.Get());

		Connection.Buffer.RemoveAt(0, FrameHeaderSize + static_cast<int32>(FrameSize));
		HandleMessage(Connection, Payload);
	}

	return true;
}

void FMCPBridgeServer::HandleMessage(FMCPBridgeConnection& Connection, const FString& Payload)
{
	TSharedPtr<FJsonObject> Request;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);

	if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
	{
		const TSharedRef<FJsonObject> Malformed = MakeShared<FJsonObject>();
		Malformed->SetBoolField(TEXT("success"), false);
		Malformed->SetStringField(TEXT("error"), TEXT("Malformed JSON request"));
		SendResponse(Connection, Malformed);
		return;
	}

	FString Command;
	Request->TryGetStringField(TEXT("command"), Command);

	const TSharedPtr<FJsonObject>* Params = nullptr;
	Request->TryGetObjectField(TEXT("params"), Params);

	const TSharedRef<FJsonObject> Response = MCPBridgeCommands::Dispatch(
		Command, (Params != nullptr && Params->IsValid()) ? *Params : MakeShared<FJsonObject>());

	// Echo the request id so the client can settle the matching pending promise.
	FString RequestId;
	if (Request->TryGetStringField(TEXT("id"), RequestId))
	{
		Response->SetStringField(TEXT("id"), RequestId);
	}

	SendResponse(Connection, Response);
}

void FMCPBridgeServer::SendResponse(FMCPBridgeConnection& Connection, const TSharedRef<FJsonObject>& Response)
{
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Response, Writer);

	const FTCHARToUTF8 Encoded(*Serialized);
	const int32 PayloadSize = Encoded.Length();

	TArray<uint8> Frame;
	Frame.Reserve(FrameHeaderSize + PayloadSize);
	Frame.Add(static_cast<uint8>(PayloadSize & 0xFF));
	Frame.Add(static_cast<uint8>((PayloadSize >> 8) & 0xFF));
	Frame.Add(static_cast<uint8>((PayloadSize >> 16) & 0xFF));
	Frame.Add(static_cast<uint8>((PayloadSize >> 24) & 0xFF));
	Frame.Append(reinterpret_cast<const uint8*>(Encoded.Get()), PayloadSize);

	int32 TotalSent = 0;
	while (TotalSent < Frame.Num())
	{
		int32 Sent = 0;
		if (!Connection.Socket->Send(Frame.GetData() + TotalSent, Frame.Num() - TotalSent, Sent) || Sent <= 0)
		{
			UE_LOG(LogMCPBridge, Warning, TEXT("Send failed after %d/%d bytes"), TotalSent, Frame.Num());
			return;
		}
		TotalSent += Sent;
	}
}

void FMCPBridgeServer::CloseConnection(FMCPBridgeConnection& Connection)
{
	DestroySocket(Connection.Socket);
	Connection.Buffer.Empty();
}

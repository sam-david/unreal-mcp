#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"

class FJsonObject;
class FSocket;
class FTcpListener;
struct FIPv4Endpoint;

/** One connected MCP client plus its partially received frame bytes. */
struct FMCPBridgeConnection
{
	FSocket* Socket = nullptr;
	TArray<uint8> Buffer;
};

/**
 * TCP server speaking the unreal-mcp plugin-bridge protocol: a 4-byte
 * little-endian payload length followed by that many bytes of UTF-8 JSON.
 *
 * Accepts happen on the listener's own thread and are only queued there.
 * Everything that touches a UObject is drained and run on the game thread
 * from Tick, so no command handler needs to think about threading.
 */
class FMCPBridgeServer
{
public:
	bool Start(uint16 InPort);
	void Stop();

private:
	/** Called on the listener thread — must not touch UObjects. */
	bool HandleConnectionAccepted(FSocket* InSocket, const FIPv4Endpoint& InEndpoint);

	bool Tick(float DeltaTime);
	void ReceiveInto(FMCPBridgeConnection& Connection);
	bool ExtractFrames(FMCPBridgeConnection& Connection);
	void HandleMessage(FMCPBridgeConnection& Connection, const FString& Payload);
	void SendResponse(FMCPBridgeConnection& Connection, const TSharedRef<FJsonObject>& Response);
	void CloseConnection(FMCPBridgeConnection& Connection);

	FTcpListener* Listener = nullptr;
	FSocket* ListenSocket = nullptr;
	FTSTicker::FDelegateHandle TickerHandle;

	/** Sockets handed over by the listener thread, adopted by the game thread. */
	TQueue<FSocket*, EQueueMode::Mpsc> PendingSockets;
	TArray<FMCPBridgeConnection> Connections;
};

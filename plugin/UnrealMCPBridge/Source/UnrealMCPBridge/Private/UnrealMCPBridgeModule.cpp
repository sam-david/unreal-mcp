#include "UnrealMCPBridgeModule.h"

#include "MCPBridgeServer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(LogMCPBridge);

#define LOCTEXT_NAMESPACE "FUnrealMCPBridgeModule"

namespace
{
	/** Must match pluginBridgePort in the unreal-mcp server's config.ts. */
	constexpr uint16 DefaultBridgePort = 55557;
}

void FUnrealMCPBridgeModule::StartupModule()
{
	// A commandline override lets a second editor instance run on another port
	// instead of silently losing the bind race with the first.
	uint16 Port = DefaultBridgePort;
	int32 OverridePort = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("MCPBridgePort="), OverridePort)
		&& OverridePort > 0 && OverridePort <= MAX_uint16)
	{
		Port = static_cast<uint16>(OverridePort);
	}

	Server = MakeUnique<FMCPBridgeServer>();
	if (!Server->Start(Port))
	{
		Server.Reset();
	}
}

void FUnrealMCPBridgeModule::ShutdownModule()
{
	if (Server.IsValid())
	{
		Server->Stop();
		Server.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUnrealMCPBridgeModule, UnrealMCPBridge)

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace MCPBridgeCommands
{
	/**
	 * Runs one bridge command on the game thread.
	 *
	 * Always returns a populated response: {"success": true, "data": {...}} or
	 * {"success": false, "error": "..."}. Never throws, so a bad request from
	 * the client can never take the editor down.
	 */
	TSharedRef<FJsonObject> Dispatch(const FString& Command, const TSharedPtr<FJsonObject>& Params);
}

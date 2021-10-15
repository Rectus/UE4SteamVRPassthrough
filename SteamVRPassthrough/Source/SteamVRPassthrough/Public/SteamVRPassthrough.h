
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSteamVRPassthrough, Log, All);

DECLARE_STATS_GROUP(TEXT("SteamVRPassthrough"), STATGROUP_SteamVRPassthrough, STATCAT_Advanced);

class FSteamVRPassthroughModule : public IModuleInterface
{
public:

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

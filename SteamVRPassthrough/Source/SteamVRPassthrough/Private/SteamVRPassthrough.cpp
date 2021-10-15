
#include "SteamVRPassthrough.h"
#include "CoreMinimal.h"
#include "openvr.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogSteamVRPassthrough);

void FSteamVRPassthroughModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/SteamVRPassthrough/Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/SteamVRPassthrough"), PluginShaderDir);
}

void FSteamVRPassthroughModule::ShutdownModule()
{

}


IMPLEMENT_MODULE(FSteamVRPassthroughModule, SteamVRPassthrough)

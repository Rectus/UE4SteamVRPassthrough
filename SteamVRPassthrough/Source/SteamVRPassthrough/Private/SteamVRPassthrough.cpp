
#include "SteamVRPassthrough.h"
#include "CoreMinimal.h"
#include "openvr.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogSteamVRPassthrough);

// Needs to be the same as the version shipped with the engine
#define OPENVR_SDK_VER TEXT("OpenVRv1_5_17")

static bool bIsOpenVRLoaded = false;

void FSteamVRPassthroughModule::StartupModule()
{
	FString PluginShaderDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/SteamVRPassthrough/Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/SteamVRPassthrough"), PluginShaderDir);

	if (FModuleManager::Get().IsModuleLoaded("SteamVR"))
	{
		bIsOpenVRLoaded = true;
	}
	else
	{
		LoadOpenVRModule();
	}
}

void FSteamVRPassthroughModule::ShutdownModule()
{
	if (OpenVRDLLHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(OpenVRDLLHandle);
		OpenVRDLLHandle = nullptr;
	}
}


bool FSteamVRPassthroughModule::IsOpenVRLoaded()
{
	return bIsOpenVRLoaded;
}


void FSteamVRPassthroughModule::LoadOpenVRModule()
{
#if PLATFORM_WINDOWS

#if PLATFORM_64BITS

	FString RootOpenVRPath = FPaths::EngineDir() / FString::Printf(TEXT("Binaries/ThirdParty/OpenVR/%s/Win64/"), OPENVR_SDK_VER);
#else
	FString RootOpenVRPath = FPaths::EngineDir() / FString::Printf(TEXT("Binaries/ThirdParty/OpenVR/%s/Win32/"), OPENVR_SDK_VER);
#endif

	FPlatformProcess::PushDllDirectory(*RootOpenVRPath);
	OpenVRDLLHandle = FPlatformProcess::GetDllHandle(*(RootOpenVRPath + "openvr_api.dll"));
	FPlatformProcess::PopDllDirectory(*RootOpenVRPath);

#elif PLATFORM_LINUX
	FString RootOpenVRPath = FPaths::EngineDir() / FString::Printf(TEXT("Binaries/ThirdParty/OpenVR/%s/linux64/"), OPENVR_SDK_VER);
	OpenVRDLLHandle = FPlatformProcess::GetDllHandle(*(RootOpenVRPath + "libopenvr_api.so"));
#else
#error "SteamVR is not supported for this platform."
#endif	//PLATFORM_WINDOWS

	if (OpenVRDLLHandle)
	{
		bIsOpenVRLoaded = true;
		return;
	}

	UE_LOG(LogSteamVRPassthrough, Error, TEXT("Failed to load OpenVR library."));
	bIsOpenVRLoaded = false;
}



IMPLEMENT_MODULE(FSteamVRPassthroughModule, SteamVRPassthrough)

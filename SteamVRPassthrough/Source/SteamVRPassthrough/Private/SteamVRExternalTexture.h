#pragma once


#include "CoreMinimal.h"
#include "Engine/Texture2DDynamic.h"
#include "TextureResource.h"
#include "openvr.h"

#include "SteamVRExternalTexture.generated.h"


UCLASS(MinimalAPI)
class USteamVRExternalTexture2D : public UTexture2DDynamic
{
	GENERATED_UCLASS_BODY()

public:
	static USteamVRExternalTexture2D* USteamVRExternalTexture2D::Create(int32 InSizeX, int32 InSizeY);
	FTextureResource* CreateResource();
	bool UpdateTextureReference(vr::TrackedCameraHandle_t CameraHandle, vr::EVRTrackedCameraFrameType FrameType);
};


class FSteamVRExternalTextureResource : public FTextureResource
{
public:
	FSteamVRExternalTextureResource(USteamVRExternalTexture2D* InOwner);
	void InitRHI();
	void ReleaseRHI();
	void UpdateTextureSRV(void* TextureSRV);
	FTexture2DRHIRef GetTexture2DRHI();

	uint32 GetSizeX() const
	{
		return Owner->SizeX;
	}

	uint32 GetSizeY() const
	{
		return Owner->SizeY;
	}

private:
	USteamVRExternalTexture2D* Owner;
	FTexture2DRHIRef Texture2DRHI;
};

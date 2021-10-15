


#include "SteamVRExternalTexture.h"
#include "SteamVRPassthrough.h"

// Surpress build errors in D3D11RHIPrivate.h in 4.27 
#ifndef INTEL_EXTENSIONS
#define INTEL_EXTENSIONS 0
#endif
#ifndef INTEL_METRICSDISCOVERY
#define INTEL_METRICSDISCOVERY 0
#endif

#include "D3D11RHI/Private/D3D11RHIPrivate.h"
#include "D3D11Util.h"

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;


USteamVRExternalTexture2D::USteamVRExternalTexture2D(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{}


USteamVRExternalTexture2D* USteamVRExternalTexture2D::Create(int32 InSizeX, int32 InSizeY)
{
	auto NewTexture = NewObject<USteamVRExternalTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);

	NewTexture->Filter = TF_Bilinear;
	NewTexture->SamplerAddressMode = AM_Clamp;
	NewTexture->SRGB = 1;
	NewTexture->CompressionSettings = TC_Default;
	NewTexture->bNoTiling = true;

#if WITH_EDITORONLY_DATA
	NewTexture->CompressionNone = true;
	NewTexture->MipGenSettings = TMGS_NoMipmaps;
	NewTexture->CompressionNoAlpha = true;
	NewTexture->DeferCompression = false;
#endif

	NewTexture->Init(InSizeX, InSizeY, EPixelFormat::PF_R8G8B8A8, false);

	return NewTexture;
}


FTextureResource* USteamVRExternalTexture2D::CreateResource()
{
	return (FTextureResource*) new FSteamVRExternalTextureResource(this);
}


bool USteamVRExternalTexture2D::UpdateTextureReference(vr::TrackedCameraHandle_t CameraHandle, vr::EVRTrackedCameraFrameType FrameType)
{
	if (Resource == nullptr)
	{
		return false;
	}

	//ID3D11Texture2D*
	void* TextureRes = Resource->GetTexture2DRHI()->GetNativeResource();

	//ID3D11ShaderResourceView**
	void** CameraTextureRes = nullptr;

	vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->GetVideoStreamTextureD3D11(CameraHandle, FrameType, TextureRes, (void**)&CameraTextureRes, nullptr, 0);
	if (Error != vr::VRTrackedCameraError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetVideoStreamTextureD3D11 error [%i]"), (int)Error);
		return false ;
	}

	((FSteamVRExternalTextureResource*) Resource)->UpdateTextureSRV(CameraTextureRes);

	return true;
}






FSteamVRExternalTextureResource::FSteamVRExternalTextureResource(USteamVRExternalTexture2D* InOwner)
{
	Owner = InOwner;
}


void FSteamVRExternalTextureResource::InitRHI()
{
	ESamplerAddressMode SamplerAddressMode = Owner->SamplerAddressMode;
	FSamplerStateInitializerRHI SamplerStateInitializer
	(
		ESamplerFilter::SF_Bilinear,
		SamplerAddressMode,
		SamplerAddressMode,
		SamplerAddressMode
	);
	SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);

	ETextureCreateFlags Flags = TexCreate_Shared;
	if (Owner->SRGB)
	{
		Flags |= TexCreate_SRGB;
	}

	FRHIResourceCreateInfo CreateInfo;
	Texture2DRHI = RHICreateTexture2D(GetSizeX(), GetSizeY(), Owner->Format, Owner->NumMips, 1, Flags, CreateInfo);


	TextureRHI = Texture2DRHI;
	TextureRHI->SetName(Owner->GetFName());
	RHIUpdateTextureReference(Owner->TextureReference.TextureReferenceRHI, TextureRHI);
}


void FSteamVRExternalTextureResource::ReleaseRHI()
{
	RHIUpdateTextureReference(Owner->TextureReference.TextureReferenceRHI, nullptr);
	FTextureResource::ReleaseRHI();
	Texture2DRHI.SafeRelease();
}


void FSteamVRExternalTextureResource::UpdateTextureSRV(void* TextureSRV)
{
	if (TextureSRV != nullptr && Texture2DRHI != nullptr)
	{
		FTexture2DRHIRef OldRHITexture = Texture2DRHI;

		TArray<TRefCountPtr<ID3D11RenderTargetView>> RenderTargetViews;

		ID3D11ShaderResourceView* SRV = (ID3D11ShaderResourceView*)TextureSRV;

		ID3D11Resource* SRVResource;
		SRV->GetResource(&SRVResource);

		ETextureCreateFlags Flags = TexCreate_Shared;
		if (Owner->SRGB)
		{
			Flags |= TexCreate_SRGB;
		}

		FD3D11DynamicRHI* DynamicRHI = (FD3D11DynamicRHI*) GDynamicRHI;

		Texture2DRHI = DynamicRHI->RHICreateTexture2DFromResource(PF_R8G8B8A8, Flags, FClearValueBinding::None, (ID3D11Texture2D*)SRVResource);

		TextureRHI = Texture2DRHI;
		TextureRHI->SetName(Owner->GetFName());
		RHIUpdateTextureReference(Owner->TextureReference.TextureReferenceRHI, TextureRHI);

		OldRHITexture.SafeRelease();
	}
}


FTexture2DRHIRef FSteamVRExternalTextureResource::GetTexture2DRHI()
{
	return Texture2DRHI;
}



#include "SteamVRPassthroughComponent.h"
#include "IXRTrackingSystem.h"
#include "IXRCamera.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "openvr.h"
#include "SteamVRPassthroughRendering.h"



USteamVRPassthroughComponent::USteamVRPassthroughComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	FrameType = ESteamVRTrackedCameraFrameType::VRFrameType_MaximumUndistorted;
	PostProcessProjectionDistance = FVector2D(600.0, 100.0);
	StencilTestValue = -1;
	bEnableSharedCameraTexture = true;


	if (HasCamera())
	{
		UpdateFrameLayout();
	}
}


USteamVRPassthroughComponent::~USteamVRPassthroughComponent()
{

}



// Called when the game starts
void USteamVRPassthroughComponent::BeginPlay()
{
	Super::BeginPlay();
}


bool USteamVRPassthroughComponent::HasCamera()
{
	if (!vr::VRSystem() || !vr::VRTrackedCamera())
	{
		return false;
	}

	int32 HMDId = GetDeviceIdForHMD();

	bool bHasCamera = false;

	vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->HasCamera(HMDId, &bHasCamera);

	if (Error != vr::VRTrackedCameraError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Error [%i] checking camera on device Id %i"), (int)Error, HMDId);
		return false;
	}

	return bHasCamera;
}


void USteamVRPassthroughComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bEnabled)
	{
		ENQUEUE_RENDER_COMMAND(UpdatePassthrough)(
			[=](FRHICommandListImmediate& RHICmdList)
		{
			if (PassthroughRenderer.Get() != nullptr)
			{
				PassthroughRenderer.Get()->UpdateFrame_RenderThread();
			}
		});
	}
}


void USteamVRPassthroughComponent::EnableVideo()
{
	if (bEnabled)
	{
		return;
	}

	if (!PassthroughRenderer.IsValid())
	{
		PassthroughRenderer = FSceneViewExtensions::NewExtension<FSteamVRPassthroughRenderer>(FrameType, bEnableSharedCameraTexture);
	}

	if (PassthroughRenderer.Get()->Initialize())
	{
		PassthroughRenderer->SetDepthStencilTestValue(StencilTestValue);
		PassthroughRenderer->SetPostProcessOverlayMode(PostProcessOverlayMode);

		if (PostProcessMaterial)
		{
			if (!PostProcessMatInstance)
			{
				PostProcessMatInstance = UMaterialInstanceDynamic::Create(PostProcessMaterial, this);
			}

			PassthroughRenderer->SetPostProcessMaterial(PostProcessMatInstance);
		}

		float WorldToMeters = GetWorld()->GetWorldSettings()->WorldToMeters;
		PassthroughRenderer->SetPostProcessProjectionDistance(PostProcessProjectionDistance.X / WorldToMeters, PostProcessProjectionDistance.Y / WorldToMeters);

		for (FSteamVRPassthoughUVTransformParameter Parameter : TransformParameters)
		{
			PassthroughRenderer->AddPassthoughTransformParameter(Parameter);
		}

		for (FSteamVRPassthoughTextureParameter Parameter : TextureParameters)
		{
			Parameter.Instance->SetTextureParameterValue(Parameter.TextureParameter, PassthroughRenderer->GetCameraTexture());
		}

		bEnabled = true;
		PrimaryComponentTick.SetTickFunctionEnable(true);
		OnVideoEnabled.Broadcast();
	}
	else
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Error enabling passthrough video!"));
	}
}


void USteamVRPassthroughComponent::DisableVideo()
{
	if (bEnabled)
	{
		// Reset materials to prevent them acessing the texture
		for (FSteamVRPassthoughTextureParameter Parameter : TextureParameters)
		{
			UTexture* DefaultTexture;
			if (Parameter.Instance->GetTextureParameterDefaultValue(Parameter.TextureParameter, DefaultTexture))
			{
				Parameter.Instance->SetTextureParameterValue(Parameter.TextureParameter, DefaultTexture);
			}
			else
			{
				Parameter.Instance->SetTextureParameterValue(Parameter.TextureParameter, DefaultTexture);
			}
			Parameter.Instance->SetTextureParameterValue(Parameter.TextureParameter, nullptr);
		}

		if (PassthroughRenderer.IsValid())
		{
			PassthroughRenderer.Get()->Shutdown();
		}
	}

	bEnabled = false;
	PrimaryComponentTick.SetTickFunctionEnable(false);
	OnVideoDisabled.Broadcast();
}


void USteamVRPassthroughComponent::AddPassthoughTransformParameter(UPARAM(ref) FSteamVRPassthoughUVTransformParameter& InParameter)
{
	FSteamVRPassthoughUVTransformParameter Param = CopyTemp(InParameter);
	float WorldToMeters = GetWorld()->GetWorldSettings()->WorldToMeters;

	Param.ProjectionDistance = Param.ProjectionDistance / WorldToMeters;

	if (PassthroughRenderer.IsValid())
	{
		PassthroughRenderer->AddPassthoughTransformParameter(Param);
	}

	TransformParameters.Add(Param);
}


void USteamVRPassthroughComponent::RemovePassthoughTransformParameters(const UMaterialInstance* Instance)
{
	if (PassthroughRenderer.IsValid())
	{
		PassthroughRenderer->RemovePassthoughTransformParameters(Instance);
	}

	TransformParameters.RemoveAll(
		[Instance](FSteamVRPassthoughUVTransformParameter& Parameter) {
		return Parameter.Instance == Instance;
	});
}


void USteamVRPassthroughComponent::SetPassthoughTextureParameter(UPARAM(ref) FSteamVRPassthoughTextureParameter& InParameter)
{
	if (PassthroughRenderer.IsValid())
	{
		InParameter.Instance->SetTextureParameterValue(InParameter.TextureParameter, PassthroughRenderer->GetCameraTexture());
	}

	TextureParameters.Add(InParameter);
}


void USteamVRPassthroughComponent::SetStencilTestValue(int32 InStencilTestValue)
{
	StencilTestValue = InStencilTestValue;

	if (bEnabled)
	{
		PassthroughRenderer->SetDepthStencilTestValue(StencilTestValue);
	}
}


void USteamVRPassthroughComponent::SetPostProcessMaterial(UMaterialInterface* Material)
{
	PostProcessMaterial = Material;

	if (Material)
	{
		if (Material->IsA(UMaterialInstanceDynamic::StaticClass()))
		{
			PostProcessMatInstance = Cast<UMaterialInstanceDynamic>(Material);
		}
		else
		{
			PostProcessMatInstance = UMaterialInstanceDynamic::Create(PostProcessMaterial, this);
		}
	}

	if (PassthroughRenderer.IsValid())
	{		
		PassthroughRenderer->SetPostProcessMaterial(PostProcessMatInstance);
	}
}


void USteamVRPassthroughComponent::SetPostProcessProjectionDistance(FVector2D InDistance)
{
	PostProcessProjectionDistance = InDistance;

	if (PassthroughRenderer.IsValid())
	{
		float WorldToMeters = GetWorld()->GetWorldSettings()->WorldToMeters;
		PassthroughRenderer->SetPostProcessProjectionDistance(PostProcessProjectionDistance.X / WorldToMeters, PostProcessProjectionDistance.Y / WorldToMeters);
	}
}


void USteamVRPassthroughComponent::SetPostProcessMode(ESteamVRPostProcessPassthroughMode InPostProcessMode)
{
	PostProcessOverlayMode = InPostProcessMode;

	if (PassthroughRenderer.IsValid())
	{
		PassthroughRenderer->SetPostProcessOverlayMode(PostProcessOverlayMode);
	}
}


void USteamVRPassthroughComponent::UpdateFrameLayout()
{
	vr::TrackedPropertyError PropError;

	int32 Layout = (vr::EVRTrackedCameraFrameLayout)vr::VRSystem()->GetInt32TrackedDeviceProperty(GetDeviceIdForHMD(), vr::Prop_CameraFrameLayout_Int32, &PropError);

	if (PropError != vr::TrackedProp_Success)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetTrackedCameraEyePoses error [%i]"), (int)PropError);
		return;
	}

	if (Layout & vr::EVRTrackedCameraFrameLayout_Stereo)
	{
		if (Layout & vr::EVRTrackedCameraFrameLayout_VerticalLayout)
		{
			FrameLayout = StereoVerticalLayout;
		}
		else
		{
			FrameLayout = StereoHorizontalLayout;
		}
	}
	else
	{
		FrameLayout = Mono;
	}
}


int32 USteamVRPassthroughComponent::GetDeviceIdForHMD()
{
	if (!vr::VRSystem())
	{
		return 0;
	}


	for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; i++)
	{
		if (vr::VRSystem()->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_HMD)
		{
			return i;
		}
	}

	return 0;
}




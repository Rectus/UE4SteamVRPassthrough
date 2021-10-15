

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IXRTrackingSystem.h"
#include "SteamVRPassthrough.h"
#include "SteamVRPassthroughRendering.h"
#include "SteamVRPassthroughComponent.generated.h"



DECLARE_DYNAMIC_MULTICAST_DELEGATE(FVideoEnabledDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FVideoDisabledDelegate);


UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class STEAMVRPASSTHROUGH_API USteamVRPassthroughComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;


	/**
	* Broadcast when the passthrough has been successfully initialized.
	*/
	UPROPERTY(BlueprintAssignable, Category = Camera)
	FVideoEnabledDelegate OnVideoEnabled;
	
	/**
	* Broadcast when the passthrough has been disabled.
	*/
	UPROPERTY(BlueprintAssignable, Category = Camera)
	FVideoDisabledDelegate OnVideoDisabled;


	/**
	* Initializes the SteamVR camera system and enables the passthrough.
	*/
	UFUNCTION(BlueprintCallable, Category = "SteamVR|Passthrough")
		void EnableVideo();

	/**
	* Disables the passthrough.
	*/
	UFUNCTION(BlueprintCallable, Category = "SteamVR|Passthrough")
		void DisableVideo();

	/**
	* Static function to detect if a camera is present.
	*/
	UFUNCTION(BlueprintPure, Category = "SteamVR|Passthrough")
		static bool HasCamera();

	/**
	* Registers a set of material parameters that will be continuously updated with the current UV transform matrix 
	* just before rendering. Only scene materials are supported.
	*/
	UFUNCTION(BlueprintCallable, Category = "SteamVR|Passthrough")
		void AddPassthoughTransformParameter(UPARAM(ref) FSteamVRPassthoughUVTransformParameter& InParameter);

	/**
	* Removes all material parameter registartions from a material instance.
	*/
	UFUNCTION(BlueprintCallable, Category = "SteamVR|Passthrough")
		void RemovePassthoughTransformParameters(const UMaterialInstance* Instance);

	/**
	* Registers a texture parameter to be continuously updated with the current camera frame. 
	* Only scene materials are supported.
	*/
	UFUNCTION(BlueprintCallable, Category = "SteamVR|Passthrough")
		void SetPassthoughTextureParameter(UPARAM(ref) FSteamVRPassthoughTextureParameter & InParameter);

	
			
	USteamVRPassthroughComponent();

	~USteamVRPassthroughComponent();

	/**
	* Sets the custom depth stencil value to test for when using the simple postprocess shader.
	* Set to -1 to disable.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter=SetStencilTestValue, Category = PostProcess, DisplayName = "Simple custom depth stencil test")
		int32 StencilTestValue;

	/**
	* Is the camera video stream enabled.
	*/
	UPROPERTY(BlueprintReadOnly, Category = Camera)
		bool bEnabled;

	/**
	* The frame type requested from SteamVR. Only VRFrameType_MaximumUndistorted is properly suppored.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Camera)
		TEnumAsByte<ESteamVRTrackedCameraFrameType> FrameType;

	/**
	* Material to use for the post process passthrough. 
	* Dynamic material instances can be passed to control parameters.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter=SetPostProcessMaterial, Category = PostProcess)
		UMaterialInterface* PostProcessMaterial;

	/**
	* The distance the passthrough image is projected for the postprocess modes.
	* The simple mode only uses the far plane currently.
	* The transformed camera frame UV coordinates are available from the Texture coordinate node
	* with channels 0 and 1 for the far and near distances respectively.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetPostProcessProjectionDistance, Category = PostProcess)
		FVector2D PostProcessProjectionDistance;

	/**
	* Mode for the automatic postprocess overlay.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetPostProcessMode, Category = PostProcess)
		TEnumAsByte<ESteamVRPostProcessPassthroughMode> PostProcessOverlayMode;

	/**
	* Directly use shared textures from the SteamVR compositor. 
	* Only supported on DirectX 11 currently.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Camera)
		bool bEnableSharedCameraTexture;

	/**
	* Read-only access to the camera frame layout.
	*/
	UPROPERTY(BlueprintReadOnly, Category = Camera)
		TEnumAsByte<ESteamVRStereoFrameLayout> FrameLayout;

	UFUNCTION(BlueprintSetter)
		void SetStencilTestValue(int32 InStencilTestValue);

	UFUNCTION(BlueprintSetter)
		void SetPostProcessMaterial(UMaterialInterface* Material);

	UFUNCTION(BlueprintSetter)
		void SetPostProcessProjectionDistance(FVector2D NewDistance = FVector2D(500.0, 100.0));

	UFUNCTION(BlueprintSetter)
		void SetPostProcessMode(ESteamVRPostProcessPassthroughMode InPostProcessMode);

protected:

	virtual void BeginPlay() override;
	void UpdateFrameLayout();
	static int32 GetDeviceIdForHMD();

private:
	
	TSharedPtr<FSteamVRPassthroughRenderer, ESPMode::ThreadSafe> PassthroughRenderer;
	
	UPROPERTY()
		TArray<FSteamVRPassthoughUVTransformParameter> TransformParameters;

	UPROPERTY()
		TArray<FSteamVRPassthoughTextureParameter> TextureParameters;

	UPROPERTY()
		UMaterialInstanceDynamic* PostProcessMatInstance;
};

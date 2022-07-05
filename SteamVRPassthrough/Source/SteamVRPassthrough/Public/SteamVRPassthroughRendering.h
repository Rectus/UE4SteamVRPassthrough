

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "SteamVRPassthrough.h"
#include "openvr.h"

#include "SteamVRPassthroughRendering.generated.h"


UENUM()
enum ESteamVRRuntimeStatus
{
	RuntimeStatus_NotRunning,
	RuntimeStatus_AsXRSystem,
	RuntimeStatus_InBackground
};


UENUM()
enum ESteamVRTrackedCameraFrameType
{
	VRFrameType_Distorted = 0,
	VRFrameType_Undistorted,
	VRFrameType_MaximumUndistorted
};


UENUM()
enum ESteamVRPostProcessPassthroughMode
{
	/** No post process passthrough */
	Mode_Disabled,

	/** Simple shader with optional custom stencil masking */
	Mode_Simple,

	/** Use provided postprocess material */
	Mode_PostProcessMaterial
};


UENUM(BlueprintType)
enum ESteamVRStereoFrameLayout
{
	Mono = 0,
	StereoVerticalLayout = 1, // Stereo frames are Bottom/Top (for left/right respectively)
	StereoHorizontalLayout = 2 // Stereo frames are Left/Right
};


USTRUCT(BlueprintType)
struct STEAMVRPASSTHROUGH_API FSteamVRPassthoughTextureParameter
{
	GENERATED_USTRUCT_BODY();

public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
		UMaterialInstanceDynamic* Instance;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
		FName TextureParameter;

	FSteamVRPassthoughTextureParameter()
		: Instance(nullptr)
		, TextureParameter(FName())
	{}
};


USTRUCT(BlueprintType)
struct STEAMVRPASSTHROUGH_API FSteamVRPassthoughUVTransformParameter
{
	GENERATED_USTRUCT_BODY();

public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	UMaterialInstance* Instance;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName MaterialParameterMatrixX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName MaterialParameterMatrixY;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FName MaterialParameterMatrixZ;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float ProjectionDistance;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 StereoPass;

	FSteamVRPassthoughUVTransformParameter()
		: Instance(nullptr)
		, MaterialParameterMatrixX(FName())
		, MaterialParameterMatrixY(FName())
		, MaterialParameterMatrixZ(FName())
		, ProjectionDistance(500.0)
		, StereoPass(0)
	{}
};


class FSteamVRPassthroughRenderer : public FSceneViewExtensionBase
{
	
public:
	FSteamVRPassthroughRenderer(const FAutoRegister& AutoRegister, ESteamVRTrackedCameraFrameType InFrameType, bool bInUseSharedCameraTexture);
	~FSteamVRPassthroughRenderer();


	bool Initialize();
	void Shutdown();
	void UpdateFrame_RenderThread();

	void SetDepthStencilTestValue(int32 InStencilTestValue)
	{
		StencilTestValue = InStencilTestValue;
	}

	void SetPostProcessOverlayMode(ESteamVRPostProcessPassthroughMode InPostProcessMode)
	{
		PostProcessMode = InPostProcessMode;
	}

	void SetPostProcessMaterial(UMaterialInstanceDynamic* Instance);

	void AddPassthoughTransformParameter(FSteamVRPassthoughUVTransformParameter& InParameter);
	void RemovePassthoughTransformParameters(const UMaterialInstance* Instance);

	UTexture* GetCameraTexture();

	void SetPostProcessProjectionDistance(float InDistanceFar, float InDistanceNear)
	{
		PostProcessProjectionDistanceFar = InDistanceFar;
		PostProcessProjectionDistanceNear = InDistanceNear;
	}

	static bool InitBackgroundRuntime();
	static void ShutdownBackgroundRuntime();
	static ESteamVRRuntimeStatus GetRuntimeStatus();
	static bool HasCamera();
	static ESteamVRStereoFrameLayout GetFrameLayout();

	// ISceneViewExtension
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PreRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override {}
	virtual void PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily) override;
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs) {}

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

	virtual void PostRenderView_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneView& InView) override {}
	virtual int32 GetPriority() const override;
	virtual bool IsActiveThisFrame(FViewport* InViewport) const override;


private:

	static void UpdateHMDDeviceID();

	void SetFramePose_RenderThread(FMatrix44f NewFramePose) {FramePose = NewFramePose;}
	
	FScreenPassTexture DrawFullscreenPassthrough_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs);

	FScreenPassTexture DrawPostProcessMatPassthrough_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs);

	void UpdateFrameTransforms();
	
	bool AcquireVideoStreamingService();
	void ReleaseVideoStreamingService();

	void GetSharedCameraTexture_RenderThread();

	void UpdateVideoStreamFrameBuffer_RenderThread();

	bool UpdateVideoStreamFrameHeader();

	bool UpdateStaticCameraParameters();

	void UpdateTransformParameters();

	void GetCameraIntrinsics(const uint32 CameraId, FVector2f& FocalLength, FVector2f& Center);
	FMatrix44f GetCameraProjection(const uint32 CameraId, const float ZNear, const float ZFar);
	FMatrix44f GetCameraProjectionInv(const uint32 CameraId, const float ZNear, const float ZFar);
	bool GetTrackedCameraEyePoses(FMatrix44f& LeftPose, FMatrix44f& RightPose);
	FMatrix44f GetHMDRawMVPMatrix(const EStereoscopicEye Eye);
	
	/**
	 * Returns a matrix that can transform a [-1 to 1] screenspace quad with the camera frame UV mapped it, 
	 * so that the frame is correctly projected for the view. The right eye UVs need to be shifted by 0.5 horizontally.
	 */
	FMatrix44f GetTrackedCameraQuadTransform(const EStereoscopicEye Eye, const float ProjectionDistanceNear, const float ProjectionDistanceFar);

	/**
	 * Returns a 3x3 matrix that transforms the screenspace UVs for the camera frame.
	 * Since the transformation is non-linear in R2, if done in the vertex shader, 
	 * the output Uvs will need to be passed as homogenous coordinates to the fragment shader.
	 */
	FMatrix44f GetTrackedCameraUVTransform(const EStereoscopicEye Eye, const float ProjectionDistance);

private:

	static bool bIsSteamVRRuntimeInitialized;
	static bool bDeferredRuntimeShutdown;
	static int HMDDeviceId;
	

	bool bIsInitialized;
	bool bUsingBackgroundRuntime;


	ESteamVRPostProcessPassthroughMode PostProcessMode;

	float PostProcessProjectionDistanceFar;
	float PostProcessProjectionDistanceNear;

	int32 StencilTestValue;

	FMatrix44f LeftFrameTransformFar;
	FMatrix44f LeftFrameTransformNear;
	FMatrix44f RightFrameTransformFar;
	FMatrix44f RightFrameTransformNear;

	FMatrix44f FramePose;
	
	vr::EVRTrackedCameraFrameType FrameType;
	vr::TrackedCameraHandle_t CameraHandle;
	vr::CameraVideoStreamFrameHeader_t CameraFrameHeader;
	ESteamVRStereoFrameLayout FrameLayout;

	FMatrix44f CameraLeftToRightPose;
	FMatrix44f CameraLeftToHMDPose;
	FMatrix44f FrameCameraToTrackingPose;

	FMatrix44f RawHMDProjectionLeft;
	FMatrix44f RawHMDViewLeft;
	FMatrix44f RawHMDProjectionRight;
	FMatrix44f RawHMDViewRight;

	TUniquePtr<TMap<FVector2f, FMatrix44f>> LeftCameraMatrixCache;
	TUniquePtr<TMap<FVector2f, FMatrix44f>> RightCameraMatrixCache;

	UTexture* CameraTexture;
	TUniquePtr<FUpdateTextureRegion2D> UpdateTextureRegion;

	uint32 CameraTextureWidth;
	uint32 CameraTextureHeight;
	uint32 CameraFrameBufferSize;
	TUniquePtr<uint8[]> FrameBuffer;
	bool bUseSharedCameraTexture;

	bool bHasValidFrame;

	TUniquePtr<TArray<FSteamVRPassthoughUVTransformParameter>> TransformParameters;

	UMaterialInstanceDynamic* PostProcessMaterial;
	UMaterialInstanceDynamic* PostProcessMaterialTemp;
	
public:
	mutable FCriticalSection ParameterLock;
	mutable FCriticalSection MaterialUpdateLock;
	mutable FCriticalSection RenderLock;
};

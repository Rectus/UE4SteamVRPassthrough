


// Contains snippets from the Unreal Engine 4 source code.
// Copyright Epic Games, Inc. All Rights Reserved.


#include "SteamVRPassthroughRendering.h"
#include "SteamVRExternalTexture.h"

#include "GlobalShader.h"
#include "SceneUtils.h"
#include "SceneInterface.h"
#include "ShaderParameterUtils.h"
#include "ScreenRendering.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessMaterial.h"
#include "SceneRendering.h"
#include "Materials/MaterialInstanceSupport.h"
#include "Materials/Material.h"
#include "MaterialShaderType.h"
#include "MaterialShader.h"
#include "HardwareInfo.h"
#include "SceneTextureParameters.h"
#include "IXRTrackingSystem.h"



DECLARE_CYCLE_STAT(TEXT("SteamVRPassthrough_FrameBufferCopy"), STAT_FrameBufferCopy, STATGROUP_SteamVRPassthrough);
DECLARE_CYCLE_STAT(TEXT("SteamVRPassthrough_FrameTextureUpdate"), STAT_FrameTextureUpdate, STATGROUP_SteamVRPassthrough);
DECLARE_CYCLE_STAT(TEXT("SteamVRPassthrough_PoseUpdate"), STAT_PoseUpdate, STATGROUP_SteamVRPassthrough);


#define MAX_PROJECTION_MATRIX_CACHE_SIZE 8


static TAutoConsoleVariable<bool> CVarAllowBackgroundRuntime(
	TEXT("vr.SteamVRPassthrough.AllowBackgroundRuntime"),
	true,
	TEXT("Allow using the camera passthrough even when the SteamVR OpenVR runtime is not the active XR system.\n")
	TEXT("This will attempt to initialize the OpenVR runtime in background mode\n") 
	TEXT("when the passthrough video is activated while another XR system is active.\n")
	TEXT("This is mainly intended to be used with the headset connected to the SteamVR OpenXR runtime.")
);


static TAutoConsoleVariable<float> CVarFallbackTimingOffset(
	TEXT("vr.SteamVRPassthrough.FallbackTimingOffset"),
	0.081f,
	TEXT("Extra latency in seconds to add when estimating the camera pose.\n")
	TEXT("This is only used when the camera frame is missing pose data.")
);



bool FSteamVRPassthroughRenderer::bIsSteamVRRuntimeInitialized = false;
bool FSteamVRPassthroughRenderer::bDeferredRuntimeShutdown = false;
int FSteamVRPassthroughRenderer::HMDDeviceId = -1;


FORCEINLINE FMatrix ToFMatrix(const vr::HmdMatrix34_t& tm)
{
	return FMatrix(
		FPlane(tm.m[0][0], tm.m[1][0], tm.m[2][0], 0.0f),
		FPlane(tm.m[0][1], tm.m[1][1], tm.m[2][1], 0.0f),
		FPlane(tm.m[0][2], tm.m[1][2], tm.m[2][2], 0.0f),
		FPlane(tm.m[0][3], tm.m[1][3], tm.m[2][3], 1.0f));
}


FORCEINLINE FMatrix ToFMatrix(const vr::HmdMatrix44_t& tm)
{
	return FMatrix(
		FPlane(tm.m[0][0], tm.m[1][0], tm.m[2][0], tm.m[3][0]),
		FPlane(tm.m[0][1], tm.m[1][1], tm.m[2][1], tm.m[3][1]),
		FPlane(tm.m[0][2], tm.m[1][2], tm.m[2][2], tm.m[3][2]),
		FPlane(tm.m[0][3], tm.m[1][3], tm.m[2][3], tm.m[3][3]));
}


class FPassthroughFullsceenVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPassthroughFullsceenVS);
	// LEGACY_BASE needed for FDrawRectangleParameters
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FPassthroughFullsceenVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FMatrix, FrameTransformMatrixFar)
		//SHADER_PARAMETER(FMatrix, FrameTransformMatrixNear)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	END_SHADER_PARAMETER_STRUCT()
};


class FPassthroughFullsceenPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FPassthroughFullsceenPS);
	SHADER_USE_PARAMETER_STRUCT(FPassthroughFullsceenPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_TEXTURE(FTexture2D, CameraTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, CameraTextureSampler)
		SHADER_PARAMETER(FVector2D, FrameUVOffset)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};


IMPLEMENT_GLOBAL_SHADER(FPassthroughFullsceenVS, "/Plugin/SteamVRPassthrough/Private/PassthroughFullsceen.usf", "MainVS", SF_Vertex)
IMPLEMENT_GLOBAL_SHADER(FPassthroughFullsceenPS, "/Plugin/SteamVRPassthrough/Private/PassthroughFullsceen.usf", "MainPS", SF_Pixel)


FScreenPassTexture FSteamVRPassthroughRenderer::DrawFullscreenPassthrough_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs)
{
	FScopeLock Lock(&RenderLock);

	// This gets passed as a FViewInfo from postprocessing
	FViewInfo& View = (FViewInfo&)InView;

	const FScreenPassTexture SceneColor = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);

	if (!IsValid(CameraTexture) || CameraTexture->Resource == nullptr)
	{
		return SceneColor;
	}

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(ERHIFeatureLevel::SM5);
	TShaderMapRef< FPassthroughFullsceenVS > VertexShader(GlobalShaderMap);
	TShaderMapRef< FPassthroughFullsceenPS > PixelShader(GlobalShaderMap);

	FScreenPassRenderTarget SceneColorRenderTarget = Inputs.OverrideOutput;

	if (!SceneColorRenderTarget.IsValid())
	{
		ERenderTargetLoadAction Action = View.bHMDHiddenAreaMaskActive ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ENoAction;

		SceneColorRenderTarget = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, Action, TEXT("Passthrough"));
	}

	AddDrawTexturePass(GraphBuilder, View, SceneColor, SceneColorRenderTarget);
	SceneColorRenderTarget.LoadAction = ERenderTargetLoadAction::ELoad;

	FPassthroughFullsceenPS::FParameters* PSPassParameters = GraphBuilder.AllocParameters<FPassthroughFullsceenPS::FParameters>();
	PSPassParameters->CameraTexture = CameraTexture->Resource->TextureRHI;
	PSPassParameters->CameraTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PSPassParameters->View = View.ViewUniformBuffer;
	PSPassParameters->RenderTargets[0] = SceneColorRenderTarget.GetRenderTargetBinding();

	FPassthroughFullsceenVS::FParameters* VSPassParameters = GraphBuilder.AllocParameters<FPassthroughFullsceenVS::FParameters>();
		
	VSPassParameters->View = View.ViewUniformBuffer;

	if (View.StereoPass == EStereoscopicPass::eSSP_LEFT_EYE)
	{
		VSPassParameters->FrameTransformMatrixFar = LeftFrameTransformFar;
		//VSPassParameters->FrameTransformMatrixNear = LeftFrameTransformNear;
		PSPassParameters->FrameUVOffset = FVector2D(0, 0);
	}
	else
	{
		VSPassParameters->FrameTransformMatrixFar = RightFrameTransformFar;
		//VSPassParameters->FrameTransformMatrixNear = RightFrameTransformNear;

		switch (FrameLayout)
		{
			case ESteamVRStereoFrameLayout::StereoHorizontalLayout:
				PSPassParameters->FrameUVOffset = FVector2D(0.5, 0);
				break;

			case ESteamVRStereoFrameLayout::StereoVerticalLayout:
				PSPassParameters->FrameUVOffset = FVector2D(0, 0.5);
				break;

			case ESteamVRStereoFrameLayout::Mono:
				PSPassParameters->FrameUVOffset = FVector2D(0, 0);
				break;
		}
	}

	FScreenPassPipelineState PipelineState;
	if (StencilTestValue < 0)
	{
		PipelineState = FScreenPassPipelineState(VertexShader, PixelShader);
	}
	else
	{
		PSPassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(Inputs.CustomDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilRead);

		PipelineState = FScreenPassPipelineState(VertexShader, PixelShader, TStaticBlendState<>::GetRHI(), TStaticDepthStencilState<false, CF_Always, true, CF_Equal>::GetRHI());
	}

	int32 StencilVal = StencilTestValue;

	AddDrawScreenPass(
		GraphBuilder,
		RDG_EVENT_NAME("SteamVRPassthrough"),
		View,
		FScreenPassTextureViewport(SceneColorRenderTarget),
		FScreenPassTextureViewport(SceneColor),
		PipelineState,
		PSPassParameters,
		EScreenPassDrawFlags::AllowHMDHiddenAreaMask,
		[VertexShader, PixelShader, PSPassParameters, VSPassParameters, StencilVal](FRHICommandList& RHICmdList)
	{
		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *VSPassParameters);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PSPassParameters);
		if (StencilVal >= 0)
		{
			RHICmdList.SetStencilRef((uint32)StencilVal);
		}
	});


	return MoveTemp(SceneColorRenderTarget);
}



BEGIN_SHADER_PARAMETER_STRUCT(FPassthroughPostProcessMatParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
	SHADER_PARAMETER_STRUCT(FScreenPassTextureViewportParameters, PostProcessOutput)
	SHADER_PARAMETER_STRUCT_ARRAY(FScreenPassTextureInput, PostProcessInput, [kPostProcessMaterialInputCountMax])
	SHADER_PARAMETER_SAMPLER(SamplerState, PostProcessInput_BilinearSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MobileCustomStencilTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, MobileCustomStencilTextureSampler)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, EyeAdaptationTexture)
	SHADER_PARAMETER_SRV(Buffer<float4>, EyeAdaptationBuffer)
	SHADER_PARAMETER(int32, MobileStencilValueRef)
	SHADER_PARAMETER(uint32, bFlipYAxis)
	SHADER_PARAMETER(uint32, bMetalMSAAHDRDecode)
	RENDER_TARGET_BINDING_SLOTS()

	SHADER_PARAMETER(FMatrix, FrameTransformMatrixFar)
	SHADER_PARAMETER(FMatrix, FrameTransformMatrixNear)
	SHADER_PARAMETER(FVector2D, FrameUVOffset)
END_SHADER_PARAMETER_STRUCT()


class FPassthroughPostProcessShader : public FMaterialShader
{
public:
	using FParameters = FPassthroughPostProcessMatParameters;
	SHADER_USE_PARAMETER_STRUCT_WITH_LEGACY_BASE(FPassthroughPostProcessShader, FMaterialShader);

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) && Parameters.MaterialParameters.MaterialDomain == MD_PostProcess;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("POST_PROCESS_MATERIAL"), 1);
		OutEnvironment.SetDefine(TEXT("POST_PROCESS_MATERIAL_BEFORE_TONEMAP"), 0);
		//OutEnvironment.SetDefine(TEXT("POST_PROCESS_AR_PASSTHROUGH"), 1);
	}

protected:
	template <typename TRHIShader>
	static void SetParameters(FRHICommandList& RHICmdList, const TShaderRef<FPassthroughPostProcessShader>& Shader, TRHIShader* ShaderRHI, const FViewInfo& View, const FMaterialRenderProxy* Proxy, const FParameters& Parameters)
	{
		FMaterialShader* MaterialShader = Shader.GetShader();
		MaterialShader->SetParameters(RHICmdList, ShaderRHI, Proxy, *Proxy->GetMaterialNoFallback(View.GetFeatureLevel()), View);
		SetShaderParameters(RHICmdList, Shader, ShaderRHI, Parameters);
	}
};


class FPassthroughPostProcessMatVS : public FPassthroughPostProcessShader
{
public:
	DECLARE_SHADER_TYPE(FPassthroughPostProcessMatVS, Material);

	static void SetParameters(FRHICommandList& RHICmdList, const TShaderRef<FPassthroughPostProcessMatVS>& Shader, const FViewInfo& View, const FMaterialRenderProxy* Proxy, const FParameters& Parameters)
	{
		FPassthroughPostProcessShader::SetParameters(RHICmdList, Shader, Shader.GetVertexShader(), View, Proxy, Parameters);
	}

	FPassthroughPostProcessMatVS() = default;
	FPassthroughPostProcessMatVS(const ShaderMetaType::CompiledShaderInitializerType & Initializer)
		: FPassthroughPostProcessShader(Initializer)
	{}
};


class FPassthroughPostProcessMatPS : public FPassthroughPostProcessShader
{
public:
	DECLARE_SHADER_TYPE(FPassthroughPostProcessMatPS, Material);

	static void SetParameters(FRHICommandList& RHICmdList, const TShaderRef<FPassthroughPostProcessMatPS>& Shader, const FViewInfo& View, const FMaterialRenderProxy* Proxy, const FParameters& Parameters)
	{
		FPassthroughPostProcessShader::SetParameters(RHICmdList, Shader, Shader.GetPixelShader(), View, Proxy, Parameters);
	}

	FPassthroughPostProcessMatPS() = default;
	FPassthroughPostProcessMatPS(const ShaderMetaType::CompiledShaderInitializerType & Initializer)
		: FPassthroughPostProcessShader(Initializer)
	{}
};


IMPLEMENT_SHADER_TYPE(, FPassthroughPostProcessMatVS, TEXT("/Plugin/SteamVRPassthrough/Private/PassthroughPostProcess.usf"), TEXT("MainVS_Passthrough"), SF_Vertex);
IMPLEMENT_SHADER_TYPE(, FPassthroughPostProcessMatPS, TEXT("/Plugin/SteamVRPassthrough/Private/PassthroughPostProcess.usf"), TEXT("MainPS_Passthrough"), SF_Pixel);



// Copied from postprocessmaterial.cpp

FRHIDepthStencilState* GetMaterialStencilState(const FMaterial* Material)
{
	static FRHIDepthStencilState* StencilStates[] =
	{
		TStaticDepthStencilState<false, CF_Always, true, CF_Less>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_LessEqual>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_Greater>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_GreaterEqual>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_Equal>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_NotEqual>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_Never>::GetRHI(),
		TStaticDepthStencilState<false, CF_Always, true, CF_Always>::GetRHI(),
	};
	static_assert(EMaterialStencilCompare::MSC_Count == UE_ARRAY_COUNT(StencilStates), "Ensure that all EMaterialStencilCompare values are accounted for.");

	return StencilStates[Material->GetStencilCompare()];
}


FRHIBlendState* GetMaterialBlendState(const FMaterial* Material)
{
	static FRHIBlendState* BlendStates[] =
	{
		TStaticBlendState<>::GetRHI(),
		TStaticBlendState<>::GetRHI(),
		TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_One>::GetRHI(),
		TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI(),
		TStaticBlendState<CW_RGB, BO_Add, BF_DestColor, BF_Zero>::GetRHI(),
		TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI(),
		TStaticBlendState<CW_RGBA, BO_Add, BF_Zero, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha>::GetRHI(),
	};
	static_assert(EBlendMode::BLEND_MAX == UE_ARRAY_COUNT(BlendStates), "Ensure that all EBlendMode values are accounted for.");

	return BlendStates[Material->GetBlendMode()];
}



FScreenPassTexture FSteamVRPassthroughRenderer::DrawPostProcessMatPassthrough_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessMaterialInputs& Inputs)
{
	FScopeLock Lock(&RenderLock);

	// This gets passed as a FViewInfo from postprocessing
	FViewInfo& View = (FViewInfo&)InView;

	Inputs.Validate();

	const FScreenPassTexture SceneColor = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);

	const FMaterialRenderProxy* MaterialProxy = PostProcessMaterial->GetRenderProxy();
	check(MaterialProxy);

	const FMaterial* const Material = MaterialProxy->GetMaterialNoFallback(InView.GetFeatureLevel());
	

	if (!IsValid(CameraTexture) || CameraTexture->Resource == nullptr || Material == nullptr)
	{
		return SceneColor;
	}

	const FMaterialShaderMap* MaterialShaderMap = Material->GetRenderingThreadShaderMap();

	TShaderRef< FPassthroughPostProcessMatVS > VertexShader = MaterialShaderMap->GetShader<FPassthroughPostProcessMatVS>();
	TShaderRef< FPassthroughPostProcessMatPS > PixelShader = MaterialShaderMap->GetShader<FPassthroughPostProcessMatPS>();

	FScreenPassRenderTarget Output = Inputs.OverrideOutput;


	FRHIDepthStencilState* DefaultDepthStencilState = FScreenPassPipelineState::FDefaultDepthStencilState::GetRHI();
	FRHIDepthStencilState* DepthStencilState = DefaultDepthStencilState;

	FRDGTextureRef DepthStencilTexture = nullptr;



	if (Material->IsStencilTestEnabled())
	{
		check(Inputs.CustomDepthTexture);
		DepthStencilTexture = Inputs.CustomDepthTexture;
		DepthStencilState = GetMaterialStencilState(Material);
	}

	FRHIBlendState* DefaultBlendState = FScreenPassPipelineState::FDefaultBlendState::GetRHI();
	FRHIBlendState* BlendState = DefaultBlendState;

	if (Material->GetBlendableOutputAlpha())
	{
		BlendState = GetMaterialBlendState(Material);
	}


	const bool bCompositeWithInput = DepthStencilState != DefaultDepthStencilState || BlendState != DefaultBlendState;


	if (!Output.IsValid() && !MaterialShaderMap->UsesSceneTexture(PPI_PostProcessInput0) && Inputs.bAllowSceneColorInputAsOutput)
	{
		Output = FScreenPassRenderTarget(SceneColor, ERenderTargetLoadAction::ELoad);
	}
	else if(!Output.IsValid())
	{

		ERenderTargetLoadAction Action = View.bHMDHiddenAreaMaskActive ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ENoAction;

		Output = FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, Action, TEXT("Passthrough"));
	}
	else
	{
		AddDrawTexturePass(GraphBuilder, View, SceneColor, Output);
		Output.LoadAction = ERenderTargetLoadAction::ELoad;
	}


	const FScreenPassTextureViewport OutputViewport(Output);

	const uint32 MaterialStencilRef = Material->GetStencilRefValue();

	FPassthroughPostProcessMatParameters* PassParameters = GraphBuilder.AllocParameters<FPassthroughPostProcessMatParameters>();

	PassParameters->EyeAdaptationTexture = GetEyeAdaptationTexture(GraphBuilder, View);
	PassParameters->SceneTextures = Inputs.SceneTextures;
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->PostProcessOutput = GetScreenPassTextureViewportParameters(OutputViewport);
	PassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	PassParameters->PostProcessInput_BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->bFlipYAxis = false;

	if (DepthStencilTexture)
	{
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			DepthStencilTexture,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthRead_StencilRead);
	}

	const FScreenPassTexture BlackDummy(RegisterExternalOrPassthroughTexture(&GraphBuilder, GSystemTextures.BlackDummy));

	GraphBuilder.RemoveUnusedTextureWarning(BlackDummy.Texture);

	FRHISamplerState* PointClampSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	for (uint32 InputIndex = 0; InputIndex < kPostProcessMaterialInputCountMax; ++InputIndex)
	{
		FScreenPassTexture Input = Inputs.GetInput((EPostProcessMaterialInput)InputIndex);

		if (!Input.Texture || !MaterialShaderMap->UsesSceneTexture(PPI_PostProcessInput0 + InputIndex))
		{
			Input = BlackDummy;
		}

		PassParameters->PostProcessInput[InputIndex] = GetScreenPassTextureInput(Input, PointClampSampler);
	}

	
	if (View.StereoPass == EStereoscopicPass::eSSP_LEFT_EYE)
	{
		PassParameters->FrameTransformMatrixFar = LeftFrameTransformFar;
		PassParameters->FrameTransformMatrixNear = LeftFrameTransformNear;
		PassParameters->FrameUVOffset = FVector2D(0, 0);
	}
	else
	{
		PassParameters->FrameTransformMatrixFar = RightFrameTransformFar;
		PassParameters->FrameTransformMatrixNear = RightFrameTransformNear;

		switch (FrameLayout)
		{
			case ESteamVRStereoFrameLayout::StereoHorizontalLayout:
				PassParameters->FrameUVOffset = FVector2D(0.5, 0);
				break;

			case ESteamVRStereoFrameLayout::StereoVerticalLayout:
				PassParameters->FrameUVOffset = FVector2D(0, 0.5);
				break;

			case ESteamVRStereoFrameLayout::Mono:
				PassParameters->FrameUVOffset = FVector2D(0, 0);
				break;
		}
	}

	ClearUnusedGraphResources(VertexShader, PixelShader, PassParameters);

	AddDrawScreenPass(
		GraphBuilder,
		RDG_EVENT_NAME("SteamVR Passthrough, material=%s", *Material->GetFriendlyName()),
		View,
		OutputViewport,
		FScreenPassTextureViewport(SceneColor),
		FScreenPassPipelineState(VertexShader, PixelShader, BlendState, DepthStencilState),
		PassParameters,
		EScreenPassDrawFlags::AllowHMDHiddenAreaMask,
		[VertexShader, PixelShader, PassParameters, MaterialProxy, &InView, MaterialStencilRef](FRHICommandList& RHICmdList)
	{
		FViewInfo& View = (FViewInfo&)InView;
		FPassthroughPostProcessMatVS::SetParameters(RHICmdList, VertexShader, View, MaterialProxy, *PassParameters);
		FPassthroughPostProcessMatPS::SetParameters(RHICmdList, PixelShader, View, MaterialProxy, *PassParameters);
		RHICmdList.SetStencilRef(MaterialStencilRef);
	});


	return MoveTemp(Output);
}






void FSteamVRPassthroughRenderer::PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily)
{
	FScopeLock Lock(&RenderLock);

	if (CameraHandle == INVALID_TRACKED_CAMERA_HANDLE || !bHasValidFrame)
	{
		return;
	}

	UpdateFrameTransforms();
	UpdateTransformParameters();
}


void FSteamVRPassthroughRenderer::SubscribeToPostProcessingPass(EPostProcessingPass PassId, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	if (!bHasValidFrame)
	{
		return;
	}

	switch (PostProcessMode)
	{
	case Mode_Simple:

		if (PassId == EPostProcessingPass::Tonemap)
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FSteamVRPassthroughRenderer::DrawFullscreenPassthrough_RenderThread));
		}

		break;

	case Mode_PostProcessMaterial:


		if (PassId == EPostProcessingPass::Tonemap && IsValid(PostProcessMaterial))
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FSteamVRPassthroughRenderer::DrawPostProcessMatPassthrough_RenderThread));
		}

		break;
	}	
}





FSteamVRPassthroughRenderer::FSteamVRPassthroughRenderer(const FAutoRegister& AutoRegister, ESteamVRTrackedCameraFrameType InFrameType, bool bInUseSharedCameraTexture)
	: FSceneViewExtensionBase(AutoRegister),
	FrameType((vr::EVRTrackedCameraFrameType) InFrameType)
{
	LeftFrameTransformFar = FMatrix::Identity;
	RightFrameTransformFar = FMatrix::Identity;
	LeftFrameTransformNear = FMatrix::Identity;
	RightFrameTransformNear = FMatrix::Identity;
	bHasValidFrame = false;
	PostProcessMode = Mode_Disabled;
	PostProcessProjectionDistanceFar = 5.0;
	PostProcessProjectionDistanceNear = 1.0;

	TransformParameters = MakeUnique<TArray<FSteamVRPassthoughUVTransformParameter>>();
	LeftCameraMatrixCache = MakeUnique<TMap<FVector2D, FMatrix>>();
	RightCameraMatrixCache = MakeUnique<TMap<FVector2D, FMatrix>>();

#if PLATFORM_WINDOWS
	bUseSharedCameraTexture = FHardwareInfo::GetHardwareInfo(NAME_RHI) == "D3D11" ? bInUseSharedCameraTexture : false;
#else
	bUseSharedCameraTexture = false;
#endif //PLATFORM_WINDOWS

	PostProcessMaterial = nullptr;
	PostProcessMaterialTemp = nullptr;
	bIsInitialized = false;
	bUsingBackgroundRuntime = false;
}


FSteamVRPassthroughRenderer::~FSteamVRPassthroughRenderer()
{
	if (bIsInitialized)
	{
		Shutdown();
	}

	if (bUsingBackgroundRuntime)
	{
		ShutdownBackgroundRuntime();
	}
}


int32 FSteamVRPassthroughRenderer::GetPriority() const { return -11; }


bool FSteamVRPassthroughRenderer::IsActiveThisFrame(FViewport* InViewport) const 
{ 
	return (CameraHandle != INVALID_TRACKED_CAMERA_HANDLE) && bHasValidFrame;
}



void FSteamVRPassthroughRenderer::UpdateFrameTransforms()
{
	SCOPE_CYCLE_COUNTER(STAT_PoseUpdate);

	if (CameraHandle == INVALID_TRACKED_CAMERA_HANDLE || !bHasValidFrame)
	{
		return;
	}

	LeftFrameTransformFar = GetTrackedCameraUVTransform(eSSP_LEFT_EYE, PostProcessProjectionDistanceFar);
	RightFrameTransformFar = GetTrackedCameraUVTransform(eSSP_RIGHT_EYE, PostProcessProjectionDistanceFar);

	if (FMath::IsNearlyEqual(PostProcessProjectionDistanceFar, PostProcessProjectionDistanceNear))
	{
		LeftFrameTransformNear = LeftFrameTransformFar;
		RightFrameTransformNear = RightFrameTransformFar;
	}
	else
	{
		LeftFrameTransformNear = GetTrackedCameraUVTransform(eSSP_LEFT_EYE, PostProcessProjectionDistanceNear);
		RightFrameTransformNear = GetTrackedCameraUVTransform(eSSP_RIGHT_EYE, PostProcessProjectionDistanceNear);
	}
}




bool FSteamVRPassthroughRenderer::Initialize()
{
	check(IsInGameThread());

	UE_LOG(LogSteamVRPassthrough, Log, TEXT("Initializing SteamVR camera passthrough."));

	if (bIsInitialized)
	{
		return true;
	}

	if (!FSteamVRPassthroughModule::IsOpenVRLoaded())
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Attempted to enable passthrough rendering, but the OpenVR library is not loaded."));
		return false;
	}

	if (!GEngine)
	{
		return false;
	}

	if (GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == TEXT("SteamVR"))
	{
		bIsSteamVRRuntimeInitialized = true;
	}
	else if (!bIsSteamVRRuntimeInitialized)
	{
		UE_LOG(LogSteamVRPassthrough, Log, TEXT("Separate XR runtime %s detected, starting background SteamVR instance."), *GEngine->XRSystem->GetSystemName().ToString());

		if (InitBackgroundRuntime())
		{
			bUsingBackgroundRuntime = true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		bUsingBackgroundRuntime = true;
	}


	if (!vr::VRSystem() || !vr::VRTrackedCamera() || !vr::VRCompositor())
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Invalid SteamVR interface handle!"));

		return false;
	}

	UpdateHMDDeviceID();


	if (HMDDeviceId < 0)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("HMD device ID not found!"));
		return false;
	}

	if (!UpdateStaticCameraParameters())
	{
		return false;
	}

	if (bUseSharedCameraTexture)
	{
		CameraTexture = USteamVRExternalTexture2D::Create(CameraTextureWidth, CameraTextureHeight);
		CameraTexture->AddToRoot();
	}
	else
	{
		UpdateTextureRegion = TUniquePtr<FUpdateTextureRegion2D>(new FUpdateTextureRegion2D(0, 0, 0, 0, CameraTextureWidth, CameraTextureHeight));

		FrameBuffer = TUniquePtr<uint8[]>(new uint8[CameraFrameBufferSize]());

		UTexture2D* NewTexture = UTexture2D::CreateTransient(CameraTextureWidth, CameraTextureHeight, EPixelFormat::PF_R8G8B8A8);

#if WITH_EDITORONLY_DATA
		NewTexture->MipGenSettings = TextureMipGenSettings::TMGS_NoMipmaps;
#endif
		NewTexture->CompressionSettings = TextureCompressionSettings::TC_Default;
		NewTexture->SRGB = 1;
		NewTexture->Filter = TextureFilter::TF_Default;
		NewTexture->AddressX = TA_Clamp;
		NewTexture->AddressY = TA_Clamp;
		NewTexture->AddToRoot();
		NewTexture->UpdateResource();

		CameraTexture = NewTexture;
	}
	
	if (AcquireVideoStreamingService())
	{
		bIsInitialized = true;
	}

	return bIsInitialized;
}


void FSteamVRPassthroughRenderer::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	UE_LOG(LogSteamVRPassthrough, Log, TEXT("Shutting down SteamVR camera passthrough."));

	FScopeLock Lock(&RenderLock);

	bHasValidFrame = false;
	bIsInitialized = false;

	if (CameraHandle != INVALID_TRACKED_CAMERA_HANDLE)
	{
		ReleaseVideoStreamingService();
	}

	if (IsValid(CameraTexture))
	{
		CameraTexture->RemoveFromRoot();
	}

	CameraTexture = nullptr;
	PostProcessMaterial = nullptr;
	PostProcessMaterialTemp = nullptr;
	TransformParameters.Get()->Empty();
}



void FSteamVRPassthroughRenderer::SetPostProcessMaterial(UMaterialInstanceDynamic* Instance)
{
	check(IsInGameThread());

	FScopeLock Lock(&MaterialUpdateLock);

	PostProcessMaterialTemp = Instance;

	if (IsValid(Instance))
	{
		PostProcessMaterialTemp->AddToRoot();

		if (IsValid(CameraTexture))
		{
			Instance->SetTextureParameterValue("CameraTexture", CameraTexture);
		}
	}
}


void FSteamVRPassthroughRenderer::UpdateFrame_RenderThread()
{
	{
		FScopeLock Lock(&MaterialUpdateLock);

		if (PostProcessMaterial != PostProcessMaterialTemp)
		{
			if (PostProcessMaterial && IsValid(PostProcessMaterial))
			{
				PostProcessMaterial->RemoveFromRoot();
			}

			PostProcessMaterial = PostProcessMaterialTemp;
		}
	}


	if (CameraHandle == INVALID_TRACKED_CAMERA_HANDLE)
	{
		return;
	}

	if (UpdateVideoStreamFrameHeader())
	{
		
		if (bUseSharedCameraTexture)
		{
			GetSharedCameraTexture_RenderThread();
		}
		else
		{
			UpdateVideoStreamFrameBuffer_RenderThread();
		}
	}		
}


UTexture* FSteamVRPassthroughRenderer::GetCameraTexture()
{
	return CameraTexture;
}


void FSteamVRPassthroughRenderer::UpdateHMDDeviceID()
{
	for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; i++)
	{
		if (vr::VRSystem()->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_HMD)
		{
			HMDDeviceId = i;
			return;
		}
	}
}


bool FSteamVRPassthroughRenderer::InitBackgroundRuntime()
{
	if (!CVarAllowBackgroundRuntime.GetValueOnAnyThread())
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Background SteamVR runtime usage is disabled!"));

		return false;
	}

	if (bIsSteamVRRuntimeInitialized)
	{
		return true;
	}

	vr::EVRInitError Error;
	vr::VR_Init(&Error, vr::EVRApplicationType::VRApplication_Background);

	if (Error != vr::EVRInitError::VRInitError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Failed to init SteamVR runtime as background app, error [%i]"), (int)Error, HMDDeviceId);
		return false;
	}

	if (!vr::VRSystem() || !vr::VRTrackedCamera() || !vr::VRCompositor())
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Invalid SteamVR interface handle!"));

		return false;
	}

	UpdateHMDDeviceID();

	if (HMDDeviceId < 0)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("HMD device ID not found!"));
		return false;
	}

	bIsSteamVRRuntimeInitialized = true;
	return true;
}


void FSteamVRPassthroughRenderer::ShutdownBackgroundRuntime()
{
	if (GetRuntimeStatus() != RuntimeStatus_InBackground)
	{
		return;
	}

	// Calling VR_Shutdown while the OpenXR runtime is active will hang the game,
	// so add a delegate to do it on shutdown.
	if (GEngine && GEngine->XRSystem.IsValid())
	{
		if (!bDeferredRuntimeShutdown)
		{
			bDeferredRuntimeShutdown = true;
			FCoreDelegates::OnExit.AddLambda([]()
			{
				FSteamVRPassthroughRenderer::ShutdownBackgroundRuntime();
			});
		}
		return;
	}

	vr::VR_Shutdown();
	bIsSteamVRRuntimeInitialized = false;
	bDeferredRuntimeShutdown = false;
}


ESteamVRRuntimeStatus FSteamVRPassthroughRenderer::GetRuntimeStatus()
{
	if (GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == TEXT("SteamVR"))
	{
		return RuntimeStatus_AsXRSystem;
	}
	
	return bIsSteamVRRuntimeInitialized ? RuntimeStatus_InBackground : RuntimeStatus_NotRunning;
}


bool FSteamVRPassthroughRenderer::HasCamera()
{
	if (!FSteamVRPassthroughModule::IsOpenVRLoaded())
	{
		return false;
	}

	ESteamVRRuntimeStatus Status = FSteamVRPassthroughRenderer::GetRuntimeStatus();

	if (Status == RuntimeStatus_NotRunning)
	{
		if (!GEngine || !GEngine->XRSystem.IsValid() || !CVarAllowBackgroundRuntime.GetValueOnAnyThread())
		{
			return false;
		}

		if (!FSteamVRPassthroughRenderer::InitBackgroundRuntime())
		{
			return false;
		}
	}

	if (HMDDeviceId < 0)
	{
		UpdateHMDDeviceID();
	}

	bool bHasCamera = false;

	vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->HasCamera(HMDDeviceId, &bHasCamera);

	if (Error != vr::VRTrackedCameraError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Error [%i] checking camera on device Id %i"), (int)Error, HMDDeviceId);
		return false;
	}

	return bHasCamera;
}


ESteamVRStereoFrameLayout FSteamVRPassthroughRenderer::GetFrameLayout()
{
	ESteamVRRuntimeStatus Status = FSteamVRPassthroughRenderer::GetRuntimeStatus();

	if (Status == RuntimeStatus_NotRunning && !FSteamVRPassthroughRenderer::InitBackgroundRuntime())
	{
		return ESteamVRStereoFrameLayout::Mono;
	}

	vr::TrackedPropertyError Error;

	int32 Layout = (vr::EVRTrackedCameraFrameLayout) vr::VRSystem()->GetInt32TrackedDeviceProperty(HMDDeviceId, vr::Prop_CameraFrameLayout_Int32, &Error);

	if (Error != vr::TrackedProp_Success)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetTrackedCameraEyePoses error [%i]"), (int)Error);
		return ESteamVRStereoFrameLayout::Mono;
	}

	if ((Layout & vr::EVRTrackedCameraFrameLayout_Stereo) != 0)
	{
		if ((Layout & vr::EVRTrackedCameraFrameLayout_VerticalLayout) != 0)
		{
			return ESteamVRStereoFrameLayout::StereoVerticalLayout;
		}
		else
		{
			return ESteamVRStereoFrameLayout::StereoHorizontalLayout;
		}
	}
	else
	{
		return ESteamVRStereoFrameLayout::Mono;
	}
}


bool FSteamVRPassthroughRenderer::AcquireVideoStreamingService()
{
	if (vr::VRTrackedCamera())
	{
		vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->AcquireVideoStreamingService(HMDDeviceId, &CameraHandle);

		if (Error != vr::VRTrackedCameraError_None)
		{
			UE_LOG(LogSteamVRPassthrough, Warning, TEXT("AcquireVideoStreamingService error [%i] on device Id %i"), (int)Error, HMDDeviceId);
			return false;
		}

		return true;
	}

	return false;
}


void FSteamVRPassthroughRenderer::ReleaseVideoStreamingService()
{
	if (vr::VRTrackedCamera() && CameraHandle != INVALID_TRACKED_CAMERA_HANDLE)
	{
		vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->ReleaseVideoStreamingService(CameraHandle);

		if (Error != vr::VRTrackedCameraError_None)
		{
			UE_LOG(LogSteamVRPassthrough, Warning, TEXT("ReleaseVideoStreamingService error [%i]"), (int)Error);
		}
	}
	CameraHandle = INVALID_TRACKED_CAMERA_HANDLE;
	bHasValidFrame = false;
}


void FSteamVRPassthroughRenderer::GetSharedCameraTexture_RenderThread()
{
	if (!IsValid(CameraTexture))
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_FrameTextureUpdate);

	USteamVRExternalTexture2D* CameraTextureExt = Cast<USteamVRExternalTexture2D>(CameraTexture);

	if (CameraTextureExt && CameraTextureExt->UpdateTextureReference(CameraHandle, FrameType))
	{
		bHasValidFrame = true;
	}
}


void FSteamVRPassthroughRenderer::UpdateVideoStreamFrameBuffer_RenderThread()
{
	check(IsInRenderingThread());

	if (!vr::VRTrackedCamera() || !FrameBuffer.IsValid())
	{
		return;
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_FrameBufferCopy);

		vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->GetVideoStreamFrameBuffer(CameraHandle, FrameType, FrameBuffer.Get(), CameraFrameBufferSize * sizeof(uint8), nullptr, 0);

		if (Error == vr::VRTrackedCameraError_NoFrameAvailable)
		{
			bHasValidFrame = false;
			return;
		}
		else if (Error != vr::VRTrackedCameraError_None)
		{
			UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetVideoStreamFrameBuffer error [%i]"), (int)Error);
			return;
		}

	}

	if (!IsValid(CameraTexture) || CameraTexture->Resource == nullptr || !FrameBuffer.IsValid())
	{
		return;
	}

	bHasValidFrame = true;

	{
		SCOPE_CYCLE_COUNTER(STAT_FrameTextureUpdate);

		RHIUpdateTexture2D(
			(FRHITexture2D*)CameraTexture->Resource->TextureRHI.GetReference(),
			0,
			*UpdateTextureRegion.Get(),
			CameraTextureWidth * 4,
			FrameBuffer.Get()
		);
	}
}


bool FSteamVRPassthroughRenderer::UpdateVideoStreamFrameHeader()
{
	if (CameraHandle == INVALID_TRACKED_CAMERA_HANDLE || !vr::VRTrackedCamera())
	{
		return false;
	}

	vr::CameraVideoStreamFrameHeader_t NewFrameHeader;

	vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->GetVideoStreamFrameBuffer(CameraHandle, FrameType, nullptr, 0, &NewFrameHeader, sizeof(vr::CameraVideoStreamFrameHeader_t));

	if (Error == vr::VRTrackedCameraError_NoFrameAvailable)
	{
		return false;
	}
	else if (Error != vr::VRTrackedCameraError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetVideoStreamFrameBuffer error [%i]"), (int)Error);
		return false;
	}

	if (NewFrameHeader.nFrameSequence == CameraFrameHeader.nFrameSequence)
	{
		return false;
	}

	CameraFrameHeader = NewFrameHeader;

	if (NewFrameHeader.standingTrackedDevicePose.bPoseIsValid)
	{
		// The pose in the frame header is of the left camera space to tracking space.
		FrameCameraToTrackingPose = ToFMatrix(CameraFrameHeader.standingTrackedDevicePose.mDeviceToAbsoluteTracking);
	}
	else
	{
		static bool ErrorSeen = false;
		if (!ErrorSeen)
		{
			UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Camera frame header is missing pose data, this will severely reduce frame stability!"));
			ErrorSeen = true;
		}

		// Sync the frame timing offset to vsync for more stability.
		float TimeRemaining = vr::VRCompositor()->GetFrameTimeRemaining();
		float FrameDelta = TimeRemaining - CVarFallbackTimingOffset.GetValueOnRenderThread();
	
		vr::TrackedDevicePose_t Poses[vr::k_unMaxTrackedDeviceCount];

		vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin::TrackingUniverseStanding, FrameDelta, Poses, vr::k_unMaxTrackedDeviceCount);

		FMatrix HMDPose = ToFMatrix(Poses[HMDDeviceId].mDeviceToAbsoluteTracking);

		FrameCameraToTrackingPose = CameraLeftToHMDPose * HMDPose;
	}

	return true;
}


bool FSteamVRPassthroughRenderer::UpdateStaticCameraParameters()
{

	vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->GetCameraFrameSize(HMDDeviceId, FrameType, &CameraTextureWidth, &CameraTextureHeight, &CameraFrameBufferSize);

	if (Error != vr::VRTrackedCameraError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("CameraFrameSize error [%i] on device Id %i"), (int)Error, HMDDeviceId);
		return false;
	}

	if (CameraTextureWidth == 0 || CameraTextureHeight == 0 || CameraFrameBufferSize == 0)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("Invalid frame size received:Width = %u, Height = %u, Size = %u"), CameraTextureWidth, CameraTextureHeight, CameraFrameBufferSize);
		return false;
	}
	
	FrameLayout = GetFrameLayout();

	RawHMDProjectionLeft = ToFMatrix(vr::VRSystem()->GetProjectionMatrix(vr::Hmd_Eye::Eye_Left, PostProcessProjectionDistanceFar * 0.1, PostProcessProjectionDistanceFar * 2.0));
	RawHMDViewLeft = ToFMatrix(vr::VRSystem()->GetEyeToHeadTransform(vr::Hmd_Eye::Eye_Left)).Inverse();

	RawHMDProjectionRight = ToFMatrix(vr::VRSystem()->GetProjectionMatrix(vr::Hmd_Eye::Eye_Right, PostProcessProjectionDistanceFar * 0.1, PostProcessProjectionDistanceFar * 2.0));
	RawHMDViewRight = ToFMatrix(vr::VRSystem()->GetEyeToHeadTransform(vr::Hmd_Eye::Eye_Right)).Inverse();

	FMatrix LeftCameraPose, RightCameraPose;
	GetTrackedCameraEyePoses(LeftCameraPose, RightCameraPose);
	CameraLeftToHMDPose = CopyTemp(LeftCameraPose);
	CameraLeftToRightPose = CopyTemp(RightCameraPose * LeftCameraPose.Inverse());

	return true;
}


void FSteamVRPassthroughRenderer::GetCameraIntrinsics(const uint32 CameraId, FVector2D& FocalLength, FVector2D& Center)
{
	if (vr::VRTrackedCamera())
	{
		vr::HmdVector2_t VRFocalLength;
		vr::HmdVector2_t VRCenter;

		vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->GetCameraIntrinsics(HMDDeviceId, CameraId, FrameType, &VRFocalLength, &VRFocalLength);

		if (Error != vr::VRTrackedCameraError_None)
		{
			UE_LOG(LogSteamVRPassthrough, Warning, TEXT("CameraIntrinsics error [%i] on device Id %i"), (int)Error, HMDDeviceId);
			return;
		}

		FocalLength.X = VRFocalLength.v[0];
		FocalLength.Y = VRFocalLength.v[1];

		Center.X = VRCenter.v[0];
		Center.Y = VRCenter.v[1];
	}
}


FMatrix FSteamVRPassthroughRenderer::GetCameraProjection(const uint32 CameraId, const float ZNear, const float ZFar)
{
	if (!vr::VRTrackedCamera())
	{
		return FMatrix::Identity;
	}
	vr::HmdMatrix44_t VRProjection;

	vr::EVRTrackedCameraError Error = vr::VRTrackedCamera()->GetCameraProjection(HMDDeviceId, CameraId, (vr::EVRTrackedCameraFrameType)FrameType, ZNear, ZFar, &VRProjection);

	if (Error != vr::VRTrackedCameraError_None)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("CameraProjection error [%i] on device Id %i"), (int)Error, HMDDeviceId);
		return FMatrix::Identity;
	}

	return ToFMatrix(VRProjection);
}


FMatrix FSteamVRPassthroughRenderer::GetCameraProjectionInv(const uint32 CameraId, const float ZNear, const float ZFar)
{
	TMap<FVector2D, FMatrix>* Cache = (CameraId == 0) ? LeftCameraMatrixCache.Get() : RightCameraMatrixCache.Get();

	FMatrix* Matrix = Cache->Find(FVector2D(ZNear, ZFar));

	if (Matrix != nullptr)
	{
		return *Matrix;
	}
	else
	{
		FMatrix NewMatrix = GetCameraProjection(CameraId, ZNear, ZFar).InverseFast();

		// Reset the cache so it doesn't grow too large in case lots of values are requested
		if (Cache->Num() >= MAX_PROJECTION_MATRIX_CACHE_SIZE)
		{
			Cache->Reset();
		}

		Cache->Add(FVector2D(ZNear, ZFar), NewMatrix);
		return NewMatrix;
	}
}


void FSteamVRPassthroughRenderer::GetTrackedCameraEyePoses(FMatrix& LeftPose, FMatrix& RightPose)
{
	if (!vr::VRSystem())
	{
		return;
	}

	vr::HmdMatrix34_t Buffer[2];
	vr::TrackedPropertyError Error;

	vr::VRSystem()->GetArrayTrackedDeviceProperty(HMDDeviceId, vr::Prop_CameraToHeadTransforms_Matrix34_Array, vr::k_unHmdMatrix34PropertyTag, &Buffer, sizeof(Buffer), &Error);

	if (Error != vr::TrackedProp_Success)
	{
		UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetTrackedCameraEyePoses error [%i]"), (int)Error);
		return;
	}

	LeftPose = ToFMatrix(Buffer[0]);

	if (FrameLayout != ESteamVRStereoFrameLayout::Mono)
	{
		RightPose = ToFMatrix(Buffer[1]);
	}
	else
	{
		RightPose = FMatrix::Identity;
	}
}


FMatrix FSteamVRPassthroughRenderer::GetHMDRawMVPMatrix(const EStereoscopicPass Eye)
{
	if (!vr::VRSystem() || !vr::VRCompositor())
	{
		return FMatrix::Identity;
	}

	FMatrix Model;

	if (bUsingBackgroundRuntime)
	{
		// GetLastPoseForTrackedDeviceIndex will not return a value in the proper tracking space
		// when using OpenXR, so calculate the timing and use GetDeviceToAbsoluteTrackingPose instead.
		float TimeRemaining = vr::VRCompositor()->GetFrameTimeRemaining();
		float DisplayFrequency = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
		float FrameDuration = 1.f / DisplayFrequency;
		float VsyncToPhotons = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);

		float PredictedSecondsFromNow = FrameDuration + TimeRemaining + VsyncToPhotons;
		vr::TrackedDevicePose_t Poses[vr::k_unMaxTrackedDeviceCount];

		vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin::TrackingUniverseStanding, PredictedSecondsFromNow, Poses, vr::k_unMaxTrackedDeviceCount);

		Model = ToFMatrix(Poses[HMDDeviceId].mDeviceToAbsoluteTracking);
	}
	else
	{
		vr::EVRCompositorError Error;
		vr::TrackedDevicePose_t HMDPose;
		Error = vr::VRCompositor()->GetLastPoseForTrackedDeviceIndex(HMDDeviceId, &HMDPose, nullptr);

		if (Error != vr::VRCompositorError_None)
		{
			UE_LOG(LogSteamVRPassthrough, Warning, TEXT("GetLastPoseForTrackedDeviceIndex error [%i]"), (int)Error);
			return FMatrix::Identity;
		}

		Model = ToFMatrix(HMDPose.mDeviceToAbsoluteTracking);
	}
	Model = Model.Inverse();

	if (Eye == eSSP_LEFT_EYE)
	{
		return CopyTemp(Model * RawHMDViewLeft * RawHMDProjectionLeft);
	}
	else
	{
		return CopyTemp(Model * RawHMDViewRight * RawHMDProjectionRight);
	}
}


FMatrix FSteamVRPassthroughRenderer::GetTrackedCameraQuadTransform(const EStereoscopicPass Eye, const float ProjectionDistanceNear, const float ProjectionDistanceFar)
{
	bool bIsStereo = FrameLayout != ESteamVRStereoFrameLayout::Mono;
	uint32 CameraId = (Eye == eSSP_RIGHT_EYE && bIsStereo) ? 1 : 0;

	FMatrix MVP = GetHMDRawMVPMatrix(Eye);
	FMatrix CameraProjectionInv = GetCameraProjectionInv(CameraId, ProjectionDistanceNear, ProjectionDistanceFar);

	// The output is transposed since the UE4 FMatrix has a different order than the shader.
	if (CameraId == 0)
	{
		return CopyTemp((CameraProjectionInv * FrameCameraToTrackingPose * MVP).GetTransposed());
	}
	else
	{
		return CopyTemp((CameraProjectionInv * CameraLeftToRightPose * FrameCameraToTrackingPose * MVP).GetTransposed());
	}
}


FMatrix FSteamVRPassthroughRenderer::GetTrackedCameraUVTransform(const EStereoscopicPass Eye, const float ProjectionDistance)
{
	bool bIsStereo = FrameLayout != ESteamVRStereoFrameLayout::Mono;
	uint32 CameraId = (Eye == eSSP_RIGHT_EYE && bIsStereo) ? 1 : 0;

	FMatrix MVP = GetHMDRawMVPMatrix(Eye);
	FMatrix CameraProjectionInv = GetCameraProjectionInv(CameraId, ProjectionDistance * 0.5, ProjectionDistance);

	FMatrix TransformToCamera;

	if (CameraId == 0)
	{
		TransformToCamera = CameraProjectionInv * FrameCameraToTrackingPose * MVP;
	}
	else
	{
		TransformToCamera = CameraProjectionInv * CameraLeftToRightPose * FrameCameraToTrackingPose * MVP;
	}

	// Calculate matrix for transforming the clip space quad to the quad output by the camera transform
	// as per: https://mrl.cs.nyu.edu/~dzorin/ug-graphics/lectures/lecture7/

	FVector4 P1 = FVector4(-1, -1, 1, 1);
	FVector4 P2 = FVector4(1, -1, 1, 1);
	FVector4 P3 = FVector4(1, 1, 1, 1);
	FVector4 P4 = FVector4(-1, 1, 1, 1);

	FVector4 Q1 = TransformToCamera.TransformPosition(P1);
	FVector4 Q2 = TransformToCamera.TransformPosition(P2);
	FVector4 Q3 = TransformToCamera.TransformPosition(P3);
	FVector4 Q4 = TransformToCamera.TransformPosition(P4);

	FVector R1 = FVector(Q1.X, Q1.Y, Q1.W);
	FVector R2 = FVector(Q2.X, Q2.Y, Q2.W);
	FVector R3 = FVector(Q3.X, Q3.Y, Q3.W);
	FVector R4 = FVector(Q4.X, Q4.Y, Q4.W);


	FVector H1 = FVector::CrossProduct(FVector::CrossProduct(R2, R1), FVector::CrossProduct(R3, R4));
	FVector H2 = FVector::CrossProduct(FVector::CrossProduct(R1, R4), FVector::CrossProduct(R2, R3));
	FVector H3 = FVector::CrossProduct(FVector::CrossProduct(R1, R3), FVector::CrossProduct(R2, R4));

	// This should ideally be a 3x3 matrix.
	FMatrix T = FMatrix(
		FPlane(H1.X, H2.X, H3.X, 0),
		FPlane(H1.Y, H2.Y, H3.Y, 0),
		FPlane(H1.Z, H2.Z, H3.Z, 0),
		FPlane(0, 0, 0, 1));

	T = T.InverseFast();
	// No transpose here since the UE4 FMatrix has a different order than the shader.

	T.M[0][3] = 0;
	T.M[1][3] = 0;
	T.M[2][3] = 0;
	T.M[3][0] = 0;
	T.M[3][1] = 0;
	T.M[3][2] = 0;
	T.M[3][3] = 1;

	FMatrix UVToScreen(
		FPlane(2.0, 0, -1, 0),
		FPlane(0, -2.0, 1, 0),
		FPlane(0, 0, 1, 0),
		FPlane(0, 0, 0, 1));

	FMatrix ScreenToUV(
		FPlane(-0.5, 0, 0.5, 0),
		FPlane(0, -0.5, 0.5, 0),
		FPlane(0, 0, 1, 0),
		FPlane(0, 0, 0, 1));


	return CopyTemp(ScreenToUV * T * UVToScreen);
}


void FSteamVRPassthroughRenderer::AddPassthoughTransformParameter(FSteamVRPassthoughUVTransformParameter& InParameter)
{
	FScopeLock Lock(&ParameterLock);

	TransformParameters->Add(InParameter);
}


void FSteamVRPassthroughRenderer::RemovePassthoughTransformParameters(const UMaterialInstance* Instance)
{
	FScopeLock Lock(&ParameterLock);

	TransformParameters->RemoveAll(
		[Instance](FSteamVRPassthoughUVTransformParameter& Parameter) {
		return Parameter.Instance == Instance;
	});
}


void FSteamVRPassthroughRenderer::UpdateTransformParameters()
{
	FScopeLock Lock(&ParameterLock);

	for (FSteamVRPassthoughUVTransformParameter ParameterStruct : *TransformParameters)
	{
		if (!IsValid(ParameterStruct.Instance))
		{
			continue;
		}
		EStereoscopicPass Eye = ParameterStruct.StereoPass == 0 ? eSSP_LEFT_EYE : eSSP_RIGHT_EYE;

		FMatrix Transform = GetTrackedCameraUVTransform(Eye, ParameterStruct.ProjectionDistance);

		FMaterialInstanceResource* Resource = ParameterStruct.Instance->Resource;

		Resource->RenderThread_UpdateParameter(ParameterStruct.MaterialParameterMatrixX, FLinearColor(Transform.M[0][0], Transform.M[0][1], Transform.M[0][2], 0));
		Resource->RenderThread_UpdateParameter(ParameterStruct.MaterialParameterMatrixY, FLinearColor(Transform.M[1][0], Transform.M[1][1], Transform.M[1][2], 0));
		Resource->RenderThread_UpdateParameter(ParameterStruct.MaterialParameterMatrixZ, FLinearColor(Transform.M[2][0], Transform.M[2][1], Transform.M[2][2], 0));

		Resource->InvalidateUniformExpressionCache(false);
		Resource->CacheUniformExpressions(false);
		Resource->UpdateDeferredCachedUniformExpressions();
	}
}



// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "GlobalShader.h"
#include "DeferredShadingRenderer.h"
#include "SceneTextureParameters.h"

#if RHI_RAYTRACING

#include "ClearQuad.h"
#include "SceneRendering.h"
#include "SceneRenderTargets.h"
#include "RHIResources.h"
#include "PostProcess/PostProcessing.h"
#include "RayTracing/RaytracingOptions.h"
#include "Raytracing/RaytracingLighting.h"
#include "TemporalAA.h"

static TAutoConsoleVariable<int32> CVarRayTracingPrimaryRaysHalfResRefractionReconstructMethod(
	TEXT("r.RayTracing.Translucency.HalfRes.RefractionReconstructMethod"),
	-1,
	TEXT(" -1 - auto select ")
	TEXT(" 0 - don't consider scene color texture ")
	TEXT(" 1 - consider scene color texture\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingPrimaryRaysHalfResRecoveredTexcoordMode(
	TEXT("r.RayTracing.Translucency.HalfRes.RecoveredTexcoordMode"),
	-1,
	TEXT(" -1 - auto select ")
	TEXT(" 0 - don't round recovered texcoord ")
	TEXT(" 1 - round recovered texcoord)\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarRayTracingPrimaryRaysReflectionCaptures(
	TEXT("r.RayTracing.PrimaryRays.ReflectionCaptures"),
	1,
	TEXT("Whether to sample reflection captures to apply secondary reflections on primary rays including translucency (default 1)"),
	ECVF_RenderThreadSafe);

enum EPrimaryRaysHalfResMode
{
	HalfRes_Off = 0,
	Checkerboard_Weighted = 1,
	Checkerboard_Interframe = 2,
	Checkerboard_Average = 3,
	MAX // required by SHADER_PERMUTATION_ENUM_CLASS()
};

DECLARE_GPU_STAT(RayTracingPrimaryRays);

class FRayTracingPrimaryRaysRGS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRayTracingPrimaryRaysRGS)
	SHADER_USE_ROOT_PARAMETER_STRUCT(FRayTracingPrimaryRaysRGS, FGlobalShader)

	class FDenoiserOutput : SHADER_PERMUTATION_BOOL("DIM_DENOISER_OUTPUT");
	class FEnableTwoSidedGeometryForShadowDim : SHADER_PERMUTATION_BOOL("ENABLE_TWO_SIDED_GEOMETRY");
	class FMissShaderLighting : SHADER_PERMUTATION_BOOL("DIM_MISS_SHADER_LIGHTING");
	class FHybridTranslucencyMode : SHADER_PERMUTATION_ENUM_CLASS("DIM_HYBRID_TRANSLUCENCY_MODE", EHybridTranslucencyMode);

	using FPermutationDomain = TShaderPermutationDomain<FDenoiserOutput, FEnableTwoSidedGeometryForShadowDim, FMissShaderLighting, FHybridTranslucencyMode>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(int32, SamplesPerPixel)
		SHADER_PARAMETER(int32, MaxRefractionRays)
		SHADER_PARAMETER(uint32, ApplyFog)
		SHADER_PARAMETER(int32, ShouldDoDirectLighting)
		SHADER_PARAMETER(int32, ReflectedShadowsType)
		SHADER_PARAMETER(int32, ShouldDoEmissiveAndIndirectLighting)
		SHADER_PARAMETER(int32, UpscaleFactor)
		SHADER_PARAMETER(int32, ShouldUsePreExposure)
		SHADER_PARAMETER(uint32, PrimaryRayFlags)
		SHADER_PARAMETER(float, TranslucencyMinRayDistance)
		SHADER_PARAMETER(float, TranslucencyMaxRayDistance)
		SHADER_PARAMETER(float, TranslucencyMaxRoughness)
		SHADER_PARAMETER(int32, TranslucencyRefraction)
		SHADER_PARAMETER(float, MaxNormalBias)
		SHADER_PARAMETER(int32, MaxTranslucencyWriteLayers)
		SHADER_PARAMETER(int32, UseMask)
		SHADER_PARAMETER(int32, HalfRes)
		SHADER_PARAMETER(uint32, FrameInfo)
		SHADER_PARAMETER(float, RoughnessMultiplier)
		SHADER_PARAMETER(uint32, UseReflectionCaptures)

		SHADER_PARAMETER(int32, AccumulateTime)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, CumulativeTime)

		SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
		SHADER_PARAMETER_SRV(StructuredBuffer<FRTLightingData>, LightDataBuffer)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSProfilesTexture)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_REF(FRaytracingLightDataPacked, LightDataPacked)
		SHADER_PARAMETER_STRUCT_REF(FReflectionUniformParameters, ReflectionStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FFogUniformParameters, FogUniformParameters)

		SHADER_PARAMETER_STRUCT_REF(FReflectionCaptureShaderData, ReflectionCapture)
		SHADER_PARAMETER_STRUCT_REF(FForwardLightData, Forward)

		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ColorOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RayHitDistanceOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, LayersColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, LayersDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ReflectionColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ReconstructionInfo)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FRayTracingPrimaryRaysRGS, "/Engine/Private/RayTracing/RayTracingPrimaryRays.usf", "RayTracingPrimaryRaysRGS", SF_RayGen);

class FReconstructSeparateTranslucencyReflectionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReconstructSeparateTranslucencyReflectionCS);
	SHADER_USE_PARAMETER_STRUCT(FReconstructSeparateTranslucencyReflectionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, SceneStencilTexture)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, ReconstructionInfo)
	SHADER_PARAMETER(FIntVector4, UseReconstructionInfo)
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FReconstructSeparateTranslucencyReflectionCS, "/Engine/Private/RayTracing/RayTracingPrimaryRaysHelper.usf", "ReconstructSeparateTranslucencyReflection_CS", SF_Compute);

void AddSeparateTranslucencyReflectionReconstructPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef& InOutColorTexture,
	FRDGTextureRef& InColorTexture,
	FRDGTextureRef& InReconstructionInfo,
	FIntPoint TextureSize)
{
	TShaderMapRef<FReconstructSeparateTranslucencyReflectionCS> ComputeShader(View.ShaderMap);

	FReconstructSeparateTranslucencyReflectionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReconstructSeparateTranslucencyReflectionCS::FParameters>();
	PassParameters->OutputColor = GraphBuilder.CreateUAV(InOutColorTexture);
	PassParameters->InputColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InColorTexture));
	PassParameters->UseReconstructionInfo = FIntVector4(0, 0, 1, 0);

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	FRDGTextureRef SceneStencilTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneDepth(), TEXT("SceneDepthZ"));
	FRDGTextureSRVDesc SRVDesc = FRDGTextureSRVDesc::CreateWithPixelFormat(SceneStencilTexture, PF_X24_G8);
	PassParameters->SceneStencilTexture = GraphBuilder.CreateSRV(SRVDesc);

	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	PassParameters->ReconstructionInfo = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InReconstructionInfo));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ReconstructSeparateTranslucencyReflectionCS"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(TextureSize, FIntPoint(32, 32)));
}

class FCompositeTranslucencyReflectionCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompositeTranslucencyReflectionCS);
	SHADER_USE_PARAMETER_STRUCT(FCompositeTranslucencyReflectionCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputReflectionColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputColor)
	END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCompositeTranslucencyReflectionCS, "/Engine/Private/RayTracing/RayTracingPrimaryRaysHelper.usf", "CompositeTranslucencyReflection_CS", SF_Compute);

void AddCompositeTranslucencyReflectionPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef& InOutColorTexture,
	FRDGTextureRef& InReflectionColorTexture,
	FRDGTextureRef& InColorTexture,
	FIntPoint TextureSize)
{
	TShaderMapRef<FCompositeTranslucencyReflectionCS> ComputeShader(View.ShaderMap);

	FCompositeTranslucencyReflectionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCompositeTranslucencyReflectionCS::FParameters>();
	PassParameters->OutputColor = GraphBuilder.CreateUAV(InOutColorTexture);
	PassParameters->InputReflectionColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InReflectionColorTexture));
	PassParameters->InputColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InColorTexture));

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("CompositeTranslucencyReflectionCS"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(TextureSize, FIntPoint(32, 32)));
}

class FCompositeSeparateTranslucencyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCompositeSeparateTranslucencyCS);
	SHADER_USE_PARAMETER_STRUCT(FCompositeSeparateTranslucencyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputColor1)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputColor)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, SceneStencilTexture)
	SHADER_PARAMETER(uint32, CompositeMode)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCompositeSeparateTranslucencyCS, "/Engine/Private/RayTracing/RayTracingPrimaryRaysHelper.usf", "CompositeSeparateTranslucency_CS", SF_Compute);

void AddCompositeSeparateTranslucencyPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef& InOutColorTexture,
	FRDGTextureRef& InSeparateTranslucencyTexture,
	FRDGTextureRef& InColorTexture,
	FIntPoint TextureSize)
{
	TShaderMapRef<FCompositeSeparateTranslucencyCS> ComputeShader(View.ShaderMap);

	FCompositeSeparateTranslucencyCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCompositeSeparateTranslucencyCS::FParameters>();
	PassParameters->OutputColor = GraphBuilder.CreateUAV(InOutColorTexture);
	PassParameters->InputColor1 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InSeparateTranslucencyTexture));
	PassParameters->InputColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InColorTexture));

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
	FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneDepth(), TEXT("SceneDepthZ"));
	FRDGTextureSRVDesc SRVDesc = FRDGTextureSRVDesc::CreateWithPixelFormat(SceneDepthTexture, PF_X24_G8);
	PassParameters->SceneStencilTexture = GraphBuilder.CreateSRV(SRVDesc);

	PassParameters->CompositeMode = 1;

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("CompositeSeparateTranslucencyCS"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(TextureSize, FIntPoint(32, 32)));
}

void FDeferredShadingSceneRenderer::PrepareRayTracingTranslucency(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	// Declare all RayGen shaders that require material closest hit shaders to be bound.
	// NOTE: Translucency shader may be used for primary ray debug view mode.
	if (GetRayTracingTranslucencyOptions(View).bEnabled || View.RayTracingRenderMode == ERayTracingRenderMode::RayTracingDebug)
	{
		FRayTracingPrimaryRaysRGS::FPermutationDomain PermutationVector;

		const bool bLightingMissShader = CanUseRayTracingLightingMissShader(View.GetShaderPlatform());
		PermutationVector.Set<FRayTracingPrimaryRaysRGS::FMissShaderLighting>(bLightingMissShader);

		PermutationVector.Set<FRayTracingPrimaryRaysRGS::FEnableTwoSidedGeometryForShadowDim>(EnableRayTracingShadowTwoSidedGeometry());

		PermutationVector.Set<FRayTracingPrimaryRaysRGS::FHybridTranslucencyMode>(GetRayTracingHybridTranslucencyMode(View));

		auto RayGenShader = View.ShaderMap->GetShader<FRayTracingPrimaryRaysRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenShader.GetRayTracingShader());
	}
}

class FReconstructSeparateTranslucencyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FReconstructSeparateTranslucencyCS);
	SHADER_USE_PARAMETER_STRUCT(FReconstructSeparateTranslucencyCS, FGlobalShader);

	class FPrimaryRaysHalfResMode : SHADER_PERMUTATION_ENUM_CLASS("PRIMARYRAYS_HALFRES_MODE", EPrimaryRaysHalfResMode);

	using FPermutationDomain = TShaderPermutationDomain<FPrimaryRaysHalfResMode>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputColor1)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, InputColor)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, SceneStencilTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float3>, SceneNormalTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, ReconstructionInfo)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, SceneBaseColorTexture)
		SHADER_PARAMETER(FIntVector4, UseReconstructionInfo)
		SHADER_PARAMETER(uint32, FrameInfo)
		SHADER_PARAMETER(int32, PrimaryRaysHalfRes)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		END_SHADER_PARAMETER_STRUCT()

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FReconstructSeparateTranslucencyCS, "/Engine/Private/RayTracing/RayTracingPrimaryRaysHelper.usf", "ReconstructSeparateTranslucency_CS", SF_Compute);

int GetRefractionReconstructMethod(const FViewInfo& View)
{
	int RefractionReconstructMethod = CVarRayTracingPrimaryRaysHalfResRefractionReconstructMethod.GetValueOnRenderThread();
	if (RefractionReconstructMethod < 0)
	{
		RefractionReconstructMethod = 0;

		FRayTracingPrimaryRaysOptions TranslucencyOptions = GetRayTracingTranslucencyOptions(View);
		int EnableRefraction = TranslucencyOptions.EnableRefraction >= 0 ? TranslucencyOptions.EnableRefraction : View.FinalPostProcessSettings.RayTracingTranslucencyRefraction;
		if (EnableRefraction == 0)
		{
			RefractionReconstructMethod = 1;
		}
	}

	return RefractionReconstructMethod;
}

int GetRecoveredTexcoordMode()
{
	int RecoveredTexcoordMode = CVarRayTracingPrimaryRaysHalfResRecoveredTexcoordMode.GetValueOnRenderThread();
	if (RecoveredTexcoordMode < 0)
	{
		RecoveredTexcoordMode = 1;
	}

	return RecoveredTexcoordMode;
}

void AddSeparateTranslucencyReconstructPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef& InOutColorTexture,
	FRDGTextureRef& InReconstructionInfo,
	FIntPoint TextureSize,
	int32 PrimaryRaysHalfRes,
	bool AbandonHistory,
	const FTemporalAAHistory& InputHistory,
	FTemporalAAHistory* OutputHistory)
{
	// Create outputs
	FTAAOutputs Outputs;

	{
		FRDGTextureDesc SceneColorDesc = FRDGTextureDesc::Create2D(
			TextureSize,
			PF_FloatRGBA,
			FClearValueBinding(FLinearColor(0,0,0,-1.0f)),
			TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);

		//if (Inputs.bOutputRenderTargetable)
		{
			//SceneColorDesc.TargetableFlags |= TexCreate_RenderTargetable;
		}

		Outputs.SceneColor = GraphBuilder.CreateTexture(
			SceneColorDesc,
			TEXT("RTSeparateTranslucency"),
			ERDGTextureFlags::MultiFrame);
	}

	//RDG_GPU_STAT_SCOPE(GraphBuilder, RECONSTRUCT_SEPARATETRANSLUCENCY);
	bool NeedsHistory = EPrimaryRaysHalfResMode(PrimaryRaysHalfRes) == Checkerboard_Interframe;

	{
		FReconstructSeparateTranslucencyCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FReconstructSeparateTranslucencyCS::FParameters>();

		PassParameters->OutputColor = GraphBuilder.CreateUAV(Outputs.SceneColor);
		PassParameters->InputColor = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InOutColorTexture));
		PassParameters->UseReconstructionInfo = FIntVector4(1, 0, 
												GetRefractionReconstructMethod(View),
												GetRecoveredTexcoordMode());
		PassParameters->FrameInfo = View.ViewState ? View.ViewState->PrimaryRaysFrameInfo : 0;
		PassParameters->PrimaryRaysHalfRes = PrimaryRaysHalfRes;

		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);
		FRDGTextureRef SceneStencilTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneDepth(), TEXT("SceneDepthZ"));
		FRDGTextureSRVDesc SRVDesc = FRDGTextureSRVDesc::CreateWithPixelFormat(SceneStencilTexture, PF_X24_G8);
		PassParameters->SceneStencilTexture = GraphBuilder.CreateSRV(SRVDesc);

		FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneContext.SceneDepthZ);
		PassParameters->SceneDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepthTexture));

		FRDGTextureRef SceneNormalTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferA);
		PassParameters->SceneNormalTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneNormalTexture));

		//FRDGTextureRef SceneBaseColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GBufferC);
		FRDGTextureRef SceneBaseColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor(), TEXT("SceneColor"));
		PassParameters->SceneBaseColorTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneBaseColorTexture));

		PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

		PassParameters->ReconstructionInfo = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(InReconstructionInfo));

		if (!InputHistory.IsValid() || AbandonHistory || !NeedsHistory)
		{
			PassParameters->InputColor1 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(GSystemTextures.GetBlackDummy(GraphBuilder)));
		}
		else
		{
			PassParameters->InputColor1 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(GraphBuilder.RegisterExternalTexture(InputHistory.RT[0])));
		}

		FReconstructSeparateTranslucencyCS::FPermutationDomain CsPermutationVector;
		CsPermutationVector.Set<FReconstructSeparateTranslucencyCS::FPrimaryRaysHalfResMode>(EPrimaryRaysHalfResMode(PrimaryRaysHalfRes));

		TShaderMapRef<FReconstructSeparateTranslucencyCS> ComputeShader(View.ShaderMap, CsPermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ReconstructSeparateTranslucencyCS"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(TextureSize, FIntPoint(32, 32)));		
	}

	if(OutputHistory && !View.bStatePrevViewInfoIsReadOnly && NeedsHistory)
	{
		OutputHistory->SafeRelease();

		GraphBuilder.QueueTextureExtraction(Outputs.SceneColor, &OutputHistory->RT[0]);
	}

	InOutColorTexture = Outputs.SceneColor;
}

EPrimaryRaysHalfResMode GetPrimaryRaysHalfResMode()
{
	static const auto RayTracingTranslucencyHalfResCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.RayTracing.Translucency.HalfRes"));
	return EPrimaryRaysHalfResMode(RayTracingTranslucencyHalfResCVar ? RayTracingTranslucencyHalfResCVar->GetValueOnRenderThread() : 0);
}

void FDeferredShadingSceneRenderer::RenderRayTracingPrimaryRaysView(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef* InOutColorTexture,
	FRDGTextureRef* InOutRayHitDistanceTexture,
	int32 SamplePerPixel,
	float ResolutionFraction,
	FRDGTextureRef* InOutLayersColor,
	FRDGTextureRef* InOutLayersDepth,
	ERayTracingPrimaryRaysFlag Flags)
{
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(GraphBuilder.RHICmdList);

	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	int32 UpscaleFactor = int32(1.0f / ResolutionFraction);
	ensure(ResolutionFraction == 1.0 / UpscaleFactor);
	ensureMsgf(FComputeShaderUtils::kGolden2DGroupSize % UpscaleFactor == 0, TEXT("PrimaryRays ray tracing will have uv misalignement."));
	FIntPoint RayTracingResolution = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), UpscaleFactor);

	EPrimaryRaysHalfResMode PrimaryRaysHalfRes = GetPrimaryRaysHalfResMode();
	bool bSeparateTranslucency = (PrimaryRaysHalfRes > 0) && (GetRayTracingHybridTranslucencyMode(View) != EHybridTranslucencyMode::Mode1);
	if (bSeparateTranslucency)
	{
		Flags |= ERayTracingPrimaryRaysFlag::SeparateTranslucency;
	}
	else
	{
		PrimaryRaysHalfRes = EPrimaryRaysHalfResMode::HalfRes_Off;
	}

	bool bHalfResSeparateReflection = (PrimaryRaysHalfRes == EPrimaryRaysHalfResMode::Checkerboard_Weighted);
	if (bHalfResSeparateReflection)
	{
		Flags |= ERayTracingPrimaryRaysFlag::SeparateTranslucencyReflection;
	}

	FRDGTextureRef OutReflectionColorTexture;
	FRDGTextureRef OutSeparateColorTexture;
	FRDGTextureRef OutSeparateReflectionColorTexture;
	FRDGTextureRef OutSeparateTranslucency;
	FRDGTextureRef OutReconstructionInfo;
	{
		FRDGTextureDesc Desc = Translate(SceneContext.GetSceneColor()->GetDesc());
		Desc.Reset();
		Desc.Format = PF_FloatRGBA;
		Desc.Flags |= TexCreate_UAV;
		Desc.Extent /= UpscaleFactor;

		if(*InOutColorTexture == nullptr) 
		{
			*InOutColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRays"));
			
		}

		OutSeparateTranslucency = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRaysSeparateTranslucency"));
		OutReconstructionInfo = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRaysReconstructionInfo"));

		OutSeparateReflectionColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRaysSeparateTranslucencyReflection"));

		OutSeparateColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRaysSeparateColor"));
		OutReflectionColorTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRaysReflectionColor"));

		Desc.Format = PF_R16F;
		if(*InOutRayHitDistanceTexture == nullptr) 
		{
			*InOutRayHitDistanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("RayTracingPrimaryRaysHitDistance"));
		}
	}

	FRayTracingPrimaryRaysRGS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRayTracingPrimaryRaysRGS::FParameters>();

	FRayTracingPrimaryRaysOptions TranslucencyOptions = GetRayTracingTranslucencyOptions(View);
	PassParameters->SamplesPerPixel = SamplePerPixel;
	PassParameters->MaxRefractionRays = TranslucencyOptions.MaxRefractionRays > -1 ? TranslucencyOptions.MaxRefractionRays : View.FinalPostProcessSettings.RayTracingTranslucencyRefractionRays;
	PassParameters->ApplyFog = TranslucencyOptions.ApplyFog;
	PassParameters->ShouldDoDirectLighting = TranslucencyOptions.EnableDirectLighting;
	PassParameters->ReflectedShadowsType = TranslucencyOptions.EnableShadows > -1 ? TranslucencyOptions.EnableShadows : (int32)View.FinalPostProcessSettings.RayTracingTranslucencyShadows;
	PassParameters->ShouldDoEmissiveAndIndirectLighting = TranslucencyOptions.EnableEmmissiveAndIndirectLighting;
	PassParameters->UpscaleFactor = UpscaleFactor;
	PassParameters->RoughnessMultiplier = TranslucencyOptions.RoughnessMultiplier;
	PassParameters->TranslucencyMinRayDistance = FMath::Min(TranslucencyOptions.MinRayDistance, TranslucencyOptions.MaxRayDistance);
	PassParameters->TranslucencyMaxRayDistance = TranslucencyOptions.MaxRayDistance;
	PassParameters->TranslucencyMaxRoughness = FMath::Clamp(TranslucencyOptions.MaxRoughness >= 0 ? TranslucencyOptions.MaxRoughness : View.FinalPostProcessSettings.RayTracingTranslucencyMaxRoughness, 0.01f, 1.0f);
	PassParameters->TranslucencyRefraction = TranslucencyOptions.EnableRefraction >= 0 ? TranslucencyOptions.EnableRefraction : View.FinalPostProcessSettings.RayTracingTranslucencyRefraction;
	PassParameters->MaxNormalBias = GetRaytracingMaxNormalBias();
	PassParameters->ShouldUsePreExposure = View.Family->EngineShowFlags.Tonemapper;
	PassParameters->PrimaryRayFlags = (uint32)Flags;
	PassParameters->TLAS = View.RayTracingScene.RayTracingSceneRHI->GetShaderResourceView();
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->LightDataPacked = View.RayTracingLightData.UniformBuffer;
	PassParameters->LightDataBuffer = View.RayTracingLightData.LightBufferSRV;

	if ((static_cast<uint32>(Flags) & static_cast<uint32>(ERayTracingPrimaryRaysFlag::TimeTracing)))
	{
		PassParameters->AccumulateTime = 1;
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalTexture(SceneContext.RayTracingTiming));
	}
	else
	{
		PassParameters->AccumulateTime = 0;
		PassParameters->CumulativeTime = GraphBuilder.CreateUAV(*InOutRayHitDistanceTexture); // bogus UAV to just keep validation happy as it is dynamically unused
	}

	PassParameters->FrameInfo = View.ViewState ? View.ViewState->PrimaryRaysFrameInfo : 0;

	if (static_cast<uint32>(Flags) & static_cast<uint32>(ERayTracingPrimaryRaysFlag::HybridTranslucency))
	{
		PassParameters->MaxTranslucencyWriteLayers = MaxHybridTranslucencyLayers();
		PassParameters->LayersColor = GraphBuilder.CreateUAV(*InOutLayersColor);
		PassParameters->LayersDepth = GraphBuilder.CreateUAV(*InOutLayersDepth);
		if (GetRayTracingHybridTranslucencyMode(View) == EHybridTranslucencyMode::Mode1)
		{
			//Mode 1 terminates after recording layers, so restrict the refraction count
			PassParameters->MaxRefractionRays = FMath::Min(PassParameters->MaxRefractionRays, PassParameters->MaxTranslucencyWriteLayers);
		}
	}
	else
	{
		PassParameters->MaxTranslucencyWriteLayers = 0;
		// RDG requires all resources to have valid references, use dummy bindings
		PassParameters->LayersColor = GraphBuilder.CreateUAV(*InOutColorTexture);
		PassParameters->LayersDepth = GraphBuilder.CreateUAV(*InOutRayHitDistanceTexture);
	}

	if (static_cast<uint32>(Flags) & static_cast<uint32>(ERayTracingPrimaryRaysFlag::StencilMask))
	{
		PassParameters->UseMask = 1;
	}
	else
	{
		PassParameters->UseMask = 0;
	}

	if (static_cast<uint32>(Flags) & static_cast<uint32>(ERayTracingPrimaryRaysFlag::HalfResolution))
	{
		PassParameters->HalfRes = (static_cast<uint32>(Flags) & static_cast<uint32>(ERayTracingPrimaryRaysFlag::CheckerboardSampling)) ? 2 : 1;
		RayTracingResolution.Y = RayTracingResolution.Y / 2;
	}
	else
	{
		PassParameters->HalfRes = int(PrimaryRaysHalfRes);
		if (PrimaryRaysHalfRes > 0)
		{
			RayTracingResolution.X /= 2;
		}
	}

	if (bHalfResSeparateReflection)
	{
		AddClearRenderTargetPass(GraphBuilder, OutReflectionColorTexture, FLinearColor(0, 0, 0, -1.0f));
	}
	PassParameters->ReflectionColor = GraphBuilder.CreateUAV(OutReflectionColorTexture);

	PassParameters->ReconstructionInfo = GraphBuilder.CreateUAV(OutReconstructionInfo);
	PassParameters->SceneTextures = SceneTextures;

	FRDGTextureRef SceneColorTexture = GraphBuilder.RegisterExternalTexture(SceneContext.GetSceneColor(), TEXT("SceneColor"));
	PassParameters->SceneColorTexture = SceneColorTexture;

	PassParameters->ReflectionStruct = CreateReflectionUniformBuffer(View, EUniformBufferUsage::UniformBuffer_SingleFrame);
	PassParameters->FogUniformParameters = CreateFogUniformBuffer(GraphBuilder, View);

	PassParameters->ReflectionCapture = View.ReflectionCaptureUniformBuffer;
	PassParameters->Forward = View.ForwardLightingResources->ForwardLightDataUniformBuffer;

	PassParameters->UseReflectionCaptures = CVarRayTracingPrimaryRaysReflectionCaptures.GetValueOnRenderThread();

	{
		if (bSeparateTranslucency)
		{
			AddClearRenderTargetPass(GraphBuilder, bHalfResSeparateReflection ? OutSeparateColorTexture : OutSeparateTranslucency, FLinearColor(0, 0, 0, -1.0f));
		}
		PassParameters->ColorOutput = bSeparateTranslucency ? GraphBuilder.CreateUAV(bHalfResSeparateReflection ? OutSeparateColorTexture : OutSeparateTranslucency) : GraphBuilder.CreateUAV(*InOutColorTexture);
	}

	PassParameters->RayHitDistanceOutput = GraphBuilder.CreateUAV(*InOutRayHitDistanceTexture);

	// TODO: should be converted to RDG
	PassParameters->SSProfilesTexture = GraphBuilder.RegisterExternalTexture(View.RayTracingSubSurfaceProfileTexture);

	const bool bMissShaderLighting = CanUseRayTracingLightingMissShader(View.GetShaderPlatform());

	FRayTracingPrimaryRaysRGS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FRayTracingPrimaryRaysRGS::FEnableTwoSidedGeometryForShadowDim>(EnableRayTracingShadowTwoSidedGeometry());
	PermutationVector.Set< FRayTracingPrimaryRaysRGS::FMissShaderLighting>(bMissShaderLighting);
	PermutationVector.Set<FRayTracingPrimaryRaysRGS::FHybridTranslucencyMode>(GetRayTracingHybridTranslucencyMode(View));

	auto RayGenShader = View.ShaderMap->GetShader<FRayTracingPrimaryRaysRGS>(PermutationVector);

	ClearUnusedGraphResources(RayGenShader, PassParameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("RayTracingPrimaryRays %dx%d", RayTracingResolution.X, RayTracingResolution.Y),
		PassParameters,
		ERDGPassFlags::Compute,
		[PassParameters, this, &View, RayGenShader, RayTracingResolution](FRHICommandList& RHICmdList)
	{
		SCOPED_GPU_STAT(RHICmdList, RayTracingPrimaryRays);
	FRayTracingPipelineState* Pipeline = View.RayTracingMaterialPipeline;

		FRayTracingShaderBindingsWriter GlobalResources;
		SetShaderParameters(GlobalResources, RayGenShader, *PassParameters);

		FRHIRayTracingScene* RayTracingSceneRHI = View.RayTracingScene.RayTracingSceneRHI;
		RHICmdList.RayTraceDispatch(Pipeline, RayGenShader.GetRayTracingShader(), RayTracingSceneRHI, GlobalResources, RayTracingResolution.X, RayTracingResolution.Y);
	});
	
	if (bSeparateTranslucency)
	{
		FPooledRenderTargetDesc Desc = SceneContext.GetSceneColor()->GetDesc();
		FIntVector TextureSize = Desc.GetSize();


		const FTemporalAAHistory& InputHistory = View.PrevViewInfo.RayTracedTranslucencyHistory;
		FTemporalAAHistory* OutputHistory = View.ViewState ? &View.ViewState->PrevFrameViewInfo.RayTracedTranslucencyHistory : nullptr;

		EPrimaryRaysHalfResMode OldPrimaryRaysHalfResMode = EPrimaryRaysHalfResMode(View.ViewState ? View.ViewState->LastPrimaryRaysHalfResMode : 0);

		AddSeparateTranslucencyReconstructPass(GraphBuilder, View, bHalfResSeparateReflection ? OutSeparateColorTexture : OutSeparateTranslucency, OutReconstructionInfo, FIntPoint(TextureSize.X, TextureSize.Y), PrimaryRaysHalfRes,
			OldPrimaryRaysHalfResMode != PrimaryRaysHalfRes, InputHistory, OutputHistory);

		if (bHalfResSeparateReflection)
		{
			AddSeparateTranslucencyReflectionReconstructPass(GraphBuilder, View, OutSeparateReflectionColorTexture, OutReflectionColorTexture, OutReconstructionInfo, FIntPoint(TextureSize.X, TextureSize.Y));

			AddCompositeTranslucencyReflectionPass(GraphBuilder, View, OutSeparateTranslucency, OutSeparateReflectionColorTexture, OutSeparateColorTexture, FIntPoint(TextureSize.X, TextureSize.Y));
		}

		AddCompositeSeparateTranslucencyPass(GraphBuilder, View, *InOutColorTexture, OutSeparateTranslucency, SceneColorTexture, FIntPoint(TextureSize.X, TextureSize.Y));
	}
	
	if (View.ViewState)
	{
		View.ViewState->LastPrimaryRaysHalfResMode = (uint32)PrimaryRaysHalfRes;
		View.ViewState->PrimaryRaysFrameInfo = (++View.ViewState->PrimaryRaysFrameInfo) % 2;
	}
}

#endif // RHI_RAYTRACING

/*
* Copyright (c) 2021 NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA Corporation and its licensors retain all intellectual property and proprietary
* rights in and to this software, related documentation and any modifications thereto.
* Any use, reproduction, disclosure or distribution of this software and related
* documentation without an express license agreement from NVIDIA Corporation is strictly
* prohibited.
*
* TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED *AS IS*
* AND NVIDIA AND ITS SUPPLIERS DISCLAIM ALL WARRANTIES, EITHER EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
* PARTICULAR PURPOSE.  IN NO EVENT SHALL NVIDIA OR ITS SUPPLIERS BE LIABLE FOR ANY
* SPECIAL, INCIDENTAL, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER (INCLUDING, WITHOUT
* LIMITATION, DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF
* BUSINESS INFORMATION, OR ANY OTHER PECUNIARY LOSS) ARISING OUT OF THE USE OF OR
* INABILITY TO USE THIS SOFTWARE, EVEN IF NVIDIA HAS BEEN ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGES.
*/

#include "ReLAX.h"
#include "NRDCommon.h"
#include "NRDPrivate.h"
#include "NRDDenoiserHistory.h"
#include "SceneTextureParameters.h"
#include "ScenePrivate.h"

#include "Shader.h"
#include "ScreenPass.h"
#include "ShaderCore.h"
#include "ShaderParameterUtils.h"
#include "GlobalShader.h"

#include "RenderGraphBuilder.h"



namespace
{
	static const uint32 RELAX_MAX_HISTORY_FRAME_NUM = 63;

	// PREPASS
	NRDCVar<float> RelaxPrepassSpecularVarianceBoost(TEXT("r.NRD.Relax.Prepass.SpecularBlurRadius"), 50.0f, TEXT("Radius in pixeles to preblur specular"), 0.0f, 100.0f);
	NRDCVar<float> RelaxPrepassDiffuseVarianceBoost(TEXT("r.NRD.Relax.Prepass.DiffuseBlurRadius"), 0.0f, TEXT("Radius in pixeles to preblur diffuse"), 0.0f, 100.0f);

	// History
	NRDCVar<int32> RelaxHistorySpecularMaxAccumulatedFrameNum(TEXT("r.NRD.Relax.History.SpecularMaxAccumulatedFrameNum"), 63, TEXT("Amount of frames in history for specular signal temporal accumulation "), 0 , RELAX_MAX_HISTORY_FRAME_NUM);
	NRDCVar<int32> RelaxHistorySpecularMaxFastAccumulatedFrameNum(TEXT("r.NRD.Relax.History.SpecularFastMaxAccumulatedFrameNum"), 4, TEXT("Amount of frames in history for responsive specular signal temporal accumulation "), 0, RELAX_MAX_HISTORY_FRAME_NUM);
	NRDCVar<int32> RelaxHistoryDiffuseMaxAccumulatedFrameNum(TEXT("r.NRD.Relax.History.DiffuseMaxAccumulatedFrameNum"), 63, TEXT("Amount of frames in history for diffuse signal temporal accumulation "), 0, RELAX_MAX_HISTORY_FRAME_NUM);
	NRDCVar<int32> RelaxHistoryDiffuseMaxFastAccumulatedFrameNum(TEXT("r.NRD.Relax.History.DiffuseFastMaxAccumulatedFrameNum"), 0, TEXT("Amount of frames in history for fast responsive signal temporal accumulation "), 0, RELAX_MAX_HISTORY_FRAME_NUM);

	// REPROJECTION
	NRDCVar<float> RelaxReprojectionSpecularVarianceBoost(TEXT("r.NRD.Relax.Reprojection.SpecularVarianceBoost"), 1.0f, TEXT("How much variance we inject to specular if reprojection confidence is low"), 0.0f, 8.0f);
	NRDCVar<float> RelaxReprojectionHistoryClampingColorBoxSigmaScale(TEXT("r.NRD.Relax.Reprojection.HistoryClampingColorBoxSigmaScale"), 2.0f, TEXT("Scale for standard deviation of color box for clamping normal history color to responsive history color"), 1.0f, 10.0f);
	NRDCVar<bool>  RelaxReprojectionBicubicFilter(TEXT("r.NRD.Relax.Reprojection.BicubicFilter"), true, TEXT("Slower but sharper filtering of the history during reprojection"), false, true);
	NRDCVar<float> RelaxReprojectionDisocclusionThreshold(TEXT("r.NRD.Relax.Reprojection.DisocclusionThreshold"), 0.01f, TEXT("Percentage of the depth value for disocclusion detection / geometry tests"), 0.001f, 1.0f);

	// DISOCCLUSION FIX
	NRDCVar<float> RelaxDisocclusionFixEdgeStoppingNormalPower(TEXT("r.NRD.Relax.DisocclusionFix.EdgeStoppingNormalPower"), 8.0f, TEXT("Normal edge stopper for cross-bilateral sparse filter"), 0.0f, 128.0f);
	NRDCVar<float> RelaxDisocclusionFixMaxRadius(TEXT("r.NRD.Relax.DisocclusionFix.MaxRadius"), 8.0f, TEXT("Maximum radius for sparse bilateral filter, expressed in pixels"), 0.0f, 100.0f);
	NRDCVar<int32> RelaxDisocclusionFixNumFramesToFix(TEXT("r.NRD.Relax.DisocclusionFix.NumFramesToFix"), 3, TEXT("Cross-bilateral sparse filter will be applied to frames with history length shorter than this value"), 0, 10);

	// SPATIAL VARIANCE ESTIMATION
	NRDCVar<int32> RelaxSpatialVarianceEstimationHistoryThreshold(TEXT("r.NRD.Relax.SpatialVarianceEstimation.HistoryThreshold"), 3, TEXT("History length threshold below which spatial variance estimation will be applied"), 0, 10);
	
	// A-TROUS
	NRDCVar<int32> RelaxAtrousIterations(	TEXT("r.NRD.Relax.Atrous.Iterations"), 5, TEXT("Number of iterations of the A-trous filter."), 2, 8);
	NRDCVar<float> RelaxAtrousDiffusePhiLuminance(TEXT("r.NRD.Relax.Atrous.DiffusePhiLuminance"), 2.0f, TEXT("A-trous edge stopping diffuse Luminance sensitivity"), 0.0f, 10.0f);
	NRDCVar<float> RelaxAtrousSpecularPhiLuminance(TEXT("r.NRD.Relax.Atrous.SpecularPhiLuminance"), 2.0f, TEXT("A-trous edge stopping specular Luminance sensitivity."), 0.0f, 10.0f);

	NRDCVar<float> RelaxAtrousMinLuminanceWeight(TEXT("r.NRD.Relax.Atrous.MinLuminanceWeight"), 0.0f, TEXT("A-trous edge stopping Luminance weight minimun."), 0.0f, 1.0f);
	NRDCVar<float> RelaxAtrousSpecularLobeAngleSlack(TEXT("r.NRD.Relax.Atrous.SpecularLobeAngleFraction"), 0.3f, TEXT("Slack (in degrees) for the specular lobe angle used in normal based rejection of specular"), 0.0f, 60.0f);
	NRDCVar<float> RelaxAtrousSpecularLobeAngleFraction(TEXT("r.NRD.Relax.Atrous.SpecularLobeAngleFraction"), 0.333f, TEXT("Base fraction of the specular lobe angle used in normal based rejection of specular."), 0.0f, 1.0f);

	NRDCVar<float> RelaxAtrousPhiNormal(TEXT("r.NRD.Relax.Atrous.PhiNormal"), 64.0f, TEXT("A-trous edge stopping Normal sensitivity for diffuse"), 0.1f, 256.0f);
	NRDCVar<float> RelaxAtrousPhiDepth(TEXT("r.NRD.Relax.Atrous.PhiDepth"), 0.0001f, TEXT(" A-trous edge stopping Depth sensitivity."), 0.0f, 1.0f);
	
	NRDCVar<float> RelaxAtrousRoughnessEdgeStoppingRelaxation(TEXT("r.NRD.Relax.Atrous.RoughnessEdgeStoppingRelaxation"), 0.3f, TEXT("How much we relax roughness based rejection in areas where specular reprojection is low"), 0.0f, 1.0f);
	NRDCVar<float> RelaxAtrousNormalEdgeStoppingRelaxation(TEXT("r.NRD.Relax.Atrous.NormalEdgeStoppingRelaxation"), 0.3f, TEXT("How much we relax normal based rejection in areas where specular reprojection is low."), 0.0f, 1.0f);
	NRDCVar<float> RelaxAtrousLuminanceEdgeStoppingRelaxation(TEXT("r.NRD.Relax.Atrous.LuminanceEdgeStoppingRelaxation"), 1.0f, TEXT("How much we relax luminance based rejection in areas where specular reprojection is low"), 0.0f, 1.0f);

	// MISC
	NRDCVar<bool>  RelaxFireflySupression(TEXT("r.NRD.Relax.FireflySupression"), false, TEXT("Whether to suppress fireflies or not"), false, true);
	NRDCVar<int32> RelaxSplitScreenPercentage(TEXT("r.NRD.Relax.SplitScreen.Percentage"), 0, TEXT("Where to split the screen between inputs and denoised outputs. In Percent"), 0, 100);
	NRDCVar<float> NRDDenoisingRange(TEXT("r.NRD.DenoisingRange"), 100000.0f, TEXT("World space range of geometry"), 0.0f, 10000000.0f);

}

class RelaxDiffuseSpecularSettings
{
private:
	RelaxDiffuseSpecularSettings()
	{
	}

public:
	static RelaxDiffuseSpecularSettings FromConsoleVariables()
	{
		RelaxDiffuseSpecularSettings Result;

		// PREPASS
		Result.SpecularBlurRadius = RelaxPrepassSpecularVarianceBoost;
		Result.DiffuseBlurRadius = RelaxPrepassDiffuseVarianceBoost;
		
		// History
		Result.specularMaxAccumulatedFrameNum = RelaxHistorySpecularMaxAccumulatedFrameNum;
		Result.specularMaxFastAccumulatedFrameNum = RelaxHistorySpecularMaxFastAccumulatedFrameNum;
		Result.diffuseMaxAccumulatedFrameNum = RelaxHistoryDiffuseMaxAccumulatedFrameNum;
		Result.diffuseMaxFastAccumulatedFrameNum = RelaxHistoryDiffuseMaxFastAccumulatedFrameNum;

		// REPROJECTION
		Result.specularVarianceBoost = RelaxReprojectionSpecularVarianceBoost;
		Result.historyClampingColorBoxSigmaScale = RelaxReprojectionHistoryClampingColorBoxSigmaScale;
		Result.bicubicFilterForReprojectionEnabled = RelaxReprojectionBicubicFilter;
		Result.disocclusionThreshold = RelaxReprojectionDisocclusionThreshold;

		// DISOCCLUSION FIX
		Result.disocclusionFixEdgeStoppingNormalPower = RelaxDisocclusionFixEdgeStoppingNormalPower;
		Result.disocclusionFixMaxRadius = RelaxDisocclusionFixMaxRadius;
		Result.disocclusionFixNumFramesToFix = RelaxDisocclusionFixNumFramesToFix;

		// SPATIAL VARIANCE ESTIMATION
		Result.spatialVarianceEstimationHistoryThreshold = RelaxSpatialVarianceEstimationHistoryThreshold;

		// A-TROUS
		Result.atrousIterationNum = RelaxAtrousIterations;
		Result.specularPhiLuminance = RelaxAtrousSpecularPhiLuminance;
		Result.diffusePhiLuminance = RelaxAtrousDiffusePhiLuminance;
		Result.phiNormal = RelaxAtrousPhiNormal;
		Result.phiDepth = RelaxAtrousPhiDepth;
		Result.roughnessEdgeStoppingRelaxation = RelaxAtrousRoughnessEdgeStoppingRelaxation;
		Result.normalEdgeStoppingRelaxation = RelaxAtrousNormalEdgeStoppingRelaxation;
		Result.luminanceEdgeStoppingRelaxation = RelaxAtrousLuminanceEdgeStoppingRelaxation;

		Result.MinLuminanceWeight = RelaxAtrousMinLuminanceWeight;
		Result.SpecularLobeAngleSlack = RelaxAtrousSpecularLobeAngleSlack;
		Result.SpecularLobeAngleFraction = RelaxAtrousSpecularLobeAngleFraction;

		// MISC
		Result.antifirefly = RelaxFireflySupression;
		Result.splitScreen = RelaxSplitScreenPercentage;

		Result.denoisingRange = NRDDenoisingRange;


		
		return Result;
	}

	// PREPASS 
	float SpecularBlurRadius;
	float DiffuseBlurRadius;

	// History
	uint32 specularMaxAccumulatedFrameNum;
	uint32 specularMaxFastAccumulatedFrameNum;
	uint32 diffuseMaxAccumulatedFrameNum;
	uint32 diffuseMaxFastAccumulatedFrameNum;
	
	// REPROJECTION
	float specularVarianceBoost;
	float historyClampingColorBoxSigmaScale;
	bool bicubicFilterForReprojectionEnabled;

	// DISOCCLUSION FIX
	float disocclusionFixEdgeStoppingNormalPower;
	float disocclusionFixMaxRadius;
	uint32 disocclusionFixNumFramesToFix;

	// SPATIAL VARIANCE ESTIMATION
	uint32 spatialVarianceEstimationHistoryThreshold;

	// A-TROUS
	uint32 atrousIterationNum;
	float specularPhiLuminance;
	float diffusePhiLuminance;
	float phiNormal;
	float phiDepth;
	float roughnessEdgeStoppingRelaxation;
	float normalEdgeStoppingRelaxation;
	float luminanceEdgeStoppingRelaxation;

	float MinLuminanceWeight;
	float SpecularLobeAngleSlack;
	float SpecularLobeAngleFraction;

	// MISC
	bool antifirefly;
	uint32 splitScreen;
	float disocclusionThreshold;
	float denoisingRange;

};

FNRDPackInputsArguments RelaxPackInputArguments()
{
	FNRDPackInputsArguments Result;
	Result.bPackDiffuseHitDistance = false;
	Result.bPackSpecularHitDistance = true;

	return Result;
}


// PREPASS
class FNRDRELAXPrepassCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXPrepassCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXPrepassCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FMatrix, gWorldToClip)
		SHADER_PARAMETER(FMatrix, gWorldToView)
		SHADER_PARAMETER(FMatrix, gViewToClip)
		SHADER_PARAMETER(FVector4, gRotator) 
		SHADER_PARAMETER(FVector4, gFrustumRight) 
		SHADER_PARAMETER(FVector4, gFrustumUp) 
		SHADER_PARAMETER(FVector4, gFrustumForward) 
		SHADER_PARAMETER(FIntPoint, gRectOrigin) 
		SHADER_PARAMETER(FVector2D, gRectOffset)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvViewSize) 
		SHADER_PARAMETER(FVector2D, gInvRectSize) 
		SHADER_PARAMETER(FVector2D, gResolutionScale) 
		SHADER_PARAMETER(float, gIsOrtho) 
		SHADER_PARAMETER(float, gUnproject) 
		SHADER_PARAMETER(uint32, gFrameIndex) 
		SHADER_PARAMETER(float, gDenoisingRange) 
		SHADER_PARAMETER(uint32, gDiffCheckerboard) 
		SHADER_PARAMETER(uint32, gSpecCheckerboard) 
		SHADER_PARAMETER(float, gDiffuseBlurRadius) 
		SHADER_PARAMETER(float, gSpecularBlurRadius) 
		SHADER_PARAMETER(float, gMeterToUnitsMultiplier) 

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, gViewZ)
		
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIllumination) 
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIllumination) 
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, gOutViewZ) 
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, gOutScaledViewZ)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXPrepassCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_Prepass.cs.usf", "main", SF_Compute);

// REPROJECT
class FNRDRELAXReprojectCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXReprojectCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXReprojectCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FMatrix,   gPrevWorldToClip)
		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FVector4,  gPrevFrustumRight)
		SHADER_PARAMETER(FVector4,  gPrevFrustumUp)
		SHADER_PARAMETER(FVector4,  gPrevFrustumForward)
		SHADER_PARAMETER(FVector,   gPrevCameraPosition)
		SHADER_PARAMETER(float,     gJitterDelta)
		SHADER_PARAMETER(FVector2D, gMotionVectorScale)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvViewSize)
		SHADER_PARAMETER(float,     gUseBicubic)
		SHADER_PARAMETER(float,     gSpecularVarianceBoost)
		SHADER_PARAMETER(float,     gWorldSpaceMotion)
		SHADER_PARAMETER(float,     gIsOrtho)
		SHADER_PARAMETER(float,     gUnproject)
		SHADER_PARAMETER(uint32,    gResetHistory)
		SHADER_PARAMETER(float,     gDenoisingRange)
		SHADER_PARAMETER(float,     gDisocclusionThreshold)
		SHADER_PARAMETER(FMatrix,   gWorldToClip)
		SHADER_PARAMETER(FIntPoint, gRectOrigin) 
		SHADER_PARAMETER(FVector2D, gInvRectSize)
		SHADER_PARAMETER(FVector2D, gRectSizePrev)
		SHADER_PARAMETER(float,     gSpecularMaxAccumulatedFrameNum)
		SHADER_PARAMETER(float,     gSpecularMaxFastAccumulatedFrameNum)
		SHADER_PARAMETER(float,     gDiffuseMaxAccumulatedFrameNum)
		SHADER_PARAMETER(float,     gDiffuseMaxFastAccumulatedFrameNum)
		SHADER_PARAMETER(uint32,    gRoughnessBasedSpecularAccumulation)
		SHADER_PARAMETER(uint32,    gVirtualHistoryClampingEnabled)
		SHADER_PARAMETER(uint32,    gFrameIndex)
		SHADER_PARAMETER(uint32,    gIsCameraStatic)
		SHADER_PARAMETER(uint32,    gSkipReprojectionTestWithoutMotion)
		SHADER_PARAMETER(uint32,    gDiffCheckerboard)
		SHADER_PARAMETER(uint32,    gSpecCheckerboard)
		SHADER_PARAMETER(float,     gCheckerboardResolveAccumSpeed)
		SHADER_PARAMETER(uint32,    gUseConfidenceInputs)
		SHADER_PARAMETER(float,     gFramerateScale)
		SHADER_PARAMETER(float,     gRejectDiffuseHistoryNormalThreshold)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gMotion)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gPrevReflectionHitT)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gPrevSpecularAndDiffuseHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gViewZ)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevSpecularIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevDiffuseIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gPrevNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gPrevViewZ)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gSpecConfidence)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gDiffConfidence)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>,  gOutReflectionHitT)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, gOutSpecularAndDiffuseHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>,  gOutSpecularReprojectionConfidence)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationResponsive)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXReprojectCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_Reproject.cs.usf", "main", SF_Compute);

// DISOCCLUSION FIX
class FNRDRELAXDisocclusionFixCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXDisocclusionFixCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXDisocclusionFixCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvRectSize)
		SHADER_PARAMETER(float,     gDisocclusionThreshold)
		SHADER_PARAMETER(float,     gDisocclusionFixEdgeStoppingNormalPower)
		SHADER_PARAMETER(float,     gMaxRadius)
		SHADER_PARAMETER(int,       gFramesToFix)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gSpecularIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gDiffuseIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,    gSpecularAndDiffuseHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,     gViewZFP16)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,  gOutSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,  gOutDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,  gOutSpecularIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,  gOutDiffuseIlluminationResponsive)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXDisocclusionFixCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_DisocclusionFix.cs.usf", "main", SF_Compute);

// HISTORY CLAMPING
class FNRDRELAXHistoryClampingCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXHistoryClampingCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXHistoryClampingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FNRDCommonSamplerParameters, CommonSamplers)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(float,     gColorBoxSigmaScale)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gSpecularIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,    gDiffuseIlluminationResponsive)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,    gSpecularAndDiffuseHistoryLength)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,  gOutSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>,  gOutDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>,  gOutSpecularAndDiffuseHistoryLength)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXHistoryClampingCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_HistoryClamping.cs.usf", "main", SF_Compute);

// FIREFLY
class FNRDRELAXFireflyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXFireflyCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXFireflyCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gViewZFP16)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIllumination)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXFireflyCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_Firefly.cs.usf", "main", SF_Compute);


// SPATIAL VARIANCE ESTIMATION
class FNRDRELAXSpatialVarianceEstimationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXSpatialVarianceEstimationCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXSpatialVarianceEstimationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(float,     gPhiNormal)
		SHADER_PARAMETER(uint32,    gHistoryThreshold)
		SHADER_PARAMETER(float,     gDenoisingRange)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gSpecularIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gDiffuseIllumination)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gViewZ)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutNormalRoughness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXSpatialVarianceEstimationCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_SpatialVarianceEstimation.cs.usf", "main", SF_Compute);

// A-TROUS (SMEM)
class FNRDRELAXAtrousShmemCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXAtrousShmemCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXAtrousShmemCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvRectSize)
		SHADER_PARAMETER(float,     gSpecularPhiLuminance)
		SHADER_PARAMETER(float,     gDiffusePhiLuminance)
		SHADER_PARAMETER(float,     gPhiDepth)
		SHADER_PARAMETER(float,     gPhiNormal)
		SHADER_PARAMETER(uint32_t,  gStepSize)
		SHADER_PARAMETER(float,     gRoughnessEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gNormalEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gLuminanceEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gDenoisingRange)
		SHADER_PARAMETER(uint32,    gFrameIndex)
		SHADER_PARAMETER(uint32,    gRoughnessEdgeStoppingEnabled)
		SHADER_PARAMETER(float,     gMaxLuminanceRelativeDifference)
		SHADER_PARAMETER(float,     gSpecularLobeAngleFraction)
		SHADER_PARAMETER(float,     gSpecularLobeAngleSlack)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gDiffuseIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>,   gHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gSpecularReprojectionConfidence)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>,   gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>,    gViewZFP16)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationAndVariance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXAtrousShmemCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_AtrousShmem.cs.usf", "main", SF_Compute);

// A-TROUS
class FNRDRELAXAtrousStandardCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXAtrousStandardCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXAtrousStandardCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FVector4,  gFrustumRight)
		SHADER_PARAMETER(FVector4,  gFrustumUp)
		SHADER_PARAMETER(FVector4,  gFrustumForward)
		SHADER_PARAMETER(FIntPoint, gResolution)
		SHADER_PARAMETER(FVector2D, gInvRectSize)
		SHADER_PARAMETER(float,     gSpecularPhiLuminance)
		SHADER_PARAMETER(float,     gDiffusePhiLuminance)
		SHADER_PARAMETER(float,     gPhiDepth)
		SHADER_PARAMETER(float,     gPhiNormal)
		SHADER_PARAMETER(uint32_t,  gStepSize)
		SHADER_PARAMETER(float,     gRoughnessEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gNormalEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gLuminanceEdgeStoppingRelaxation)
		SHADER_PARAMETER(float,     gDenoisingRange)
		SHADER_PARAMETER(uint32,    gFrameIndex)
		SHADER_PARAMETER(uint32,    gRoughnessEdgeStoppingEnabled)
		SHADER_PARAMETER(float,     gMaxLuminanceRelativeDifference)
		SHADER_PARAMETER(float,     gSpecularLobeAngleFraction)
		SHADER_PARAMETER(float,     gSpecularLobeAngleSlack)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gDiffuseIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, gHistoryLength)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, gSpecularReprojectionConfidence)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, gNormalRoughness)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, gViewZFP16)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutSpecularIlluminationAndVariance)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, gOutDiffuseIlluminationAndVariance)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXAtrousStandardCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_AtrousStandard.cs.usf", "main", SF_Compute);

// SPLIT_SCREEN
class FNRDRELAXSplitScreenCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNRDRELAXSplitScreenCS);
	SHADER_USE_PARAMETER_STRUCT(FNRDRELAXSplitScreenCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FIntPoint, gRectOrigin)
		SHADER_PARAMETER(FVector2D, gInvRectSize)
		SHADER_PARAMETER(float, gSplitScreen)
		SHADER_PARAMETER(uint32, gDiffCheckerboard)
		SHADER_PARAMETER(uint32, gSpecCheckerboard)
		SHADER_PARAMETER(float, gInf)

		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, gIn_ViewZ)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float3>, gIn_Spec)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float3>, gIn_Diff)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, gOut_Spec)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, gOut_Diff)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		NRDModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_GLOBAL_SHADER(FNRDRELAXSplitScreenCS, "/Plugin/NRD/Private/RELAX_DiffuseSpecular_SplitScreen.cs.usf", "main", SF_Compute);


// moral equivalent of DenoiserImpl::UpdateMethod_RelaxDiffuseSpecular(const MethodData& methodData)
FRelaxOutputs AddRelaxPasses(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FRelaxPassParameters& Inputs,
	FNRDRelaxHistoryRef History
	)
{
	Inputs.Validate();
	check(History.IsValid());

	RDG_EVENT_SCOPE(GraphBuilder, "RELAX::DiffuseSpecular");

	RelaxDiffuseSpecularSettings RelaxSettings = RelaxDiffuseSpecularSettings::FromConsoleVariables();

	const FIntPoint DenoiseBufferSize = Inputs.Diffuse->Desc.Extent;
	check(Inputs.Diffuse->Desc.Extent == View.ViewRect.Size());
	const bool bNeedHistoryReset = View.bCameraCut || !History->HasValidResources() || History->FrameIndex == 0;

	// Extract camera parameters from View.

	FMatrix WorldToViewMatrix = View.ViewMatrices.GetTranslatedViewMatrix();
	FMatrix WorldToViewMatrixPrev = View.PrevViewInfo.ViewMatrices.GetTranslatedViewMatrix();
	FMatrix WorldToClipMatrixPrev = View.PrevViewInfo.ViewMatrices.GetTranslatedViewProjectionMatrix().GetTransposed();
	FMatrix ViewToClipMatrix = View.ViewMatrices.GetProjectionMatrix();
	FMatrix ViewToClipMatrixPrev = View.PrevViewInfo.ViewMatrices.GetProjectionMatrix();
	FVector2D JitterDelta2D = View.ViewMatrices.GetTemporalAAJitter() - View.PrevViewInfo.ViewMatrices.GetTemporalAAJitter();
	float JitterDelta = FMath::Max(abs(JitterDelta2D.X), abs(JitterDelta2D.Y));

	// Calculate camera right and up vectors in worldspace scaled according to frustum extents,
	// and unit forward vector, for fast worldspace position reconstruction in shaders
	float TanHalfFov = 1.0f / ViewToClipMatrix.M[0][0];
	float Aspect = ViewToClipMatrix.M[0][0] / ViewToClipMatrix.M[1][1];
	FVector FrustumRight = WorldToViewMatrix.GetColumn(0) * TanHalfFov;
	FVector FrustumUp = WorldToViewMatrix.GetColumn(1) * TanHalfFov * Aspect;
	FVector FrustumForward = WorldToViewMatrix.GetColumn(2);

	float PrevTanHalfFov = 1.0f / ViewToClipMatrixPrev.M[0][0];
	float PrevAspect = ViewToClipMatrixPrev.M[0][0] / ViewToClipMatrixPrev.M[1][1];
	FVector PrevFrustumRight = WorldToViewMatrixPrev.GetColumn(0) * TanHalfFov;
	FVector PrevFrustumUp = WorldToViewMatrixPrev.GetColumn(1) * PrevTanHalfFov * PrevAspect;
	FVector PrevFrustumForward = WorldToViewMatrixPrev.GetColumn(2);

	// subrect parameters
	// EHartNV TODO - replace with proper subrect support
	const FIntPoint ViewRectOrigin = FIntPoint(0, 0);
	const FVector2D ViewRectSize = View.ViewRect.Size();
	const FVector2D InvViewRectSize = FVector2D(1.0f / ViewRectSize.X, 1.0f / ViewRectSize.Y);
	const FVector2D ViewRectSizePrev = ViewRectSize;
	
	//Timing
	double CurrentTime = FPlatformTime::Seconds();

	// Helper funcs
	auto CreateIntermediateTexture = [&GraphBuilder, DenoiseBufferSize](EPixelFormat Format, const TCHAR* DebugName)
	{
		return GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(
				DenoiseBufferSize, Format, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV),
			DebugName
		);
	};

	auto ImportFromHistory = [&GraphBuilder, &History](TRefCountPtr <IPooledRenderTarget>& Buffer, TRefCountPtr <IPooledRenderTarget>& Fallback)
	{
		check((History->FrameIndex != 0) == History->HasValidResources());
		// RegisterExternalTextureWithFallback checks whether Buffer IsValid()
		return GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(
			RegisterExternalTextureWithFallback(GraphBuilder, Buffer , Fallback)
		));
	};

	// Precondition data
	FRDGTextureRef PrepassDiffuseIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.Prepass.DiffuseIllumination"));
	FRDGTextureRef PrepassSpecularIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.Prepass.SpecularIllumination"));
	FRDGTextureRef PrepassViewZ = CreateIntermediateTexture(PF_R32_FLOAT, TEXT("NRD.Relax.Prepass.ViewZ"));
	FRDGTextureRef PrepassScaledViewZ = CreateIntermediateTexture(PF_R16F, TEXT("NRD.Relax.Prepass.ScaledViewZ"));
	{
		TShaderMapRef<FNRDRELAXPrepassCS> ComputeShader(View.ShaderMap);

		FNRDRELAXPrepassCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXPrepassCS::FParameters>();

		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();

		PassParameters->gWorldToClip = View.ViewMatrices.GetTranslatedViewProjectionMatrix().GetTransposed();
		PassParameters->gWorldToView = View.ViewMatrices.GetTranslatedViewMatrix().GetTransposed();
		PassParameters->gViewToClip = View.ViewMatrices.GetProjectionMatrix().GetTransposed();

		// EHartNV TODO - should be a white-noise rotation matrix cos, sin, -sin, cos
		//PassParameters->gRotator = FVector4(1.0f, 0.0f, 0.0f, 1.0f);
		float ModIndex = History->FrameIndex % 63;
		float Phi = (ModIndex * 1.61803398875f) * 2.0f * 3.1415927;
		float CosPhi = FMath::Cos(Phi);
		float SinPhi = FMath::Sin(Phi);
		PassParameters->gRotator = FVector4(CosPhi, SinPhi, -SinPhi, CosPhi);

		PassParameters->gFrustumRight = FrustumRight;
		PassParameters->gFrustumUp = FrustumUp;
		PassParameters->gFrustumForward = FrustumForward;

		// subrect parameters
		PassParameters->gRectOrigin = ViewRectOrigin;
		PassParameters->gRectOffset = FVector2D(0.0f,0.0f); // this is the offset in UV space
		PassParameters->gInvRectSize = InvViewRectSize;
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gInvViewSize = InvViewRectSize; // presently UE4 has view and rect identical due to copying
		PassParameters->gInvRectSize = InvViewRectSize;
		PassParameters->gResolutionScale = FVector2D(1.0f, 1.0f); 

		PassParameters->gIsOrtho = !View.IsPerspectiveProjection();
		PassParameters->gUnproject = 1.0f / (0.5f * DenoiseBufferSize.Y); // projection component is 1 due to UE4 matrix style
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
		PassParameters->gDiffCheckerboard = 2; // why is 2  off?????
		PassParameters->gSpecCheckerboard = 2;
		PassParameters->gFrameIndex = History->FrameIndex;
		PassParameters->gDiffuseBlurRadius = RelaxSettings.DiffuseBlurRadius;
		PassParameters->gSpecularBlurRadius = RelaxSettings.SpecularBlurRadius;
		PassParameters->gMeterToUnitsMultiplier = 100.0f; // assuming standard UE unit of 1 cm

		// Set SRVs
		PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
		PassParameters->gViewZ = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.ViewZ));
		PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Diffuse));
		PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Specular));

		// Set UAVs
		PassParameters->gOutDiffuseIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(PrepassDiffuseIllumination));
		PassParameters->gOutSpecularIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(PrepassSpecularIllumination));
		PassParameters->gOutViewZ = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(PrepassViewZ));
		PassParameters->gOutScaledViewZ = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(PrepassScaledViewZ));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Pack input data"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}

	// Reproject
	FRDGTextureRef ReprojectDiffuseIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.Reproject.DiffuseIllumination"));
	FRDGTextureRef ReprojectSpecularIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.Reproject.SpecularIllumination"));
	FRDGTextureRef ReprojectDiffuseIlluminationResponsive = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.Reproject.DiffuseIlluminationResponsive"));
	FRDGTextureRef ReprojectSpecularIlluminationResponsive = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.Reproject.SpecularIlluminationResponsive"));
	FRDGTextureRef ReprojectReflectionHitT = CreateIntermediateTexture(PF_R16F, TEXT("NRD.Relax.Reproject.ReflectionHitT"));
	FRDGTextureRef ReprojectSpecularAndDiffuseHistoryLength = CreateIntermediateTexture(PF_R8G8, TEXT("NRD.Relax.Reproject.SpecularAndDiffuseHistoryLength"));
	FRDGTextureRef ReprojectSpecularReprojectionConfidence = CreateIntermediateTexture(PF_R16F, TEXT("NRD.Relax.Reproject.SpecularReprojectionConfidence"));
	{
		TShaderMapRef<FNRDRELAXReprojectCS> ComputeShader(View.ShaderMap);

		FNRDRELAXReprojectCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXReprojectCS::FParameters>();
		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();
		PassParameters->gPrevWorldToClip = WorldToClipMatrixPrev;
		PassParameters->gFrustumRight = FrustumRight;
		PassParameters->gFrustumUp = FrustumUp;
		PassParameters->gFrustumForward = FrustumForward;
		PassParameters->gPrevFrustumRight = PrevFrustumRight;
		PassParameters->gPrevFrustumUp = PrevFrustumUp;
		PassParameters->gPrevFrustumForward = PrevFrustumForward;
		PassParameters->gPrevCameraPosition = -View.ViewMatrices.GetViewOrigin() + View.PrevViewInfo.ViewMatrices.GetViewOrigin();
		PassParameters->gJitterDelta = JitterDelta;
		PassParameters->gMotionVectorScale = FVector2D(1.0f, 1.0f);
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gInvViewSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
		PassParameters->gUseBicubic = RelaxSettings.bicubicFilterForReprojectionEnabled;
		PassParameters->gSpecularVarianceBoost = RelaxSettings.specularVarianceBoost;
		PassParameters->gWorldSpaceMotion = 0;
		PassParameters->gIsOrtho = !View.IsPerspectiveProjection();
		PassParameters->gUnproject = 1.0f / (0.5f * DenoiseBufferSize.Y);
		PassParameters->gResetHistory = bNeedHistoryReset;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
		PassParameters->gDisocclusionThreshold = RelaxSettings.disocclusionThreshold;
		PassParameters->gWorldToClip = View.ViewMatrices.GetTranslatedViewProjectionMatrix().GetTransposed();
		
		// subrect parameters
		PassParameters->gRectOrigin = ViewRectOrigin;
		PassParameters->gInvRectSize = InvViewRectSize;
		PassParameters->gRectSizePrev = ViewRectSizePrev;

		PassParameters->gSpecularMaxAccumulatedFrameNum = RelaxSettings.specularMaxAccumulatedFrameNum;
		PassParameters->gSpecularMaxFastAccumulatedFrameNum = RelaxSettings.specularMaxFastAccumulatedFrameNum;
		PassParameters->gDiffuseMaxAccumulatedFrameNum = RelaxSettings.diffuseMaxAccumulatedFrameNum;
		PassParameters->gDiffuseMaxFastAccumulatedFrameNum = RelaxSettings.diffuseMaxFastAccumulatedFrameNum;

		PassParameters->gRoughnessBasedSpecularAccumulation = 1; // needs control
		PassParameters->gVirtualHistoryClampingEnabled = 1; // needs control
		PassParameters->gFrameIndex = History->FrameIndex; 
		PassParameters->gIsCameraStatic = 0; // TODO: tie through pause
		PassParameters->gSkipReprojectionTestWithoutMotion = 0;
		PassParameters->gDiffCheckerboard = 2;
		PassParameters->gSpecCheckerboard = 2;
		PassParameters->gCheckerboardResolveAccumSpeed = 1.0f; // need to tie up control once checkerboard is supported
		PassParameters->gUseConfidenceInputs = 0; // confidence inputs not yet supported upstream
		PassParameters->gFramerateScale = float(FMath::Clamp(16.66667 / 1000.0*(CurrentTime - History->Time), 0.25, 4.0)); // framerate scale relative to 60 Hz
		PassParameters->gRejectDiffuseHistoryNormalThreshold = 0.0f; // need to hook-up parameter

		// Set SRVs for input & intermediate textures
		PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassSpecularIllumination));
		PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassDiffuseIllumination));
		PassParameters->gMotion  = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.MotionVectors));

		PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
		PassParameters->gViewZ = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.ViewZ));

		// Set SRVs for history buffers
		PassParameters->gPrevReflectionHitT = ImportFromHistory(History->ReflectionHitT, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularAndDiffuseHistoryLength = ImportFromHistory(History->SpecularAndDiffuseHistoryLength, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularIllumination = ImportFromHistory(History->SpecularIllumination, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevDiffuseIllumination = ImportFromHistory(History->DiffuseIllumination, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevSpecularIlluminationResponsive = ImportFromHistory(History->SpecularIlluminationResponsive, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevDiffuseIlluminationResponsive = ImportFromHistory(History->DiffuseIlluminationResponsive, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevViewZ = ImportFromHistory(History->ViewZ, GSystemTextures.ZeroUIntDummy);
		PassParameters->gPrevNormalRoughness = ImportFromHistory(History->NormalRoughness, GSystemTextures.ZeroUIntDummy);

		//confidence parameters, not yet supported
		PassParameters->gSpecConfidence = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create( GraphBuilder.RegisterExternalTexture(GSystemTextures.ZeroUIntDummy)));
		PassParameters->gDiffConfidence = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(GraphBuilder.RegisterExternalTexture(GSystemTextures.ZeroUIntDummy)));

		// Set UAVs
		PassParameters->gOutReflectionHitT = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectReflectionHitT));
		PassParameters->gOutSpecularAndDiffuseHistoryLength = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularAndDiffuseHistoryLength));
		PassParameters->gOutSpecularReprojectionConfidence = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularReprojectionConfidence));

		PassParameters->gOutSpecularIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularIllumination));
		PassParameters->gOutDiffuseIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectDiffuseIllumination));
		PassParameters->gOutSpecularIlluminationResponsive = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectSpecularIlluminationResponsive));
		PassParameters->gOutDiffuseIlluminationResponsive = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ReprojectDiffuseIlluminationResponsive));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Reproject"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}

	// Disocclusion fix
	FRDGTextureRef DisocclusionFixSpecularIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.DisocclusionFix.SpecularIllumination"));
	FRDGTextureRef DisocclusionFixDiffuseIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.DisocclusionFix.DiffuseIllumination"));
	FRDGTextureRef DisocclusionFixSpecularIlluminationResponsive = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.DisocclusionFix.SpecularIlluminationResponsive"));
	FRDGTextureRef DisocclusionFixDiffuseIlluminationResponsive = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.DisocclusionFix.DiffuseIlluminationResponsive"));
	{
		TShaderMapRef<FNRDRELAXDisocclusionFixCS> ComputeShader(View.ShaderMap);

		FNRDRELAXDisocclusionFixCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXDisocclusionFixCS::FParameters>();
		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();
		PassParameters->gFrustumRight = FrustumRight;
		PassParameters->gFrustumUp = FrustumUp;
		PassParameters->gFrustumForward = FrustumForward;
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gInvRectSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
		PassParameters->gDisocclusionFixEdgeStoppingNormalPower = RelaxSettings.disocclusionFixEdgeStoppingNormalPower;
		PassParameters->gMaxRadius = RelaxSettings.disocclusionFixMaxRadius;
		PassParameters->gFramesToFix = RelaxSettings.disocclusionFixNumFramesToFix;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
		PassParameters->gDisocclusionThreshold = RelaxSettings.disocclusionThreshold;

		// Set SRVs
		PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularIllumination));
		PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectDiffuseIllumination));
		PassParameters->gSpecularIlluminationResponsive = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularIlluminationResponsive));
		PassParameters->gDiffuseIlluminationResponsive = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectDiffuseIlluminationResponsive));
		PassParameters->gSpecularAndDiffuseHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuseHistoryLength));
		PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
		PassParameters->gViewZFP16 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassScaledViewZ));

		// Set UAVs
		PassParameters->gOutSpecularIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixSpecularIllumination));
		PassParameters->gOutDiffuseIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixDiffuseIllumination));
		PassParameters->gOutSpecularIlluminationResponsive = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixSpecularIlluminationResponsive));
		PassParameters->gOutDiffuseIlluminationResponsive = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DisocclusionFixDiffuseIlluminationResponsive));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Disocclusion fix"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}

	// History clamping
	FRDGTextureRef HistoryClampingSpecularIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.HistoryClamping.SpecularIllumination"));
	FRDGTextureRef HistoryClampingDiffuseIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.HistoryClamping.DiffuseIllumination"));
	FRDGTextureRef HistoryClampingSpecularAndDiffuseHistoryLength = CreateIntermediateTexture(PF_R8G8, TEXT("NRD.Relax.HistoryClamping.SpecularAndDiffuse2ndMoments"));
	{
		TShaderMapRef<FNRDRELAXHistoryClampingCS> ComputeShader(View.ShaderMap);

		FNRDRELAXHistoryClampingCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXHistoryClampingCS::FParameters>();
		PassParameters->CommonSamplers = CreateNRDCommonSamplerParameters();
		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gColorBoxSigmaScale = RelaxSettings.historyClampingColorBoxSigmaScale;

		// Set SRVs
		PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixSpecularIllumination));
		PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixDiffuseIllumination));
		PassParameters->gSpecularIlluminationResponsive = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixSpecularIlluminationResponsive));
		PassParameters->gDiffuseIlluminationResponsive = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(DisocclusionFixDiffuseIlluminationResponsive));
		PassParameters->gSpecularAndDiffuseHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularAndDiffuseHistoryLength));

		// Set UAVs
		PassParameters->gOutSpecularIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HistoryClampingSpecularIllumination));
		PassParameters->gOutDiffuseIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HistoryClampingDiffuseIllumination));
		PassParameters->gOutSpecularAndDiffuseHistoryLength = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HistoryClampingSpecularAndDiffuseHistoryLength));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("History Clamping"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
		);
	}

	if (RelaxSettings.antifirefly)
	{
		// Firefly suppression
		FRDGTextureRef FireflySpecularIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.FireFly.SpecularIllumination"));
		FRDGTextureRef FireflyDiffuseIllumination = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.FireFly.DiffuseIllumination"));
		{
			TShaderMapRef<FNRDRELAXFireflyCS> ComputeShader(View.ShaderMap);

			FNRDRELAXFireflyCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXFireflyCS::FParameters>();

			PassParameters->gResolution = DenoiseBufferSize;
			PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;

			PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularIllumination));
			PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingDiffuseIllumination));
			PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
			PassParameters->gViewZFP16 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassScaledViewZ));

			PassParameters->gOutSpecularIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FireflySpecularIllumination));
			PassParameters->gOutDiffuseIllumination = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FireflyDiffuseIllumination));

			ClearUnusedGraphResources(ComputeShader, PassParameters);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Firefly suppression"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
			);
		}

		HistoryClampingSpecularIllumination = FireflySpecularIllumination;
		HistoryClampingDiffuseIllumination = FireflyDiffuseIllumination;
	}

	// Spatial variance estimation
	FRDGTextureRef SpatialVarianceEstimationSpecularIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.SpatialVarianceEstimation.SpecularIlluminationAndVariance"));
	FRDGTextureRef SpatialVarianceEstimationDiffuseIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.SpatialVarianceEstimation.DiffuseIlluminationAndVariance"));
	FRDGTextureRef SpatialVarianceEstimationNormalRoughness = CreateIntermediateTexture(NRDGetNormalRoughnessFormat(), TEXT("NRD.Relax.SpatialVarianceEstimation.NormalRoughness"));
	{
		TShaderMapRef<FNRDRELAXSpatialVarianceEstimationCS> ComputeShader(View.ShaderMap);

		FNRDRELAXSpatialVarianceEstimationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXSpatialVarianceEstimationCS::FParameters>();

		PassParameters->gResolution = DenoiseBufferSize;
		PassParameters->gPhiNormal = RelaxSettings.phiNormal;
		PassParameters->gHistoryThreshold = RelaxSettings.spatialVarianceEstimationHistoryThreshold;
		PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;

		PassParameters->gSpecularIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularIllumination));
		PassParameters->gDiffuseIllumination = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingDiffuseIllumination));
		PassParameters->gHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseHistoryLength));
		PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
		PassParameters->gViewZ = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassViewZ));

		PassParameters->gOutSpecularIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpatialVarianceEstimationSpecularIlluminationAndVariance));
		PassParameters->gOutDiffuseIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpatialVarianceEstimationDiffuseIlluminationAndVariance));
		PassParameters->gOutNormalRoughness = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(SpatialVarianceEstimationNormalRoughness));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Spatial Variance Estimation"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 16)
		);
	}

	FRDGTextureRef FinalAtrousOutputDiffuse = nullptr;
	FRDGTextureRef FinalAtrousOutputSpecular = nullptr;
	{
		RDG_EVENT_SCOPE(GraphBuilder, "A-trous");

		// A-trous (first) SMEM
		FRDGTextureRef AtrousPingSpecularIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPing.SpecularIlluminationAndVariance"));
		FRDGTextureRef AtrousPingDiffuseIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPing.DiffuseIlluminationAndVariance"));
		FRDGTextureRef AtrousPongSpecularIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPong.SpecularIlluminationAndVariance"));
		FRDGTextureRef AtrousPongDiffuseIlluminationAndVariance = CreateIntermediateTexture(PF_FloatRGBA, TEXT("NRD.Relax.AtrousPong.DiffuseIlluminationAndVariance"));
		{
			TShaderMapRef<FNRDRELAXAtrousShmemCS> ComputeShader(View.ShaderMap);

			FNRDRELAXAtrousShmemCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXAtrousShmemCS::FParameters>();

			PassParameters->gFrustumRight = FrustumRight;
			PassParameters->gFrustumUp = FrustumUp;
			PassParameters->gFrustumForward = FrustumForward;
			PassParameters->gResolution = DenoiseBufferSize;
			PassParameters->gInvRectSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
			PassParameters->gSpecularPhiLuminance = RelaxSettings.specularPhiLuminance;
			PassParameters->gDiffusePhiLuminance = RelaxSettings.diffusePhiLuminance;
			PassParameters->gPhiDepth = RelaxSettings.phiDepth;
			PassParameters->gPhiNormal = RelaxSettings.phiNormal;
			PassParameters->gStepSize = 1;
			PassParameters->gRoughnessEdgeStoppingRelaxation = RelaxSettings.roughnessEdgeStoppingRelaxation;
			PassParameters->gNormalEdgeStoppingRelaxation = RelaxSettings.normalEdgeStoppingRelaxation;
			PassParameters->gLuminanceEdgeStoppingRelaxation = RelaxSettings.luminanceEdgeStoppingRelaxation;
			PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
			PassParameters->gFrameIndex = History->FrameIndex;
			PassParameters->gRoughnessEdgeStoppingEnabled = 1;
			PassParameters->gMaxLuminanceRelativeDifference = - FMath::Loge( RelaxSettings.MinLuminanceWeight); //TODO is ln correct? 
			PassParameters->gSpecularLobeAngleFraction = RelaxSettings.SpecularLobeAngleFraction;
			PassParameters->gSpecularLobeAngleSlack = FMath::DegreesToRadians(RelaxSettings.SpecularLobeAngleSlack);

			PassParameters->gSpecularIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialVarianceEstimationSpecularIlluminationAndVariance));
			PassParameters->gDiffuseIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SpatialVarianceEstimationDiffuseIlluminationAndVariance));
			PassParameters->gHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseHistoryLength));
			PassParameters->gSpecularReprojectionConfidence = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularReprojectionConfidence));
			PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
			PassParameters->gViewZFP16 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassScaledViewZ));

			PassParameters->gOutSpecularIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousPingSpecularIlluminationAndVariance));
			PassParameters->gOutDiffuseIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousPingDiffuseIlluminationAndVariance));

			ClearUnusedGraphResources(ComputeShader, PassParameters);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("A-Trous SHMEM"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
			);
		}
	
		check(2 <= RelaxSettings.atrousIterationNum && RelaxSettings.atrousIterationNum <= 7);
		// Running variable number of Atrous passes
		for (uint32_t i = 1; i < RelaxSettings.atrousIterationNum; i++)
		{
			FRDGTextureRef AtrousInputDiffuse;
			FRDGTextureRef AtrousInputSpecular;
			FRDGTextureRef AtrousOutputDiffuse;
			FRDGTextureRef AtrousOutputSpecular;

			if ((i % 2) == 1)
			{
				AtrousInputDiffuse = AtrousPingDiffuseIlluminationAndVariance;
				AtrousInputSpecular = AtrousPingSpecularIlluminationAndVariance;
				AtrousOutputDiffuse = AtrousPongDiffuseIlluminationAndVariance;
				AtrousOutputSpecular = AtrousPongSpecularIlluminationAndVariance;
			}
			else
			{
				AtrousInputDiffuse = AtrousPongDiffuseIlluminationAndVariance;
				AtrousInputSpecular = AtrousPongSpecularIlluminationAndVariance;
				AtrousOutputDiffuse = AtrousPingDiffuseIlluminationAndVariance;
				AtrousOutputSpecular = AtrousPingSpecularIlluminationAndVariance;
			}

			TShaderMapRef<FNRDRELAXAtrousStandardCS> ComputeShader(View.ShaderMap);

			FNRDRELAXAtrousStandardCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXAtrousStandardCS::FParameters>();

			PassParameters->gFrustumRight = FrustumRight;
			PassParameters->gFrustumUp = FrustumUp;
			PassParameters->gFrustumForward = FrustumForward;
			PassParameters->gResolution = DenoiseBufferSize;
			PassParameters->gInvRectSize = FVector2D(1.0f, 1.0f) / FVector2D(DenoiseBufferSize);
			PassParameters->gSpecularPhiLuminance = RelaxSettings.specularPhiLuminance;
			PassParameters->gDiffusePhiLuminance = RelaxSettings.diffusePhiLuminance;
			PassParameters->gPhiDepth = RelaxSettings.phiDepth;
			PassParameters->gPhiNormal = RelaxSettings.phiNormal;
			PassParameters->gStepSize = 1 << i;
			PassParameters->gRoughnessEdgeStoppingRelaxation = RelaxSettings.roughnessEdgeStoppingRelaxation;
			PassParameters->gNormalEdgeStoppingRelaxation = RelaxSettings.normalEdgeStoppingRelaxation;
			PassParameters->gLuminanceEdgeStoppingRelaxation = RelaxSettings.luminanceEdgeStoppingRelaxation;
			PassParameters->gDenoisingRange = RelaxSettings.denoisingRange;
			PassParameters->gFrameIndex = History->FrameIndex;
			PassParameters->gRoughnessEdgeStoppingEnabled = 1;
			PassParameters->gMaxLuminanceRelativeDifference = -FMath::Loge(RelaxSettings.MinLuminanceWeight); //TODO is natural log correct? 
			PassParameters->gSpecularLobeAngleFraction = RelaxSettings.SpecularLobeAngleFraction;
			PassParameters->gSpecularLobeAngleSlack = FMath::DegreesToRadians(RelaxSettings.SpecularLobeAngleSlack);

			PassParameters->gSpecularIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(AtrousInputSpecular));
			PassParameters->gDiffuseIlluminationAndVariance = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(AtrousInputDiffuse));
			PassParameters->gHistoryLength = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(HistoryClampingSpecularAndDiffuseHistoryLength));
			PassParameters->gSpecularReprojectionConfidence = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(ReprojectSpecularReprojectionConfidence));
			PassParameters->gNormalRoughness = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.NormalAndRoughness));
			PassParameters->gViewZFP16 = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassScaledViewZ));

			PassParameters->gOutSpecularIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousOutputSpecular));
			PassParameters->gOutDiffuseIlluminationAndVariance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(AtrousOutputDiffuse));

			ClearUnusedGraphResources(ComputeShader, PassParameters);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("A-Trous standard"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
			);
		}
		// Selecting final output of the set of Atrous passes
		FinalAtrousOutputDiffuse = ((RelaxSettings.atrousIterationNum % 2) == 1) ? AtrousPongDiffuseIlluminationAndVariance : AtrousPingDiffuseIlluminationAndVariance;
		FinalAtrousOutputSpecular = ((RelaxSettings.atrousIterationNum % 2) == 1) ? AtrousPongSpecularIlluminationAndVariance : AtrousPingSpecularIlluminationAndVariance;
	}

	// split screen
	const int SplitScreenPercentage = RelaxSettings.splitScreen;
	if(0 != SplitScreenPercentage)
	{
		TShaderMapRef<FNRDRELAXSplitScreenCS> ComputeShader(View.ShaderMap);

		FNRDRELAXSplitScreenCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNRDRELAXSplitScreenCS::FParameters>();

		PassParameters->gInvRectSize = FVector2D( 1.0f / DenoiseBufferSize.X, 1.0f / DenoiseBufferSize.Y) ;
		PassParameters->gSplitScreen = float(FMath::Clamp(SplitScreenPercentage, 0, 100)) / 100.0f;
		PassParameters->gDiffCheckerboard = 2;
		PassParameters->gSpecCheckerboard = 2;
		PassParameters->gRectOrigin = FIntPoint(0, 0);
		PassParameters->gInf = RelaxSettings.denoisingRange;


		PassParameters->gIn_ViewZ = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(PrepassViewZ));
		PassParameters->gIn_Diff = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Diffuse));
		PassParameters->gIn_Spec = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Inputs.Specular));

		PassParameters->gOut_Diff = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FinalAtrousOutputDiffuse));
		PassParameters->gOut_Spec = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(FinalAtrousOutputSpecular));

		ClearUnusedGraphResources(ComputeShader, PassParameters);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("split screen"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(DenoiseBufferSize, 8)
		);
	}
	


	// now queue up the history extraction
	if (!View.bStatePrevViewInfoIsReadOnly && History)
	{
		GraphBuilder.QueueTextureExtraction(HistoryClampingSpecularIllumination, &History->SpecularIllumination);
		GraphBuilder.QueueTextureExtraction(HistoryClampingDiffuseIllumination, &History->DiffuseIllumination);
		GraphBuilder.QueueTextureExtraction(DisocclusionFixSpecularIlluminationResponsive, &History->SpecularIlluminationResponsive);
		GraphBuilder.QueueTextureExtraction(DisocclusionFixDiffuseIlluminationResponsive, &History->DiffuseIlluminationResponsive);

		GraphBuilder.QueueTextureExtraction(SpatialVarianceEstimationNormalRoughness, &History->NormalRoughness);
		GraphBuilder.QueueTextureExtraction(PrepassViewZ, &History->ViewZ);
		GraphBuilder.QueueTextureExtraction(HistoryClampingSpecularAndDiffuseHistoryLength, &History->SpecularAndDiffuseHistoryLength);
		GraphBuilder.QueueTextureExtraction(ReprojectReflectionHitT, &History->ReflectionHitT);

		++History->FrameIndex;
		History->Time = CurrentTime;
	}

	FRelaxOutputs Outputs;

	Outputs.Diffuse = FinalAtrousOutputDiffuse;
	Outputs.Specular = FinalAtrousOutputSpecular;

	return Outputs;
}
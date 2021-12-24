// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	RaytracingOptions.h declares ray tracing options for use in rendering
=============================================================================*/

#pragma once

#include "RHIDefinitions.h"

class FSkyLightSceneProxy;
class FViewInfo;
class FLightSceneInfoCompact;
class FLightSceneInfo;

// be sure to also update the definition in the `RayTracingPrimaryRays.usf`
enum class ERayTracingPrimaryRaysFlag: uint32
{
	None                      =      0,
	UseGBufferForMaxDistance  = 1 << 0,
	ConsiderSurfaceScatter	  = 1 << 1,
	AllowSkipSkySample		  = 1 << 2,
	HybridTranslucency		  = 1 << 3,
	StencilMask				  = 1 << 4,
	HalfResolution			  = 1 << 5,
	CheckerboardSampling	  = 1 << 6,
	TimeTracing				  = 1 << 7,
	TranslucentTopLayer		  = 1 << 8,
	IgnoreBackfaceOpacity	  = 1 << 9,
	SeparateTranslucency	  = 1 << 10,
	SeparateTranslucencyReflection = 1 << 11
};

ENUM_CLASS_FLAGS(ERayTracingPrimaryRaysFlag);

struct FRayTracingPrimaryRaysOptions
{
	bool bEnabled;
	int32 SamplerPerPixel;
	uint32 ApplyFog;
	float PrimaryRayBias;
	float MaxRoughness;
	int32 MaxRefractionRays;
	int32 EnableEmmissiveAndIndirectLighting;
	int32 EnableDirectLighting;
	int32 EnableShadows;
	float MinRayDistance;
	float MaxRayDistance;
	int32 EnableRefraction;
	float RoughnessMultiplier;
};

// be sure to also update the definition in the `RayTracingPrimaryRays.usf`
enum class EHybridTranslucencyMode : uint8
{
	None = 0,
	Mode1 = 1, // r.RayTracing.Translucency == 2, working with ray traced reflection only
	Mode2 = 2,  // r.RayTracing.Translucency == 3, working with full ray traced tanslucency features (reflection & refraction)
	MAX // required by SHADER_PERMUTATION_ENUM_CLASS()
};

#if RHI_RAYTRACING

// Whether a particular effect should be used, taking into account debug override
extern bool ShouldRenderRayTracingEffect(bool bEffectEnabled);

extern bool AnyRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View);
extern FRayTracingPrimaryRaysOptions GetRayTracingTranslucencyOptions(const FViewInfo& View);

extern bool ShouldRenderRayTracingSkyLight(const FSkyLightSceneProxy* SkyLightSceneProxy);
extern bool ShouldRenderRayTracingAmbientOcclusion(const FViewInfo& View);
extern bool ShouldRenderRayTracingReflections(const FViewInfo& View);
extern bool ShouldRenderRayTracingGlobalIllumination(const FViewInfo& View);
extern bool ShouldRenderRayTracingTranslucency(const FViewInfo& View);
extern bool ShouldRenderRayTracingShadows();
extern bool ShouldRenderRayTracingShadowsForLight(const FLightSceneProxy& LightProxy);
extern bool ShouldRenderRayTracingShadowsForLight(const FLightSceneInfoCompact& LightInfo);
extern bool ShouldRenderExperimentalPluginRayTracingGlobalIllumination();
extern bool CanOverlayRayTracingOutput(const FViewInfo& View);
extern EHybridTranslucencyMode GetRayTracingHybridTranslucencyMode(const FViewInfo& View);
extern bool ShouldRenderRayTracingSampledLighting();

extern bool EnableRayTracingShadowTwoSidedGeometry();
extern float GetRaytracingMaxNormalBias();

extern bool CanUseRayTracingLightingMissShader(EShaderPlatform ShaderPlatform);
extern bool CanUseRayTracingAMDHitToken();

extern int32 MaxHybridTranslucencyLayers();

extern bool VisualizeRayTracingTiming(const FViewInfo& View);

extern bool SupportSampledLightingForType(ELightComponentType Type);
extern bool SupportSampledLightingForLightFunctions();

// 0 - off, 1 - for systems requesting it, 2 - forced on
extern int32 UseSampledLightingForParticles();

#else // RHI_RAYTRACING

FORCEINLINE bool ShouldRenderRayTracingEffect(bool bEffectEnabled)
{
	return false;
}

FORCEINLINE bool AnyRayTracingPassEnabled(const FScene* Scene, const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingSkyLight(const FSkyLightSceneProxy* SkyLightSceneProxy)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingAmbientOcclusion(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingReflections(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingGlobalIllumination(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingTranslucency(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingHybridTranslucency()
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingShadows()
{
	return false;
}

FORCEINLINE EHybridTranslucencyMode GetRayTracingHybridTranslucencyMode(const FViewInfo& View)
{
	return EHybridTranslucencyMode::None;
}

FORCEINLINE bool ShouldRenderRayTracingShadowsForLight(const FLightSceneProxy& LightProxy)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingShadowsForLight(const FLightSceneInfoCompact& LightInfo)
{
	return false;
}

FORCEINLINE bool ShouldRenderExperimentalPluginRayTracingGlobalIllumination()
{
	return false;
}

FORCEINLINE bool CanOverlayRayTracingOutput(const FViewInfo& View)
{
	return true;
}

FORCEINLINE bool CanUseRayTracingLightingMissShader(EShaderPlatform)
{
	return false;
}

FORCEINLINE bool CanUseRayTracingAMDHitToken()
{
	return false;
}

FORCEINLINE int32 MaxHybridTranslucencyLayers()
{
	return 1;
}

FORCEINLINE bool VisualizeRayTracingTiming(const FViewInfo& View)
{
	return false;
}

FORCEINLINE bool ShouldRenderRayTracingSampledLighting()
{
	return false;
}

FORCEINLINE bool SupportSampledLightingForType(ELightComponentType Type)
{
	return false;
}

FORCEINLINE bool SupportSampledLightingForLightFunctions()
{
	return false;
}

FORCEINLINE int32 UseSampledLightingForParticles()
{
	return 0;
}
#endif // RHI_RAYTRACING

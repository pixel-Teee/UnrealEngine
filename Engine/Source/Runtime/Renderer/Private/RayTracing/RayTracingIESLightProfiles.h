// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShaderParameters.h"
#include "Engine/TextureLightProfile.h"
#include "RendererInterface.h"

#if RHI_RAYTRACING

class FIESLightProfileResource
{
public:
	void BuildIESLightProfilesTexture(FRHICommandListImmediate& RHICmdList, const TArray<UTextureLightProfile*, SceneRenderingAllocator>& NewIESProfilesArray);

	uint32 GetIESLightProfilesCount() const
	{
		return IESTextureData.Num();
	}

	void Release()
	{
		check(IsInRenderingThread());

		DefaultTexture.SafeRelease();
		AtlasTexture.SafeRelease();
		AtlasUAV.SafeRelease();
		IESTextureData.Empty();
	}

	FTexture2DRHIRef GetTexture()
	{
		return AtlasTexture;
	}

private:
	FTexture2DRHIRef					DefaultTexture;
	FTexture2DRHIRef					AtlasTexture;
	FUnorderedAccessViewRHIRef			AtlasUAV;
	TArray<const UTextureLightProfile*>	IESTextureData;

	bool IsIESTextureFormatValid(const UTextureLightProfile* Texture) const;

	static constexpr uint32 AllowedIESProfileWidth = 256;
	static constexpr EPixelFormat AllowedIESProfileFormat = PF_FloatRGBA;
};

class FIESLightProfile2DResource
{
public:
	struct FIESLightProfileIndex
	{
		uint32 Page;
		uint32 Start;
	};

	void BuildIESLightProfilesTexture(FRHICommandListImmediate& RHICmdList, const TArray<UTextureLightProfile*, SceneRenderingAllocator>& NewIESProfilesArray);

	uint32 GetIESLightProfilesCount() const
	{
		return IESTextureData.Num();
	}

	uint32 GetIESLightProfilesPerPage() const
	{
		return AllowedIESProfileDim;
	}

	FIESLightProfileIndex GetProfileIndex(int32 Index)
	{
		return IESIndexData[Index];
	}

	void Release()
	{
		check(IsInRenderingThread());

		DefaultTexture.SafeRelease();
		AtlasTexture.SafeRelease();
		AtlasUAV.SafeRelease();
		IESTextureData.Empty();
	}

	FTexture2DArrayRHIRef GetTexture()
	{
		return AtlasTexture;
	}

private:
	FTexture2DRHIRef					DefaultTexture;
	FTexture2DArrayRHIRef				AtlasTexture;
	FUnorderedAccessViewRHIRef			AtlasUAV;
	TArray<const UTextureLightProfile*>	IESTextureData;
	TArray<FIESLightProfileIndex>		IESIndexData;

	bool IsIESTextureFormatValid(const UTextureLightProfile* Texture) const;

	static constexpr uint32 AllowedIESProfileDim = 256;
	static constexpr EPixelFormat AllowedIESProfileFormat = PF_FloatRGBA;
};

#endif // RHI_RAYTRACING

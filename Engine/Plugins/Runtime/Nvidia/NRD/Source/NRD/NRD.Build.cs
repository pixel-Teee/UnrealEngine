// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class NRD : ModuleRules
	{
		public NRD(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicIncludePaths.AddRange(
				new string[] {
					// ... add public include paths required here ...
				}
				);

			PrivateIncludePaths.AddRange(
				new string[] {
					// "../../../../../../Source/Runtime/Renderer/Private",
					// "../../../../Source/Runtime/Engine/",
					// "../../../../Source/Runtime/RHI/",
					// ... add other private include paths required here ...
					EngineDirectory + "/Source/Runtime/Renderer/Private",
				}
				);

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					// ... add other public dependencies that you statically link with here ...
				}
				);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
					"RenderCore",
					"Renderer",
					"RHI",
					"Projects",
					// ... add private dependencies that you statically link with here ...
				}
				);

			DynamicallyLoadedModuleNames.AddRange(
				new string[]
				{
					// ... add any modules that your module loads dynamically here ...
				}
				);
		}
	}
}



using System.IO;

namespace UnrealBuildTool.Rules
{
	public class SteamVRPassthrough : ModuleRules
	{
		public SteamVRPassthrough(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
				
			
			PrivateIncludePaths.AddRange(
				new string[] {
                    Path.Combine(EngineDirectory, "Source/Runtime/Engine/Private"),
                    Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"),
					Path.Combine(EngineDirectory,"Source/Runtime/Windows/D3D11RHI/Private"),
                    Path.Combine(EngineDirectory,"Source/Runtime/Windows/D3D11RHI/Private/Windows"),
				}
				);
			

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
					"RHI",
					"RenderCore",
					"Renderer",
                    "InputCore",
					"HeadMountedDisplay",
					"D3D11RHI"
                }
				);

			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenVR");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "NVAftermath");
			
        }
	}
}

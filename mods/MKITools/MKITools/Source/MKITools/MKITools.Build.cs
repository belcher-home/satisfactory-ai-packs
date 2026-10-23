using UnrealBuildTool;

public class MKITools : ModuleRules
{
    public MKITools(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        // MKI Tools depends only on the game, SML, and the engine (InputCore for the holds, Slate for the auto pickup bar and icon, Json for the bridge-facing answers). MKI Bridge finds
        // it by reflection, so neither mod requires the other.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore", "UMG", "Foliage", "FactoryGame", "SML", "Json" });
    }
}

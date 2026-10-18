using UnrealBuildTool;

public class MKIRoads : ModuleRules
{
    public MKIRoads(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        // MKI Roads depends only on the game, SML, the engine's input modules (the spline road hologram's Plus/Minus lane
        // input), and the engine's ProceduralMeshComponent plugin (the swept spline road deck), never on MKI Bridge.
        // AbstractInstance went with the modular road holograms (#180); the spline road resolves lightweight foundation hits
        // through the engine's FActorInstanceHandle.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "EnhancedInput", "InputCore", "FactoryGame", "SML", "ProceduralMeshComponent" });
    }
}

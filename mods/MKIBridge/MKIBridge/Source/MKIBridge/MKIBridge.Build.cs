using UnrealBuildTool;

public class MKIBridge : ModuleRules
{
    public MKIBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "FactoryGame", "SML", "AbstractInstance", "Foliage",
            "Json", "JsonUtilities", "HTTP", "HTTPServer", "Sockets", "Slate", "SlateCore", "Projects"
        });
    }
}

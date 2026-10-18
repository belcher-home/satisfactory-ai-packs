import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, relative, resolve } from "node:path";
import test from "node:test";
import { parseRoadKitDocument, roadJunctionEntries, roadKit, roadSupportEntries, roadPackAssetPackages, roadPackEditorPlan, splineRoadPatterns, splineRoadVariants } from "../src/road-kit.js";
import { encodePng, loadRoadKitIcons, PATTERN_ICON_LINE, renderRoadKitIcons, summarizeIcon } from "../src/road-pack-icons.js";
import {
  assertRoadNetworkAvailable, BINARY_FRESHNESS_SLACK_MS, containerEntries, cookedRegistryProblems, shippingBuildOutputs, dependencyContractProblems, descriptorModules, packagedArtifactProblems, pluginDependency, readPluginDescriptor, roadNetworkAvailability, satisfiesRange, REGISTRATION_CHAIN_CLASSES, ROAD_NETWORK_FEATURE, ROAD_PACK_PLUGIN,
} from "../src/road-pack.js";
import { bridgePluginDirectory, nativeBridgeSource, repositoryRoot, roadPackDirectory, roadPackPluginDirectory } from "../src/workspace-paths.js";

const bridgeDescriptorPath = resolve(bridgePluginDirectory, "MKIBridge.uplugin");
const roadPackDescriptorPath = resolve(roadPackPluginDirectory, "MKIRoads.uplugin");

/** A `/v1/capabilities` body as the bridge reports it, with MKI Roads overrides. */
function capabilities(companion: Record<string, unknown> = {}, feature: Record<string, unknown> = {}) {
  const version = roadKit().package.version;
  return {
    apiVersion: "v1",
    companions: [{ name: "MKIRoads", declared: true, required: true, requiredVersion: `^${version}`, present: true, version, compatible: true, mountPoint: "/MKIRoads/", features: ["road_network"], ...companion }],
    features: { road_network: { available: true, requires: ["MKIRoads"], ...feature } },
  };
}

test("SemVer ranges follow the SML/node-semver subset the descriptors use", () => {
  const cases: [string, string, boolean][] = [
    ["0.1.0", "^0.1.0", true], ["0.1.7", "^0.1.0", true], ["0.2.0", "^0.1.0", false], ["0.0.9", "^0.1.0", false], ["1.0.0", "^0.1.0", false],
    ["1.4.2", "^1.2.3", true], ["1.2.2", "^1.2.3", false], ["2.0.0", "^1.2.3", false], ["0.0.3", "^0.0.3", true], ["0.0.4", "^0.0.3", false],
    ["1.2.9", "~1.2.3", true], ["1.3.0", "~1.2.3", false], ["0.1.0", "0.1.0", true], ["0.1.1", "=0.1.0", false],
    ["0.5.0", ">=0.1.0 <1.0.0", true], ["1.0.0", ">=0.1.0 <1.0.0", false], ["2.1.0", "^1.0.0 || ^2.0.0", true], ["3.0.0", "^1.0.0 || ^2.0.0", false],
    ["not-a-version", "^0.1.0", false], ["3.12.1", "^3.12.0", true],
  ];
  for (const [version, range, expected] of cases) assert.equal(satisfiesRange(version, range), expected, `${version} ${range}`);
  assert.throws(() => satisfiesRange("1.0.0", "^banana"), /Unsupported version comparator/);
});

test("MKI Roads is an SML plugin with one runtime module and does not depend on the bridge", () => {
  const descriptor = readPluginDescriptor(roadPackDescriptorPath), kit = roadKit();
  assert.equal(descriptor.SemVersion, kit.package.version, "the manifest describes the plugin version it ships with");
  assert.equal(descriptor.VersionName, descriptor.SemVersion);
  assert.equal(descriptor.CanContainContent, true);
  // One runtime module (package.nativeModule): the spline road classes and the root game-world module.
  assert.deepEqual(descriptor.Modules, [{ Name: kit.package.nativeModule.name, Type: "Runtime", LoadingPhase: "Default" }]);
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const buildRules = readFileSync(resolve(source, "MKIRoads.Build.cs"), "utf8");
  assert.match(buildRules, /PublicDependencyModuleNames\.AddRange\(new\[\] \{ "Core", "CoreUObject", "Engine", "EnhancedInput", "InputCore", "FactoryGame", "SML", "ProceduralMeshComponent" \}\);/, "the module depends only on the engine, the game, SML, Unreal input modules, and the engine's ProceduralMeshComponent plugin");
  const header = readFileSync(resolve(source, "Public", "MKIRoadsGameWorldModule.h"), "utf8"), body = readFileSync(resolve(source, "Private", "MKIRoadsGameWorldModule.cpp"), "utf8");
  assert.equal(`/Script/${kit.package.nativeModule.name}.${/class MKIROADS_API U(\w+) : public UGameWorldModule/.exec(header)?.[1]}`, kit.package.nativeModule.rootModuleParentClass);
  // #159: roads are asphalt only, so the root module joins no material descriptors and sets no menu material; the
  // five-material version is recorded in the git history (last at a77661e).
  for (const retired of [/AddType/, /mValidBuildables/, /RemoveAvailableRecipes/, /MaterialGroups/, /SetDefaultMatDescForBuildableCategory/]) {
    assert.doesNotMatch(header, retired);
    assert.doesNotMatch(body, retired);
  }
  assert.match(header, /git history \(last at a77661e\)/);
  for (const text of [buildRules, header, body]) assert.doesNotMatch(text, /MKIBridge/, "MKI Roads source never references the bridge");
  // EnhancedInput is the engine plugin of the spline road hologram's lane input (#126) and ProceduralMeshComponent sweeps
  // its deck; both are base plugins the game ships, so SML stays the only mod dependency. AbstractInstance went with the
  // modular road holograms (#180).
  assert.deepEqual(descriptor.Plugins?.map((item) => [item.Name, item.BasePlugin ?? false]), [["SML", false], ["EnhancedInput", true], ["ProceduralMeshComponent", true]]);
  assert.equal(pluginDependency(descriptor, "MKIBridge"), undefined);
  // No uncontrolled third-party gameplay mod may be required by either plugin.
  const bridge = readPluginDescriptor(bridgeDescriptorPath), allowed = new Set(["SML", "MKIRoads", "AbstractInstance"]);
  for (const [name, item] of [["MKIRoads", descriptor], ["MKIBridge", bridge]] as const) for (const dependency of item.Plugins ?? []) {
    assert.ok(allowed.has(dependency.Name) || dependency.BasePlugin === true, `${name} depends on ${dependency.Name}`);
  }
});

test("the modular road pieces' native classes and their foundation snap hook are gone (#180)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const files = [...readdirSync(resolve(source, "Public")).map((name) => `Public/${name}`), ...readdirSync(resolve(source, "Private")).map((name) => `Private/${name}`)].sort();
  assert.deepEqual(files, [
    "Private/MKIRoadAutoPaths.cpp", "Private/MKIRoadBuildSnap.cpp", "Private/MKIRoadCustomizerIcons.cpp", "Private/MKIRoadJunction.cpp", "Private/MKIRoadJunctionHologram.cpp", "Private/MKIRoadLanePath.cpp", "Private/MKIRoadLegacyRedirects.cpp", "Private/MKIRoadLegendGlyphs.generated.h",
    "Private/MKIRoadMarkingPattern.cpp", "Private/MKIRoadPaintGuard.cpp", "Private/MKIRoadSplineRoad.cpp", "Private/MKIRoadSplineRoadHologram.cpp", "Private/MKIRoadSupports.cpp", "Private/MKIRoadSupportsHologram.cpp", "Private/MKIRoadTile.cpp", "Private/MKIRoadWorldSubsystem.cpp", "Private/MKIRoadsGameWorldModule.cpp", "Private/MKIRoadsModule.cpp",
    "Public/MKIRoadAutoPaths.h", "Public/MKIRoadBuildSnap.h", "Public/MKIRoadCustomizerIcons.h", "Public/MKIRoadJunction.h", "Public/MKIRoadJunctionHologram.h", "Public/MKIRoadLanePath.h", "Public/MKIRoadLegacyRedirects.h",
    "Public/MKIRoadMarkingPattern.h", "Public/MKIRoadPaintGuard.h", "Public/MKIRoadSplineRoad.h", "Public/MKIRoadSplineRoadDisqualifiers.h", "Public/MKIRoadSplineRoadHologram.h", "Public/MKIRoadSupports.h", "Public/MKIRoadSupportsHologram.h", "Public/MKIRoadTile.h", "Public/MKIRoadWorldSubsystem.h", "Public/MKIRoadsGameWorldModule.h",
  ]);
  const texts = files.map((file) => readFileSync(resolve(source, file), "utf8"));
  for (const retired of [/MKIRoadsZoopHologram/, /MKIRoadsAdjustableHologram/, /MKIRoadsSeamPointType/, /SnapFoundationToRoadSeam/, /AbstractInstanceManager\.h/]) {
    assert.ok(texts.every((text) => !retired.test(text)), `nothing references ${retired}`);
  }
  // The modular seam hook is gone. Since #188 the module hooks the vanilla foundation and walkway snaps again, but only to
  // offer a spline road hit to FMKIRoadBuildSnap (game builds only); every other hit keeps the vanilla snap.
  const module = readFileSync(resolve(source, "Private", "MKIRoadsModule.cpp"), "utf8");
  assert.match(module, /IMPLEMENT_MODULE\(FMKIRoadsModule, MKIRoads\);/);
  assert.equal((module.match(/SUBSCRIBE_UOBJECT_METHOD\(/g) ?? []).length, 7);
  // #208: the customizer aimed at a road with a vanilla material or pattern shows it invalid, and a click is cancelled.
  assert.ok(module.includes("SUBSCRIBE_UOBJECT_METHOD_AFTER(UFGBuildGunStatePaint, TickState_Implementation, [](UFGBuildGunStatePaint* Self, float DeltaTime)\n\t\t{\n\t\t\tif (FMKIRoadPaintGuard::Refuses(Self)) FMKIRoadPaintGuard::MarkInvalid(Self);\n\t\t});"));
  assert.ok(module.includes("SUBSCRIBE_UOBJECT_METHOD(UFGBuildGunStatePaint, PrimaryFire_Implementation, [](auto& Scope, UFGBuildGunStatePaint* Self)\n\t\t{\n\t\t\tif (FMKIRoadPaintGuard::Refuses(Self)) Scope.Cancel();\n\t\t});"));
  // #220: the customizer tile maps categories with a fixed map of the four vanilla ones (read from BPHUDHelpers), so a
  // category derived from CC_Pattern reads as CC_Pattern, only for the tile (BPW_CustomiserIcon); the tabs see MKI Patterns.
  assert.ok(module.includes("FMKIRoadCustomizerIcons::Install();"));
  assert.ok(module.indexOf("#if !WITH_EDITOR\n\t\t// The customizer tile") > 0, "game builds only, at startup");
  const icons = readFileSync(resolve(source, "Private", "MKIRoadCustomizerIcons.cpp"), "utf8");
  assert.match(icons, /GOriginalGetCategory\(Context, Stack, RESULT_PARAM\);/);
  assert.match(icons, /if \(!RESULT_PARAM \|\| !FMKIRoadCustomizerIcons::IsCustomizerTile\(Stack\.Object\)\) return;/);
  assert.match(icons, /return ScriptObject && ScriptObject->GetClass\(\)->GetName\(\)\.StartsWith\(TEXT\("BPW_CustomiserIcon"\)\);/);
  assert.match(icons, /return Pattern && Category && Category != Pattern && Category->IsChildOf\(Pattern\) \? Pattern : Category;/);
  const guard = readFileSync(resolve(source, "Private", "MKIRoadPaintGuard.cpp"), "utf8");
  // Refused: any vanilla material, a vanilla stencil pattern; not swatches, road marking patterns, or removers.
  assert.match(guard, /if \(Customization->IsChildOf\(UFGFactoryCustomizationDescriptor_Material::StaticClass\(\)\)\) return true;/);
  assert.match(guard, /AMKIRoadSplineRoad::IsVanillaPattern\(/);
  // #225 (the maintainer): vanilla colours and paint finishes (swatches) are refused too, and the refusal shows as a material's
  // does. Clearing the flags after the tick (and reporting the hit status) still showed a valid outline, so before the tick the
  // aimed road's own coloring and patterning flags allow only what it takes.
  assert.match(guard, /if \(Customization->IsChildOf\(UFGFactoryCustomizationDescriptor_Swatch::StaticClass\(\)\)\) return true;/);
  assert.match(guard, /Paint->\*Get\(FSwatchFlag\(\)\) = false;/);
  assert.match(guard, /\(Paint->\*Get\(FDisqualifiers\(\)\)\)\.AddUnique\(UFGCDInvalidCustomizationTarget::StaticClass\(\)\);/);
  assert.match(guard, /Allow\(Road, !bSwatch, !bSwatch && !bVanillaPattern\);/);
  assert.match(guard, /if \(Last\.IsValid\(\) && Last\.Get\(\) != Road\) Allow\(Last\.Get\(\), true, true\);/);
  // #227: an intersection takes the same customizations as a road.
  assert.match(guard, /if \(!Cast<AMKIRoadSplineRoad>\(HitActor\) && !AMKIRoadJunction::FromActor\(HitActor\)\) return false;/);
  assert.ok(module.includes("SUBSCRIBE_UOBJECT_METHOD(UFGBuildGunStatePaint, TickState_Implementation, [](auto& Scope, UFGBuildGunStatePaint* Self, float DeltaTime)\n\t\t{\n\t\t\tFMKIRoadPaintGuard::PrepareAim(Self);\n\t\t});"));
  // Negative: the road marking patterns (not vanilla) and anything aimed off a road stay valid.
  assert.doesNotMatch(guard, /UFGFactoryCustomizationDescriptor_Pattern::StaticClass\(\)\)\) return true;/);
  // The station's bay (its pivot) on the edge lane's center, its building (its -Y, read from Build_TruckStation) outside.
  const dock = /bool FMKIRoadBuildSnap::SnapDockingStation\([\s\S]*?\n\}/.exec(readFileSync(resolve(source, "Private", "MKIRoadBuildSnap.cpp"), "utf8"))?.[0] ?? "";
  assert.match(dock, /const FVector Location = Center \+ Right \* \(Side \* \(HalfDeck - Lane \* 0\.5f\)\);/);
  assert.match(dock, /const FVector Heading = Side > 0\.f \? -Forward : Forward;/);
  // #201: a truck station aimed at a road goes on its edge; aimed at a vehicle path it keeps the vanilla path snap.
  assert.ok(module.includes("SUBSCRIBE_UOBJECT_METHOD(AFGDockingStationHologram, TrySnapToActor, [](auto& Scope, AFGDockingStationHologram* Self, const FHitResult& HitResult)\n\t\t{\n\t\t\tif (FMKIRoadBuildSnap::SnapDockingStationHologram(Self, HitResult)) Scope.Override(true);\n\t\t});"));
  // #192: after the vanilla vehicle path hologram places itself, a road lane takes over its plan.
  assert.ok(module.includes("SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGVehiclePathSegmentHologram, SetHologramLocationAndRotation, [](AFGVehiclePathSegmentHologram* Self, const FHitResult& HitResult)\n\t\t{\n\t\t\tFMKIRoadLanePath::AfterSetHologramLocation(Self, HitResult);\n\t\t});"));
  for (const hologram of ["AFGFoundationHologram", "AFGWalkwayHologram"]) {
    assert.ok(module.includes(`SUBSCRIBE_UOBJECT_METHOD(${hologram}, TrySnapToActor, [](auto& Scope, ${hologram}* Self, const FHitResult& HitResult)\n\t\t{\n\t\t\tif (FMKIRoadBuildSnap::SnapHologram(Self, HitResult)) Scope.Override(true);\n\t\t});`), hologram);
  }
  assert.ok(module.indexOf("#if !WITH_EDITOR") < module.indexOf("SUBSCRIBE_UOBJECT_METHOD"), "game builds only");
  // The spline road keeps snapping to its own ends and to foundation tops; a module seam is no longer a snap target.
  const hologram = readFileSync(resolve(source, "Private", "MKIRoadSplineRoadHologram.cpp"), "utf8");
  assert.match(hologram, /Cast<AMKIRoadSplineRoad>\(HitActor\)/);
  assert.match(hologram, /Cast<AFGBuildableFoundation>\(HitActor\)/);
  assert.doesNotMatch(hologram, /GetAttachmentPoints/);
});

/** UFUNCTION parameters named after a UPROPERTY of the same UCLASS/USTRUCT: UnrealHeaderTool rejects them as shadowing. */
function uhtShadowedParameters(header: string): string[] {
  const shadowed: string[] = [];
  for (const scope of header.split(/\b(?:UCLASS|USTRUCT)\(/)) {
    const properties = new Set([...scope.matchAll(/UPROPERTY\([^)]*\)\s*[^;{]*?\b(\w+)\s*(?:=[^;]*)?;/g)].map((match) => match[1]));
    for (const match of scope.matchAll(/UFUNCTION\([^)]*\)\s*[^;{(]*\(([^)]*)\)/g)) {
      for (const parameter of match[1].split(",")) {
        const name = /(\w+)\s*(?:=.*)?$/.exec(parameter.trim())?.[1];
        if (name && properties.has(name)) shadowed.push(name);
      }
    }
  }
  return shadowed;
}

test("no MKI Roads UFUNCTION parameter shadows a UPROPERTY, which UnrealHeaderTool rejects", () => {
  const publicDir = resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Public");
  for (const file of readdirSync(publicDir).filter((name) => name.endsWith(".h"))) {
    assert.deepEqual(uhtShadowedParameters(readFileSync(resolve(publicDir, file), "utf8")), [], file);
  }
  const shadowing = "UPROPERTY(EditDefaultsOnly)\n\tTArray<int32> Choices;\n\tUFUNCTION(BlueprintPure)\n\tstatic int32 Pick(const TArray<int32>& Choices, int32 Index);\n";
  assert.deepEqual(uhtShadowedParameters(shadowing), ["Choices"], "the check flags a shadowing parameter");
  assert.deepEqual(uhtShadowedParameters(shadowing.replace("& Choices,", "& Candidates,")), [], "a distinct name passes");
  assert.deepEqual(uhtShadowedParameters(`USTRUCT()\nstruct F { ${shadowing.split("\tUFUNCTION")[0]} };\nUCLASS()\nclass A { \tUFUNCTION${shadowing.split("\tUFUNCTION")[1]} };`), [], "another type's property is not shadowed");
});

test("the bridge declares a required, versioned MKI Roads dependency that the shipped version satisfies", () => {
  const bridge = readPluginDescriptor(bridgeDescriptorPath), roadPack = readPluginDescriptor(roadPackDescriptorPath);
  const dependency = pluginDependency(bridge, ROAD_PACK_PLUGIN);
  assert.ok(dependency, "bridge descriptor lists MKIRoads");
  assert.equal(dependency.Enabled, true);
  assert.equal(dependency.Optional, undefined, "MKI Roads is a hard dependency: the engine and mod managers must not load the bridge without it");
  assert.deepEqual(dependencyContractProblems(bridge, roadPack), []);
  assert.match(dependency.SemVersion ?? "", /^\^\d+\.\d+\.\d+$/);
  assert.ok(satisfiesRange(roadPack.SemVersion!, dependency.SemVersion!), `${roadPack.SemVersion} satisfies ${dependency.SemVersion}`);
  // The native bridge gates the road_network feature on this companion and reads the range from its own descriptor.
  const source = nativeBridgeSource();
  assert.match(source, /\{ TEXT\("MKIRoads"\), \{ TEXT\("road_network"\) \} \}/);
  assert.match(source, /companion_unavailable/);
  assert.match(source, /SetBoolField\(TEXT\("required"\), Status\.bRequired\)/);
  assert.doesNotMatch(source, /SetBoolField\(TEXT\("optional"\)/);
});

test("the dependency contract check rejects optional, disabled, unversioned, unsatisfied, and reversed dependencies", () => {
  const bridge = readPluginDescriptor(bridgeDescriptorPath), roadPack = readPluginDescriptor(roadPackDescriptorPath);
  const withDependency = (change: Record<string, unknown>) => ({ ...bridge, Plugins: bridge.Plugins!.map((item) => item.Name === ROAD_PACK_PLUGIN ? { ...item, ...change } : item) });
  const cases: [string, ReturnType<typeof readPluginDescriptor>, ReturnType<typeof readPluginDescriptor>, RegExp][] = [
    ["optional", withDependency({ Optional: true }), roadPack, /declares MKIRoads optional; it must be required/],
    ["disabled", withDependency({ Enabled: false }), roadPack, /disables MKIRoads/],
    ["unversioned", withDependency({ SemVersion: undefined }), roadPack, /no SemVersion range/],
    ["unsatisfied", withDependency({ SemVersion: "^0.9.0" }), roadPack, /does not satisfy the MKIBridge requirement \^0\.9\.0/],
    ["missing", { ...bridge, Plugins: bridge.Plugins!.filter((item) => item.Name !== ROAD_PACK_PLUGIN) }, roadPack, /does not declare MKIRoads/],
    ["reversed", bridge, { ...roadPack, Plugins: [...roadPack.Plugins!, { Name: "MKIBridge", Enabled: true, SemVersion: "^0.2.0" }] }, /MKIRoads must not depend on MKIBridge/],
  ];
  for (const [name, bridgeDescriptor, roadPackDescriptor, pattern] of cases) assert.match(dependencyContractProblems(bridgeDescriptor, roadPackDescriptor).join("; "), pattern, name);
});

test("road network availability is decided fail-closed from the bridge capabilities", () => {
  const version = roadKit().package.version;
  const ok = roadNetworkAvailability(capabilities(), version);
  assert.deepEqual(ok, { available: true, roadPackVersion: version, requiredVersion: `^${version}`, mountPoint: "/MKIRoads/" });
  const refused: [string, unknown, RegExp][] = [
    ["older bridge", { apiVersion: "v1", read: [], write: [] }, /does not report companion mods/],
    ["undeclared", capabilities({ declared: false }), /does not declare MKIRoads/],
    ["declared optional", capabilities({ required: false }), /declares MKIRoads optional, not as its required dependency/],
    ["no required flag", capabilities({ required: undefined }), /does not report companion mods/],
    ["absent", capabilities({ present: false, version: undefined, compatible: false }), /MKIRoads is not installed/],
    ["incompatible flag", capabilities({ version: "0.3.3", compatible: false }), /0.3.3 does not satisfy the bridge requirement/],
    // 0.1.0 had the deck-top origin: the bridge refuses it, so no 0.2.0+ transform is ever sent to a 0.1.0 buildable.
    ["deck-top-origin release", capabilities({ version: "0.1.0", compatible: false }), /0.1.0 does not satisfy the bridge requirement/],
    // 0.2.0 shipped only the two-lane pair, with the turn under its old name: the 0.3.0 family's assets are absent.
    ["two-lane-only release", capabilities({ version: "0.2.0", compatible: false }), /0.2.0 does not satisfy the bridge requirement/],
    ["bridge flag contradicts range", capabilities({ version: "1.0.0", compatible: true }), /1.0.0 does not satisfy the bridge requirement/],
    ["feature down", capabilities({}, { available: false, reason: "MKIRoads: not installed or not enabled" }), /reports road_network unavailable: MKIRoads: not installed/],
    ["feature missing", { ...capabilities(), features: {} }, /reports road_network unavailable/],
  ];
  for (const [name, body, pattern] of refused) {
    const result = roadNetworkAvailability(body, version);
    assert.equal(result.available, false, name);
    assert.match(result.available ? "" : result.reason, pattern, name);
  }
  // An installed release older than the manifest lacks its assets even when the bridge accepts it.
  const older = roadNetworkAvailability(capabilities({ requiredVersion: "^0.4.0", version: "0.4.0" }), "0.4.1");
  assert.equal(older.available, false);
  assert.match(older.available ? "" : older.reason, /does not provide the assets of manifest version 0.4.1/);
  assert.equal(ROAD_NETWORK_FEATURE, "road_network");
  // The guard a road builder calls before emitting MKI Roads placements: no check, or a refusal, never passes.
  assert.throws(() => assertRoadNetworkAvailable(undefined), /needs a road_network availability check/);
  assert.throws(() => assertRoadNetworkAvailable({ available: false, reason: "x" }), /road placement is unavailable: x/);
  assert.doesNotThrow(() => assertRoadNetworkAvailable(ok));
});

test("the repository MKI Roads plugin holds no generated content", () => {
  // Content is produced in the SDK checkout by `npm run road-pack -- import` and never committed.
  assert.ok(!existsSync(resolve(roadPackPluginDirectory, "Content")));
  assert.ok(existsSync(resolve(roadPackPluginDirectory, "Config", "Alpakit.ini")));
  assert.ok(existsSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "MKIRoads.Build.cs")));
});

// Lines as UnrealPak prints them for a packaged MKI Roads (pak entries are relative to the pak mount point).
const LISTING = [
  'LogPakFile: Display: Listing MKIRoadsFactoryGame-Windows.pak with mount point "../../../FactoryGame/Mods/MKIRoads/"',
  'LogPakFile: Display: "AssetRegistry.bin" offset: 0, size: 2950 bytes, sha1: 578FE297D367605AA7EA3625DF0402C1768A4D99, compression: Oodle.',
  'LogPakFile: Display: "Config/Alpakit.ini" offset: 3023, size: 34 bytes, sha1: CB61B41D35777F761F38D5758A9C38F01C7121E3, compression: None.',
  "LogIoStore: Display: Mount point ../../../FactoryGame/Mods/MKIRoads/Content/",
  'LogIoStore: Display: "../../../FactoryGame/Mods/MKIRoads/Content/Buildables/Build_MKIR_Road_L2_Straight_8m.uasset" offset: 0, size: 3059 bytes, hash: cf77, compression: Oodle.',
  'LogIoStore: Display: "../../../FactoryGame/Mods/MKIRoads/Content/Meshes/SM_MKIR_Road_L2_Turn45R_R800.ubulk" offset: 22336, size: 17556 bytes, hash: b5bd, compression: Oodle.',
  'LogIoStore: Display: "<ContainerHeader>" offset: 39936, size: 464 bytes, hash: b68a, compression: None.',
];
// A native plugin ships one manifest and one DLL per module for each store target (Steam and EGS).
const STORES = ["Steam", "EGS"] as const;
const ROAD_PACK_FILES = [
  "MKIRoads.uplugin", ...STORES.flatMap((store) => [`Binaries/Win64/FactoryGame${store}-MKIRoads-Win64-Shipping.dll`, `Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`]),
  ...["pak", "ucas", "utoc"].map((extension) => `Content/Paks/Windows/MKIRoadsFactoryGame-Windows.${extension}`),
];
const ROAD_PACK_MODULES: Record<string, string> = Object.fromEntries(STORES.map((store) => [`Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`, JSON.stringify({ BuildId: "SML", Modules: { MKIRoads: `FactoryGame${store}-MKIRoads-Win64-Shipping.dll` } })]));

test("container listings resolve pak entries against their mount point", () => {
  assert.deepEqual(containerEntries(LISTING), [
    "../../../FactoryGame/Mods/MKIRoads/AssetRegistry.bin",
    "../../../FactoryGame/Mods/MKIRoads/Config/Alpakit.ini",
    "../../../FactoryGame/Mods/MKIRoads/Content/Buildables/Build_MKIR_Road_L2_Straight_8m.uasset",
    "../../../FactoryGame/Mods/MKIRoads/Content/Meshes/SM_MKIR_Road_L2_Turn45R_R800.ubulk",
  ]);
});

test("packaged artifacts must hold only their own plugin content and distributable binaries", () => {
  // The manifests must name the modules the real descriptors declare.
  assert.deepEqual(descriptorModules(readPluginDescriptor(roadPackDescriptorPath)), ["MKIRoads"]);
  assert.deepEqual(descriptorModules(readPluginDescriptor(bridgeDescriptorPath)), ["MKIBridge"]);
  const base = { plugin: "MKIRoads", version: "0.3.0", files: ROAD_PACK_FILES, containerListing: LISTING, modulesManifests: ROAD_PACK_MODULES, expectsBinaries: true, nativeModules: ["MKIRoads"], expectedAssets: ["Build_MKIR_Road_L2_Straight_8m"] };
  assert.deepEqual(packagedArtifactProblems(base), []);
  const bridgeModules = (buildId: string) => Object.fromEntries(STORES.map((store) => [`Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`, `{\r\n\t"BuildId": "${buildId}",\r\n\t"Modules": \r\n\t{\r\n\t\t"MKIBridge": "FactoryGame${store}-MKIBridge-Win64-Shipping.dll"\r\n\t}\r\n}`]));
  const bridgeListing = [
    'LogPakFile: Display: Listing MKIBridgeFactoryGame-Windows.pak with mount point "../../../FactoryGame/Mods/GameFeatures/MKIBridge/"',
    'LogIoStore: Display: Mount point ../../../FactoryGame/Mods/GameFeatures/MKIBridge/Content/',
    'LogIoStore: Display: "../../../FactoryGame/Mods/GameFeatures/MKIBridge/Content/MKIBridge.uasset" offset: 0, size: 1 bytes, hash: 0, compression: None.',
  ];
  const bridgeFiles = ["MKIBridge.uplugin", ...STORES.flatMap((store) => [`Binaries/Win64/FactoryGame${store}-MKIBridge-Win64-Shipping.dll`, `Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`]), ...["pak", "ucas", "utoc"].map((extension) => `Content/Paks/Windows/MKIBridgeFactoryGame-Windows.${extension}`)];
  const bridge = { plugin: "MKIBridge", version: "0.4.0", files: bridgeFiles, containerListing: bridgeListing, modulesManifests: bridgeModules("SML"), expectsBinaries: true, nativeModules: ["MKIBridge"] };
  assert.deepEqual(packagedArtifactProblems(bridge), [], "game-feature plugins mount under Mods/GameFeatures");
  const cases: [string, Parameters<typeof packagedArtifactProblems>[0], RegExp][] = [
    ["missing descriptor", { ...base, files: ROAD_PACK_FILES.slice(1) }, /MKIRoads.uplugin is missing/],
    ["missing container", { ...base, files: ROAD_PACK_FILES.slice(0, -1) }, /MKIRoadsFactoryGame-Windows.utoc is missing/],
    ["missing MKI Roads module", { ...base, files: ROAD_PACK_FILES.filter((file) => !file.endsWith(".dll")) }, /names missing FactoryGameSteam-MKIRoads-Win64-Shipping.dll/],
    ["base-game copy", { ...base, containerListing: [...LISTING, 'LogIoStore: Display: "../../../FactoryGame/Content/FactoryGame/Buildable/SM_Foundation.uasset" offset: 0, size: 1 bytes, hash: 0, compression: None.'] }, /base-game content is packaged/],
    ["foreign mount", { ...base, containerListing: [...LISTING, 'LogIoStore: Display: "../../../FactoryGame/Mods/X3Roads/Content/SM_Road.uasset" offset: 0, size: 1 bytes, hash: 0, compression: None.'] }, /entry outside the MKIRoads mount/],
    ["empty listing", { ...base, containerListing: [] }, /listing is empty/],
    // MKI Roads is an ordinary mod: content mounted under Mods/GameFeatures/MKIRoads would not belong to the installed plugin.
    ['game-feature mount for MKI Roads', { ...base, containerListing: LISTING.map((line) => line.replaceAll('FactoryGame/Mods/MKIRoads/', 'FactoryGame/Mods/GameFeatures/MKIRoads/')) }, /container mount point \.\.\/\.\.\/\.\.\/FactoryGame\/Mods\/GameFeatures\/MKIRoads\/ is not the MKIRoads install directory \.\.\/\.\.\/\.\.\/FactoryGame\/Mods\/MKIRoads\//],
    ['ordinary mount for the bridge', { ...bridge, containerListing: bridgeListing.map((line) => line.replaceAll('Mods/GameFeatures/', 'Mods/')) }, /entry outside the MKIBridge mount/],
    ['split mount', { ...base, containerListing: [...LISTING, 'LogIoStore: Display: Mount point ../../../FactoryGame/Mods/MKIRoads/Other/'] }, /do not share one install directory/],
    ["missing asset", { ...base, expectedAssets: ["Recipe_MKIR_Road_L2_Straight_8m"] }, /Recipe_MKIR_Road_L2_Straight_8m.uasset is not packaged/],
    ["content plugin with binaries", { ...base, expectsBinaries: false, modulesManifests: {} }, /content-only plugin ships binaries/],
    ["raw UBT BuildId", { ...bridge, modulesManifests: bridgeModules("43139311") }, /BuildId is "43139311", not "SML"/],
    ["missing DLL", { ...bridge, files: bridgeFiles.filter((file) => !file.endsWith(".dll")) }, /names missing FactoryGameSteam-MKIBridge-Win64-Shipping.dll/],
    ["no manifest", { ...bridge, modulesManifests: {} }, /no .modules manifest/],
    // Stale SDK build outputs Alpakit would archive alongside the current module.
    ["stale renamed module", { ...base, files: [...ROAD_PACK_FILES, "Binaries/Win64/FactoryGameSteam-MKIRoadsOld-Win64-Shipping.dll"] }, /binaries no packaged .modules manifest names \(stale build outputs\?\): Binaries\/Win64\/FactoryGameSteam-MKIRoadsOld-Win64-Shipping.dll/],
    ["stale editor DLL", { ...bridge, files: [...bridgeFiles, "Binaries/Win64/UnrealEditor-MKIBridge.dll"] }, /stale build outputs\?\): Binaries\/Win64\/UnrealEditor-MKIBridge.dll/],
    ["orphan symbols", { ...base, files: [...ROAD_PACK_FILES, "Binaries/Win64/FactoryGameEGS-MKIRoadsOld-Win64-Shipping.pdb"] }, /stale build outputs\?\): Binaries\/Win64\/FactoryGameEGS-MKIRoadsOld-Win64-Shipping.pdb/],
    ["DLL beside another manifest", { ...base, files: [...ROAD_PACK_FILES, "Binaries/Linux/FactoryGameSteam-MKIRoads-Win64-Shipping.dll"] }, /stale build outputs\?\): Binaries\/Linux\/FactoryGameSteam-MKIRoads-Win64-Shipping.dll/],
  ];
  for (const [name, artifact, pattern] of cases) assert.ok(packagedArtifactProblems(artifact).some((problem) => pattern.test(problem)), name);
  // Only the store-target manifests may authorize binaries, and only for the descriptor's modules and their store-target DLLs.
  const steam = "Binaries/Win64/FactoryGameSteam-Win64-Shipping.modules", oldDll = "FactoryGameSteam-MKIRoadsOld-Win64-Shipping.dll";
  const withManifest = (path: string, modules: Record<string, string>, extra: string[]) => ({
    ...base, files: [...ROAD_PACK_FILES, path, ...extra], modulesManifests: { ...ROAD_PACK_MODULES, [path]: JSON.stringify({ BuildId: "SML", Modules: modules }) },
  });
  for (const [name, path, dll] of [
    ["stale renamed store target", "Binaries/Win64/FactoryGameSteamOld-Win64-Shipping.modules", "Binaries/Win64/FactoryGameSteamOld-MKIRoads-Win64-Shipping.dll"],
    ["stale editor manifest", "Binaries/Win64/UnrealEditor.modules", "Binaries/Win64/UnrealEditor-MKIRoads.dll"],
    ["manifest in another directory", "Binaries/Linux/FactoryGameSteam-Win64-Shipping.modules", "Binaries/Linux/FactoryGameSteam-MKIRoads-Win64-Shipping.dll"],
  ] as const) {
    const problems = packagedArtifactProblems(withManifest(path, { MKIRoads: dll.slice(dll.lastIndexOf("/") + 1) }, [dll, dll.replace(/\.dll$/, ".pdb")])).join("\n");
    assert.ok(problems.includes(`${path} is not a store-target manifest (Binaries/Win64/FactoryGameSteam-Win64-Shipping.modules, Binaries/Win64/FactoryGameEGS-Win64-Shipping.modules)`), name);
    assert.ok(problems.includes(`binaries no packaged .modules manifest names (stale build outputs?): ${path}, ${dll}, ${dll.replace(/\.dll$/, ".pdb")}`), `${name}: ${problems}`);
  }
  const keyed = (modules: Record<string, string>, extra: string[] = []) => packagedArtifactProblems({ ...base, files: [...ROAD_PACK_FILES, ...extra], modulesManifests: { ...ROAD_PACK_MODULES, [steam]: JSON.stringify({ BuildId: "SML", Modules: modules }) } });
  assert.deepEqual(keyed({ MKIRoadsOld: oldDll }, [`Binaries/Win64/${oldDll}`]), [
    `${steam} names modules MKIRoadsOld, not the MKIRoads.uplugin modules MKIRoads`,
    `binaries no packaged .modules manifest names (stale build outputs?): Binaries/Win64/FactoryGameSteam-MKIRoads-Win64-Shipping.dll, Binaries/Win64/${oldDll}`,
  ], "a wrong module key authorizes nothing");
  assert.deepEqual(keyed({ MKIRoads: "FactoryGameSteam-MKIRoads-Win64-Shipping.dll", MKIRoadsOld: oldDll }, [`Binaries/Win64/${oldDll}`]), [
    `${steam} names modules MKIRoads, MKIRoadsOld, not the MKIRoads.uplugin modules MKIRoads`,
    `binaries no packaged .modules manifest names (stale build outputs?): Binaries/Win64/${oldDll}`,
  ], "an extra module key authorizes nothing");
  assert.deepEqual(keyed({ MKIRoads: oldDll }, [`Binaries/Win64/${oldDll}`]), [
    `${steam} maps MKIRoads to "${oldDll}", not FactoryGameSteam-MKIRoads-Win64-Shipping.dll`,
    `binaries no packaged .modules manifest names (stale build outputs?): Binaries/Win64/FactoryGameSteam-MKIRoads-Win64-Shipping.dll, Binaries/Win64/${oldDll}`,
  ], "a module mapped to another DLL authorizes nothing");
  assert.ok(packagedArtifactProblems({ ...base, nativeModules: [] }).includes("MKIRoads.uplugin declares no native modules for a plugin built with binaries"));
  assert.ok(packagedArtifactProblems({ ...base, nativeModules: undefined }).includes("MKIRoads.uplugin declares no native modules for a plugin built with binaries"));
  // Both store-target manifests are required: a package missing either store's manifest is rejected.
  for (const store of STORES) {
    const manifest = `Binaries/Win64/FactoryGame${store}-Win64-Shipping.modules`;
    const otherStore = Object.fromEntries(Object.entries(ROAD_PACK_MODULES).filter(([path]) => path !== manifest));
    assert.deepEqual(packagedArtifactProblems({ ...base, files: ROAD_PACK_FILES.filter((file) => file !== manifest), modulesManifests: otherStore }), [
      `${manifest} is missing; a native plugin ships one manifest per store target`,
      `binaries no packaged .modules manifest names (stale build outputs?): Binaries/Win64/FactoryGame${store}-MKIRoads-Win64-Shipping.dll`,
    ], `missing ${store} manifest`);
  }
  // The live layout: each store's module with its symbols.
  const withSymbols = { ...base, files: [...ROAD_PACK_FILES, ...STORES.map((store) => `Binaries/Win64/FactoryGame${store}-MKIRoads-Win64-Shipping.pdb`)] };
  assert.deepEqual(packagedArtifactProblems(withSymbols), [], "every manifest-named module and its .pdb is accepted");
});

test("a package never ships a store DLL older than its own run, and packaging relinks both store targets (#134)", () => {
  // #134: UBT judged a stale Steam DLL up to date and relinked only the EGS target; the archive shipped the older bridge
  // with every other check passing. Each store module DLL must now be newer than the package run's start.
  const base = { plugin: "MKIRoads", version: "0.6.16", files: ROAD_PACK_FILES, containerListing: LISTING, modulesManifests: ROAD_PACK_MODULES, expectsBinaries: true, nativeModules: ["MKIRoads"] };
  const started = Date.parse("2026-10-10T21:37:32Z"), dll = (store: string) => `Binaries/Win64/FactoryGame${store}-MKIRoads-Win64-Shipping.dll`;
  const at = (steam: number, egs: number) => ({ ...base, freshness: { packageStartedMs: started, binaryModifiedMs: { [dll("Steam")]: steam, [dll("EGS")]: egs } } });
  assert.deepEqual(packagedArtifactProblems(at(started + 60_000, started + 61_000)), [], "both DLLs linked during the run");
  // The zip stores 2 s timestamps: a DLL linked within the slack of the start still counts as fresh.
  assert.deepEqual(packagedArtifactProblems(at(started - BINARY_FRESHNESS_SLACK_MS + 1, started + 1)), []);
  // The observed failure: the Steam DLL from 13:17 local beside a fresh EGS DLL.
  const stale = packagedArtifactProblems(at(Date.parse("2026-10-10T20:17:00Z"), started + 61_000));
  assert.equal(stale.length, 1);
  assert.match(stale[0], /^Binaries\/Win64\/FactoryGameSteam-MKIRoads-Win64-Shipping\.dll was linked 2026-10-10T20:17:00\.000Z, before this package run started \(2026-10-10T21:37:32\.000Z\): a stale build output$/);
  assert.match(packagedArtifactProblems(at(started + 1, started - 3_600_000)).join("\n"), /FactoryGameEGS-MKIRoads-Win64-Shipping\.dll was linked .* a stale build output/, "either store");
  assert.match(packagedArtifactProblems({ ...base, freshness: { packageStartedMs: started, binaryModifiedMs: {} } }).join("\n"), /has no recorded modification time/);
  // Negative: content-only plugins have no binaries to check, and freshness never excuses a missing DLL.
  assert.deepEqual(packagedArtifactProblems({ ...base, plugin: "MKIRoads", files: ROAD_PACK_FILES.filter((file) => !file.endsWith(".dll")), freshness: { packageStartedMs: started, binaryModifiedMs: {} } }).filter((problem) => /stale build output|modification time/.test(problem)), []);
  assert.match(packagedArtifactProblems({ ...base, files: ROAD_PACK_FILES.filter((file) => !file.endsWith(".dll")), freshness: { packageStartedMs: started, binaryModifiedMs: {} } }).join("\n"), /names missing FactoryGameSteam-MKIRoads-Win64-Shipping.dll/);
  // Before UAT runs, packaging moves the plugin's shipping outputs (both stores' DLLs, symbols, and manifests) out of the SDK
  // Binaries directory, so both targets relink; editor binaries and unrelated files stay.
  assert.deepEqual(shippingBuildOutputs([
    "FactoryGameSteam-MKIRoads-Win64-Shipping.dll", "FactoryGameSteam-MKIRoads-Win64-Shipping.pdb", "FactoryGameEGS-MKIRoads-Win64-Shipping.dll",
    "FactoryGameSteam-Win64-Shipping.modules", "FactoryGameEGS-Win64-Shipping.modules", "UnrealEditor-MKIRoads.dll", "UnrealEditor.modules", "readme.txt",
  ]), ["FactoryGameEGS-MKIRoads-Win64-Shipping.dll", "FactoryGameEGS-Win64-Shipping.modules", "FactoryGameSteam-MKIRoads-Win64-Shipping.dll", "FactoryGameSteam-MKIRoads-Win64-Shipping.pdb", "FactoryGameSteam-Win64-Shipping.modules"]);
  // The package script applies both: it moves the outputs aside before UAT and passes the archived binaries' times.
  const script = readFileSync(resolve(repositoryRoot, "tools", "build-tools", "scripts", "road-pack.ts"), "utf8");
  const pkg = script.slice(script.indexOf("function packagePlugin("), script.indexOf("loadRoadKit();", script.indexOf("function packagePlugin(")));
  assert.ok(pkg.indexOf("renameSync(join(sdkBinaries, name), join(aside, name))") > 0 && pkg.indexOf("renameSync(join(sdkBinaries, name)") < pkg.indexOf("RunUAT.bat"), "outputs move aside before UAT runs");
  assert.match(pkg, /freshness: spec\.build \? \{ packageStartedMs, binaryModifiedMs \} : undefined,/);
});

test("the import step creates exactly the manifest's MKI Roads packages", () => {
  const packages = roadPackAssetPackages(roadKit());
  // Since 0.7.0 (#180) the kit is the spline road: per deck thickness a buildable, descriptor, recipe, and icon; the MKI
  // Patterns > Roads category, subcategory, and per pattern a descriptor, recipe, and icon; the two paints; the build menu;
  // the schematic and root module. No module mesh, decoration template, attachment-point type, or road hologram is built.
  assert.deepEqual(packages, [
    "/MKIRoads/BuildMenu/BC_MKIPacks", "/MKIRoads/BuildMenu/SC_MKIPacks_Attachments", "/MKIRoads/BuildMenu/SC_MKIPacks_Roads",
    "/MKIRoads/Customization/CC_MKIR_MKIPatterns", "/MKIRoads/Customization/CSC_MKIR_MKIPatterns_Roads",
    "/MKIRoads/Customization/PatternDesc_MKIR_NorthAmericanRoad", "/MKIRoads/Customization/Recipe_Pattern_MKIR_NorthAmericanRoad",
    ...["Pattern_NorthAmericanRoad", "SplineRoad1m"].map((stem) => `/MKIRoads/Icons/Icon_MKIR_${stem}`),
    "/MKIRoads/Materials/M_MKIR_RoadPaint_Orange", "/MKIRoads/Materials/M_MKIR_RoadPaint_White", "/MKIRoads/Materials/M_MKIR_RoadPaint_Yellow",
    "/MKIRoads/RootGameWorld_MKIRoads", "/MKIRoads/Schematics/Schematic_MKIR_RoadKit",
    // #256: the one Road, 1 m thick (the unlisted 2 m and 4 m roads are gone).
    ...["Build", "Desc", "Recipe"].map((kind) => `/MKIRoads/SplineRoad/${kind}_MKIR_SplineRoad1m`),
    // #227: the 3-way and 4-way intersections, each with its own icon.
    // #236: the turnaround, an entry of the same kind.
    ...["Intersection2Way", "Intersection3Way", "Intersection4Way", "Turnaround", "TruckDepotLarge", "TruckDepotLargeBoth"].flatMap((stem) => [...["Build", "Desc", "Recipe"].map((kind) => `/MKIRoads/SplineRoad/${kind}_MKIR_${stem}`), `/MKIRoads/Icons/Icon_MKIR_${stem}`]),
    // #194: the concrete road supports.
    ...["Build", "Desc", "Recipe"].map((asset) => `/MKIRoads/Supports/${asset}_MKIR_SupportsConcrete`),
  ].sort());
  assert.ok(packages.every((path) => !/\/(?:Attachment|Adjustable|Prototype|Holograms|Meshes|Buildables|Recipes)\//.test(path)), "no retired modular asset is built");
  assert.deepEqual(packages, [...new Set(packages)].sort());
});

test("the spline road entries and patterns are listed, unlocked, and costed from the manifest", () => {
  const kit = roadKit(), variants = splineRoadVariants(kit), patterns = splineRoadPatterns(kit);
  // One MKI Packs > Roads entry per deck thickness, thinnest first, each a Blueprint of the native road and its hologram.
  assert.deepEqual(variants.map((item) => [item.displayName, item.thicknessCm, item.listed]), [["Road", 100, true]]);
  assert.deepEqual(variants.map((item) => item.menuPriority), [88]);
  for (const item of variants) {
    assert.deepEqual(item.buildMenu, { category: "/MKIRoads/BuildMenu/BC_MKIPacks.BC_MKIPacks_C", subCategory: "/MKIRoads/BuildMenu/SC_MKIPacks_Roads.SC_MKIPacks_Roads_C" });
    assert.equal(item.parentClass, "/Script/MKIRoads.MKIRoadSplineRoad");
    assert.equal(item.hologramClass, "/Script/MKIRoads.MKIRoadSplineRoadHologram");
    assert.ok(item.description.endsWith(`Deck: ${item.thicknessCm / 100} m thick.`), item.description);
  }
  // The maintainer: the roads in Roads, everything attached to them in Attachments.
  assert.deepEqual(kit.package.buildMenu.subCategories.map((item) => item.displayName), ["Roads", "Attachments"]);
  for (const key of ["supports"] as const) assert.equal(kit.package[key].subCategory, "/MKIRoads/BuildMenu/SC_MKIPacks_Attachments.SC_MKIPacks_Attachments_C", key);
  // The editor plan carries exactly the entries, the patterns, and the paints; nothing of the modular pieces.
  const plan = roadPackEditorPlan(kit);
  assert.deepEqual(Object.keys(plan).sort(), ["assetPackages", "paints", "schema", "schemaVersion", "splineRoad", "supports"]);
  assert.equal(plan.schemaVersion, 16);
  assert.deepEqual(plan.splineRoad.entries, variants);
  assert.deepEqual(plan.splineRoad.patterns, patterns);
  assert.deepEqual(plan.paints, kit.package.markings.paints);
  assert.deepEqual(plan.assetPackages, roadPackAssetPackages(kit));
  // The recipe is one vanilla asphalt 8x4 foundation (5 Concrete); the hologram and the bridge multiply it by the deck area.
  assert.equal(kit.package.cost.basisRecipe, kit.package.asphalt.vanillaFoundationRecipe);
  assert.deepEqual(kit.package.cost.perFoundation, [{ item: "/Game/FactoryGame/Resource/Parts/Cement/Desc_Cement.Desc_Cement_C", amount: 5 }]);
});

test("the manifest refuses a malformed or inconsistent spline road kit", () => {
  const raw = JSON.parse(readFileSync(resolve(roadPackDirectory, "modules.json"), "utf8"));
  const changed = (change: (copy: typeof raw) => void) => { const copy = structuredClone(raw); change(copy); return copy; };
  assert.doesNotThrow(() => parseRoadKitDocument(changed(() => {})));
  const cases: [string, (copy: typeof raw) => void, RegExp][] = [
    // The retired modular schema is not accepted: no modules, sockets, or adjustable families come back by accident.
    ["modules", (copy) => { copy.modules = []; }, /Unrecognized key/],
    ["socket types", (copy) => { copy.socketTypes = []; }, /Unrecognized key/],
    ["adjustable families", (copy) => { copy.package.adjustable = {}; }, /Unrecognized key/],
    ["unlocksModules", (copy) => { copy.package.schematic.unlocksModules = true; }, /Unrecognized key/],
    ["old schema version", (copy) => { copy.schemaVersion = 15; }, /schemaVersion/],
    ["no variants", (copy) => { copy.package.splineRoad.variants = []; }, /variants/],
    ["unsupported thickness", (copy) => { copy.package.splineRoad.variants[0].thicknessCm = 300; }, /thicknessCm/],
    ["duplicate thickness", (copy) => { copy.package.splineRoad.variants.push({ ...copy.package.splineRoad.variants[0], stem: "SplineRoadAgain" }); }, /duplicate thicknessCm/],
    ["duplicate stem", (copy) => { copy.package.splineRoad.variants.push({ ...copy.package.splineRoad.variants[0], thicknessCm: 200 }); }, /duplicate stem.*already used/],
    ["foreign native class", (copy) => { copy.package.splineRoad.nativeClass = "/Script/MKIRoads.MKIRoadsZoopHologram"; }, /nativeClass/],
    ["undeclared subcategory", (copy) => { copy.package.splineRoad.subCategory = "/MKIRoads/BuildMenu/SC_MKIPacks_Turns.SC_MKIPacks_Turns_C"; }, /is not one of package\.buildMenu\.subCategories/],
    ["empty subcategory", (copy) => { copy.package.buildMenu.subCategories.push({ path: "/MKIRoads/BuildMenu/SC_MKIPacks_Ramps.SC_MKIPacks_Ramps_C", displayName: "Ramps", menuPriority: 2 }); }, /Ramps lists nothing/],
    ["subcategory without SC_", (copy) => { copy.package.buildMenu.subCategories[0].path = "/MKIRoads/BuildMenu/MKIPacks_Roads.MKIPacks_Roads_C"; copy.package.splineRoad.subCategory = "/MKIRoads/BuildMenu/MKIPacks_Roads.MKIPacks_Roads_C"; }, /must name an SC_ asset/],
    ["category reuses the schematic", (copy) => { copy.package.buildMenu.category.path = copy.package.schematic.path; }, /already used/],
    ["pattern in the vanilla ID range", (copy) => { copy.package.splineRoad.patterns.entries[0].id = 64; }, /id/],
    ["duplicate pattern", (copy) => { copy.package.splineRoad.patterns.entries.push({ ...copy.package.splineRoad.patterns.entries[0] }); }, /duplicate id/],
    ["edge line on the chamfer", (copy) => { copy.package.markings.edgeInsetCm = 8; }, /must clear the deck's 1\.5 cm top chamfer/],
    ["paint not a material", (copy) => { copy.package.markings.paints.white.material = "/MKIRoads/Materials/T_White.T_White"; }, /must name an M_ material asset/],
    ["same paint twice", (copy) => { copy.package.markings.paints.white.material = copy.package.markings.paints.yellow.material; }, /already used/],
    ["unregistered base-game asset", (copy) => { copy.package.splineRoad.surfaceMaterial = "/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/MI_Concrete.MI_Concrete"; }, /has no registered placeholder/],
    ["unreferenced placeholder", (copy) => { copy.placeholders.push({ ...copy.placeholders[0], path: "/Game/FactoryGame/Buildable/Building/Walkway/Build_WalkwayStraight.Build_WalkwayStraight" }); }, /not referenced by any package setting/],
    ["duplicate cost item", (copy) => { copy.package.cost.perFoundation.push({ ...copy.package.cost.perFoundation[0] }); }, /duplicate item/],
    ["material customization", (copy) => { copy.package.materialCustomization.recipeSource = "vanilla"; }, /recipeSource/],
  ];
  for (const [name, change, pattern] of cases) assert.throws(() => parseRoadKitDocument(changed(change)), pattern, name);
});

test("there are no MKI Roads barrier, railing, fence, walkway, or catwalk entries (#223, #224)", () => {
  // The maintainer: the vanilla barriers, railings, fences (#223), walkways, and catwalks (#224) snap to roads and zoop along them,
  // so the MKI Roads entries of #181, #189, and #191 are gone, and nothing builds or verifies them.
  const kit = roadKit();
  assert.ok(!("barriers" in kit.package) && !("barriers" in roadPackEditorPlan(kit)));
  assert.ok(kit.placeholders.every((item) => !/Walkway_Straight|Catwalk_Straight|WalkwayStraight|CatwalkStraight/.test(item.path)), "no walkway placeholder");
  assert.ok(roadPackAssetPackages(kit).every((path) => !path.startsWith("/MKIRoads/Barrier/")));
  const script = readFileSync(resolve(roadPackPluginDirectory, "..", "unreal", "road_pack_editor.py"), "utf8");
  assert.doesNotMatch(script, /build_barriers|verify_barriers|barrier_entries|vanilla_barrier/);
  assert.match(script, /def menu_entries\(\):\n[\s\S]*?return \[entry for entry in spline_road_entries\(\) if entry\["listed"\]\] \+ junction_entries\(\) \+ support_entries\(\)/);
  // #233 (the maintainer: remove Road (2 m) and (4 m)): a save keeps the recipes it unlocked, so a retired road has no recipe at all;
  // its buildable and descriptor stay for saved roads, which refund through the Road's recipe.
  assert.match(script, /if not entry\["listed"\]:\n            continue\n        recipe = create_blueprint/);
  const roadSource = readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Private", "MKIRoadSplineRoad.cpp"), "utf8");
  assert.match(roadSource, /if \(!GetBuiltWithRecipe\(\)\) SetBuiltWithRecipe\(LoadClass<UFGRecipe>\(nullptr, TEXT\("\/MKIRoads\/SplineRoad\/Recipe_MKIR_SplineRoad1m\.Recipe_MKIR_SplineRoad1m_C"\)\)\);/);
  // #234 (the maintainer): new roads and intersections get the North American Road pattern unless the build gun carries one.
  for (const file of ["MKIRoadSplineRoadHologram.cpp", "MKIRoadJunctionHologram.cpp"]) assert.match(readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Private", file), "utf8"), /if \(!(Road|Junction)->GetCustomizationData_Native\(\)\.PatternDesc\) (Road|Junction)->GetCustomizationData_Native\(\)\.PatternDesc = MKIRoadDefaultPattern\(\);/, file);
  // Negative: a manifest that still declares barriers is refused.
  const raw = JSON.parse(readFileSync(resolve(roadPackDirectory, "modules.json"), "utf8"));
  assert.throws(() => parseRoadKitDocument({ ...raw, package: { ...raw.package, barriers: { nativeClass: "/Script/MKIRoads.MKIRoadBarrier" } } }), /Unrecognized key/);
});

test("vanilla barriers, fences, railings, walkways, and catwalks snap to a road and zoop along it (#223, #224)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const snap = readFileSync(resolve(source, "Private", "MKIRoadBuildSnap.cpp"), "utf8"), module = readFileSync(resolve(source, "Private", "MKIRoadsModule.cpp"), "utf8");
  // #223: the vanilla pieces the maintainer named, less the two fences (#312) (read from the paks: Holo_Barrier is an AFGFenceHologram,
  // Holo_Railing an AFGWallHologram); every other wall keeps vanilla. The step is the piece's own length (mWidth: 4 m, the
  // industrial railing Build_Fence_01 8 m).
  const classes = [...snap.matchAll(/TEXT\("(\/Game\/FactoryGame\/Buildable\/Building\/[^"]+)"\)/g)].map((match) => match[1].split(".").pop());
  assert.deepEqual(classes, ["Build_Concrete_Barrier_01_C", "Build_Barrier_Low_01_C", "Build_Barrier_Tall_01_C", "Build_Railing_01_C", "Build_Fence_01_C"]);
  // Negative (#312): chain link and tarp fences block the truck path beside them, so they never snap to roads.
  assert.doesNotMatch(snap, /Build_ChainLinkFence|Build_TarpFence/);
  assert.match(snap, /if \(!BuildClass \|\| !BuildClass->IsChildOf\(AFGBuildableWall::StaticClass\(\)\)\) return false;/);
  assert.match(snap, /const float Width = GetDefault<AFGBuildableWall>\(const_cast<UClass\*>\(BuildClass\)\)->mWidth;/);
  // #224: level walkways and catwalks (straights, corners, T-junctions, crosses: 4 m squares); ramps and stairs keep vanilla.
  assert.match(snap, /if \(FMath::Abs\(Walkway->mElevation\) > KINDA_SMALL_NUMBER \|\| Walkway->mSize <= 1\.f\) return false;/);
  assert.match(snap, /return IsRoadWall\(BuildClass, OutStepCm\) \|\| IsRoadWalkway\(BuildClass, OutStepCm\);/);
  // A wall: the third of the deck aimed at, its middle on the edge so it straddles it as vanilla does (#312), its length
  // axis (+Y) along the chord (a quarter turn on it). A walkway at an edge: outside it, half a piece out, turned by quarter turns.
  assert.match(snap, /const float Lateral = Side == EMKIRoadSide::Center \? 0\.f : \(Side == EMKIRoadSide::Left \? -HalfDeck : HalfDeck\);/);
  // Negative (#312): no piece is pulled in off the edge onto the deck any more, which narrowed the lanes for trucks.
  assert.doesNotMatch(snap, /WallEdgeMarginCm|Inner = FMath::Max/);
  assert.match(snap, /return PlaceOnChord\(\*Hologram, \*Road, Start, End, Lateral, 90\.f \* XSign\);/);
  assert.match(snap, /const float Lateral = Side \* \(Road\.GetDeckWidthCm\(\) \* 0\.5f \+ Step \* 0\.5f\);/);
  assert.match(snap, /return PlaceOnChord\(\*Hologram, Road, Start, End, Lateral, FMath::RoundToFloat\(Hologram->GetScrollRotateValue\(\) \/ 90\.f\) \* 90\.f\);/);
  assert.match(snap, /if \(Mode == EMKIRoadSnapMode::Edge && SnapRoadWalkwayEdge\(Hologram, \*Road, HitResult\.Location, HitResult\.ImpactNormal\)\) return true;/);
  // The first piece is the one on the road's step marks (from its start) under the aim, kept on the road; its footprint's
  // middle goes on its chord's middle.
  assert.match(snap, /CenteredSpan\(\*Spline, \(FMath::FloorToFloat\(Along \/ Step\) \+ 0\.5f\) \* Step, Lateral, Step, Start, End\);/);
  assert.match(snap, /const float Shift = Middle - Half < 0\.f \? Half - Middle : \(Middle \+ Half > Length \? Length - Middle - Half : 0\.f\);/);
  assert.match(snap, /Hologram\.SetActorTransform\(FTransform\(Rotation, Chord\.GetLocation\(\) - Rotation\.RotateVector\(FootprintMiddle\(Hologram\)\)\)\);/);
  // The maintainer (switchback): spaced along the centerline, pieces on the outside of a bend stood apart. Each piece's chord along
  // its own offset line is one step long, and each starts where the last ended; the chord frame pitches with the grade.
  assert.match(snap, /To = From \+ \(To - From\) \* Step \/ Chord;/);
  assert.match(snap, /if \(Index > 0\) \{ OutStart = OutEnd; OutEnd = ChainEnd\(Spline, OutStart, Place\.Lateral, Place\.Step, 1\.f\); \}/);
  assert.match(snap, /else \{ OutEnd = OutStart; OutStart = ChainEnd\(Spline, OutEnd, Place\.Lateral, Place\.Step, -1\.f\); \}/);
  assert.match(snap, /Out = FTransform\(FRotationMatrix::MakeFromXZ\(Forward, FVector::UpVector\)\.ToQuat\(\), \(A \+ B\) \* 0\.5f\);/);
  // The maintainer (intersections): the zoop looked the road up again and could take the road beside it. It keeps the road the
  // hologram snapped to; a hologram without one takes a road beside which it stands (not one past an end), preferring the
  // road running along the piece.
  assert.match(snap, /SnappedRoads\(\)\.Add\(&Hologram, &Road\);/);
  assert.match(snap, /if \(Snapped && PointByRoad\(\*Snapped, Middle, Along, Lateral, Height\)\) Out\.Road = Snapped;/);
  assert.match(snap, /return FMath::Abs\(OutLateral\) <= Road\.GetDeckWidthCm\(\) \* 0\.5f \+ 600\.f && OutHeight <= 50\.f && Ahead <= 100\.f;/);
  // The zoop: whole pieces toward the aim, chained along the road, never past its ends or vanilla's limit, on the local axis
  // vanilla lets the piece zoop along (railings and T-junctions Y, straight walkways X) that lies along the road; else vanilla.
  assert.match(snap, /while \(Pieces < Max\)/);
  assert.match(snap, /if \(!PieceSpan\(Spline, Place, \(Pieces \+ 1\) \* static_cast<int32>\(Sign\), NextStart, NextEnd\)\) break;/);
  assert.match(snap, /if \(FMath::Abs\(Alignment\) < 0\.7f\) return false;/);
  assert.match(snap, /OutZoop = bUseX \? FIntVector\(Local, 0, 0\) : FIntVector\(0, Local, 0\);/);
  // The maintainer: railings did not zoop when vanilla's zoop was skipped; a limit still 0 gets 10.
  assert.match(snap, /if \(Buildable->GetMaxZoopAmount\(\) <= 0\) Buildable->SetMaxZoopAmount\(RoadRunDefaultMaxZoop\);/);
  // Each instance: vanilla's k pieces along the zoop axis become the road's k-th piece, in the hologram's pose on it.
  assert.match(snap, /Index = FMath::RoundToInt\(static_cast<float>\(Offset\.Size\(\)\) \/ Place\.Step\) \* \(Along >= 0\.f \? 1 : -1\);/);
  assert.match(snap, /OutInstance = \(Place\.OnChord \* Chord\)\.GetRelativeTransform\(Hologram->GetActorTransform\(\)\);/);
  // Hooks: the snap on the wall and fence holograms (the walkway's goes through SnapHologram), the zoop after vanilla's on
  // walls and walkways, and each instance (CreateZoopInstances is protected: naming it through a derived struct did not
  // compile with MSVC, so the public, non-virtual GenerateZoopInstance is hooked).
  for (const hologram of ["AFGWallHologram", "AFGFenceHologram"]) {
    assert.ok(module.includes(`SUBSCRIBE_UOBJECT_METHOD(${hologram}, TrySnapToActor, [](auto& Scope, ${hologram}* Self, const FHitResult& HitResult)\n\t\t{\n\t\t\tif (FMKIRoadBuildSnap::SnapRoadWallHologram(Self, HitResult)) Scope.Override(true);\n\t\t});`), hologram);
  }
  for (const hologram of ["AFGWallHologram", "AFGWalkwayHologram"]) {
    assert.ok(module.includes(`SUBSCRIBE_UOBJECT_METHOD_AFTER(${hologram}, SetZoopFromHitresult, [](${hologram}* Self, const FHitResult& HitResult)\n\t\t{\n\t\t\tFIntVector Zoop;\n\t\t\tif (FMKIRoadBuildSnap::RoadRunZoopFromHit(Self, HitResult, Zoop)) Self->SetZoopAmount(Zoop);\n\t\t});`), hologram);
  }
  assert.match(module, /SUBSCRIBE_METHOD\(AFGBuildableHologram::GenerateZoopInstance, /);
  assert.match(module, /if \(bIsChildZoopInstance \|\| !FMKIRoadBuildSnap::RoadRunZoopInstance\(Self, InstanceTransform, OnRoad\)\) return;\n\t\t\tScope\(Self, OnRoad, bIsChildZoopInstance\);/);
  assert.doesNotMatch(module, /FMKIRoadWallHologramAccess|SUBSCRIBE_METHOD_VIRTUAL/);
  // Negative: not on a road, or not a road run piece, the vanilla snap, zoop, and instances stand.
  assert.match(snap, /if \(!Hologram \|\| !FindRoadRunPlace\(\*Hologram, Place\)\) return false;/);
  assert.match(snap, /if \(!Road \|\| !bRoadWall\) return false;/);
  assert.match(snap, /if \(!FMKIRoadBuildSnap::IsRoadRun\(Hologram\.GetBuildClass\(\), &Out\.Step\) \|\| !Hologram\.GetWorld\(\)\) return false;/);
  // The road barriers, railings, and fences are gone from the manifest.
  const kit = roadKit();
  assert.ok(kit.placeholders.every((item) => !/Barrier_01|Barrier_low|Barrier_tall|Railing_01|Fence_01|TarpFence|ChainLinkFence/i.test(item.path)), "no placeholder of a removed barrier");
});

test("there are no road barriers: the classes are gone, and a road dismantles only its supports (#181, #223)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const road = read("Private/MKIRoadSplineRoad.cpp");
  // The maintainer removed the MKI Roads barrier classes; vanilla barriers snap to roads instead (#223).
  for (const dir of ["Public", "Private"]) assert.ok(!readdirSync(resolve(source, dir)).some((name) => /Barrier/.test(name)), `no barrier files in ${dir}`);
  assert.doesNotMatch(road, /AMKIRoadBarrier/);
  assert.match(road, /void AMKIRoadSplineRoad::GetChildDismantleActors_Implementation[\s\S]*?TActorIterator<AMKIRoadSupports> It\(GetWorld\(\)\); It; \+\+It\) if \(It->GetRoad\(\) == this\) OutChildDismantleActors\.AddUnique\(\*It\);/);
  // #185: built with the build gun, the deck collision had its slabs and collision enabled but no physics state until the
  // save was reloaded. The road creates the missing physics state after building the slabs, on the next tick, and once
  // the spawn has settled. The build effect was not the cause, so both buildables keep the vanilla one (negative control).
  const ensure = /void AMKIRoadSplineRoad::EnsureCollisionPhysics\(\)\n\{([\s\S]*?)\n\}/.exec(road)?.[1] ?? "";
  assert.match(ensure, /mCollisionSlabs->IsPhysicsStateCreated\(\)\) return;/, "an existing physics state is left alone");
  assert.match(ensure, /mCollisionSlabs->RecreatePhysicsState\(\);/);
  // The mesh and slabs are set before the component registers, so its bodies are created with it; the state is then checked.
  const build = /void AMKIRoadSplineRoad::BuildCollision\(\)\n\{([\s\S]*?)\n\}\n/.exec(road)?.[1] ?? "";
  assert.match(build, /ON_SCOPE_EXIT\n\t\{\n\t\tif \(bFresh\) mCollisionSlabs->RegisterComponent\(\);\n\t\tEnsureCollisionPhysics\(\);\n\t\};/);
  assert.ok(build.indexOf("RegisterComponent") > build.indexOf("ON_SCOPE_EXIT") && build.lastIndexOf("RegisterComponent") === build.indexOf("RegisterComponent"), "registered only once, at scope exit, after the mesh and instances");
  assert.ok(build.indexOf("SetStaticMesh(Mesh)") > 0 && build.indexOf("AddInstance(") > build.indexOf("SetStaticMesh(Mesh)"));
  assert.equal((road.match(/CreateWeakLambda\(this, \[this\]\(\) \{ EnsureCollisionPhysics\(\); \}\)/g) ?? []).length, 2, "next tick and once settled");
  for (const header of ["Public/MKIRoadSplineRoad.h", "Public/MKIRoadAutoPaths.h", "Public/MKIRoadSplineRoadHologram.h"]) {
    assert.doesNotMatch(read(header), /ShouldSkipBuildEffect/, `${header} keeps the vanilla build effect`);
  }
});

test("vanilla foundations, walkways, and catwalks snap to a spline road's top, edges, and ends in 2 m steps (#188)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const header = readFileSync(resolve(source, "Public", "MKIRoadBuildSnap.h"), "utf8"), snap = readFileSync(resolve(source, "Private", "MKIRoadBuildSnap.cpp"), "utf8");
  assert.match(header, /static constexpr float StepCm = 200\.f;/, "the maintainer: 2 m steps");
  assert.match(header, /static constexpr float BandCm = 100\.f;/);
  // Spans start on the 2 m grid nearest to centering them on the aim.
  assert.match(snap, /return FMath::RoundToFloat\(\(Center - Length \* 0\.5f\) \/ StepCm\) \* StepCm;/);
  // Modes: an end (within the band of either end, on the deck), else the outer band or the side face, else the top.
  assert.match(snap, /bool bAtStart = Along <= BandCm, bAtEnd = Along >= Length - BandCm;/);
  // A side or end face is told by the hit normal (live: the build gun's hit location on the side was not on that face).
  assert.match(snap, /if \(!Normal\.IsNearlyZero\(\) && FMath::Abs\(Normal\.Z\) < 0\.5f\)/);
  assert.match(snap, /if \(FMath::Abs\(FVector::DotProduct\(Normal, Forward\)\) > 0\.7f\) \{ Mode = EMKIRoadSnapMode::End;/);
  assert.match(snap, /else \{ Mode = EMKIRoadSnapMode::Edge; Lateral = FVector::DotProduct\(Normal, Right\) >= 0\.f \? HalfDeck : -HalfDeck; \}/);
  assert.match(snap, /Snap\(\*Road, HitResult\.Location, HitResult\.ImpactNormal, Piece,/);
  assert.match(snap, /else if \(bBelowTop \|\| FMath::Abs\(Lateral\) >= HalfDeck - BandCm\) Mode = EMKIRoadSnapMode::Edge;/);
  // Edge: outside the edge, against it. End: past the end, against it. Top: on the deck, kept on it.
  assert.match(snap, /Side \* \(HalfDeck \+ Piece\.WidthCm \* 0\.5f\)/);
  assert.match(snap, /Location = Center \+ Outward \* \(Piece\.LengthCm \* 0\.5f\) \+ Right \* Across\(Lateral\);/);
  assert.match(snap, /FMath::Clamp\(SnapSpanStart\(Wanted \+ HalfDeck, Width\), 0\.f, 2\.f \* HalfDeck - Width\)/);
  // Heights, from the vanilla pivots measured live: a foundation's is its center, a walkway's its floor.
  assert.match(snap, /if \(Mode == EMKIRoadSnapMode::Top\) Location\.Z = Piece\.bPivotOnFloor \? Top : Top \+ Piece\.HeightCm \* 0\.5f;\n\telse Location\.Z = Piece\.bPivotOnFloor \? Top : Top - Piece\.HeightCm \* 0\.5f;/);
  // Level pieces, turned with the road; scroll rotation in quarter turns.
  assert.match(snap, /Out = FTransform\(FRotator\(0\.f, Yaw, 0\.f\), Location\);/);
  assert.match(snap, /FMath::RoundToFloat\(ExtraYawDeg \/ 90\.f\) \* 90\.f/);
  // Negative controls: only a spline road hit, only level foundations and walkways; ramps and stairs keep vanilla.
  const hologram = /bool FMKIRoadBuildSnap::SnapHologram\([\s\S]*?\n\}/.exec(snap)?.[0] ?? "";
  assert.match(hologram, /if \(!Road \|\| !BuildClass\) return false;/);
  assert.match(hologram, /FMath::Abs\(Foundation->mElevation\) > KINDA_SMALL_NUMBER/);
  assert.match(hologram, /FMath::Abs\(Walkway->mElevation\) > KINDA_SMALL_NUMBER/);
  assert.match(hologram, /else return false;/, "any other buildable keeps the vanilla snap");
});

test("a spline road refuses vanilla patterns and keeps its road marking pattern (#190)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const road = readFileSync(resolve(source, "Private", "MKIRoadSplineRoad.cpp"), "utf8");
  assert.match(road, /return PatternDesc && !PatternDesc->IsChildOf\(UMKIRoadMarkingPattern::StaticClass\(\)\) && GetDefault<UFGFactoryCustomizationDescriptor_Pattern>\(PatternDesc\)->ID != 0;/, "road patterns and removers (ID 0) are not vanilla");
  const apply = /void AMKIRoadSplineRoad::ApplyCustomizationData_Native\([\s\S]*?\n\}/.exec(road)?.[0] ?? "";
  assert.match(apply, /if \(IsVanillaPattern\(CustomizationData\.PatternDesc\)\)\n\t\{\n\t\tCustomizationData\.PatternDesc = mRoadPattern;/);
  assert.match(apply, /GetCustomizationData_Native\(\)\.PatternDesc = mRoadPattern;/, "the saved customization keeps the road pattern");
  assert.match(apply, /mRoadPattern = CustomizationData\.PatternDesc && CustomizationData\.PatternDesc->IsChildOf\(UMKIRoadMarkingPattern::StaticClass\(\)\) \? CustomizationData\.PatternDesc : nullptr;/, "a road pattern is remembered; a remover clears it");
  assert.ok(apply.indexOf("IsVanillaPattern") < apply.indexOf("Super::ApplyCustomizationData_Native(CustomizationData);"), "refused before vanilla applies it");
});

test("vanilla vehicle paths on a road follow its lane line (#192)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const header = readFileSync(resolve(source, "Public", "MKIRoadLanePath.h"), "utf8"), lane = readFileSync(resolve(source, "Private", "MKIRoadLanePath.cpp"), "utf8");
  // The vanilla spacing and lift, seen live in a path hologram's planned points.
  assert.match(header, /static constexpr float PointStepCm = 250\.f;/);
  assert.match(header, /static constexpr float LiftCm = 1\.f;/);
  // Lane centers on 8 m lanes, clamped to the deck.
  assert.match(lane, /const int32 Lane = FMath::Clamp\(FMath::FloorToInt32\(\(LateralCm \+ Half\) \/ Width\), 0, FMath::Max\(0, Lanes - 1\)\);/);
  assert.match(lane, /return -Half \+ \(Lane \+ 0\.5f\) \* Width;/);
  // The plan: points on the start's lane center from the start to the end, tangents along the lane in travel direction.
  assert.match(lane, /OutWorldPoints\.Add\(FSplinePointData\(Center \+ Right \* Lateral \+ FVector\(0\.f, 0\.f, LiftCm\), Tangent\)\);/);
  assert.match(lane, /const FVector Tangent = Direction \* Sign \* FMath::Abs\(Step\);/);
  // Negative controls: off the deck, too short, or not a road keeps the vanilla routing.
  assert.match(lane, /if \(!OnDeck\(Road, Start, StartAlong, StartLateral\) \|\| !OnDeck\(Road, End, EndAlong, EndLateral\)\) return false;/);
  assert.match(lane, /if \(FMath::Abs\(Span\) < PointStepCm\) return false;/);
  // #196: a point past a road's end is not on it (live, a path started on the highway and aimed into the junction square
  // was planned along the square's lane); within EndToleranceCm of an end it still is (a node exactly at the end).
  assert.match(header, /static constexpr float EndToleranceCm = 50\.f;/);
  // #198: a start on an existing node follows the lane only if the node already faces along it (within 30 degrees);
  // otherwise (a U-turn on a wide apron) vanilla's routing stays. Free starts still follow the lane (negative control).
  assert.match(header, /static constexpr float MaxNodeTurnDeg = 30\.f;/);
  // An aim in another lane than the start's is a lane change or maneuver: vanilla routes it.
  assert.match(lane, /if \(FMath::Abs\(EndLateral - Lane\) > AMKIRoadSplineRoad::LaneWidthCm \* 0\.5f\) return false;/);
  assert.match(lane, /if \(FPathHologramAccess::IsStartOnNode\(Hologram\) && Points\.Num\(\) >= 2 && World\.Num\(\) >= 2\)/);
  assert.match(lane, /FVector::DotProduct\(VanillaStart, LaneStart\) < FMath::Cos\(FMath::DegreesToRadians\(MaxNodeTurnDeg\)\)\) return;/);
  assert.match(lane, /const float Beyond = FMath::Abs\(FVector::DotProduct\(Point - Center, Forward\)\);\n\t\treturn Beyond <= FMKIRoadLanePath::EndToleranceCm && /);
  // #210: a start or end within RoadEndSnapCm of a road's end goes exactly to it, so paths on consecutive roads meet.
  assert.match(header, /static constexpr float RoadEndSnapCm = 400\.f;/);
  assert.match(lane, /if \(Along <= RoadEndSnapCm\) return 0\.f;\n\tif \(Along >= Length - RoadEndSnapCm\) return Length;\n\treturn Along;/);
  assert.match(lane, /EndAlong = SnapToRoadEnd\(EndAlong, Road\.GetSplineComponent\(\)->GetSplineLength\(\)\);/);
  assert.match(lane, /Along = SnapToRoadEnd\(Along, Spline->GetSplineLength\(\)\);/);
  // The line leaves from the start itself and eases onto the lane center over EaseCm (no kink from a node beside it).
  assert.match(lane, /World\[0\]\.Location = Hologram->GetActorLocation\(\);/);
  assert.match(lane, /const float Lateral = Lane \+ Offset \* \(1\.f - Ease \* Ease \* \(3\.f - 2\.f \* Ease\)\);/);
  assert.match(lane, /AMKIRoadSplineRoad\* Road = RoadOf\(HitResult\);\n\tif \(!Road\) return;/);
  // The points go into the hologram's own space, then vanilla rebuilds its preview and validation from them.
  assert.match(lane, /Points\.Add\(FSplinePointData\(Frame\.InverseTransformPosition\(Point\.Location\), Tangent\)\);/);
  assert.match(lane, /Access->OnRep_SplineData\(\);\n\t\t\tAccess->UpdatePathValidationInfoFromSplineComponent\(\);\n\t\t\tAccess->UpdateHologramVisualState\(\);/);
});

test("the vanilla street light snaps to a road's edges and centerline; there is no Road Street Light (#222)", () => {
  const kit = roadKit(), plan = roadPackEditorPlan(kit);
  // The maintainer: no separate road light; the vanilla street light works on roads in its stead.
  assert.ok(!("lights" in kit.package) && !("lights" in plan), "no Road Street Light entry");
  assert.ok(kit.placeholders.every((item) => !/StreetLight/.test(item.path)), "no street light placeholder");
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const snap = readFileSync(resolve(source, "Private", "MKIRoadBuildSnap.cpp"), "utf8"), header = readFileSync(resolve(source, "Public", "MKIRoadBuildSnap.h"), "utf8");
  const module = readFileSync(resolve(source, "Private", "MKIRoadsModule.cpp"), "utf8"), road = readFileSync(resolve(source, "Private", "MKIRoadSplineRoad.cpp"), "utf8");
  // After the vanilla factory hologram placed itself (Holo_StreetLight's native parent is AFGFactoryHologram), a street
  // light aimed at a road is snapped; any other hologram or aim keeps the vanilla placement.
  assert.ok(module.includes("SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGFactoryHologram, SetHologramLocationAndRotation, [](AFGFactoryHologram* Self, const FHitResult& HitResult)\n\t\t{\n\t\t\tFMKIRoadBuildSnap::SnapStreetLightHologram(Self, HitResult);\n\t\t});"));
  assert.match(snap, /if \(!Road \|\| !IsStreetLight\(Hologram->GetBuildClass\(\)\)\) return false;/);
  assert.match(snap, /TEXT\("\/Game\/FactoryGame\/Buildable\/Factory\/StreetLight\/Build_StreetLight\.Build_StreetLight_C"\)/);
  // The placement of the removed Road Street Light (#199, #200, #203): the third of the deck aimed at picks the side, the
  // pole 50 cm inside the edge, 4 m steps along the road, the arm (+Y) over the road, then the hologram's rotation.
  assert.match(header, /static constexpr float LightEdgeInsetCm = 50\.f;/);
  assert.match(header, /static constexpr float LightStepCm = 400\.f;/);
  assert.match(snap, /const EMKIRoadSide Side = RoadSideAt\(Road, Hit\);/);
  assert.match(snap, /const float Distance = FMath::Clamp\(FMath::RoundToFloat\(Spline->GetDistanceAlongSplineAtSplineInputKey\(Key\) \/ LightStepCm\) \* LightStepCm, 0\.f, Length\);/);
  assert.match(snap, /FRotationMatrix::MakeFromYZ\(Right \* Toward, FVector::UpVector\)/);
  assert.match(snap, /Out = FTransform\(FQuat\(FVector::UpVector, FMath::DegreesToRadians\(ExtraYawDeg\)\) \* Facing, Center \+ Right \* Offset\);/);
  assert.match(snap, /SnapStreetLight\(\*Road, HitResult\.Location, static_cast<float>\(Hologram->GetScrollRotateValue\(\)\), Snapped\)/);
  // The Road Street Light fix-up is gone with the maintainer's cleanup: no recipe restore, no subsystem for it, and no
  // SML hook on PostLoadGame (an IFGSaveInterface function; a hook on AFGBuildable's crashed the load, live).
  assert.doesNotMatch(module, /PostLoadGame/);
  assert.doesNotMatch(snap + header, /RestoreStreetLightRecipe|Recipe_StreetLight/);
  assert.ok(!readdirSync(resolve(source, "Private")).some((name) => /StreetLightRecipes/.test(name)));
  // Lights are ordinary street lights: a road no longer dismantles them, and the light row classes are gone.
  assert.doesNotMatch(road, /LightRow/);
  assert.ok(!readdirSync(resolve(source, "Private")).some((name) => /LightRow/.test(name)));
  // Negative: a manifest that still declares a light entry is refused.
  const raw = JSON.parse(readFileSync(resolve(roadPackDirectory, "modules.json"), "utf8"));
  const changed = (change: (copy: typeof raw) => void) => { const copy = structuredClone(raw); change(copy); return copy; };
  assert.throws(() => parseRoadKitDocument(changed((copy) => { copy.package.lights = { nativeClass: "/Script/MKIRoads.MKIRoadLightRow" }; })), /Unrecognized key/);
  // #202: MKI Patterns must name the base-game category it derives from (the vanilla Patterns category).
  assert.throws(() => parseRoadKitDocument(changed((copy) => { delete copy.package.splineRoad.patterns.category.parent; })), /package\.splineRoad\.patterns\.category\.parent/);
  assert.throws(() => parseRoadKitDocument(changed((copy) => { copy.package.splineRoad.patterns.category.parent = "/MKIRoads/Customization/CC_Other.CC_Other"; })), /package\.splineRoad\.patterns\.category\.parent/);
});

test("road supports lay concrete pillar columns under a raised road, down to the ground (#194)", () => {
  const kit = roadKit(), supports = roadSupportEntries(kit);
  assert.deepEqual(supports.map((item) => [item.displayName, item.menuPriority]), [["Road Support (Concrete)", 89.6]]);
  assert.deepEqual(supports.map((item) => [item.baseMesh.split(".").pop(), item.middleMesh.split(".").pop()]), [["SM_Pillar_01", "SM_Pillar_MiddleConcrete"]]);
  assert.deepEqual(roadPackEditorPlan(kit).supports.entries, supports);
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const header = readFileSync(resolve(source, "Public", "MKIRoadSupports.h"), "utf8"), lay = readFileSync(resolve(source, "Private", "MKIRoadSupports.cpp"), "utf8");
  // #200: 4 m steps (the maintainer).
  for (const constant of ["StepCm = 400.f", "HalfFootprintCm = 400.f", "MiddleHalfCm = 200.f", "PieceHeightCm = 400.f", "MaxPieces = 64", "HeightStepCm = 200.f", "MinHeightCm = 600.f"]) assert.ok(header.includes(constant), constant);
  // #215 (the maintainer): where a support stands is the player's call. The on-road check (#199, refined in #214) is gone, and
  // no trace looks for roads or vehicle paths under the column; the spacing and gap checks remain (negative controls).
  assert.doesNotMatch(lay, /CheckSpan|AFGVehiclePathSegment|JointMarginCm/);
  assert.doesNotMatch(header, /OnRoad/);
  assert.doesNotMatch(readFileSync(resolve(source, "Public", "MKIRoadSplineRoadDisqualifiers.h"), "utf8"), /UMKIRoadCDSupportsOnRoad/);
  // #203: on the centerline; the height in 2 m steps (at least the 4 m base), an odd step a half-height top piece.
  // #217: no column shorter than 8 m (the maintainer: short ones don't look right), wherever its height comes from.
  assert.match(lay, /return FMath::Clamp\(Height, MinHeightCm, MaxPieces \* PieceHeightCm\);/);
  assert.match(lay, /const float Height = FMath::Clamp\(HeightCm, MinHeightCm, MaxPieces \* PieceHeightCm\);/);
  // #216: the top snaps across the road too, every StepCm from the centerline, inside the deck (two under a 4-lane road).
  assert.match(lay, /const float Lateral = FMath::Clamp\(FMath::RoundToFloat\(FVector::DotProduct\(Aim - Center, Right\) \/ StepCm\) \* StepCm, -Limit, Limit\);/);
  assert.match(lay, /OutTop = Center \+ Right \* Lateral - FVector\(0\.f, 0\.f, Road\.GetDeckThicknessCm\(\)\);/);
  assert.match(lay, /const float Height = FMath::CeilToFloat\(DepthCm \/ HeightStepCm - 0\.01f\) \* HeightStepCm;/);
  assert.match(lay, /float Rest = Above - Full \* PieceHeightCm;/);
  assert.match(lay, /FVector\(1\.f, 1\.f, Rest \/ PieceHeightCm\)/);
  // Pieces go on by their mesh bounds center (the vanilla pillar meshes sit 200 cm below their pivots), scaled with a
  // half-height piece; refund per piece.
  assert.match(lay, /PieceCenter\.GetLocation\(\) - PieceCenter\.GetRotation\(\)\.RotateVector\(PieceCenter\.GetScale3D\(\) \* Center\)/);
  assert.match(lay, /return FMath::Max\(1, CountPieces\(mColumns\)\);/);
  // The road's dismantle takes its supports; none where there is no gap; none next to another.
  assert.match(readFileSync(resolve(source, "Private", "MKIRoadSplineRoad.cpp"), "utf8"), /for \(TActorIterator<AMKIRoadSupports> It\(GetWorld\(\)\); It; \+\+It\) if \(It->GetRoad\(\) == this\) OutChildDismantleActors\.AddUnique\(\*It\);/);
  const hologram = readFileSync(resolve(source, "Private", "MKIRoadSupportsHologram.cpp"), "utf8");
  // #200: a zoop. The first click fixes the top (only where there is room for a piece); the second, the base the aim set,
  // the length the aim set. A reset steps back.
  // #216 (the maintainer): anywhere. No spacing or ground-gap check, no disqualifier for them; only an aim off a road is refused.
  assert.match(hologram, /if \(!HasDeck()\(\) \|\| mColumns\.Num\(\) == 0\) AddConstructDisqualifier\(UMKIRoadCDNeedsRoad::StaticClass\(\)\);/);
  assert.doesNotMatch(hologram, /IsNearAnother|SupportsNoGap|SupportsTaken|MinGapCm/);
  assert.doesNotMatch(readFileSync(resolve(source, "Public", "MKIRoadSplineRoadDisqualifiers.h"), "utf8"), /UMKIRoadCDSupports/);
  assert.match(hologram, /if \(!HasDeck()\(\) \|\| mFit != EMKIRoadSupportFit::Fits \|\| mColumns\.Num\(\) == 0\) return false;\r?\n\t\tmPlacingBase = true;\r?\n\t\treturn false;/);
  assert.match(hologram, /mFit = HasDeck()\(\) \? EMKIRoadSupportFit::Fits : EMKIRoadSupportFit::NoGround;/);
  // The base no longer snaps to the ground (the maintainer, 2026-10-18: on uneven ground it still floated on one side):
  // the aim sets it in 2 m steps and nudging up or down moves it by the nudge distance.
  assert.match(hologram, /const float Height = AMKIRoadSupports::HeightForDepth\(Depth\);/);
  assert.doesNotMatch(hologram, /DepthToGround|bAimedAtSurface/, "no ground snap");
  assert.doesNotMatch(readFileSync(resolve(source, "Public", "MKIRoadSupports.h"), "utf8"), /DepthToGround/);
  assert.match(hologram, /MakeColumn\(mTop, mForward, FMath::Max\(AMKIRoadSupports::MinHeightCm, mNudgeHeight - Offset\.Z\)\)/, "nudged by the nudge distance, never shorter than one piece");
  assert.doesNotMatch(hologram, /FMath::Max\((Depth|Ground), AMKIRoadSupports::PieceHeightCm\)/);
  // #219: before the first click the preview is the shortest column (it reached to the ground, 8 m in the maintainer's case).
  assert.match(hologram, /mColumns\.Add\(AMKIRoadSupports::MakeColumn\(mTop, mForward, AMKIRoadSupports::MinHeightCm\)\);/);
  // #218: right-click while setting the base steps back (the build gun resets a changed hologram, else opens the menu);
  // the preview shows the pillar's own concrete, the invalid material only when it cannot be built.
  assert.match(hologram, /bool AMKIRoadSupportsHologram::IsChanged\(\) const\r?\n\{[\s\S]*?return mPlacingBase \|\| Super::IsChanged\(\);/);
  assert.match(hologram, /UMaterialInterface\* Material = State == EHologramMaterialState::HMS_ERROR \? mInvalidPlacementMaterial\.Get\(\) : nullptr;/);
  assert.match(lay, /if \(Rest < MinPieceCm && Full > 0 && Rest > 0\.5f\) \{ --Full; Rest \+= PieceHeightCm; \}/);
  assert.match(hologram, /if \(!mPlacingBase\) return Super::HandleReset\(\);\r?\n\tmPlacingBase = false;\r?\n\treturn true;/);
});

test("the cooked asset registry must name the whole registration chain and the root-module tag", () => {
  const packages = roadPackAssetPackages(roadKit());
  // Name and path tables as they appear in the cooked AssetRegistry.bin (concatenated, binary-separated).
  const cooked = Buffer.from(["\u0000", packages.join(""), "\u0001/Script/SML\u0000GameWorldModule\u0002FGSchematic FGRecipe FGBuildingDescriptor MKIRoadSplineRoad FGBuildCategory FGBuildSubCategory\u0000bRootModule\u0000True"].join(""), "latin1");
  assert.deepEqual(cookedRegistryProblems(cooked, packages), []);
  assert.deepEqual(REGISTRATION_CHAIN_CLASSES.length, 7);
  const withoutRoot = Buffer.from(cooked.toString("latin1").replace("/MKIRoads/RootGameWorld_MKIRoads", "").replaceAll("bRootModule", ""), "latin1");
  assert.deepEqual(cookedRegistryProblems(withoutRoot, packages), ["the cooked AssetRegistry.bin does not name /MKIRoads/RootGameWorld_MKIRoads", "the cooked AssetRegistry.bin does not name bRootModule"]);
  assert.match(cookedRegistryProblems(Buffer.from(cooked.toString("latin1").replace("FGBuildSubCategory", ""), "latin1"), packages).join(), /does not name FGBuildSubCategory/);
  // The buildables are Blueprints of the native spline road; a registry without it lost them (the modular FGBuildable pieces are gone, #180).
  assert.match(cookedRegistryProblems(Buffer.from(cooked.toString("latin1").replace("MKIRoadSplineRoad", "FGBuildable"), "latin1"), packages).join(), /does not name MKIRoadSplineRoad/);
  assert.match(cookedRegistryProblems(new Uint8Array(), packages).join(), /AssetRegistry.bin is empty/);
});

test("the editor verify walks the registration chain from the root module SML discovers", () => {
  const script = readFileSync(resolve(roadPackPluginDirectory, "..", "unreal", "road_pack_editor.py"), "utf8");
  // Discovery mirrors SML's FPluginModuleLoader: Blueprint assets under the mount tagged bRootModule=True.
  assert.match(script, /def discovered_root_modules\(\):[\s\S]*?get_assets_by_path\(MOUNT, recursive=True\)[\s\S]*?get_tag_value\("bRootModule"\)\) == "True"/);
  const walk = /def verify_registration_chain\(\):([\s\S]*?)\n\n\ndef /.exec(script)?.[1] ?? "";
  for (const link of ["mSchematics", "mUnlocks", "FGUnlockRecipe", "mRecipes", "mProduct", "mBuildableClass", "mHologramClass", "mDecoratorClass", "resolve_menu(descriptor)", "mProducedIn"]) assert.ok(walk.includes(link), `the chain walk follows ${link}`);
  assert.doesNotMatch(walk.split("splines = {")[0], /load_class\((?:PACKAGE|MENU|PLAN|entry)\[/, "links are followed through object references, not looked up from the manifest or plan");
  assert.ok(walk.includes('"decorator": None'), "a spline road has no decoration template");
  const verify = /def run_verify\(\):([\s\S]*?)\n\n\ntry:/.exec(script)?.[1] ?? "";
  for (const step of ["verify_spline_roads()", "verify_spline_patterns()", "verify_categories()", "verify_vanilla_asphalt()", "verify_paints()", "verify_registration()", "verify_registration_chain()", "verify_native_registration()"]) assert.ok(verify.includes(step), `run_verify runs ${step}`);
});

test("the editor builds and verifies only the spline road kit, from the TypeScript plan", () => {
  const script = readFileSync(resolve(roadPackPluginDirectory, "..", "unreal", "road_pack_editor.py"), "utf8");
  assert.match(script, /PLAN = json\.load\(open\(CONFIG\["plan"\]/, "derived values come from roadPackEditorPlan");
  // #180: nothing of the modular pieces is imported, built, or verified any more.
  for (const retired of ["fbxDir", "import_mesh", "import_markings_mesh", "build_attachment_types", "build_decorator", "build_holograms", "configure_adjustable_holograms", "build_variant", "verify_mesh", "verify_attachment", "verify_variant", "verify_hologram", "verify_markings", "verify_vanilla_materials", 'PLAN["modules"]', 'MANIFEST["modules"]', "unlocksModules", "MKIRoadsZoopHologram", "MKIRoadsSeamPointType", "MKIRoadsAdjustableHologram"]) {
    assert.ok(!script.includes(retired), `the editor no longer uses ${retired}`);
  }
  // #159: asphalt only; the verify confirms the root module carries no material registration properties.
  const native = /def verify_native_registration\(\):([\s\S]*?)\n\n\ndef /.exec(script)?.[1] ?? "";
  assert.match(native, /for name in \("MaterialGroups", "RoadBuildCategory", "RoadBuildSubCategories", "DefaultRoadMaterial"\):/);
  const vanilla = /def verify_vanilla_asphalt\(\):([\s\S]*?)\n\n\ndef /.exec(script)?.[1] ?? "";
  for (const evidence of ["mSwatchGroup", "mDefaultSwatchCustomizationOverride", "manifest_basis_ingredients()"]) assert.ok(vanilla.includes(evidence), `vanilla evidence checks ${evidence}`);
  // The schematic unlocks every spline road and road pattern recipe, and the import builds the paints the patterns use.
  assert.ok(script.includes("build_registration(build_spline_roads(shared) + build_junctions(shared) + build_spline_patterns(shared) + build_supports(shared))"), "every spline road, intersection, road pattern, and support recipe is unlocked");
  assert.ok(!/build_lights|verify_lights|light_entries/.test(script), "no Road Street Light is built or verified (#222)");
  assert.ok(script.includes('unlock.set_editor_property("mRecipes", list(recipes))'));
  assert.match(script, /for settings in PLAN\["paints"\]\.values\(\):\n {8}build_paint_material\(settings\)/);
  const roads = /def build_spline_roads\(shared\):([\s\S]*?)\n\n\ndef /.exec(script)?.[1] ?? "";
  for (const setting of ['set_editor_property("mAllowColoring", True)', 'set_editor_property("mAllowPatterning", True)', 'set_editor_property("mMaterialCustomizationRecipe", None)', 'load_class(ASPHALT["swatchGroup"])']) assert.ok(roads.includes(setting), `the spline roads set ${setting}`);
  const verifyRoads = /def verify_spline_roads\(\):([\s\S]*?)\n\n\ndef /.exec(script)?.[1] ?? "";
  for (const checked of ["so its material cannot be swapped", "refund_overrides(", 'ASPHALT["swatchGroup"]', "verify_icon("]) assert.ok(verifyRoads.includes(checked), `the verify checks ${checked}`);
  const menu = /def verify_build_menu\(chain\):([\s\S]*?)\n\n\n/.exec(script)?.[1] ?? "";
  for (const checked of ['sorted(listed.get(settings["path"], []), key=menu_priority)', "mMaterialCustomizationRecipe", "every unlocked road recipe is categorized"]) assert.ok(menu.includes(checked), `the menu check uses ${checked}`);
  assert.ok(script.includes('report["registrationChain"] = chain\n    verify_build_menu(chain)'), "the verify runs the menu check after the chain walk");
  assert.match(script, /def build_categories\(\):[\s\S]*?for settings, parent in menu_categories\(\):/, "every planned subcategory is created");
});

test("every spline road entry and road pattern has its own deterministic icon, equal to the golden (#86, #180)", () => {
  const kit = roadKit(), golden = loadRoadKitIcons(), rendered = renderRoadKitIcons(kit);
  // Only the spline road entries' and the road patterns' icons remain; the 54 module icons went with the modules.
  assert.deepEqual(rendered.map((icon) => icon.key), ["spline_road_1m", "road_pattern_200", "road_junction_2way", "road_junction_3way", "road_junction_4way", "road_turnaround", "road_truck_depot_7", "road_truck_depot_9"]);
  assert.deepEqual(rendered.map((icon) => icon.iconPath), [...splineRoadVariants(kit).map((item) => item.iconPath), ...splineRoadPatterns(kit).entries.map((item) => item.iconPath), ...roadJunctionEntries(kit).map((item) => item.iconPath)]);
  assert.deepEqual(Object.keys(golden.icons).sort(), rendered.map((icon) => icon.iconPath).sort());
  for (const icon of rendered) assert.deepEqual(summarizeIcon(icon), golden.icons[icon.iconPath], icon.iconPath);
  // The road entries keep the pixels they had before the modular icons were removed (0.6.17); the pattern is line art.
  assert.deepEqual(rendered.slice(0, 2).map((icon) => summarizeIcon(icon).rgbaSha256.slice(0, 16)), ["6598f8e9ddd94f91", "523d0ee2c51467a5"]);
  assert.equal(new Set(rendered.map((icon) => summarizeIcon(icon).rgbaSha256)).size, rendered.length);
  // The maintainer: the pattern icon read as a color swatch. It is now drawn as the vanilla line patterns' icons are: only the
  // vanilla line grey, on a transparent tile, with no asphalt fill (negative control: a road entry's icon is filled).
  const pattern = rendered[1], at = (icon: typeof pattern, x: number, y: number) => Array.from(icon.rgba.subarray((y * icon.size + x) * 4, (y * icon.size + x) * 4 + 4));
  for (let index = 0; index < pattern.rgba.length; index += 4) {
    if (pattern.rgba[index + 3] > 0) assert.deepEqual(Array.from(pattern.rgba.subarray(index, index + 3)), [...PATTERN_ICON_LINE]);
  }
  assert.equal(at(pattern, 64, 256)[3], 0, "the middle of a lane is transparent");
  assert.equal(at(pattern, 18, 256)[3], 255, "the left edge line");
  assert.ok(summarizeIcon(pattern).coverage < 0.2, "line art, not a filled tile");
  assert.ok(at(rendered[0], 256, 256)[3] === 255 && at(rendered[0], 200, 256)[0] !== PATTERN_ICON_LINE[0], "a road entry's icon is still the filled deck");
  const png = encodePng(rendered[0].rgba, rendered[0].size, rendered[0].size);
  assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
  assert.equal(png.readUInt32BE(16), rendered[0].size);
  // Negative: the module-era golden (schema version 1) is refused rather than compared.
  const dir = mkdtempSync(join(tmpdir(), "road-kit-icons-"));
  try {
    const old = join(dir, "icons.json");
    writeFileSync(old, JSON.stringify({ ...golden, schemaVersion: 1 }));
    assert.throws(() => loadRoadKitIcons(old), /Unsupported road kit icons document mki-roads\.icons v1/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test("the icon rasterizer's per-sample path avoids array destructuring, which crashes Node 20 (V8 11.3) on Windows", () => {
  // Millions of `const [x, y] = point` / `([p, q]) =>` evaluations per 512 px icon intermittently crashed Node 20.20.2
  // ("Check failed: fixed_size_above_fp ...", "Array Iterator.next called on incompatible receiver", or a bare segfault; #91).
  const source = readFileSync(resolve(repositoryRoot, "tools", "build-tools", "src", "road-pack-icons.ts"), "utf8");
  const start = source.indexOf("  const samples = SUPERSAMPLE * SUPERSAMPLE, rgba = new Uint8Array(size * size * 4);");
  const loop = source.slice(start, source.indexOf("  return { iconPath: spec.iconPath", start));
  assert.ok(loop.length > 200, "sample loop not found");
  assert.doesNotMatch(loop, /(?:const|let)\s*\[|\(\s*\[|=>/, "the sample loop uses array destructuring or a closure");
  assert.doesNotMatch(loop, /\.(?:some|map|every|find|reduce|forEach)\(/, "the sample loop calls an array callback per sample");
});

test("no base-game or packaged asset files are committed and the plugin content does not mirror /Game", () => {
  const forbidden = /\.(uasset|umap|uexp|ubulk|pak|ucas|utoc|fbx|blend|blend1)$/i, skipped = new Set([".git", "node_modules", ".local", ".worktrees", "dist"]);
  const offenders: string[] = [];
  const walk = (directory: string) => {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      if (skipped.has(entry.name)) continue;
      const path = resolve(directory, entry.name);
      if (entry.isDirectory()) walk(path); else if (forbidden.test(entry.name)) offenders.push(relative(repositoryRoot, path));
    }
  };
  walk(repositoryRoot);
  assert.deepEqual(offenders, []);
  // Plugin content is generated in the SDK checkout by the import step; base-game placeholders are never copied into a plugin.
  for (const plugin of [bridgePluginDirectory, roadPackPluginDirectory]) {
    assert.ok(!existsSync(resolve(plugin, "Content", "FactoryGame")), `${plugin} content must not contain base-game placeholder copies`);
  }
  // #180: the Blender pipeline and its geometry golden are gone with the modular pieces.
  assert.ok(!existsSync(resolve(roadPackDirectory, "blender")));
  assert.ok(!existsSync(resolve(roadPackDirectory, "generated", "road-kit.geometry.json")));
});

const sdkDirectory = process.env.SATISFACTORY_SDK_DIR ?? "D:/SatisfactoryModding/SatisfactoryModLoader";
test("registered placeholders exist in the local SML starter project", { skip: existsSync(resolve(sdkDirectory, "FactoryGame.uproject")) ? false : `no SDK at ${sdkDirectory}` }, () => {
  for (const placeholder of roadKit().placeholders) {
    const file = resolve(sdkDirectory, "Content", `${placeholder.path.slice("/Game/".length).split(".")[0]}.uasset`);
    assert.ok(existsSync(file), `${placeholder.path} -> ${file}`);
  }
  for (const item of roadKit().package.cost.perFoundation) {
    const file = resolve(sdkDirectory, "Content", `${item.item.slice("/Game/".length).split(".")[0]}.uasset`);
    assert.ok(existsSync(file), `${item.item} -> ${file}`);
  }
});

test("the SDK's FGBuildable refunds the built-with recipe times its multiplier", { skip: existsSync(resolve(sdkDirectory, "FactoryGame.uproject")) ? false : `no SDK at ${sdkDirectory}` }, () => {
  // package.cost.dismantleRefund "built-with-recipe" rests on this behaviour; the spline road overrides the multiplier with its
  // equivalent foundations, and the editor verify checks its Blueprints add no refund override.
  const source = readFileSync(resolve(sdkDirectory, "Source", "FactoryGame", "Private", "Buildables", "FGBuildable.cpp"), "utf8").replace(/\r\n/g, "\n");
  assert.match(source, /int32 AFGBuildable::GetDismantleRefundReturnsMultiplier\(\) const\n\{\n\treturn 1;\n\}/);
  assert.match(source, /GetDismantleRefundReturnsMultiplier\(\);\n\n\tfor\( const auto& cost : UFGRecipe::GetIngredients\( this, GetBuiltWithRecipe\(\) \) \)/);
});

test("road intersections: 2-, 3-, and 4-way pieces with 8 m aprons and square corners with a painted curb, joined by road ends (#227, #228, #230, #314)", () => {
  const kit = roadKit(), entries = roadJunctionEntries(kit);
  // The maintainer: one entry per arm count, Plus and Minus for 1, 2, or 4 lanes, after the roads in MKI Packs > Roads.
  // #228 (the maintainer): a 2-way as well, a corner for tight turns.
  assert.deepEqual(entries.map((item) => [item.displayName, item.arms, item.menuPriority]), [["Intersection (2-way)", 2, 88.1], ["Intersection (3-way)", 3, 88.11], ["Intersection (4-way)", 4, 88.12], ["Turnaround", 1, 88.13], ["Truck Depot", 7, 88.14], ["Truck Depot (Both Sides)", 9, 88.15]]);
  assert.match(readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Private", "MKIRoadJunction.cpp"), "utf8"), /TArray<FVector2D> Directions = \{ FVector2D\(1\.0, 0\.0\) \};\n\t+if \(Arms >= 3\) Directions\.Add\(FVector2D\(-1\.0, 0\.0\)\);/);
  for (const item of entries) {
    assert.equal(item.parentClass, "/Script/MKIRoads.MKIRoadJunction");
    assert.equal(item.hologramClass, "/Script/MKIRoads.MKIRoadJunctionHologram");
    assert.deepEqual(item.buildMenu.subCategory, kit.package.splineRoad.subCategory);
  }
  assert.deepEqual(roadPackEditorPlan(kit).splineRoad.junctions, entries);
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const junction = read("Private/MKIRoadJunction.cpp"), header = read("Public/MKIRoadJunction.h"), hologram = read("Private/MKIRoadJunctionHologram.cpp");
  const road = read("Private/MKIRoadSplineRoadHologram.cpp"), pattern = read("Private/MKIRoadMarkingPattern.cpp");
  // The shape of the maintainer's intersection (measured in the save): an 8 m apron per arm and an 8 m right triangle per corner.
  assert.match(header, /static constexpr float ApronCm = 800\.f;/);
  // #255 (the maintainer): no 1-lane variants, trucks acted oddly on them.
  assert.match(header, /static bool IsSupportedLaneCount(int32 Lanes) { return Lanes == 2 || Lanes == 4; }/);
  assert.match(read255("Public/MKIRoadSplineRoad.h"), /static constexpr int32 MinLanes = 2;/);
  assert.match(read255("Public/MKIRoadSplineRoadHologram.h"), /static constexpr int32 StepLaneCounts\[\] = \{ 2, 4 \};/);
  assert.match(junction, /if \(Arms >= 4\) Directions\.Add\(FVector2D\(0\.0, 1\.0\)\);/);
  // #314 (the maintainer: every foundation makes squares): the tile between two arms is paved whole, and the curb return of #230
  // (the apron's 8 m about the outer corner, #232) is painted across it, meeting the arms' edge lines; no edge line round it.
  assert.match(junction, /Square\(FMath::Min\(SX \* Half, SX \* Reach\), FMath::Min\(SY \* Half, SY \* Reach\)\);\s*Out\.CellIsCorner\.SetNum\(Out\.Cells\.Num\(\) - 1, false\);\s*Out\.CellIsCorner\.Add\(true\);/);
  assert.match(header, /static constexpr float CurbRadiusCm = 800\.f;/);
  // #316: white beyond the curb, out to the tile's edges (a filled quarter circle widened by the trim).
  assert.match(junction, /const float Radius = CurbRadiusCm \+ Trim;\s*TArray<FVector2D> Corner = \{ Outer \};/);
  assert.match(junction, /if \(AddFan\(Paint\[1\], Corner\)\) \+\+Count;/);
  assert.match(junction, /if \(Shape\.CellIsCorner\.IsValidIndex\(Shape\.OutlineCells\[EdgeIndex\]\) && Shape\.CellIsCorner\[Shape\.OutlineCells\[EdgeIndex\]\]\) continue;/);
  // #316: every other edge gets a trim from the edge in, not a line near it.
  assert.match(junction, /AddQuad\(Paint\[bPad \? 0 : 1\], \(P \+ Q\) \* 0\.5 \+ Normal \* \(Trim \* 0\.5f\), Along, static_cast<float>\(\(Q - P\)\.Size\(\)\), Trim\);/);
  // Negative (#314): no rounded deck cell or strip collision remains.
  assert.doesNotMatch(junction, /Corner\.Add\(Outer - FVector2D|bStraights|ToOuter/);
  assert.match(junction, /const FVector2D Out2D = OutwardNormal\(Layout\.Cells\[Layout\.OutlineCells\[EdgeIndex\]\], P, Q\);/);
  assert.match(junction, /return Twice >= 0\.0 \? Right : -Right;/);
  assert.doesNotMatch(junction, /Centroid\(Layout\.Cells|Centroid\(Shape\.Cells/, "negative: no vertex-average inside test");
  assert.doesNotMatch(junction, /Out\.Cells\.Add\(\{ Inner, FVector2D\(SX \* Reach, SY \* Half\), FVector2D\(SX \* Half, SY \* Reach\) \}\);/, "no straight cut corner");
  // The outline is every cell edge no other cell shares; arm ends get no edge line.
  assert.match(junction, /if \(Uses\.FindRef\(EdgeKey\(P, Q\)\) != 1\) continue;/);
  assert.match(junction, /if \(Shape\.OutlineIsArmEnd\[EdgeIndex\]\) continue;/);
  // Cost: one asphalt 8x4 foundation per started 64 m2, as the road.
  assert.match(junction, /return FMath::Max\(1, FMath::CeilToInt32\(LayoutAreaCm2\(Layout\(Arms, Lanes\)\) \/ AMKIRoadSplineRoad::FoundationAreaCm2 - 1e-6\)\);/);
  assert.match(junction, /return FoundationEquivalents\(mArms, mLanes\);/);
  // Collision is its squares' now (the maintainer, 2026-10-18): real foundations under the deck, spawned next tick.
  assert.match(junction, /if \(HasAuthority\(\)\) World->GetTimerManager\(\)\.SetTimerForNextTick\(FTimerDelegate::CreateWeakLambda\(this, \[this\]\(\) \{ SyncTiles\(\); \}\)\);/);
  assert.doesNotMatch(junction, /mCollisionBoxes/);
  // Road ends snap to the middle of an arm's end, heading out, locked; a road leaving an arm takes its width.
  assert.match(road, /if \(Junction->NearSide\(HitResult\.Location, OnSide, Outward, bArm\) && bArm && !Outward\.IsNearlyZero\(\)\)/);
  assert.match(road, /const int32 SourceLanes = Aim\.Junction\.IsValid\(\) \? Aim\.JunctionLanes :/);
  // The hologram: an arm joins an aimed road end (scroll picks it), the width follows the road's; Plus/Minus step 1, 2, 4.
  assert.match(hologram, /SetActorTransform\(FaceSideToEnd\(ArmCount\(\), mLanes, mQuarterTurns, End, Outward\.GetSafeNormal2D\(\)\)\);/);
  assert.match(hologram, /SetLanes\(AMKIRoadSplineRoadHologram::StepLanes\(mLanes, 1\)\);/);
  assert.match(hologram, /return AMKIRoadJunction::FoundationEquivalents\(ArmCount\(\), mLanes\);/);
  // The maintainer: it could not be placed on foundations; its top sat on theirs, the deck inside them. It rests on top, as a road.
  assert.match(hologram, /OnGrid\(Local\.Y, HalfY\), TopZ\)\) \+ FVector\(0\.f, 0\.f, DeckThicknessCm\(\)\);/);
  assert.match(hologram, /const FVector Free = HitResult\.Location \+ FVector\(0\.f, 0\.f, DeckThicknessCm\(\)\);/);
  assert.match(hologram, /SetActorLocationAndRotation\(Free, FRotator\(0\.f, FRotator::NormalizeAxis\(View \+ static_cast<float>\(mRotateStepDeg \* mRotateNotches\)\), 0\.f\)\);/);
  // Negative: a road or junction deck is not a foundation grid for it.
  assert.match(hologram, /!AMKIRoadJunction::FromActor\(HitActor\) && !Cast<AMKIRoadSplineRoad>\(HitActor\)/);
  // #229 (the maintainer, a North American intersection photo): each arm gets a crosswalk, a stop line across the incoming lanes
  // (traffic arriving drives on the arm's left), and the double yellow line out to its end; none on a one-lane arm.
  assert.match(junction, /const float StopFrom = -Half \+ CrosswalkSideInsetCm, StopTo = Lanes >= 2 \? -\(PairOffset \+ LineWidthCm \* 0\.5f\) : Half - CrosswalkSideInsetCm;/);
  assert.match(junction, /if \(Lanes >= 2\)\n\t\{\n\t\tfor \(const float Side : \{ -1\.f, 1\.f \}\) Out\.Add\(\{ Middle, Side \* PairOffset, Run, LineWidthCm, true \}\);/);
  assert.match(junction, /Out\.Add\(\{ \(CrosswalkFrom \+ CrosswalkTo\) \* 0\.5f, Lateral, CrosswalkDepthCm, BarWidthCm, false \}\);/);
  assert.match(header, /static constexpr float CrosswalkDepthCm = 300\.f;/);
  // The road marking pattern applies to junctions too.
  assert.match(pattern, /mValidBuildables = \{ AMKIRoadSplineRoad::StaticClass\(\), AMKIRoadJunction::StaticClass\(\), AMKIRoadTile::StaticClass\(\) \};/);
  // Negative: an arm count other than 3 or 4 is refused by the manifest.
  const raw = JSON.parse(readFileSync(resolve(roadPackDirectory, "modules.json"), "utf8"));
  const changed = structuredClone(raw);
  changed.package.splineRoad.junctions.entries[0].arms = 5;
  assert.throws(() => parseRoadKitDocument(changed), /arms/);
  changed.package.splineRoad.junctions.entries[0].arms = 1;
  assert.throws(() => parseRoadKitDocument(changed), /arms/);
});

test("roads and intersections lay their own Universal vehicle paths, joined at shared ends (#235)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const paths = read("Private/MKIRoadAutoPaths.cpp"), road = read("Private/MKIRoadSplineRoad.cpp"), junction = read("Private/MKIRoadJunction.cpp");
  // The maintainer: Universal paths, and every move at an intersection (straight, right, left).
  assert.match(paths, /Build_VehiclePath_Universal\.Build_VehiclePath_Universal_C/);
  // Traffic keeps right: lanes right of the centerline run forward, left of it back; a lone center lane both ways.
  assert.match(paths, /OutPaths\.Add\(Lateral > 0\.f \? LaneLine\(\*Spline, Lateral, 0\.f, Length\) : LaneLine\(\*Spline, Lateral, Length, 0\.f\)\);/);
  // Every arm to every other arm, lane k to lane k; arriving lanes left of the arm's outward direction, leaving right.
  assert.match(paths, /if \(From == To\) continue;/);
  assert.match(paths, /const float Lateral = \(Lane \+ 0\.5f\) \* AMKIRoadSplineRoad::LaneWidthCm \* \(bLeaving \? 1\.f : -1\.f\);/);
  // #256: no one-lane branches left (2 or 4 lanes only).
  assert.doesNotMatch(paths, /Lanes == 1/);
  assert.match(paths, /Magnitude = 4\.0 \* FMath::Tan\(Theta \* 0\.25\) \* Radius;/);
  // Ends join existing nodes (the joining road's or intersection's), so sections added later connect.
  // Live: the vanilla OverlapPathNode found nothing, so no ends joined; the nodes are searched directly, and paths laid by
  // an older version are laid again.
  assert.match(paths, /for \(TActorIterator<AFGVehiclePathNode> It\(World\); It; \+\+It\)/);
  assert.doesNotMatch(paths, /OverlapPathNode/);
  assert.match(readFileSync(resolve(source, "Public", "MKIRoadAutoPaths.h"), "utf8"), /static constexpr int32 Version = 9;/);
  assert.match(paths, /Segment->SetNodeConnections\(FindNode\(Path\[0\]\.Location\), FindNode\(Path\.Last\(\)\.Location\)\);/);
  // Laid once (saved roads too, the first time they load), a tick after BeginPlay, on the authority; removed with the road.
  for (const [text, name] of [[road, "road"], [junction, "junction"]] as const) {
    // Old paths are all removed in BeginPlay, before any road or intersection lays new ones next tick.
    // Live (#239): removed by the vanilla dismantle a tick later (Destroy() left other nodes linked to them, and a later save
    // crashed on load), stale node links dropped, and laid the tick after.
    assert.match(text, /for \(AFGVehiclePathSegment\* Old : mAutoPaths\) if \(IsValid\(Old\) && !Old->IsActorBeingDestroyed\(\)\) IFGDismantleInterface::Execute_Dismantle\(Old\);\s*mAutoPaths\.Reset\(\);[\s\S]*?FMKIRoadAutoPaths::CleanStaleNodeLinks\(GetWorld\(\)\);\s*GetWorld\(\)->GetTimerManager\(\)\.SetTimerForNextTick\(FTimerDelegate::CreateWeakLambda\(this, \[this\]\(\) \{ LayAutoPaths\(\); \}\)\);/, name);
    assert.doesNotMatch(text, /Old->Destroy\(\)/, name);
    assert.doesNotMatch(text.slice(text.indexOf("::LayAutoPaths()")), /Old->Destroy\(\)/, name);
    // #244: removed by the piece itself (live: as dismantle children the vehicle network crashed), and highlighted with it.
    assert.match(text, /for \(AFGVehiclePathSegment\* Path : mAutoPaths\) if \(IsValid\(Path\) && !Path->IsActorBeingDestroyed\(\)\) IFGDismantleInterface::Execute_Dismantle\(Path\);/, name);
    assert.doesNotMatch(text, /OutChildDismantleActors\.AddUnique\(Path\)/, name);
    assert.match(text, /Path->TogglePendingDismantleMaterial\(true\);/, name);
  }
  // Negative: no recipe is set on the paths, so they cost and refund nothing.
  assert.doesNotMatch(paths, /SetBuiltWithRecipe/);
});
test("turnaround: a round road end looping each arriving lane back to its leaving lane, with markings (#236)", () => {
  const kit = roadKit(), entry = roadJunctionEntries(kit).find((item) => item.arms === 1);
  // The maintainer: the least space that loops a 1, 2, or 4 lane road's incoming traffic back to its outgoing lanes, with markings.
  assert.ok(entry && entry.displayName === "Turnaround" && /loops each incoming lane back into its outgoing lane/.test(entry.description));
  assert.ok(roadJunctionEntries(kit).filter((item) => item.arms >= 2 && item.arms <= 4).every((item) => item.description === kit.package.splineRoad.junctions.description), "intersections keep theirs");
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const junction = readFileSync(resolve(source, "Private", "MKIRoadJunction.cpp"), "utf8"), header = readFileSync(resolve(source, "Public", "MKIRoadJunction.h"), "utf8");
  const paths = readFileSync(resolve(source, "Private", "MKIRoadAutoPaths.cpp"), "utf8");
  assert.match(header, /static constexpr float TurnLoopRadiusCm = 1200\.f;/);
  assert.match(header, /static constexpr float TurnVeerRadiusCm = 800\.f;/);
  assert.match(junction, /if \(IsTurnaroundShape\(Arms\)\) TurnaroundCells\(Out, Count\);/);
  // The deck (#314): whole tiles from the arm end back, the least grid square that holds the round end; each gets a
  // clearance box. Negative: no round hull is cut into partial tiles any more.
  assert.match(junction, /const int32 Back = FMath::CeilToInt32\(\(ArmEnd \+ Radius\) \/ Tile - 1e-3\);/);
  assert.match(junction, /const int32 Side = FMath::Max\(0, FMath::CeilToInt32\(\(Radius - Half\) \/ Tile - 1e-3\)\);/);
  assert.match(junction, /if \(!IsWholeTile\(Cell\)\) continue;/);
  assert.doesNotMatch(junction, /ClipAxis|TArray<FVector2D> Hull = ConvexHull\(Points\);\s*const FBox2D Box\(Hull\);/);
  // #316: white outside the round deck it had (the hull, drawn in by the trim), out to the rectangle's edge.
  assert.match(junction, /Radius = TurnaroundDeckRadiusCm\(Lanes\) - Trim;/);
  assert.match(junction, /const FVector2D OutA = RayToBox\(Box, A\), OutB = RayToBox\(Box, B\);/);
  // Paths: two halves per arriving lane (no path starts and ends on one node); no crosswalk or stop line, an island ring.
  assert.match(paths, /if \(Junction\.GetArmCount\(\) == 1\)[\s\S]*AMKIRoadJunction::TurnaroundLoops\(Lanes, Halves\);/);
  assert.match(junction, /OutHalves\.Add\(MoveTemp\(In\)\);\s*OutHalves\.Add\(MoveTemp\(Back\)\);/);
  assert.match(junction, /\(Shape\.bTurnaround \|\| Shape\.bTruckDepot \? TArray<FMKIRoadJunctionArm>\(\) : Shape\.Arms\)/);
  assert.match(junction, /const float Island = TurnaroundIslandRadiusCm\(\) - Pattern->LineWidthCm \* 0\.5f;/);

  // The loop geometry, as TurnaroundLoops lays it: continuous, smooth, back on the leaving lane, and on the deck.
  const lane = 800, loop0 = 1200, veer = 800;
  for (const lanes of [1, 2, 4]) {
    const items = Array.from({ length: Math.max(1, lanes / 2) }, (_, k) => {
      const lateral = lanes === 1 ? 0 : (k + 0.5) * lane, loop = loop0 + k * lane;
      return { lateral, loop, x1: Math.sqrt((loop + veer) ** 2 - (lateral + veer) ** 2) };
    });
    const armEnd = Math.ceil(Math.max(...items.map((item) => item.x1)) / 100) * 100, deck = Math.max(...items.map((item) => item.loop)) + lane / 2 + 50;
    for (const { lateral: l, loop: r, x1 } of items) {
      const points: [number, number, number][] = [];
      const arc = (cx: number, cy: number, sx: number, sy: number, sweep: number) => {
        for (let step = 0; step <= 64; step++) {
          const a = sweep * step / 64, vx = sx * Math.cos(a) - sy * Math.sin(a), vy = sx * Math.sin(a) + sy * Math.cos(a);
          points.push([cx + vx, cy + vy, sweep > 0 ? Math.atan2(vx, -vy) : Math.atan2(-vx, vy)]);
        }
      };
      points.push([armEnd, -l, Math.PI], [(armEnd + x1) / 2, -l, Math.PI]);
      const vcx = x1, vcy = -(l + veer), fx = -vcx / (r + veer), fy = -vcy / (r + veer);
      const veerSweep = Math.atan2(0 * fy * veer - veer * fx * veer, veer * fy * veer);
      arc(vcx, vcy, 0, veer, veerSweep);
      const tx = -fx * r, ty = -fy * r, alpha = Math.abs(Math.atan2(ty, tx));
      arc(0, 0, tx, ty, -(2 * Math.PI - 2 * alpha));
      arc(x1, l + veer, fx * veer, -fy * veer, veerSweep);
      points.push([(armEnd + x1) / 2, l, 0], [armEnd, l, 0]);
      for (let index = 1; index < points.length; index++) {
        const [ax, ay] = points[index - 1], [bx, by] = points[index];
        assert.ok(Math.hypot(bx - ax, by - ay) < 400, `${lanes} lanes: continuous at ${index}`);
        const turn = Math.abs(Math.atan2(Math.sin(points[index][2] - points[index - 1][2]), Math.cos(points[index][2] - points[index - 1][2])));
        assert.ok(turn < 0.2, `${lanes} lanes: smooth at ${index} (${turn})`);
        assert.ok(Math.hypot(bx, by) <= Math.max(deck, Math.hypot(armEnd, lanes * lane / 2)) + 1, `${lanes} lanes: on the deck`);
      }
      const last = points[points.length - 3];
      assert.ok(Math.abs(last[0] - x1) < 1 && Math.abs(last[1] - l) < 1, `${lanes} lanes: veers back into the leaving lane`);
      assert.ok(Math.min(...points.map(([x]) => x)) < -r + 1, "round the far side of the loop");
    }
  }
});

test("truck depots: one side or both, each joined to its stations (#237, #240, #250, #256)", () => {
  const kit = roadKit(), entries = roadJunctionEntries(kit).filter((item) => item.arms >= 6);
  const read0 = (file: string) => readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", file), "utf8");
  // The maintainer: depots that include the road and paths (#237); a small footprint (#239); a small depot and a large one with a
  // bay, each also on both sides of the road, and a depot at the end of a turnaround (#240).
  assert.deepEqual(entries.map((item) => [item.displayName, item.arms]), [["Truck Depot", 7], ["Truck Depot (Both Sides)", 9]]);
  // #251 (the maintainer): the small depots are gone (a fluid station on one blocked cargo trucks with no feedback); one Truck Depot.
  assert.ok(!roadJunctionEntries(kit).some((item) => (item.arms as number) === 6 || (item.arms as number) === 8), "negative: no small depots");
  // #250: the turnaround depot is gone; a small depot and a turnaround build it.
  assert.ok(!roadJunctionEntries(kit).some((item) => (item.arms as number) === 10 || /Turnaround Depot/.test(item.displayName)), "negative: no turnaround depot");
  assert.doesNotMatch(read0("Public/MKIRoadJunction.h"), /TurnaroundDepotShape/);
  assert.ok(entries.every((item) => /Truck Station or Fluid Truck Station aimed at it snaps onto the nearest dock/.test(item.description)));
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const junction = read("Private/MKIRoadJunction.cpp"), header = read("Public/MKIRoadJunction.h"), paths = read("Private/MKIRoadAutoPaths.cpp");
  const snap = read("Private/MKIRoadBuildSnap.cpp"), subsystem = read("Private/MKIRoadWorldSubsystem.cpp"), hologram = read("Private/MKIRoadJunctionHologram.cpp");
  assert.match(header, /static constexpr float DepotHalfLengthCm = 2400\.f;/);
  assert.match(header, /static constexpr float TruckBayBendRunCm = 1700\.f;/);
  // #256: every depot takes 2 or 4 lanes, as every road piece.
  assert.match(hologram, /if \(!AMKIRoadJunction::IsSupportedLaneCount\(Lanes\) \|\| Lanes == mLanes\) return;/);
  assert.match(junction, /if \(IsTurnaroundShape\(Arms\)\) TurnaroundCells\(Out, Count\);\s*else if \(IsDepotShape\(Arms\)\) TruckDepotCells\(Out, Count, Arms\);/);
  // Docks: the bay's center, mirrored on -Y (both sides); the station turned so its building stands outside the deck.
  assert.match(junction, /const double Y = Half \+ Width \* 0\.5;\s*OutDocks\.Add\(\{ FVector2D\(0\.0, Y\), 180\.f \}\);\s*if \(IsBothSidesShape\(Arms\)\) OutDocks\.Add\(\{ FVector2D\(0\.0, -Y\), 0\.f \}\);/);
  assert.match(junction, /return Loop \+ AMKIRoadSplineRoad::LaneWidthCm \* 0\.5f \+ 50\.f;/);
  // Paths: the through lanes whole; bays on two arcs, mirrored for -Y; the turnaround's loop halves.
  assert.match(paths, /if \(Junction\.IsTruckDepot\(\)\)[\s\S]*AMKIRoadJunction::DepotPaths\(Junction\.GetArmCount\(\), Lanes, Local\);/);
  assert.match(junction, /if \(Dock\.Point\.Y < 0\.0\) for \(TArray<TPair<FVector2D, FVector2D>>\* Path : \{ &In, &ToDock, &FromDock, &Out \}\)/);
  // A station on a dock: that dock's paths laid again onto its docking node, once per dock; removed, laid again on their own.
  assert.match(junction, /if \(mDocksJoined & \(1 << Index\)\) continue;/);
  assert.match(junction, /if \(bOurs\) RelayDockLane\(Index\);\s*mDocksJoined \|= 1 << Index;/);
  assert.doesNotMatch(junction, /->MoveConnectionsToNode\(|mBayPathTimer|BayPathCheckSeconds/, "negative: no node hand-over, no periodic re-lay");
  assert.match(junction, /if \(!TouchesDock\(Segment\)\) continue;[^;]*IFGDismantleInterface::Execute_Dismantle\(Segment\);/);
  assert.match(junction, /void AMKIRoadJunction::OnDockStationRemovedAt\(const FVector& Location\)[^}]*RelayDockLane\(Dock\);/);
  assert.match(junction, /const bool bJoined = DockNode->GetArrivingConnections\(\)\.Num\(\) > 0 && DockNode->GetLeavingConnections\(\)\.Num\(\) > 0;/);
  assert.match(subsystem, /for \(const float Delay : \{ 0\.5f, 2\.f \}\)/);
  assert.match(subsystem, /It->IsTruckDepot\(\) && It->FindDepotDock\(Location, 200\.f\) != INDEX_NONE\) It->LayBayPaths\(\);/);
  assert.match(subsystem, /It->IsTruckDepot\(\) && It->FindDepotDock\(Location, 200\.f\) != INDEX_NONE\) It->OnDockStationRemovedAt\(Location\);/);
  // Live: saved path networks still listed nodes removed with Destroy(), and loads crashed rebuilding them.
  assert.match(read("Private/MKIRoadAutoPaths.cpp"), /RemoveAll\(\[\]\(const AFGVehiclePathSegment\* Segment\) \{ return !IsValid\(Segment\) \|\| Segment->IsActorBeingDestroyed\(\); \}\)/);
  assert.match(subsystem, /RemoveAll\(\[\]\(const AFGVehiclePathNode\* Node\) \{ return !IsValid\(Node\); \}\)/);
  assert.match(subsystem, /mBuildableRemovedDelegate\.AddDynamic\(this, &UMKIRoadWorldSubsystem::OnBuildableRemoved\);/);
  // The station snaps to the nearest dock (on the depot or on its paths, a path's collision about 2.5 m up); a road keeps
  // its own edge snap (#201).
  assert.match(snap, /const int32 Dock = Depot->FindDepotDock\(HitResult\.Location, TNumericLimits<float>::Max\(\)\);/);
  assert.match(snap, /Local\.Z >= -100\.0 && Local\.Z <= 600\.0\) Depot = \*It;/);
  assert.match(snap, /if \(!Road \|\| !SnapDockingStation\(\*Road, HitResult\.Location, HitResult\.ImpactNormal, Snapped\)\) return false;/);

  // Geometry: the station's building (its -Y, 4 m to 13 m from its dock) starts at the deck's edge on every variant.
  for (const lanes of [1, 2, 4]) {
    const half = lanes * 400, small = lanes === 1 ? 0 : half - 400, bay = half + 400;
    assert.equal(small + 400, half, `${lanes} lanes, small: the building starts at the deck's edge`);
    assert.equal(bay + 400, half + 800, `${lanes} lanes, large: the building starts at the bay's edge`);
    // The bay's two arcs (17 m run, the maintainer) meet halfway across.
    const outer = lanes === 1 ? 0 : half - 400, run = 1700, r = (run * run + (bay - outer) ** 2) / (4 * (bay - outer)), theta = Math.asin(run / (2 * r));
    assert.ok(Math.abs(outer + r * (1 - Math.cos(theta)) - (outer + bay) / 2) < 1 && Math.abs(2 * r * Math.sin(theta) - run) < 1, `${lanes} lanes: the bay bends fit`);
  }
});
test("every road piece snaps to the others and takes their lanes, once per piece (#238)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const hologram = readFileSync(resolve(source, "Private", "MKIRoadJunctionHologram.cpp"), "utf8"), road = readFileSync(resolve(source, "Private", "MKIRoadSplineRoadHologram.cpp"), "utf8");
  // The maintainer: a turnaround did not snap to an intersection; every piece should snap to every other and take its lanes.
  assert.match(hologram, /if \(const AMKIRoadJunction\* Other = AMKIRoadJunction::FromActor\(HitActor\)\)[\s\S]*if \(bArm\) TakeLanesFrom\(Other, Other->GetLanes\(\)\);[\s\S]*SetActorTransform\(FaceSideToEnd\(ArmCount\(\), mLanes, mQuarterTurns, OnSide, Outward\)\);/);
  assert.match(hologram, /TakeLanesFrom\(Road, Road->GetLanes\(\)\);/);
  assert.match(hologram, /if \(Source == mLanesTakenFrom\.Get\(\)\) return;/);
  // A road from a road end takes that road's lanes, from an arm the piece's; once per piece, so Plus and Minus still work.
  assert.match(road, /const int32 SourceLanes = Aim\.Junction\.IsValid\(\) \? Aim\.JunctionLanes : \(Aim\.Road\.IsValid\(\) \? Aim\.Road->GetLanes\(\) : 0\);/);
  assert.match(road, /if \(Source != mLanesTakenFrom\.Get\(\)\)/);
  // Negative: an unsupported lane count is never taken.
  assert.match(road, /if \(Source && SourceLanes != mLanes && AMKIRoadSplineRoad::IsSupportedLaneCount\(SourceLanes\)\) mLanes = SourceLanes;/);
  assert.match(hologram, /void AMKIRoadJunctionHologram::SetLanes\(int32 Lanes\)\s*\{[^}]*if \(!AMKIRoadJunction::IsSupportedLaneCount\(Lanes\)/);
});

test("depot station pads, marked holograms, and whole-piece dismantle (#241)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const junction = read("Private/MKIRoadJunction.cpp");
  const junctionHologram = read("Private/MKIRoadJunctionHologram.cpp"), roadHologram = read("Private/MKIRoadSplineRoadHologram.cpp");
  // The maintainer: asphalt foundations under where the station goes: a pad of deck under its footprint beside each dock.
  assert.match(junction, /const FVector2D Min\(-2\.0 \* AMKIRoadSplineRoad::LaneWidthCm, -400\.0 - AMKIRoadSplineRoad::LaneWidthCm\), Max\(2\.0 \* AMKIRoadSplineRoad::LaneWidthCm, -400\.0\);/);
  assert.match(junction, /const TArray<FVector2D> Corners = AMKIRoadJunction::DepotPadCorners\(Dock\);/);
  // The maintainer: the North American Road markings on every road piece's hologram, so its orientation shows.
  assert.match(junctionHologram, /AMKIRoadJunction::MarkingGeometry\(ArmCount\(\), mLanes, Pattern, Paint\);\s*MKIRoadApplyPaint\(mPreviewMarkings, \*Pattern, Paint\);/);
  assert.match(roadHologram, /AMKIRoadSplineRoad::MarkingGeometry\(\*Along, mLanes, \*Pattern, Paint\);\s*MKIRoadApplyPaint\(mPreviewMarkings, \*Pattern, Paint\);/);
  assert.match(roadHologram, /Along = mStubSpline;/);
  // #248 (the maintainer): paths stay individual items for dismantling and quick copy; the selection redirect is gone.
  assert.ok(!existsSync(resolve(source, "Private", "MKIRoadDismantleSelect.cpp")), "negative: no whole-piece selection");
});

test("a station dismantled off a depot does not crash the vehicle network (#241)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const paths = readFileSync(resolve(source, "Private", "MKIRoadAutoPaths.cpp"), "utf8"), subsystem = readFileSync(resolve(source, "Private", "MKIRoadWorldSubsystem.cpp"), "utf8");
  // Live: re-laid the tick after a fluid station was dismantled, a dock's paths joined its dying docking node and the
  // vehicle network crashed rebuilding. The re-lay waits a second, and a docking node is joined only while its station stands.
  assert.match(paths, /if \(It->GetClass\(\)->GetName\(\)\.Contains\(TEXT\("DockingStation"\)\) && !HasStandingStation\(World, It->GetActorLocation\(\)\)\) continue;/);
  assert.match(paths, /if \(IsValid\(\*It\) && !It->IsActorBeingDestroyed\(\) && FVector::Dist\(It->GetActorLocation\(\), Location\) <= 2\.f \* JoinRadiusCm\) return true;/);
  assert.match(subsystem, /It->OnDockStationRemovedAt\(Location\);\s*\}\s*\}\), 1\.f, false\);/);
  assert.doesNotMatch(subsystem, /SetTimerForNextTick\(FTimerDelegate::CreateWeakLambda\(this, \[this, Location\]\(\)\s*\{\s*for \(TActorIterator<AMKIRoadJunction> It\(GetWorld\(\)\); It; \+\+It\)\s*\{\s*if \(It->IsTruckDepot\(\) && It->FindDepotDock\(Location, 200\.f\) != INDEX_NONE\) It->OnDockStationRemovedAt/, "negative: not the next tick");
});

test("no depot lays a bypass any more (#243, #251, #256)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp"), header = read255("Public/MKIRoadJunction.h");
  // The small depots that needed it are gone; the depot's through lanes pass its bays.
  assert.doesNotMatch(junction + header, /Bypass/);
  assert.match(junction, /\/\/ The through lanes, whole\.\n\tStraightLanePaths\(Count, static_cast<float>\(Reach\), OutPaths\);/);
});



test("removed path nodes leave the vehicle path networks each frame (#245)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const header = readFileSync(resolve(source, "Public", "MKIRoadWorldSubsystem.h"), "utf8"), subsystem = readFileSync(resolve(source, "Private", "MKIRoadWorldSubsystem.cpp"), "utf8");
  // Live: removing a road piece under an autodriving truck crashed the network rebuild on an already-collected node.
  assert.match(header, /class MKIROADS_API UMKIRoadWorldSubsystem : public UTickableWorldSubsystem/);
  assert.match(subsystem, /void UMKIRoadWorldSubsystem::Tick\(float DeltaTime\)[\s\S]*GetNetworkElementsOnServer\(\)\)\.RemoveAll\(\[\]\(const AFGVehiclePathNode\* Node\) \{ return !IsValid\(Node\); \}\)/);
  // Negative: clients never touch the networks.
  assert.match(subsystem, /if \(!World \|\| World->GetNetMode\(\) == NM_Client\) return;/);
});

test("depot station pads carry factory-zone stripes in orange paint (#246)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const junction = readFileSync(resolve(source, "Private", "MKIRoadJunction.cpp"), "utf8"), header = readFileSync(resolve(source, "Public", "MKIRoadJunction.h"), "utf8");
  // The maintainer: the pad under the stations should look like the Full Factory Zone pattern. Live: the vanilla pattern did not
  // show on the procedural pad, so the stripes are painted: 45 degree bands of the orange paint, clipped to each pad.
  assert.equal(roadKit().package.markings.paints.orange.material, "/MKIRoads/Materials/M_MKIR_RoadPaint_Orange.M_MKIR_RoadPaint_Orange");
  assert.match(header, /static constexpr float PadStripeWidthCm = 70\.f;/);
  assert.match(junction, /const TArray<FVector2D> Band = ClipHalfPlane\(ClipHalfPlane\(Rect, Across, From, false\), Across, From \+ PadStripeWidthCm, true\);/);
  assert.match(junction, /BuildDeck\(\);\s*BuildPadStripes\(\);/);
  assert.doesNotMatch(junction, /Recipe_Pattern_ZoneFull/, "negative: no vanilla pattern on the pad");
});

test("every dock has 5 m of straight path each side before any curve (#247, #250)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const junction = readFileSync(resolve(source, "Private", "MKIRoadJunction.cpp"), "utf8"), header = readFileSync(resolve(source, "Public", "MKIRoadJunction.h"), "utf8");
  // The maintainer: a path out of a station that went straight into a curve made the truck stutter and jump (vanilla too); 8 m
  // straight into and out of each station, then the curve, fixed it; #250: 5 m is enough.
  assert.match(header, /static constexpr float DockStraightCm = 700\.f;/);
  assert.match(junction, /AddLine\(ToDock, FVector2D\(-DockStraightCm, Bay\), FVector2D\(0\.0, Bay\)\);\s*AddLine\(FromDock, FVector2D\(0\.0, Bay\), FVector2D\(DockStraightCm, Bay\)\);/);
});

test("a depot's bends end on its ends, after 7 m dock straights (#250, #256)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp"), header = read255("Public/MKIRoadJunction.h");
  // The maintainer: the paths terminate right on the border to the next piece; 17 m bends, 7 m straights.
  assert.match(header, /static constexpr float TruckBayBendRunCm = 1700\.f;/);
  assert.match(junction, /static_assert\(DockStraightCm \+ TruckBayBendRunCm == DepotHalfLengthCm, "a bay's straight and bend fill half the depot"\);/);
  assert.match(junction, /AddArc\(In, FVector2D\(-Reach, Outer \+ R\), FVector2D\(0\.0, -R\), Theta\);/);
  assert.match(junction, /AddArc\(Out, FVector2D\(Reach, Outer \+ R\), FVector2D\(-R \* S, -R \* C\), Theta\);/);
  assert.doesNotMatch(junction + header, /DepotMergeStubCm|DepotBayMergeXCm|IsBayShape/, "negative: no merge inside the piece, no non-bay depot");
  // Geometry, 2 and 4 lanes: two arcs over the bay's 8 m offset run the 17 m.
  for (const lanes of [2, 4]) {
    const half = lanes * 400, outer = half - 400, bay = half + 400, run = 1700, r = (run * run + (bay - outer) ** 2) / (4 * (bay - outer)), theta = Math.asin(run / (2 * r));
    assert.ok(Math.abs(2 * r * Math.sin(theta) - run) < 1 && Math.abs(2 * r * (1 - Math.cos(theta)) - (bay - outer)) < 1, `${lanes} lanes: the bend fits`);
  }
});
function read(source: string, file: string): string { return readFileSync(resolve(source, file), "utf8"); }
test("a station snapped to a depot dock puts its own docking node on the dock (#249)", () => {
  const snap = readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Private", "MKIRoadBuildSnap.cpp"), "utf8");
  // Live: a fluid station's docking node stands 88 cm from its pivot, so it missed the dock and its paths did not join.
  assert.match(snap, /const FVector Offset = Station->GetDockingTargetNodeLocation\(\)->GetRelativeLocation\(\);\s*Placed\.AddToTranslation\(-Placed\.GetRotation\(\)\.RotateVector\(FVector\(Offset\.X, Offset\.Y, 0\.0\)\)\);/);
});
test("a depot's exit bend starts its own path facing forward (#250)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp");
  // Live: the exit path was red before any station; an empty line before the bend started it facing -X. No line now
  // precedes either bend.
  assert.match(junction, /TArray<TPair<FVector2D, FVector2D>> In, ToDock, FromDock, Out;\n\t\tAddArc\(In,/);
  assert.match(junction, /AddLine\(FromDock, FVector2D\(0\.0, Bay\), FVector2D\(DockStraightCm, Bay\)\);\n\t\tAddArc\(Out,/);
  assert.doesNotMatch(junction.slice(junction.indexOf("void AMKIRoadJunction::DepotPaths(")), /AddLine\(Out,|AddLine\(In,/);
});
function read255(file: string): string { return readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", file), "utf8"); }

test("road pieces turn in 45 degree steps on foundations and freely on the ground (#253)", () => {
  const hologram = read255("Private/MKIRoadJunctionHologram.cpp");
  // The maintainer: road pieces rotate in 45 degree steps on top of foundations, and freely on open ground.
  assert.match(hologram, /FRotator\(0\.f, Foundation->GetActorRotation\(\)\.Yaw \+ 45\.f \* mRotateNotches, 0\.f\)/);
  assert.match(hologram, /FRotator\(0\.f, FRotator::NormalizeAxis\(View \+ static_cast<float>\(mRotateStepDeg \* mRotateNotches\)\), 0\.f\)/);
  // Negative: at a road end the scroll still picks the joining arm in quarter turns.
  assert.match(hologram, /mQuarterTurns = \(\(mQuarterTurns \+ Notch\) % 4 \+ 4\) % 4;/);
  // Live (bridge): a scroll of -3 turned one notch; every notch a scroll carries counts.
  assert.match(hologram, /mRotateNotches \+= Delta;/);
  assert.doesNotMatch(hologram, /RoundToFloat\(View \/ 90\.f\)/, "negative: no snapping to the view's quarter heading");
});

test("a yellow trim, not a white line, round a depot's station pad (#254, #316)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp");
  // #254, the maintainer: no solid white line round the factory-zone pattern; #316: a yellow edge round it instead, over the stripes.
  assert.match(junction, /const bool bPad = Shape\.CellIsPad\.IsValidIndex\(Shape\.OutlineCells\[EdgeIndex\]\) && Shape\.CellIsPad\[Shape\.OutlineCells\[EdgeIndex\]\];/);
  assert.match(junction, /AddQuad\(Paint\[bPad \? 0 : 1\]/);
  assert.match(junction, /Out\.Vertices\.Add\(FVector\(Point\.X, Point\.Y, 0\.3f\)\);/, "the stripes sit under the markings");
  // #316 follow-up (the maintainer): the bay keeps a white edge in front of the pad, the pad a yellow one;
  // the bay's ends meet the joined road's trim at a right angle.
  // #318 (the maintainer): in front of the bay, white on the deck side (the bay's edge runs on unbroken) and yellow on the pad side.
  assert.match(junction, /AddQuad\(Paint\[1\], \(P \+ Q\) \* 0\.5 \+ Outward \* \(Trim \* 0\.5f\)[^\n]*\n\t+AddQuad\(Paint\[0\], \(P \+ Q\) \* 0\.5 - Outward \* \(Trim \* 0\.5f\)/);
  // The bay's ends are plain trim meeting the joined road's at a right angle (#318: the quarter circles replaced).
  assert.match(junction, /AddQuad\(Paint\[1\], FVector2D\(End \* \(Reach - Trim \* 0\.5f\), Side \* \(Half - Trim \* 0\.5f\)\), FVector2D\(1\.0, 0\.0\), Trim, Trim\);/);
  // Negative: the one-sided depot gets no fill on its plain side, whose own outline trim already lines up.
  assert.match(junction, /if \(Side < 0\.f && !IsBothSidesShape\(Arms\)\) continue;/);
});

test("road piece icons carry the road's lines, and the depot's its pad (#252)", () => {
  const icons = readFileSync(resolve(repositoryRoot, "tools", "build-tools", "src", "road-pack-icons.ts"), "utf8");
  // The maintainer: make the road parts' icons as illustrative as the Road's, with road lines.
  assert.match(icons, /function roadPieceLine\(x: number, y: number, arms: number, half: number, pair: number, dash: number\): number/);
  // #316: white trims to the edge, filled curb corners and turnaround surround.
  assert.match(icons, /const marking = curb \|\| surround \|\| stub \|\| \(!keepsNear && !atArmEnd && !corner\) \? 2 : roadPieceLine\(x, y, arms, half, pair, dash\);/);
  // #314: square corner tiles with the curb painted, and a square turnaround.
  assert.match(icons, /if \(arms === 1\) return ax <= 2 && ay <= 2;/);
  assert.match(icons, /function onDepotPad\(x: number, y: number, arms: number\): boolean/);
  // Negative: no small-depot shapes are drawn.
  assert.doesNotMatch(icons, /arms === 6|arms === 8/);
});

test("the depot's station pad runs its whole length, and vanilla foundations, walls, and railings snap round road pieces through their squares (#257, #258)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp"), snap = read255("Private/MKIRoadBuildSnap.cpp");
  // The maintainer: the station area as wide as the bay; foundations snap around the depot so players can extend it.
  assert.match(junction, /const FVector2D Min\(-2\.0 \* AMKIRoadSplineRoad::LaneWidthCm, -400\.0 - AMKIRoadSplineRoad::LaneWidthCm\), Max\(2\.0 \* AMKIRoadSplineRoad::LaneWidthCm, -400\.0\);/);
  // Since the foundation squares (the maintainer, 2026-10-18) the game snaps vanilla foundations, walls, railings and
  // barriers to the squares itself; the hand-made snaps to a piece's own box are gone.
  assert.doesNotMatch(snap, /SnapJunctionFoundation|SnapJunctionWall/);
  assert.doesNotMatch(read255("Public/MKIRoadBuildSnap.h"), /SnapJunctionFoundation|SnapJunctionWall/);
  // A spline road keeps its own snaps (negative: its curves are no foundation box).
  assert.match(snap, /bool FMKIRoadBuildSnap::SnapRoadWallHologram\(/);
  assert.match(snap, /bool FMKIRoadBuildSnap::SnapHologram\(/);
  // The pad is 4x1 foundations on the depot's 8 m grid, for 2 and 4 lanes.
  for (const lanes of [2, 4]) {
    const half = lanes * 400, padFar = half + 800 + 800;
    assert.equal(4800 % 800, 0);
    assert.equal((padFar + half) % 800, 0, `${lanes} lanes: across the deck`);
  }
});

test("the import renders icons in a child process, retries a crash, and fails a golden mismatch that repeats (#314, #316)", async () => {
  const { isRenderCrash, ICON_RENDER_ATTEMPTS } = await import("../src/road-pack-icons.js");
  assert.equal(isRenderCrash(3221225477, null), true, "Windows access violation");
  assert.equal(isRenderCrash(139, null), true);
  assert.equal(isRenderCrash(null, "SIGSEGV"), true);
  // Negative: a reported failure (a golden mismatch exits 1) and success are not crashes.
  assert.equal(isRenderCrash(1, null), false);
  assert.equal(isRenderCrash(0, null), false);
  assert.ok(ICON_RENDER_ATTEMPTS >= 2 && ICON_RENDER_ATTEMPTS <= 5);
  const script = readFileSync(resolve(repositoryRoot, "tools", "build-tools", "scripts", "road-pack.ts"), "utf8");
  assert.match(script, /iconDir: mode === "import" \? iconsIsolated\(\) :/);
  assert.match(script, /if \(!isRenderCrash\(result\.status, result\.signal\) && \+\+mismatches >= 2\) throw new Error/, "a mismatch that repeats fails");
});

test("depot bays: a solid bay line, a thick stall box with LOADING or UNLOADING in block letters, white stripes round it (#318)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp"), header = read255("Public/MKIRoadJunction.h");
  // The maintainer chose option D, then refined it in game: one word, readable from the lanes, smooth letters, stripes outside.
  // The bay line is solid its whole length (the maintainer: with the bay striped, no dashes needed); negative: no dashed bay line.
  assert.match(junction, /AddQuad\(Paint\[1\], FVector2D\(0\.0, Side \* \(Half - Trim \* 0\.5f\)\), FVector2D\(1\.0, 0\.0\), 2\.f \* Reach, Trim\);/, "the joined road's edge trim carried on");
  assert.doesNotMatch(junction, /Dashed\(Side \* Half|Dashed\(Half, -Reach, Reach\)|BaySolidPastDockCm/);
  assert.match(junction, /AddQuad\(Paint\[1\], FVector2D\(End \* StallHalfLengthCm, Side \* Across\), FVector2D\(0\.0, 1\.0\), Depth \+ Trim, Trim\);/, "box lines as wide as the trims");
  assert.match(header, /static constexpr float StallHalfLengthCm = 800\.f;/, "the box two foundations long");
  assert.match(junction, /const TCHAR\* Word = Mode > 0 \? TEXT\("LOADING"\) : TEXT\("UNLOADING"\);/);
  // Regression (live: the word read mirrored): up toward the station with the reader's right, as seen from the lanes.
  assert.match(junction, /AddWord\(Paint\[1\], Word, FVector2D\(0\.0, Side \* Across\), FVector2D\(0\.0, Side\), FVector2D\(-Side, 0\.0\), ScaleAcross, ScaleUp\);/);
  assert.doesNotMatch(junction, /Up\(0\.0, -Side\)|TEXT\("ONLY"\)/);
  // Roboto Black (the maintainer): the font's own outlines, generated into a header and painted with the other markings.
  assert.match(junction, /#include "MKIRoadLegendGlyphs\.generated\.h"/);
  assert.match(junction, /const float ScaleAcross = InsideLength \/ LegendWidth\(TEXT\("UNLOADING"\)\), ScaleUp = FMath::Min\(InsideDepth, ScaleAcross \* LegendStretch\);/);
  assert.match(junction, /mMarkingLineCount = MarkingGeometry\(mArms, mLanes, Pattern, Paint, &mPaintedDockModes\);/);
  assert.doesNotMatch(junction, /UTextRenderComponent|LegendStrokes/, "no runtime font, no block letters");
  const glyphs = read255("Private/MKIRoadLegendGlyphs.generated.h");
  for (const letter of "ADGILNOU") assert.match(glyphs, new RegExp("\\{ TEXT\\('" + letter + "'\\), [0-9.]+f, [0-9]+, [1-9][0-9]* \\}"), letter);
  assert.match(glyphs, /Roboto, Apache License 2\.0/);
  assert.match(junction, /if \(AddFan\(Paint\[1\], ClipHalfPlane\(ClipHalfPlane\(Rect, Slant, From, false\), Slant, From \+ PadStripeWidthCm, true\)\)\) \+\+Count;/, "white stripes outside the box");
  // The legend follows the station's own mode and is repainted when it changes; no station, no legend.
  assert.match(junction, /if \(Mode < 0\) continue;/);
  assert.match(junction, /Mode = It->GetIsInLoadMode\(\) \? 1 : 0;/);
  assert.match(junction, /if \(mPaintedPattern && GetDockModes\(\) != mPaintedDockModes\) BuildMarkings\(mPaintedPattern\);/);
  assert.match(junction, /const int8 Mode = DockModes && DockModes->IsValidIndex\(DockIndex\) \? \(\*DockModes\)\[DockIndex\] : -1;/);
  assert.doesNotMatch(junction, /AddFan\(Paint\[1\], Bay\)/);
});

test("the bay legend's glyph header is valid C++ and matches its generator (#318)", async () => {
  const { cppFloat, legendGlyphsHeader } = await import("../src/road-pack-glyphs.js");
  // Regression: "0f" is not a C++ float literal; every value carries a decimal point.
  assert.equal(cppFloat(0), "0.0f");
  assert.equal(cppFloat(1), "1.0f");
  assert.equal(cppFloat(0.25), "0.25f");
  const header = legendGlyphsHeader([{ letter: "I", advance: 0.42, triangles: [0, 0, 1, 0, 1, 1] }], "test.ttf");
  assert.match(header, /\t0\.0f, 0\.0f, 1\.0f, 0\.0f, 1\.0f, 1\.0f,/);
  assert.match(header, /\{ TEXT\('I'\), 0\.4200f, 0, 3 \}/);
  const generated = readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Private", "MKIRoadLegendGlyphs.generated.h"), "utf8");
  assert.doesNotMatch(generated, /(^|[ ,\t-])[0-9]+f/m, "no integer float literals in the committed header");
});

test("intersection arms read STOP before each stop line, in Roboto Black, for the arriving driver (#318)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp"), header = read255("Public/MKIRoadJunction.h");
  // The maintainer: STOP right before the line into an intersection.
  assert.match(junction, /AddWord\(Paint\[1\], TEXT\("STOP"\), Center, -Out, -Right, StopHeight \/ LegendStretch, StopHeight\);/);
  assert.match(junction, /const float StopFrom = CrosswalkInsetCm \+ CrosswalkDepthCm \+ StopLineGapCm \+ StopLineWidthCm \* 0\.5f \+ StopWordGapCm;/);
  assert.match(junction, /const float StopHeight = FMath::Min\(StopWordHeightCm, ApronCm - StopFrom - StopWordGapCm\);/, "kept inside the arm");
  assert.match(header, /static constexpr float StopWordHeightCm = 240\.f;/);
  const glyphs = read255("Private/MKIRoadLegendGlyphs.generated.h");
  for (const letter of "STOP") assert.match(glyphs, new RegExp("\\{ TEXT\\('" + letter + "'\\), [0-9.]+f, [0-9]+, [1-9][0-9]* \\}"), letter);
  // The words fit: 4.7 m of arm past the stop line leaves room for 2.4 m letters and their gaps.
  assert.ok(800 - (50 + 300 + 120 + 15 + 25) - 25 >= 240);
});

test("intersections and turnarounds snap to foundation edges as roads do (#319)", () => {
  const junctionHologram = read255("Private/MKIRoadJunctionHologram.cpp"), roadHologram = read255("Private/MKIRoadSplineRoadHologram.cpp");
  // The maintainer: roads snapped to existing foundations, the road pieces did not. One edge rule, shared.
  assert.match(junctionHologram, /if \(AMKIRoadSplineRoadHologram::FoundationEdgeEnd\(\*Foundation, HitResult, this, OnEdge, EdgeOutward\)\)\s*\{\s*SetActorTransform\(FaceSideToEnd\(ArmCount\(\), mLanes, mQuarterTurns, OnEdge, EdgeOutward\)\);/);
  assert.match(roadHologram, /if \(FoundationEdgeEnd\(\*Foundation, HitResult, this, OnEdge, EdgeOutward\)\)/);
  // Negative: a seam inside a pad is still not an edge, and a road or piece deck is not treated as a foundation.
  assert.match(roadHologram, /EdgeX = EdgeY = 0\.f;/);
  assert.match(junctionHologram, /!AMKIRoadJunction::FromActor\(HitActor\) && !Cast<AMKIRoadSplineRoad>\(HitActor\)/);
});

test("road supports stand under intersections, turnarounds and depots too (#320)", () => {
  const supports = read255("Private/MKIRoadSupports.cpp"), hologram = read255("Private/MKIRoadSupportsHologram.cpp"), junction = read255("Private/MKIRoadJunction.cpp"), header = read255("Public/MKIRoadSupports.h");
  assert.match(header, /static bool FindTop\(const AMKIRoadJunction& Junction, const FVector& Aim, FVector& OutTop, FVector& OutForward\);/);
  assert.match(hologram, /\(Junction && AMKIRoadSupports::FindTop\(\*Junction, HitResult\.Location, mTop, mForward\)\)/);
  assert.match(hologram, /AMKIRoadJunction::FromActor\(HitActor\) != nullptr/);
  // The column's middle lies wholly under the deck, and a piece's supports are dismantled with it, as a road's are.
  assert.match(supports, /const FVector2D At = Point \+ Corner \* MiddleHalfCm;/);
  assert.match(junction, /if \(It->GetJunction\(\) == this\) OutChildDismantleActors\.AddUnique\(\*It\);/);
  // Saved and sent with the build: the piece's path rides the construct message, and a road keeps priority.
  assert.match(hologram, /Ar << JunctionPath;/);
  assert.match(supports, /mJunction = Road \? nullptr : Junction;/);
});

test("a truck station snapped to a dock or road clears its legs, and the pad stripes are pattern paint (#321, #322)", () => {
  const snap = read255("Private/MKIRoadBuildSnap.cpp"), junction = read255("Private/MKIRoadJunction.cpp");
  // #321 (the maintainer: placed by hand, a station's legs reached under the platform): both station snaps clear the legs.
  assert.match(snap, /if \(UFGFactoryLegsComponent\* Legs = Cast<UFGFactoryLegsComponent>\(LegsProperty->GetObjectPropertyValue_InContainer\(Hologram\)\)\) Legs->SetFeetOffsetsToZero\(\);/);
  assert.equal((snap.match(/ClearHologramLegs\(Hologram\);/g) ?? []).length, 2, "the depot dock and the road snap");
  // #322 (the maintainer: every road marking part of the pattern): the stripes are drawn into the pattern's paint, not their own mesh.
  assert.match(junction, /Count \+= PadStripeGeometry\(Arms, Lanes, Paint\[0\]\);/);
  assert.match(junction, /void AMKIRoadJunction::BuildPadStripes\(\)\n\{[^}]*if \(mPadStripes\) mPadStripes->ClearAllMeshSections\(\);\n\}/);
  assert.doesNotMatch(junction, /mPadStripes->CreateMeshSection/, "negative: nothing paints the pad outside the pattern");
});

test("the bridge reports a building's leg offsets so a station on a deck can be checked (#321)", () => {
  const internal = readFileSync(resolve(bridgePluginDirectory, "Source", "MKIBridge", "Private", "BridgeInternal.cpp"), "utf8");
  assert.match(internal, /for \(const FFeetOffset& Foot : Legs->GetCachedFeetOffsets\(\)\) Feet\.Add\(MakeShared<FJsonValueNumber>\(Foot\.OffsetZ\)\);/);
  assert.match(internal, /Item->SetArrayField\(TEXT\("legOffsetsCm"\), Feet\);/);
});

test("a depot clears the legs of a station already standing on its dock (#321)", () => {
  const junction = read255("Private/MKIRoadJunction.cpp");
  // Live: the maintainer's two hand-placed stations reported feet up to 4.3 m under the deck; the depot clears them.
  assert.match(junction, /for \(const FFeetOffset& Foot : Legs->GetCachedFeetOffsets\(\)\) bLegs \|= FMath::Abs\(Foot\.OffsetZ\) > 1\.f;\s*if \(bLegs\) Legs->SetFeetOffsetsToZero\(\);/);
});

test("every pre-rename name (AI RoadPack, MKI RoadPack) redirects to an asset or class the mod still has", () => {
  const source = readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Private", "MKIRoadLegacyRedirects.cpp"), "utf8");
  const redirects = [...source.matchAll(/\{ ECoreRedirectFlags::Type_(\w+), TEXT\("([^"]+)"\), TEXT\("([^"]+)"\) \}/g)].map(([, type, from, to]) => ({ type: type!, from: from!, to: to! }));
  const packages = new Set(roadPackAssetPackages(roadKit()));
  // Every content package and Blueprint class the mod builds is reached from its old name, and every target exists.
  for (const path of packages) {
    assert.ok(redirects.some((r) => r.type === "Package" && r.to === path), `${path} has no redirect from its old name`);
    const name = path.split("/").pop()!;
    if (/^(Build|Desc|Recipe|Schematic)_/.test(name)) assert.ok(redirects.some((r) => r.type === "Class" && r.to === `${path}.${name}_C`), `${name}_C has no class redirect`);
  }
  for (const r of redirects.filter((r) => r.type === "Package" && r.to.startsWith("/MKIRoads/"))) assert.ok(packages.has(r.to), `${r.from} redirects to ${r.to}, which the mod does not build`);
  // Every native class, struct and enum a save can name is redirected from /Script/AIRoadPack.
  const headers = readdirSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Public")).map((file) => readFileSync(resolve(roadPackPluginDirectory, "Source", "MKIRoads", "Public", file), "utf8")).join("\n");
  let natives = 0;
  // Classes added after the rename have no old name to redirect from (the road-piece squares, 2026-10-18).
  const newSinceRename = new Set(["MKIRoadTile"]);
  for (const [, kind, name] of headers.matchAll(/U(CLASS|STRUCT|ENUM)\([^)]*\)\s*\n\s*(?:class|struct|enum class)\s+(?:MKIROADS_API\s+)?[UAFE](MKIRoad\w+)/g)) {
    if (newSinceRename.has(name)) continue;
    const type = kind === "CLASS" ? "Class" : kind === "STRUCT" ? "Struct" : "Enum", target = `/Script/MKIRoads.${kind === "ENUM" ? "E" : ""}${name}`;
    for (const legacy of ["/Script/AIRoadPack.", "/Script/MKIRoadPack."]) assert.ok(redirects.some((r) => r.type === type && r.to === target && r.from.startsWith(legacy)), `${type} ${target} has no redirect from ${legacy}`);
    natives++;
  }
  assert.ok(natives >= 14, `only ${natives} native types were checked`);
  // Old names only on the left, new names only on the right.
  const legacy = /AIRoad|AIRP|AIPacks|AIPatterns|MKIRoadPack|MKIRP/;
  for (const r of redirects) { assert.match(r.from, legacy); assert.doesNotMatch(r.to, legacy); }
  // Both earlier names are covered: every package redirect from AI RoadPack has its MKI RoadPack twin.
  const fromAi = redirects.filter((r) => r.type === "Package" && r.from.startsWith("/AIRoadPack/"));
  assert.equal(fromAi.length, redirects.filter((r) => r.type === "Package" && r.from.startsWith("/MKIRoadPack/")).length);
});

test("road pieces lock and nudge with the build gun (#377)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  assert.match(read("Public/MKIRoadSplineRoad.h"), /static constexpr float NudgeStepCm = 100\.f;/);
  for (const name of ["MKIRoadSplineRoadHologram", "MKIRoadJunctionHologram", "MKIRoadSupportsHologram"]) {
    const header = read(`Public/${name}.h`), body = read(`Private/${name}.cpp`);
    assert.match(header, /virtual bool CanNudgeHologram\(\) const override \{ return true; \}/, `${name} can be nudged`);
    assert.match(header, /virtual float GetNudgeDistance\(\) const override \{ return AMKIRoadSplineRoad::NudgeStepCm; \}/);
    assert.match(body, /mCanLockHologram = true;\s*mCanNudgeHologram = true;\s*mDefaultNudgeDistance = AMKIRoadSplineRoad::NudgeStepCm;/, `${name} turns on the lock and nudge`);
  }
  // The road nudges the point being placed, from where the aim last set it, along that point's heading; nudged off a road
  // end or arm, it no longer joins it.
  const road = read("Private/MKIRoadSplineRoadHologram.cpp");
  assert.match(road, /mNudgeBase = mBuildStep == ESplineHologramBuildStep::SHBS_FindStart \? mStart : mEnd;/);
  assert.match(road, /FMKIRoadSplineEnd Nudged = mNudgeBase;\s*const FVector Offset = GetNudgeSpaceTransform\(\)\.TransformVectorNoScale\(GetHologramNudgeOffset\(\)\);/);
  assert.match(road, /Nudged\.bSnapped = false;\s*Nudged\.bHeadingLocked = false;\s*Nudged\.Road = nullptr;\s*Nudged\.Junction = nullptr;/);
  assert.match(road, /return FTransform\(FRotator\(0\.f, CurrentHeadingYaw\(mBuildStep == ESplineHologramBuildStep::SHBS_FindStart\), 0\.f\), mNudgeBase\.Location\);/);
  // Supports: the top moves along and across (never up), the base by the nudge distance, never below the shortest column.
  const supports = read("Private/MKIRoadSupportsHologram.cpp");
  assert.match(supports, /mTop = mNudgeTop \+ FVector\(Offset\.X, Offset\.Y, 0\.f\);/);
  assert.match(supports, /FMath::Max\(AMKIRoadSupports::MinHeightCm, mNudgeHeight - Offset\.Z\)/);
});

test("road pieces are refused through other buildings and foundations, but not vehicle paths (#378)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const rule = /TSubclassOf<UFGConstructDisqualifier> AMKIRoadSplineRoad::PieceClearanceDisqualifier\([^)]*\)\n\{([\s\S]*?)\n\}/.exec(read("Private/MKIRoadSplineRoad.cpp"))?.[1] ?? "";
  assert.match(rule, /if \(Result == EClearanceOverlapResult::COR_None \|\| !Other\) return Vanilla;/, "no overlap keeps the vanilla result");
  assert.match(rule, /if \(Other->IsA<AFGVehiclePathSegment>\(\) \|\| Other->IsA<AFGVehiclePathNode>\(\)\) return nullptr;/, "vehicle paths are no obstacle");
  assert.doesNotMatch(rule, /AFGBuildableFoundation/, "foundations and ramps block too (the maintainer, 2026-10-18): a deck resting on one only touches it");
  assert.match(rule, /return UFGCDEncroachingClearance::StaticClass\(\);\s*$/, "anything else blocks");
  for (const name of ["MKIRoadSplineRoadHologram", "MKIRoadJunctionHologram", "MKIRoadSupportsHologram"]) {
    assert.match(read(`Private/${name}.cpp`), new RegExp(String.raw`TSubclassOf<UFGConstructDisqualifier> A${name}::GetConstructDisqualifierFromClearanceOverlap\(const EClearanceOverlapResult& OverlapResult, AActor\* OtherActor\) const\n\{\n\treturn AMKIRoadSplineRoad::PieceClearanceDisqualifier\(OverlapResult, OtherActor, Super::GetConstructDisqualifierFromClearanceOverlap\(OverlapResult, OtherActor\)\);`), `${name} applies the rule`);
  }
  // The junction hologram checks the built piece's clearance, rebuilt with its layout, and leaves out what an arm joins.
  const junction = read("Private/MKIRoadJunctionHologram.cpp");
  assert.match(junction, /AMKIRoadJunction::MakeClearanceData\(AMKIRoadJunction::Layout\(ArmCount\(\), mLanes\), DeckThicknessCm\(\), mClearanceData\);/);
  assert.match(junction, /mPreviewShowsDeck = false;\s*UpdateClearance\(\);/);
  assert.match(junction, /if \(AActor\* Joined = mJoinedTo\.Get\(\)\)\s*\{\s*IgnoredActors\.Add\(Joined\);\s*\/\/[^\n]*\n\s*if \(const AMKIRoadJunction\* Piece = AMKIRoadJunction::FromActor\(Joined\)\) Piece->AddSelfAndTiles\(IgnoredActors\);/);
  assert.equal((junction.match(/mJoinedTo = const_cast</g) ?? []).length, 2, "a road end and another piece's arm are both joins");
  // Supports get boxes round their columns, and ignore the deck they hold up.
  const supports = read("Private/MKIRoadSupportsHologram.cpp");
  assert.match(supports, /UpdateClearance\(\);\s*SetMaterialState\(mPlacementMaterialState\);/);
  assert.match(supports, /if \(mRoad\) IgnoredActors\.Add\(mRoad\);\s*if \(mJunction\) mJunction->AddSelfAndTiles\(IgnoredActors\);/);
  // Hologram boxes are hard: two soft boxes ignore each other, so a soft-boxed road crossed a foundation unchallenged (live).
  assert.match(read("Public/MKIRoadSplineRoad.h"), /static void HardenForPlacement\(TArray<FFGClearanceData>& Clearance\) \{ for \(FFGClearanceData& Data : Clearance\) Data\.Type = EClearanceType::CT_Default; \}/);
  assert.match(read("Private/MKIRoadSplineRoadHologram.cpp"), /AMKIRoadSplineRoad::HardenForPlacement\(mClearanceData\);\s*if \(mClearanceData\.Num\(\) > 0\) SetupClearanceDetector\(\);/);
  assert.match(junction, /DeckThicknessCm\(\), mClearanceData\);\s*AMKIRoadSplineRoad::HardenForPlacement\(mClearanceData\);/);
  assert.doesNotMatch(supports, /CT_Soft/, "support boxes are hard too");
  // Foundations far from the aim point are only actors near an instance converter, so the road keeps one over its bounds.
  const roadHologram = read("Private/MKIRoadSplineRoadHologram.cpp");
  assert.match(roadHologram, /SetupClearanceDetector\(\);\s*UpdateLightweightConverter\(\);\s*\}/);
  assert.match(roadHologram, /Lightweights->AddInstanceConverterInstigator\(Radius, nullptr, FTransform\(Bounds\.GetCenter\(\)\)\);/);
  assert.match(roadHologram, /RemoveLightweightConverter\(\);\s*Super::EndPlay\(EndPlayReason\);/, "the converter goes with the hologram");
  assert.match(roadHologram, /if \(mClearanceData\.Num\(\) == 0 \|\| !mSplineComponent\) \{ RemoveLightweightConverter\(\); return; \}/, "none before the start is set");
  // The built pieces keep soft boxes, so lights, stations and paths still go on them (negative).
  assert.doesNotMatch(read("Private/MKIRoadSplineRoad.cpp"), /HardenForPlacement/);
});

test("Ctrl locks road pieces to the world grid, as foundations (#379)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const header = read("Public/MKIRoadSplineRoad.h");
  assert.match(header, /static constexpr float WorldGridCm = 400\.f;\s*static constexpr float WorldGridHeightCm = 100\.f;/);
  assert.match(header, /static float WorldGridYaw\(float Yaw\) \{ return 90\.f \* FMath::RoundToFloat\(FRotator::NormalizeAxis\(Yaw\) \/ 90\.f\); \}/);
  // The road: Ctrl is straight mode, and a free start (never a road end, an arm, or a foundation snap) goes on the grid along a world axis.
  const road = read("Private/MKIRoadSplineRoadHologram.cpp");
  assert.match(road, /if \(mStraightMode && mBuildStep == ESplineHologramBuildStep::SHBS_FindStart\) SnapStartToWorldGrid\(\);/);
  assert.match(road, /if \(mStart\.bSnapped \|\| mStart\.bHeadingLocked \|\| mStart\.bOnFoundation \|\| mStart\.Junction\.IsValid\(\)\) return;\s*mStart\.Location = AMKIRoadSplineRoad::WorldGridLocation\(mStart\.Location\);/);
  // Intersections, turnarounds and depots: a free piece on the grid in quarter turns; joins and foundation snaps come first (negative).
  const junction = read("Private/MKIRoadJunctionHologram.cpp");
  assert.match(junction, /void AMKIRoadJunctionHologram::SetSnapToGuideLines\(bool bIsEnabled\)\n\{\n\tSuper::SetSnapToGuideLines\(bIsEnabled\);\n\tmWorldGrid = bIsEnabled;/);
  assert.match(junction, /if \(mWorldGrid\)\s*\{[^}]*SetActorLocationAndRotation\(AMKIRoadSplineRoad::WorldGridLocation\(Free\), FRotator\(0\.f, AMKIRoadSplineRoad::WorldGridYaw\(View\) \+ 90\.f \* mQuarterTurns, 0\.f\)\);/);
  assert.ok(junction.indexOf("if (mWorldGrid)") > junction.indexOf("FoundationEdgeEnd("), "a road end, an arm, and a foundation keep their own snap");
});

test("a snapped intersection turns to any side, not only an arm, so it can run alongside a foundation", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const hologram = readFileSync(resolve(source, "Private", "MKIRoadJunctionHologram.cpp"), "utf8");
  const body = /FTransform AMKIRoadJunctionHologram::FaceSideToEnd\([^)]*\)\n\{([\s\S]*?)\n\}/.exec(hologram)?.[1] ?? "";
  assert.match(body, /const FVector2D Side = Sides\[\(\(QuarterTurn % 4\) \+ 4\) % 4\];/, "four sides, one per quarter turn");
  assert.match(body, /Bounds \+= Corner;/, "a blank side's middle comes from the piece's outline");
  assert.match(body, /if \(FVector2D::DotProduct\(Arm\.Outward\.GetSafeNormal\(\), Side\) > 0\.99\) \{ Contact = Arm\.Center; break; \}/, "an arm on that side still joins with its end on the point");
  assert.doesNotMatch(hologram, /JoinArmToEnd|Shape\.Arms\[\(\(ArmIndex/, "the turn no longer picks among the arms only");
  assert.equal((hologram.match(/SetActorTransform\(FaceSideToEnd\(ArmCount\(\), mLanes, mQuarterTurns,/g) ?? []).length, 3, "road ends, other pieces' arms and foundation edges");
});

test("the foundation edge snap reaches 2 m in from the edge", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  assert.match(readFileSync(resolve(source, "Public", "MKIRoadSplineRoadHologram.h"), "utf8"), /static constexpr float FoundationEdgeBandCm = AMKIRoadSplineRoad::SnapBandCm;/);
  assert.match(readFileSync(resolve(source, "Public", "MKIRoadSplineRoad.h"), "utf8"), /static constexpr float SnapBandCm = 200\.f;/);
  const road = readFileSync(resolve(source, "Private", "MKIRoadSplineRoadHologram.cpp"), "utf8");
  assert.match(road, /const float BandX = FoundationEdgeBandCm, BandY = FoundationEdgeBandCm;/);
  assert.doesNotMatch(readFileSync(resolve(source, "Public", "MKIRoadSplineRoadHologram.h"), "utf8"), /FoundationEdgeBandCm = 50/, "no 50 cm strip");
  // A seam between two foundations of a pad is still not an edge (negative).
  assert.match(road, /LineTraceSingleByChannel\(Beyond, Past \+ FVector\(0\.f, 0\.f, 50\.f\), Past - FVector\(0\.f, 0\.f, 50\.f\), ECC_Visibility, Query\)\) EdgeX = EdgeY = 0\.f;/);
});

test("every road piece snaps within 2 m of a road end or another piece's side, as vanilla foundations do", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const nearEnd = /bool AMKIRoadSplineRoad::NearEnd\([^)]*\)\n\{([\s\S]*?)\n\}/.exec(read("Private/MKIRoadSplineRoad.cpp"))?.[1] ?? "";
  assert.match(nearEnd, /const float Band = SnapBandCm, HalfWidth/, "2 m back from the end");
  assert.match(nearEnd, /if \(In > Band \|\| Across > HalfWidth \+ 100\.f\) continue;/, "further along the road, or off the deck, is no snap (negative)");
  const nearSide = /bool AMKIRoadJunction::NearSide\([^)]*\) const\n\{([\s\S]*?)\n\}/.exec(read("Private/MKIRoadJunction.cpp"))?.[1] ?? "";
  assert.match(nearSide, /const double Band = AMKIRoadSplineRoad::SnapBandCm;/);
  assert.match(nearSide, /if \(Sides\[Index\]\.In > Sides\[Index\]\.Band\) continue;/, "the middle of a piece is no snap (negative)");
  assert.match(nearSide, /Point = Arm\.Center;\s*bOutArm = true;/, "a side with an arm snaps on the arm");
  // No fixed 8 m reach any more: every piece uses the shared share.
  for (const file of ["Private/MKIRoadSplineRoadHologram.cpp", "Private/MKIRoadJunctionHologram.cpp", "Public/MKIRoadSplineRoadHologram.h"]) assert.doesNotMatch(read(file), /EndSnapDistanceCm/, file);
  assert.match(read("Private/MKIRoadJunctionHologram.cpp"), /AMKIRoadSplineRoad::NearEnd\(\*Road, HitResult\.Location, End, Outward\)/);
  assert.match(read("Private/MKIRoadSplineRoadHologram.cpp"), /AMKIRoadSplineRoad::NearEnd\(\*Road, HitResult\.Location, End, Outward\)/);
});

test("a road end at a foundation edge turns in quarter turns, so the road can run along the edge", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const road = readFileSync(resolve(source, "Private", "MKIRoadSplineRoadHologram.cpp"), "utf8");
  assert.match(road, /Result\.bOnEdge = true;\s*Result\.EdgePoint = OnEdge;\s*Result\.EdgeOutward = EdgeOutward;/);
  assert.match(road, /End\.Outward = End\.EdgeOutward\.RotateAngleAxis\(90\.f \* Quarter, FVector::UpVector\)\.GetSafeNormal2D\(\);/);
  assert.match(road, /End\.Location = End\.EdgePoint \+ \(Quarter % 2 == 1 \? End\.EdgeOutward \* HalfWidth : FVector::ZeroVector\);/, "along the edge, the deck lies beside the foundation, not on it");
  assert.match(road, /if \(End\.bOnEdge\)\s*\{[\s\S]*?\+= Delta > 0 \? 1 : \(Delta < 0 \? -1 : 0\);/, "a quarter turn per scroll notch");
  // Turns come from the edge's own point and direction each time, so applying them twice changes nothing; other locked ends stay locked (negative).
  assert.match(road, /if \(End\.bHeadingLocked\) return;/);
  assert.match(road, /mStartEdgeTurns = mEndEdgeTurns = 0;/);
});

test("road supports go under level vanilla foundations too", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const supports = read("Private/MKIRoadSupports.cpp");
  const findTop = /bool AMKIRoadSupports::FindTop\(const AFGBuildableFoundation& Foundation, [^)]*\)\n\{([\s\S]*?)\n\}/.exec(supports)?.[1] ?? "";
  assert.match(findTop, /if \(Foundation\.mHeight <= 0\.f \|\| FMath::Abs\(Foundation\.mElevation\) > KINDA_SMALL_NUMBER\) return false;/, "not under a ramp (negative)");
  assert.match(findTop, /OutTop = Frame\.TransformPosition\(FVector\(X, Y, -Foundation\.mHeight \* 0\.5f\)\);/, "the top on the foundation's underside");
  const hologram = read("Private/MKIRoadSupportsHologram.cpp");
  assert.match(hologram, /AFGBuildableFoundation\* Foundation = \(Road \|\| Junction\) \? nullptr : Cast<AFGBuildableFoundation>\(HitActor\);/, "road pieces keep their own top");
  assert.match(hologram, /mUnderFoundation = Foundation && AMKIRoadSupports::FindTop\(\*Foundation, HitResult\.Location, mTop, mForward\);/);
  // The construct message cannot name a lightweight foundation's temporary actor, so it carries the fact instead.
  assert.match(hologram, /Ar << JunctionPath;\s*Ar << mUnderFoundation;/);
  assert.match(hologram, /if \(!HasDeck\(\) \|\| mColumns\.Num\(\) == 0\) AddConstructDisqualifier\(UMKIRoadCDNeedsRoad::StaticClass\(\)\);/);
});

test("road pieces are built of real 8 m foundation squares, dismantled and painted as one piece (the maintainer, 2026-10-18)", () => {
  const source = resolve(roadPackPluginDirectory, "Source", "MKIRoads");
  const read = (file: string) => readFileSync(resolve(source, file), "utf8");
  const tile = read("Private/MKIRoadTile.cpp"), tileHeader = read("Public/MKIRoadTile.h"), junction = read("Private/MKIRoadJunction.cpp");
  // A plain foundation box the game snaps to, never lightweight (it would drop its piece), with collision and nothing drawn.
  assert.match(tileHeader, /class MKIROADS_API AMKIRoadTile : public AFGBuildableFoundation/);
  assert.match(tile, /mCanContainLightweightInstances = false;\s*mManagedByLightweightBuildableSubsystem = false;/);
  assert.match(tile, /mCollision->SetCollisionProfileName\(TEXT\("BuildingMesh"\)\);/);
  assert.doesNotMatch(tile, /mDisableSnapOn|mDisableAttachmentSnapOn/, "every snap allowed");
  assert.match(tile, /mDepth = mSize\.X;\s*mWidth = mSize\.Y;\s*mHeight = mSize\.Z;/);
  // One piece: dismantling a square takes the piece and its other squares; a square refunds nothing; paint goes to the piece.
  assert.match(tile, /OutChildDismantleActors\.AddUnique\(mJunction\);\s*IFGDismantleInterface::Execute_GetChildDismantleActors\(mJunction, OutChildDismantleActors\);/);
  assert.match(tile, /void AMKIRoadTile::GetDismantleRefund_Implementation\([^)]*\) const\n\{\n\t\/\/ The piece refunds itself; a square costs nothing\.\n\}/);
  assert.match(tile, /mJunction->SetCustomizationData_Native\(CustomizationData\);/);
  // The middle-click sample aimed at a square picks its piece (the maintainer: sampling did nothing); it still refunds nothing.
  assert.match(tile, /if \(mJunction\) SetBuiltWithRecipe\(mJunction->GetBuiltWithRecipe\(\)\);/);
  assert.match(junction, /for \(AMKIRoadTile\* Tile : mTiles\) if \(IsValid\(Tile\) && !Tile->IsActorBeingDestroyed\(\)\) OutChildDismantleActors\.AddUnique\(Tile\);/);
  // The squares: each cell split into 8 m squares; saved ones kept and refitted, a piece without them (an older save) gets them.
  assert.match(junction, /const int32 CountX = FMath::Max\(1, FMath::RoundToInt32\(Size\.X \/ Tile\)\), CountY = FMath::Max\(1, FMath::RoundToInt32\(Size\.Y \/ Tile\)\);/);
  assert.match(junction, /bool bKeep = mTiles\.Num\(\) == Wanted\.Num\(\);/);
  assert.match(junction, /Tile->Setup\(this, Box\.Value\);\s*Tile->FinishSpawning\(Placed\);/);
  // Our own snaps find the piece through a square; vanilla foundations and walls now snap to the squares themselves.
  assert.match(junction, /const AMKIRoadTile\* Tile = Cast<AMKIRoadTile>\(Actor\);\s*return Tile \? Tile->GetJunction\(\) : nullptr;/);
  for (const file of ["Private/MKIRoadSplineRoadHologram.cpp", "Private/MKIRoadSupportsHologram.cpp", "Private/MKIRoadJunctionHologram.cpp", "Private/MKIRoadPaintGuard.cpp"]) assert.match(read(file), /AMKIRoadJunction::FromActor\(/, file);
  // Holograms ignore the squares of what they join or hold up.
  assert.match(read("Private/MKIRoadSupportsHologram.cpp"), /if \(mJunction\) mJunction->AddSelfAndTiles\(IgnoredActors\);/);
});

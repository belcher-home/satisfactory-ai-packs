# MKI Roads

MKI Roads is the first-party SML content mod that ships the **spline road**: a road laid like vanilla railway track, from a click at the start to a drag to the end, following straights, curves, and grades, 2 or 4 lanes of 8 m wide. The build menu lists it under **MKI Packs** > **Roads** as **Road**, with the intersections, turnaround, truck depots, and road supports beside it (see [Build menu and material](#build-menu-and-material)). The build-menu customizer adds an **MKI Patterns** > **Roads** group with the **North American Road** marking pattern, which the road draws for its own width along its own curve.

Every road is asphalt (the vanilla asphalt foundation's material, swatch group, and default swatch), takes the game's color, finish, and pattern customizer, and costs, and refunds when dismantled, one vanilla asphalt 8x4 foundation per started 64 m² of deck. It is packaged and versioned on its own and can be used manually with the build gun, without MKI Bridge. MKI Bridge declares it as a required, versioned dependency, so the bridge is only loaded with a compatible MKI Roads installed; MKI Roads never depends on the bridge (see [Dependency contract](#dependency-contract)).

Through 0.6.17 MKI Roads also shipped the modular road pieces of issue #63 and #92: 8 m straights, 15°, 45°, and 90° turns, and 1, 2, and 4 m ramps and double ramps, as adjustable 2/4/8-lane entries. The maintainer retired them in favor of the spline road (2026-10-12), and 0.7.0 removed them entirely (#180): their native zoop, adjustable, and seam classes, their meshes, decoration templates, holograms, buildables, and icons, the Blender pipeline, and the bridge's `build_road` tool that composed them. A save that still holds a modular piece loses it on load. The modular manifest, its documentation, and the generator are in the history at the parent of the #180 commits.

This directory holds source only. It contains no Unreal assets, no base-game files, and nothing from X3 Roads or any other mod. The plugin's Unreal content is a build output: `npm run road-pack -- import` regenerates all of it inside the SML development checkout, from the manifest and the deterministic icons.

## Layout

| Path | Role |
| --- | --- |
| `MKIRoads/` | The SML plugin source: `MKIRoads.uplugin` (one runtime module; its only mod dependency is SML; it also enables the engine's EnhancedInput and ProceduralMeshComponent as base plugins), `Config/Alpakit.ini` (Windows target), and `Source/MKIRoads`. `MKIRoads.Build.cs` makes the module depend on Core, CoreUObject, Engine, EnhancedInput, InputCore, FactoryGame, SML, and ProceduralMeshComponent. The classes are `MKIRoadSplineRoad` (the road), `MKIRoadSplineRoadHologram` (its build-gun hologram), `MKIRoadSplineRoadDisqualifiers` (its road-worded placement messages), `MKIRoadMarkingPattern` (the road marking pattern descriptor), and `MKIRoadsGameWorldModule`, the native parent of the root game-world module, which registers the schematic. The module itself installs no hooks. `sync` copies it to `<SML>/Mods/MKIRoads`. |
| `modules.json` | The manifest (`mki-roads.modules` v13, `ROAD_KIT_SCHEMA_VERSION` in `road-kit.ts`): the package settings (version, mount point, native module, schematic, root module, build-menu category and Roads subcategory, icon folder, the asphalt swatch settings, material customization rule, lane-marking dimensions and paints, the spline road entries and patterns, cost rule) and the base-game placeholder references. The loader rejects any key of the retired modular schema. |
| `generated/road-kit.icons.json` | Committed golden of the build-menu icons: the RGBA SHA-256, coverage, and bounds of each spline road entry's and road pattern's icon. Tests and `import` render the icons again and require the same pixels. |
| `unreal/road_pack_editor.py` | Unreal Python run headlessly by the SML editor. `import` builds every MKI Roads asset from the editor plan (`roadPackEditorPlan` in `road-kit.ts`). `verify` reloads them in a fresh editor and checks them against the manifest, the plan, and the vanilla evidence. |

The manifest loader and the spline road, pattern, asset-list, and editor-plan helpers are in `packages/mcp-server/src/road-kit.ts`. The icon renderer is `src/road-pack-icons.ts`. The companion contract (SemVer ranges, capability checks, and package verification) is in `packages/mcp-server/src/road-pack.ts`. The SDK workflow is `packages/mcp-server/scripts/road-pack.ts`. The game install and check tooling is `scripts/road-pack-install.ts` and `src/road-pack-install.ts`. Tests are in `packages/mcp-server/test/road-pack.test.ts`, `spline-road.test.ts`, and `road-pack-install.test.ts` (fake game trees only).

Generated files never enter source control. Icons, editor reports, logs, and SDK backups go to `.local/road-pack/`. Packaged archives go to `.local/packages/`.

## Toolchain

- The SML development checkout (`SML_ROOT`, default `D:/SatisfactoryModding/SatisfactoryModLoader`; validated at `d2162c99`, headers CL502094, SML 3.12) and its Unreal Engine 5.6.1-CSS build (`UE_ROOT`, default `D:/DevTools/UnrealEngine-CSS`) with Alpakit and the Python Editor Script Plugin enabled.
- Node.js 20 or later.

## Workflow

```sh
# 1. Mirror the plugin sources into the SML checkout (Mods/MKIRoads and Mods/GameFeatures/MKIBridge).
npm run road-pack -- sync
#    Render the build-menu icons and compare them with the golden (import does this too):
npm run road-pack -- icons            # --update-golden after an intentional icon change
# 2. Compile the native modules for the SDK editor, rebuild the MKI Roads content in a headless SML editor,
#    then verify it in a fresh editor.
npm run road-pack -- import           # runs "editor" (FactoryEditor Development build) first
# 3. Cook and package with the Alpakit PackagePlugin automation, then verify the archives.
npm run road-pack -- package
# Or all of steps 1-3:
npm run road-pack -- all
# Limit sync/package to one plugin:
npm run road-pack -- package --plugins MKIRoads
```

What each step changes:

- **`sync`** copies `MKIRoads.uplugin`, `Config/`, and `Source/` to `<SML>/Mods/MKIRoads`, and the bridge's `.uplugin`, `Config/`, and `Source/` to `<SML>/Mods/GameFeatures/MKIBridge`. `Source/` is mirrored exactly, so the native files #180 deleted are deleted in the SDK too; SDK-only files under `Config/` and `Content/` are kept. Every overwritten file is first copied to `.local/road-pack/sdk-backup-<stamp>/`, and `.local/road-pack/sync.json` records the commit, whether the whole repository was clean, and the file hashes. The SML repository ignores both plugin directories (`Mods/*`).
- **`editor`** compiles the synced native modules of both plugins for the SDK editor (`Build.bat FactoryEditor Win64 Development`), because `import` and `verify` load MKI Roads' native classes. Log: `.local/road-pack/editor-build.log`.
- **`icons`** renders one icon per spline road entry and road pattern to `.local/road-pack/icons/` and fails unless the pixels equal `generated/road-kit.icons.json`.
- **`import`** runs `editor` and `icons`, empties `<SML>/Mods/MKIRoads/Content`, writes the editor plan (`.local/road-pack/editor-plan.json`), and runs `unreal/road_pack_editor.py` through `UnrealEditor-Cmd -run=pythonscript -NullRHI`. A second editor process then runs `verify`. Reports: `.local/road-pack/import-report.json` and `verify-report.json`. Each run also writes `import-provenance.json` or `verify-provenance.json`: the commit, whether the repository was clean, and the SHA-256 of `sync.json`, of the report, and of the `Content` tree it produced or checked. `import` deletes both earlier provenance records first.
- **`package`** runs the same UAT command line Alpakit issues for a Windows Shipping build (`RunUAT PackagePlugin -DLCName=<Plugin> -platform=Win64 -clientconfig=Shipping -nocompileeditor -installed`, plus `-build`). Both plugins are packaged with `-build`, because both ship a native module. It copies the archive to `.local/packages/<Plugin>-<version>-<commit>-<stamp>/` with `SHA256SUMS.txt`, `source-commit.txt`, the `UnrealPak -List` container listing, and `package-report.json`. The step fails unless the archive has its descriptor and containers, every container mount point is the plugin's [install directory](#installing-into-the-game) (recorded as `installPath`), every container entry is inside that mount, no base-game content is packed, MKI Roads holds exactly the manifest's assets, and the packaged `.uplugin` keeps the [dependency contract](#dependency-contract). Native binaries must have exactly the expected Steam and EGS Shipping `.modules` manifests; each must carry `"BuildId": "SML"`, declare exactly the native modules in the plugin descriptor, and name the corresponding store-target DLL. Only those manifests, DLLs, and matching PDBs are allowed, so a stale manifest, renamed module, editor binary, or orphaned DLL/PDB fails the package. For MKI Roads, the step also extracts the cooked `AssetRegistry.bin` from the pak into `pak-extract/`. SML finds root modules only through that registry, so it must name every manifest package, the chain's native classes (`GameWorldModule`, `FGSchematic`, `FGRecipe`, `FGBuildingDescriptor`, `FGBuildable`, `FGBuildCategory`, `FGBuildSubCategory`), and the `bRootModule` tag. Before UAT runs, it moves the plugin's shipping outputs (`FactoryGameSteam*`/`FactoryGameEGS*` DLLs, symbols, and `.modules` manifests) out of the SDK `Binaries/Win64` directory into the artifact's `replaced-build-outputs/`, so both store targets relink from the synced source. The archive then fails verification if either store DLL is older than the run (`package-report.json` `freshness`; #134): UBT once relinked only the EGS target and shipped a stale Steam bridge that passed every other check.
- **Package provenance.** `package` refuses to start unless its inputs provably come from the current clean `HEAD` (`packageProvenanceProblems` in `src/road-pack-provenance.ts`). `git status --porcelain` must be empty for the whole repository, including untracked files. `sync.json` must record that `HEAD` and a clean tree. The mirrored repository files must be tracked, equal the hashes `sync.json` recorded, and equal the SDK copy. Every other file Alpakit could package from the SDK plugin directory must be an approved SDK-only file whose SHA-256 equals the value pinned in `PluginSpec.sdkOnly` (`scripts/road-pack.ts`): the bridge's two `Content/` assets, which a fresh SML checkout lacks (docs/NATIVE_BUILD.md). The bridge's `Config/Alpakit.ini` is tracked and mirrored since #3. Only the top-level build outputs (`Binaries/`, `Intermediate/`, `Saved/`, exact names) and MKI Roads' editor-checked `Content/` are exempt (`sdkInputFiles`). A symbolic link or junction anywhere in the SDK plugin directory, including in place of an exempt directory, blocks packaging instead of being followed. Any other extra `Config/`, `Resources/`, or `Content/` file, or a changed or missing approved file, blocks packaging. For MKI Roads, the import and verify provenance must also name that `HEAD`, a clean tree, a pass, the current `sync.json`, and their unchanged reports, and both must match the current SDK `Content` tree. The check runs again after UAT finishes. `source-commit.txt` says `clean` only when both checks pass. `package-report.json` records the provenance: the commit, the SHA-256 of `sync.json`, a digest of the SDK input files (`sdkInputsSha256`), MKI Roads' content digest, and `verified: true` when the re-check after UAT matched. After any commit, rerun `sync`, `import`, and `package` (or `all`).

None of these steps touches a game installation, a running game, or a save.

### Native build in CI

The Node CI job cannot compile native code: a hosted runner has no SML development checkout or CSS Unreal Engine. `.github/workflows/native.yml` runs the native half on a controlled self-hosted Windows runner (#41). From a clean checkout it runs `npm ci`, `npm run versions -- check`, `npm run build`, `sync`, `editor` (both plugins' native modules for the SDK editor), and `package --plugins MKIBridge` (Steam and Epic Shipping, verified as above). Then it uploads the editor build log and the package report, checksums, and source commit. It starts only by hand (`workflow_dispatch`), never on a pull request, because a self-hosted runner must not run untrusted code. One run at a time (`concurrency: native-build`), because `sync` writes into the runner's SML checkout. The runner needs:

- the labels `self-hosted`, `windows`, `satisfactory-sdk`;
- Node.js 20 or later;
- `SML_ROOT` and `UE_ROOT` set (or the default paths above) to an SML checkout and engine build used only by the runner.

Content import and verify (`import`) are not in the job: they rebuild and recheck MKI Roads' content in two headless editor sessions, which packaging MKI Roads needs but a native compile check does not. MKI Roads' native module still compiles in `editor`.

## Versions

Each product has one version source, and every other copy must equal it (#41, `src/project-versions.ts`):

| Product | Source | Copies |
| --- | --- | --- |
| `workspace` (npm workspace and the MCP server's reported version) | root `package.json` `version` | `packages/mcp-server/package.json`, three `package-lock.json` records |
| `bridge` (MKI Bridge) | `MKIBridge.uplugin` `SemVersion` | its `VersionName` |
| `roadPack` (MKI Roads) | `modules.json` `package.version` | `MKIRoads.uplugin` `SemVersion` and `VersionName`, the version stated in this README |

The three stay independent: the bridge requires MKI Roads by a descriptor range (`^0.8.0`), and the MCP server talks to the bridge by capability, not by version.

```sh
npm run versions                              # print every product's version; exits 1 on any drift (CI runs "check")
npm run versions -- set roadPack 0.7.1        # rewrite the source and every copy, all or nothing
```

`set` accepts only `MAJOR.MINOR.PATCH`. It changes no file unless every file it touches is readable and in canonical JSON format (2-space indent, LF), and it changes nothing but the version strings.

## Spline road

`AMKIRoadSplineRoad` (investigation) is a road laid along a spline like vanilla railway track, 2 or 4 lanes of 8 m wide. Each build-menu entry is a Blueprint of the native class with its deck thickness (`package.splineRoad.variants`), with its own icon (`renderSplineRoadIcon`: a road running up the icon with a solid white line along each outside edge, the double solid yellow centre line, and a side band showing the deck thickness), and its recipe costs one asphalt 8x4 foundation. MKI Bridge places it from explicit points with `POST /v1/place-spline-road`, and can drive its hologram like the build gun (game-mod README).

- **Class.** An `AFGBuildableFoundation`, because the build gun places vehicles only on foundations and terrain. Live, a plain `AFGBuildable` deck let the vehicle hologram pass through to the ground below. Foundation edge and attachment snapping onto it is disabled on every side, and it is never a lightweight instance.
- **Saved state.** `mSplineData` (the deck top centerline as `FSplinePointData`, actor-local) and `mLanes`, both `SaveGame` and replicated. It implements `IFGSplineBuildableInterface`; its connections are null for now.
- **Deck.** Swept procedurally along the spline into one `UProceduralMeshComponent` (the engine's ProceduralMeshComponent plugin, which the game ships; a base-plugin dependency). Cross-sections are at most 25 cm apart, and each is level crosswise with its edges offset along the horizontal normal, so both edges stay concentric with the spline. Each section has top, bottom, both sides, and end caps, at the full width and the deck thickness (`mDeckThicknessCm`). A spline mesh that deforms the vanilla foundation mesh bends only where the mesh has vertices; live, its curves and ramps read as 8 m facets with sheared texture.
- **Surface.** The deck uses the vanilla asphalt foundation material (`MI_AsphaltFoundation`, `package.splineRoad.surfaceMaterial`) by reference. The procedural deck carries the vanilla foundation UV layout per 8 m lane tile: UV0 on the top runs across and back, centered on each lane tile, the bottom from 2.5, the sides and end caps hang the vanilla side band from the deck top, and UV1 is the top-down stencil channel. Each tile spans one whole UV unit, so tiles meet seamlessly at lane and 8 m boundaries; a vanilla top spans 0.974 of one. The swatch and paint finish reach the deck as the material's custom primitive data (`ApplyCustomizationData_Native`), so it colors and paints like an asphalt foundation.
- **Collision.** One hidden instanced static mesh component of the vanilla `SM_Foundation_Concrete_8x4` (its simple box collision), one instance per oriented slab, with the `BuildingMesh` profile (which blocks pawns, vehicles, the build gun, and the `VehicleWheelQuery` channel). Each slab is the full deck width and thickness, level crosswise, at most 4 m long and shorter wherever the heading or grade turns more than 2°; the procedural deck itself has no collision. The vehicle hologram accepts only static mesh (and landscape) hits as a floor, and a vehicle path's obstacle check ignores only the floor component under it, so the slabs are instances of one component.
- **Cost.** Built with the vanilla asphalt 8x4 foundation recipe, charged and refunded `ceil(length x width / 64 m²)` times (the hologram's `GetBaseCostMultiplier`, the road's `GetDismantleRefundReturnsMultiplier`, and the bridge's charge agree).
- **Build gun.** `AMKIRoadSplineRoadHologram` (an `AFGSplineHologram`) places it like railway track:
  - **Start.** The first click sets the start, by the maintainer's rules (as vanilla rail):
    - Within 8 m (or half the road's width) of an aimed spline road end, it snaps to that end, heading locked.
    - On a foundation's top, the point snaps to the foundation's 1 m grid, counted from its edges in its own frame, and the deck rests on the top. At a foundation's edge (#207), aimed at its side face or at its top within 2 m of an exposed edge, the point goes on that edge, on the 1 m grid along it. The deck top is flush with the foundation's, and the heading points straight out and level, as at a road end; the scroll turns it in quarter turns, and turned along the edge the deck lies beside the foundation. A seam inside a pad is not an edge: something continues past it. A free start there faces the nearest of the foundation's axes, and rotates in steps counted from them, so the road lines up with the grid. A lightweight (player-built) foundation is resolved through the hit's `FActorInstanceHandle`.
    - In open space, it is free, with the deck resting on the aimed surface.
    - Ends snap the same way.
  - **Rotation.** Only spline road ends lock the heading. Before the first click, the rotate input (scroll, in the game's own step) turns the start's heading; after it, the end's, and the curve follows both. Before the first click, the preview is an 8 m stub of the real width and thickness, pointing along the start heading.
  - **End.** The end follows the aim, and the second click builds.
  - **Shape.** From a snapped start, the road continues that heading and bends as a circular arc to the end, at most 45° per Hermite segment. The arc may turn up to 270°, so a half circle can be laid (#206). Until #206 it stopped at about 174° and fell to the too-sharp chord. An end snapped to a road end that the arc already arrives along (facing back beside the start) also takes the arc. From a free start it is straight. Holding Ctrl (the build gun's snap modifier, as for rail) is straight mode: the road runs straight ahead along the start's heading, as far as the aim reaches along it, ignoring the aim's sideways offset. Snapping the end to another end makes the road arrive along that end's heading. Grades ease between the ends' own grades, so a road leaving a level end starts level.
  - **Width.** Plus and Minus step between 2 and 4 lanes (8 m each; #226); a new road starts at 2. A road of any other width in a save reopens at 2 lanes, and the bridge accepts 2 or 4 (#256).
  - **Validation.** The hologram turns red, with road-worded messages, for a length outside 4-100 m, a grade above 25° (46.6 %, as vanilla rail allows), a horizontal radius tighter than the deck width, or an end the arc cannot reach (beyond 270°). It checks the saved spline points, which the construct message carries, never the placing hologram's private plan. It never snaps through `mSnappedBuilding`: live, an end aimed at a lightweight foundation carried that foundation's temporary actor in the construct message, and the second click built nothing.
  - **Colour.** The preview shows the deck's own asphalt in every state; the custom-depth stencil carries the state the build gun set, so an invalid road shows red or yellow outlines.
  - **Clearance.** The road (built and as a hologram) has soft clearance along its deck: oriented boxes at most 4 m long, ending 50 cm below the deck top, as vanilla railway track has. A hologram overlapping another buildable's clearance turns yellow and warns, as rail does; the roads its ends join are ignored.
- **Things built on it.** Foundation-style snapping, such as the wheeled vehicle hologram's `SnapToFloor`, takes the top of `mHeight` in the foundation's frame. The road's actor sits at the deck start. While the local player's build gun holds any hologram but the road's own aimed at the deck, `GetLightweightInstanceActorTransform` is a vanilla-shaped foundation volume (`mHeight` the deck thickness) centered half the thickness below the deck point under the aim, turned to the road's heading, so its top is the deck. A truck, foundation, or wall therefore rests on the deck there, on curves and grades.
- **MCP.** `build_road` (#183) lays spline roads through given points; terrain-aware corridors are #260.

## Road intersections

**Intersection (2-way)** (#228, a corner for tight turns), **Intersection (3-way)**, and **Intersection (4-way)** (#227, `AMKIRoadJunction`, `package.splineRoad.junctions`) follow the roads in **MKI Packs** > **Roads**. The maintainer asked for one piece per arm count, with Plus and Minus setting 1, 2, or 4 lanes, shaped like the intersection he built from foundations so a path can turn right from the outer lane into the nearest lane of the cross road.
- **Shape.** A square of the road's width, an 8 m apron on each arm, and the tile between two arms paved whole, so the outline is square like every foundation's (#314). A curb return is painted across that tile (#230): a white line about the corner's outer point, radius the apron's 8 m, meeting both arms' edge lines (#232 tried 4 m; it looked odd). The right turn is about 12 m in radius, inside that tile; a plain square leaves about 4 m, which the vanilla path refuses. A turnaround's deck is likewise the least square of whole 8 m tiles that holds its loop. The 3-way has the through road and a stem; the side opposite the stem is a straight edge. The 2-way is a corner: one arm and the stem, with the one corner tile between them.
- **Placing.** On the ground, the center rests on the aimed point and the scroll turns it in quarter turns. On a foundation's top, the center goes on the foundation's half-tile (4 m) grid along its axes. Aimed within 2 m of a road's end, another piece's side or a foundation's edge (as vanilla foundations snap to each other), the piece snaps against it and the scroll turns it in quarter turns to any side: a side with an arm joins there, and a side without one lies flush along it, so a piece can run alongside a foundation. The width follows a road's or piece's.
- **Joining roads.** A road's start or end aimed at an arm snaps to the middle of the arm's end, heading straight out, and a road leaving an arm takes its width.
- **Deck.** The road's: the vanilla asphalt material with the per-tile UVs, a level top. Its collision is its squares (below). A road marking pattern draws a white edge line inside every edge but the arm ends and, on each arm (#229), a 3 m crosswalk of 60 cm bars next to the square, a stop line across the incoming lanes, and the double yellow center line (and solid white lane lines on 4 lanes) out to the arm's end. Colours and the road pattern apply as on a road (#225).
- **Squares.** Built like a blueprint (the maintainer, 2026-10-18): placing a piece also lays one real 8 m foundation square (`AMKIRoadTile`) per tile under its deck. The squares carry the collision and are plain foundations to the game, so vanilla foundations, walls, walkways, attachments, poles and supports snap to them natively. Nothing of a square is drawn: the piece draws the deck and markings whole. Dismantling any square dismantles the whole piece (one refund, the piece's), paint aimed at a square repaints the whole piece, and squares are never lightweight instances. A piece saved without squares gets them when the save loads.
- **Cost.** One asphalt 8x4 foundation per started 64 m2 of deck, refunded the same.
## Road patterns and lane markings

The build-menu customizer has an **MKI Patterns** category with a **Roads** subcategory (`CC_MKIR_MKIPatterns`, `CSC_MKIR_MKIPatterns_Roads`, with the vanilla pattern category icon by reference). As a plain `FGCustomizerCategory`, it showed the pattern as an orange and blue swatch circle, while the vanilla **Patterns** category shows its patterns' icons. So since #202, MKI Patterns is a Blueprint child of the vanilla `CC_Pattern` (`package.splineRoad.patterns.category.parent`). The tile itself maps categories with a fixed map of the four vanilla ones, so a child still drew as a color (#220). `MKIRoadCustomizerIcons` wraps `UFGItemDescriptor::GetCategory` so that, for the customizer tile only, a child of CC_Pattern reads as CC_Pattern. Its first pattern is **North American Road** (#159, `package.markings`):

- a solid double yellow center line between the two traffic directions;
- dashed white lines between lanes of the same direction;
- a solid white line inside each outside edge.

It uses the agreed marking dimensions: 15 cm lines, 15 cm between the yellow pair, edge lines 30 cm in (clear of the vanilla 1.5 cm top chamfer), and 2 m dashes with 2 m gaps, in the two project paint materials (`M_MKIR_RoadPaint_Yellow`, `M_MKIR_RoadPaint_White`: one flat color and roughness each). The pattern is a vanilla pattern descriptor (a Blueprint of the native `UMKIRoadMarkingPattern`) with ID 200, clear of the vanilla 0-64. Its `FGCustomizationRecipe` is produced in the build gun and unlocked by the MKI Roads schematic, and it applies only to `MKIRoadSplineRoad`.

A vanilla pattern is a fixed stencil texture on UV1, picked by ID in the foundation material. It cannot know a road's lane count or follow its curve and grade. So the road reads the applied pattern in `ApplyCustomizationData_Native` and sweeps each line as a strip along its own spline, for its own width, 0.5 cm above the deck, with no collision and no shadow. Dashes are measured along the centerline from the road's start. The deck itself gets pattern ID 0, so no vanilla stencil shows under the lines. `roadMarkingPatternLines` in `road-kit.ts` mirrors the native layout, so it is unit-tested for every lane count. An odd count from 3 is marked with the same pattern as one lane fewer, each wider (3 lanes as two 12 m lanes, 5 as four 10 m lanes; the maintainer's choice over a center turn lane); a one-lane road has its edge lines only.

## Road supports

**Road Support (Concrete)** (#194, #199, #200, #203, `MKIRoadSupports`, `package.supports`) places one concrete pillar column under a raised road, as a zoop.
- **First click:** fixes the column's top under the aimed point, snapped to 4 m along the road and 4 m across it from the centre line, kept 2 m inside the edges (#216), so a wide road can stand on several columns. The preview is the shortest column (6 m, #219).
- **No ground snap:** the base no longer snaps to the ground (#211 is withdrawn). On uneven ground a snapped base still floated on one side, so how deep it goes is the player's call: the aim sets it in 2 m steps, and Page Up/Down (with the hologram locked, or while setting the base) raises or lowers it a metre a press, the top piece scaled to fit.
- **Then the aim sets the base:** the ground aimed at near the column, or else the point on the column's line nearest the view, as a vanilla pillar zoop. It's set in 2 m steps and at least 6 m tall (#217, #218: shorter columns sink their base into what is below); it may go into the ground. An odd step is made up by a half-height (scaled) top piece. The second click builds it, and right-click goes back to the first step (#218). The preview shows the pillar's own concrete, red only when it cannot be built.
- **Anywhere:** where a column stands is the player's call. It may stand on a road or vehicle path (#215), next to another support, or close to the ground (#216). Only an aim off a road is refused.
- **Each column:** the vanilla Pillar Base (`SM_Pillar_01`) at the bottom and stacked Concrete Pillar Middles (`SM_Pillar_MiddleConcrete`) up to the deck's underside.
- **Why meshes:** vanilla pillars become lightweight instances once built, so the supports are the meshes by reference (collision, paint), dismantled with the road.

## Street lights on roads

There is no separate road light (#222): the vanilla **Street Light** (`Build_StreetLight`) snaps to a road. Aimed at a road, it goes on the left edge, centre, or right edge (the third of the deck aimed at), snapped to 4 m along the road, the placement of the removed Road Street Light (#193, #199, #200, #203).
- **Placement:** the pole stands 50 cm inside the edge (or on the centre line), its arm across the road, turned further by the hologram's rotation. An SML hook after the vanilla factory hologram places itself (`FMKIRoadBuildSnap::SnapStreetLightHologram`; `Holo_StreetLight` derives from `AFGFactoryHologram`) moves it; aimed anywhere else, and for every other building, the vanilla placement stands.
- **An ordinary street light:** its cost, power, name, sampling, and dismantling are vanilla. A road does not dismantle its lights.
- **No double (#221):** two lights back to back serve a median.

## Automatic vehicle paths

Every road and intersection lays its own vanilla **Universal** vehicle paths (#235, `FMKIRoadAutoPaths`):
- **Roads:** a path down the center of each lane in its direction of travel (traffic keeps right); a one-lane road has a path each way.
- **Intersections:** a path for every move, from each arm's arriving lanes to every other arm's leaving lanes, straight, right, and left (lane k to lane k from the center).
- **Joining:** paths end on the lane centers at road and arm ends; an end within 50 cm of an existing path node uses that node, so roads and intersections added later join the same network.
- **Once:** laid a tick after the road or intersection begins play, also the first time a saved one loads; dismantling it removes its paths. They have no recipe, so they cost and refund nothing.

## Vehicle paths along lanes

The vanilla truck, tractor, explorer, and universal path tools follow a road's lanes (#192, `MKIRoadLanePath`, an SML hook after the vanilla path hologram places itself):
- A free start snaps to the centre of the lane under the aim.
- A start, or an end on the same road, aimed within 4 m of a road's end goes exactly to that end (#210). So a path laid to the end of one road and one started at the start of the next meet at one point, and vanilla joins their nodes.
- An end on the same road makes the path that lane's own line, on curves and grades: points every 250 cm, 1 cm above the deck. A start on an existing node eases onto the lane centre over 10 m.
- Vanilla builds the preview, validation, and nodes from those points, so segments chain as usual.
- An aim off the road keeps the vanilla routing.

## Vanilla barriers, fences, and railings on roads

There are no separate road barriers, railings, or fences (#223; the entries of #181 and #191 were removed). The vanilla Concrete Road Barrier, Barrier Low and Tall, Modern and Industrial Railing, Fence, Chain-link Fence, and Tarp Fence snap to a road and zoop along it (`FMKIRoadBuildSnap`, SML hooks on the wall and fence holograms; any other wall, and any other aim, keeps vanilla). The same zoop serves walkways and catwalks (below):
- **Placing.** Aimed at a road, the left, middle, or right third of the deck picks the left edge (5 cm inside it), the centre line, or the right edge. The piece's ends go on the road's marks every piece length from its start: 4 m, or 8 m for the Industrial Railing (`mWidth`, read from the game). Half a turn of scroll rotation turns it round.
- **Zoop.** The second step stretches it piece by piece along the road toward the aim, never past the road's ends. Each piece lies on its own stretch of road, so a zoop follows curves and ramps.
- **Ordinary vanilla pieces.** Cost, paint, collision, and dismantling are vanilla; a road does not dismantle them.

Vanilla patterns (#190) are refused on roads, since their stencils repeat on every 8 m tile. The road keeps its road marking pattern, and a vanilla remover still clears it.

## Walkways and catwalks along roads

There are no separate road walkways or catwalks (#224; the entries of #189 were removed). The vanilla walkway and catwalk straights, corners, T-junctions, and crosses (each a 4 m square, `mSize`) aimed at a road's edge, its outer metre, or its side face go outside that edge, against it, with the floor flush with the deck. They sit on the road's 4 m marks from its start, turned with the road and by the scroll rotation in quarter turns. A zoop along the road follows it piece by piece round curves and grades, exactly as the barriers do (above), and stops at the road's ends. A zoop across the road (a T-junction turned so its zoop axis points away from the road) keeps vanilla. Ramps and stairs keep vanilla, and the deck-top and road-end snaps below stay.

## Building on and along roads

Vanilla foundations, walkways, and catwalks snap to a road (#188, `MKIRoadBuildSnap`). The runtime module hooks the vanilla foundation and walkway holograms' `TrySnapToActor` (game builds only) and handles only an aim at a spline road; anything else keeps the vanilla snap. Pieces turn with the road and step along it in 2 m steps counted from its start:

- **On the deck:** the piece rests on the deck, its sides on 2 m marks across the deck from the left edge, kept on the deck.
- **At an edge:** the outer metre of the deck, or the road's side face, puts the piece outside that edge, against it. A foundation's top, or a walkway's floor, is flush with the deck top.
- **At an end:** the last metre of the deck, or the end face, puts the piece past the end, against it, flush.

Pieces stay level, at the deck height of their center. Side and end faces are told by the hit normal. Vanilla ramps and stairs keep the vanilla snap.

**The customizer** (#208, `MKIRoadPaintGuard`) keeps vanilla materials and vanilla patterns off roads. To the vanilla checks a road is a foundation, so they were accepted, and nothing changed. SML hooks on the paint state clear its can-apply flags when it's aimed at a road with one of them active, and before each tick set the aimed road's own coloring and patterning flags for the active customization, so the aim shows invalid as for a material, and cancel the click. Since #225 vanilla colours and paint finishes are refused too. Road marking patterns and the vanilla pattern removers still apply.

**Truck stations** (#201) snap to a road's edge through the same kind of hook, on the vanilla docking station hologram:
- Aimed at a road, the station's docking point (its pivot) goes on the centre of the lane along the nearer edge (or the side face aimed at), on the deck, in 2 m steps along the road.
- It's turned so its building stands outside the edge, with its arm over that lane, which becomes the truck bay. From `Build_TruckStation`, the building is on the station's -Y side, the truck's left. So at a road's right edge the truck docks heading against the road's direction, and at the left edge along it.
- Aimed at a vehicle path on the road, it keeps the vanilla path snap.

## Build menu and material

The build menu shows one road, **Road** (a 1 m deck; #233), in **MKI Packs** > **Roads** (`package.splineRoad.subCategory`). The 2 m and 4 m roads are retired (`listed: false`): they have no recipe, since a save keeps the recipes it unlocked, and their buildables and descriptors stay so roads already built with them load and refund through the Road's recipe. New roads and intersections get the North American Road pattern unless the build gun carries one (#234). Since 0.6.14 (#159) every road is asphalt. No recipe sets `mMaterialCustomizationRecipe` (`package.materialCustomization.recipeSource` `none`), and that gives both behaviors we want. The shipped build-menu filter (`GetAvailableRecipesForMaterialDescriptorInSubCategory`, CL502094) keeps every recipe without one, so the roads are listed whatever material is selected. The customizer refuses to swap their material (`UFGCDInvalidCustomizationTarget`, as MKI Roads 0.4.0 showed live). The buildables keep `mAllowColoring` and `mAllowPatterning` and the vanilla asphalt foundation's swatch group and default swatch (`package.asphalt`), so colors and patterns still apply.

Through 0.6.13 the modular pieces shipped in all five vanilla foundation materials, joined to the vanilla material descriptors at runtime. The git history records how that worked (last at `a77661e`).

## Installing into the game

Each plugin must be installed in the directory its containers mount at. Alpakit bakes the plugin's SDK location into the pak and IoStore mount points, and `package` records the result as `installPath` in `package-report.json`:

| Plugin | Install directory (game root relative) | Container mount point |
| --- | --- | --- |
| MKI Roads | `FactoryGame/Mods/MKIRoads` | `../../../FactoryGame/Mods/MKIRoads/` |
| MKI Bridge | `FactoryGame/Mods/GameFeatures/MKIBridge` | `../../../FactoryGame/Mods/GameFeatures/MKIBridge/` |

MKI Roads is an ordinary content mod, not a game feature. A copy under `FactoryGame/Mods/GameFeatures/MKIRoads` still satisfies the bridge's plugin dependency, so the bridge loads. Its content, however, mounts under `Mods/MKIRoads/`, which that plugin does not own, so the root module, schematic, recipes, and the **MKI Packs** menu never register (#85).

`npm run road-pack:install` installs and checks the packaged archives. Every command takes an explicit `--game <root>`, the directory that contains `FactoryGame/`. Nothing defaults to the live game.

```sh
# Report the installed layout: misplaced or duplicate copies, missing files, SML, and dependency/version problems. Exits 1 on any problem.
npm run road-pack:install -- check   --game "D:/SteamLibrary/steamapps/common/Satisfactory"
# Show what install would do, without changing anything.
npm run road-pack:install -- install --game "<root>" --dry-run
# Install the newest ok packages built from HEAD (or --artifacts <dir>,<dir>).
npm run road-pack:install -- install --game "<root>"
# Undo an install from the backup directory it printed.
npm run road-pack:install -- restore --game "<root>" --backup ".local/road-pack/install-<stamp>"
# Rehearse against a temporary fake tree that mirrors the current FactoryGame/Mods (read-only on the source).
npm run road-pack:install -- fake-game "%TEMP%/mkir-fake" --mirror "<root>"
```

- **Artifacts.** `install` and `check` accept an artifact directory, including one given with `--artifacts`, only when its `package-report.json` is `ok` with no problems and records verified provenance whose full commit equals `source-commit.txt`, it was packaged from a clean tree, its directory name still reads `<Plugin>-<version>-<commit>-<stamp>` for that plugin, version, and commit, the zip and every `contents/` file match `SHA256SUMS.txt`, and its container mount points name the plugin's install directory. Without `--artifacts`, it uses the newest such package of each plugin built from the current `HEAD`.
- **Plan.** For each plugin, the plan installs or replaces the install directory and removes every misplaced copy, such as `Mods/GameFeatures/MKIRoads`. An existing install directory is always backed up and replaced, never merged into. This includes leftover files without a descriptor. It refuses unless SML is installed and satisfies both descriptors' SML range, and the bridge's MKI Roads range is satisfied by the MKI Roads that will be installed. Files in a replaced directory that the artifact does not contain are listed as `dropped` and kept in the backup. Unrelated mods are never touched.
- **What may be replaced or removed.** The installer replaces or removes a directory only when that directory is attributable to its plugin (`removalProblems`). It must be strictly inside `FactoryGame/Mods`, named after the plugin, free of links, and hold no `.uplugin` at any depth other than its own top-level descriptor. Otherwise the plan fails closed with a problem and schedules nothing for that directory. That covers a descriptor extracted straight into `Mods` or `Mods/GameFeatures`, a directory shared with SML or another plugin, a bundle directory with another name, and a target with a nested foreign descriptor. Move such files by hand. A descriptor in a directory above an install directory also blocks the plan, because the engine stops discovering plugins there. `install` re-checks every directory just before it backs anything up, and refuses if the tree changed since planning.
- **Backup and rollback.** Before anything changes, every replaced or removed directory is copied to `.local/road-pack/install-<stamp>/<game-relative path>` and checked hash by hash. Use `--backup-dir` to choose another location, which must be empty and outside `FactoryGame/Mods`, where the engine would load backed-up plugins. `install-receipt.json` records the artifacts, actions, and backed-up hashes. If any later step fails, the backup is restored automatically (`rolled-back`). After copying, the installed files must equal the artifact, and `check` must pass for both plugins.
- **Restore.** `restore` and the automatic rollback first check that every replaced or removed directory has a backup, that every backup is present and hash-identical to the receipt, and that every directory they would delete is still attributable to its plugin. Only then do they delete anything. A refusal changes nothing. `restore` also refuses any receipt the installer could not have written (`receiptProblems`). Every action must name MKI Roads or the bridge. `install` and `replace` must target exactly that plugin's install directory. `remove-misplaced` must target another normalized directory under `FactoryGame/Mods` that is named after the plugin, is not inside SML or a first-party install directory, and has no descriptor above it. The backup of a removed copy must hold that plugin's own descriptor and no other. `FactoryGame`, `Mods`, `Mods/GameFeatures`, and `Mods/SML` are never touched, in any letter case. A path segment Windows could resolve to another directory (a trailing dot or space such as `SML.`, an 8.3 short name such as `SML~1`, or a reserved character) is refused, and on disk every directory the installer replaces, removes, or restores must be reached without a junction or symbolic link above it and must resolve to itself under the game root, so a forged or corrupt receipt cannot delete or overwrite SML, a third-party mod, or a shared folder. If a rollback fails, the receipt records `rollback-failed` with both errors. If a restore fails part-way, it records `restore-failed`. The backups are only read, so `restore` retries from either state, and from an interrupted `backed-up` install.
- **Safety.** `install` and `restore` refuse while a `FactoryGame*.exe` process is running. Use `--skip-process-check` only for fake trees. No command starts the game or reads or writes a save. Satisfactory Mod Manager may manage the same `Mods` folder. Run `check` after using it. Artifacts are hash- and provenance-checked against their own package report but are not signed; signing is future hardening for community distribution.

### What `import` builds

| Asset | Source |
| --- | --- |
| `/MKIRoads/Icons/Icon_MKIR_SplineRoad1m`, `2m`, `4m`, `Icon_MKIR_Pattern_NorthAmericanRoad` | One 512 × 512 sRGB UI texture per spline road entry and road pattern, imported from the generated PNG (no mips, `TC_EditorIcon`). |
| `/MKIRoads/Materials/M_MKIR_RoadPaint_Yellow`, `M_MKIR_RoadPaint_White` | The lane-marking paints (#159): opaque lit materials of one flat color and roughness each. |
| `/MKIRoads/BuildMenu/BC_MKIPacks`, `SC_MKIPacks_Roads` | The `MKI Packs` `FGBuildCategory` and its `Roads` `FGBuildSubCategory` (since 0.6.13), with the vanilla asphalt icon by reference. |
| `/MKIRoads/SplineRoad/Build_MKIR_SplineRoad<n>m`, `Desc_`, `Recipe_` | Per deck thickness (1, 2, 4 m): a Blueprint of the native `MKIRoadSplineRoad` (`mDeckThicknessCm` and `mHeight` the thickness, the vanilla asphalt deck material, the asphalt swatch group and default swatch, coloring and patterning allowed), its building descriptor in **MKI Packs** > **Roads**, and a recipe of one vanilla asphalt 8x4 foundation (5 Concrete, read from the SDK; the import fails if it differs from `package.cost.perFoundation`) with no material customization recipe. |
| `/MKIRoads/Customization/CC_MKIR_MKIPatterns`, `CSC_MKIR_MKIPatterns_Roads` | The customizer's **MKI Patterns** category (a Blueprint child of the vanilla `CC_Pattern`) and its **Roads** `FGCustomizerSubCategory`, with the vanilla pattern category icon by reference. |
| `/MKIRoads/Customization/PatternDesc_MKIR_NorthAmericanRoad`, `Recipe_Pattern_MKIR_NorthAmericanRoad` | The road marking pattern (a Blueprint of the native `MKIRoadMarkingPattern` with its ID, dimensions, paints, and the spline road as its only valid buildable) and its `FGCustomizationRecipe`, produced in the build gun. |
| `/MKIRoads/Schematics/Schematic_MKIR_RoadKit` | The **MKI Roads** milestone: Tier 3, beside the vanilla Vehicular Transport milestone and built in the same time, costing 400 Concrete, 100 Reinforced Iron Plate and 50 Modular Frame (`package.schematic`). It unlocks every spline road, support, and road pattern recipe. Saves that completed it before 0.8.1 keep it. |
| `/MKIRoads/RootGameWorld_MKIRoads` | The SML root game-world module (native parent `MKIRoadsGameWorldModule`). It registers the schematic and no material variants. |

`verify` covers the following:

- **Spline roads.** Each entry's native parent, deck thickness, hologram class, name, swatch group and default swatch (the vanilla asphalt foundation's), deck material, coloring and patterning, no refund override (so dismantling refunds the road's own multiple of its recipe), icon, a recipe of exactly one asphalt 8x4 foundation, and **no** material-customization recipe (so no material swap, #159).
- **Barriers.** Each entry's native parent, mesh, hologram class, name, the vanilla barrier's swatch settings and icons, a recipe equal to the vanilla recipe with no material customization recipe, and no refund override.
- **Patterns.** The MKI Patterns category (a child of the vanilla Patterns category) and Roads subcategory; each pattern's native parent, ID, valid buildable, categories, dimensions, icon, and recipe, which the schematic unlocks.
- **Vanilla evidence.** The vanilla asphalt 8x4 foundation's cost, swatch group, and default swatch.
- **Content.** Schematic unlocks (exactly the spline road and pattern recipes); the root module and its `bRootModule` asset-registry tag; the paints; and that the plugin content is exactly the planned assets with no base-game copies.
- **Registration chain.** It walks the chain the way the game does. It discovers root modules as SML does (assets under the mount tagged `bRootModule=True`) and requires exactly `RootGameWorld_MKIRoads`. From there it follows object references only: `mSchematics` (the Tier 3 MKI Roads milestone, with its manifest cost and the Vehicular Transport build time), its `FGUnlockRecipe` recipes, each recipe's product descriptor and build gun, the descriptor's `mCategory`/`mSubCategories`, and its buildable, hologram, and decoration template (none). The chain must reach exactly the planned recipes. Every road recipe must land in **MKI Packs** > **Roads**. The report records the walk as `registrationChain`.
- **Build menu.** From that chain it rebuilds the Roads subcategory the way `GetAvailableRecipesInSubCategory` does (product category and subcategory, sorted by menu priority). It must hold exactly the spline road entries, once each, in plan order, **without** a customization key, so the shipped material filter lists every road under any selected material. The report records the menu as `buildMenu`.
- **Native registration.** The root module must carry no material registration properties (#159).

## Dependency contract

- `MKIRoads.uplugin` declares `SemVersion` (currently `0.8.1`), which must equal `modules.json` `package.version`. Its only mod dependency is SML. Its one runtime module (`MKIRoads.Build.cs`) depends on Core, CoreUObject, Engine, EnhancedInput, InputCore, FactoryGame, SML, and ProceduralMeshComponent; EnhancedInput and InputCore serve the spline road hologram's `+`/`-` lane input, and ProceduralMeshComponent sweeps its deck. EnhancedInput and ProceduralMeshComponent are plugins the game itself enables, so the `.uplugin` lists them as `BasePlugin`s (#126). 0.7.0 removed the modular pieces and the game's AbstractInstance plugin dependency that resolved their seam snaps on lightweight foundations (#180); a save holding a modular piece loses it on load, so 0.7.0 is a breaking release and the bridge range moved to `^0.7.0` with it. 0.8.0 is the first public release; under SemVer's 0.x rule `^0.7.0` excludes it, so the bridge (0.4.5) requires `^0.8.0`.
- `MKIBridge.uplugin` lists `MKIRoads` with `"Enabled": true`, `"SemVersion": "^0.8.0"`, and no `Optional` flag: a hard, required first-party dependency. The engine does not load the bridge, and mod managers require MKI Roads, unless a compatible version is installed. `MKIRoads.uplugin` must never list the bridge. `dependencyContractProblems` in `road-pack.ts` enforces both in tests and on every packaged `.uplugin`.
- At runtime the bridge reads the declared range from its own descriptor and the installed MKI Roads version from that plugin's descriptor. `GET /v1/capabilities` reports `companions[]` (`declared`, `required`, `requiredVersion`, `present`, `version`, `compatible`, `mountPoint`, `features`, `reason`) and `features.road_network.available`.
- Generic `POST /v1/place` accepts `/MKIRoads/...` class and recipe paths only while that companion is compatible. Otherwise it answers `409 companion_unavailable` with the reason. The spline road routes resolve `/Script/MKIRoads.MKIRoadSplineRoad` by path and refuse with `spline_road_unavailable` when it is missing.
- `roadNetworkAvailability(capabilities, manifestVersion)` in `road-pack.ts` fails closed for older bridges, a companion the bridge declares optional, an undeclared, absent, or incompatible MKI Roads, or an installed release older than the manifest; `assertRoadNetworkAvailable` refuses without a positive result. A road builder calls them before emitting MKI Roads placements.
- A release bundle ships both archives. Raising MKI Roads' major version (or minor version while it is `0.x`) requires updating the bridge range in the same change. Tests enforce that the shipped version satisfies the range.

## Changing the kit

1. Edit `modules.json`: a new deck thickness is a `package.splineRoad.variants` entry (1, 2, or 4 m), a new pattern a `package.splineRoad.patterns.entries` entry (ID 65-255, `north-american` layout). Reference unmodified base-game content only through registered `placeholders` entries (full `/Game/FactoryGame/...Name.Name` object paths). The loader rejects duplicate thicknesses, stems, IDs, and asset paths.
2. Run `npm run road-pack -- icons --update-golden`, review the golden diff and the icons in `.local/road-pack/icons`, run `npm test`, then `npm run road-pack -- all`.
3. Bump the version with `npm run versions -- set roadPack <version>`, and the bridge range if the change is not compatible.

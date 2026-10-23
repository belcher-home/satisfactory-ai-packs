import { readFileSync } from "node:fs";
import * as z from "zod/v4";
import { roadKitModulesPath } from "./workspace-paths.js";

/**
 * The MKI Roads content manifest (mods/MKIRoads/modules.json). Since 0.7.0 (#180) the kit is the spline road: the
 * native MKIRoadSplineRoad laid like railway track, one build-menu entry per deck thickness, and the road marking patterns
 * it draws itself. The modular road pieces, their sockets, zoop and adjustable holograms, and the Blender meshes are gone
 * (the last manifest with them is schema version 12). The manifest is planner and editor data: the bridge only places,
 * previews, and dismantles the spline road through its generic and spline routes.
 */

export const ROAD_KIT_SCHEMA = "mki-roads.modules";
export const ROAD_KIT_SCHEMA_VERSION = 16;
/** Base-game assets are referenced through same-path placeholders and never packaged. */
export const BASE_GAME_ASSET_PREFIX = "/Game/FactoryGame/";
/** Project-owned assets live in the MKI Roads plugin's content mount. */
export const PROJECT_ASSET_PREFIX = "/MKIRoads/";
/** Deck area of one vanilla 8 m x 8 m foundation; the spline road's cost scales from it. */
export const FOUNDATION_CELL_AREA_CM2 = 800 * 800;

const baseGamePath = z.string().startsWith(BASE_GAME_ASSET_PREFIX);
const projectPath = z.string().startsWith(PROJECT_ASSET_PREFIX);
const projectClassPath = z.string().regex(/^\/MKIRoads\/(?:[A-Za-z0-9_]+\/)*([A-Za-z0-9_]+)\.\1_C$/, "must be a /MKIRoads/.../Name.Name_C class path");
const baseGameClassPath = z.string().regex(/^\/Game\/FactoryGame\/(?:[A-Za-z0-9_]+\/)*([A-Za-z0-9_]+)\.\1_C$/, "must be a /Game/FactoryGame/.../Name.Name_C class path");
const scriptClassPath = z.string().regex(/^\/Script\/[A-Za-z0-9_]+\.[A-Za-z0-9_]+$/);
const projectFolder = z.string().regex(/^\/MKIRoads\/[A-Za-z0-9_/]+[A-Za-z0-9_]$/);
const hexColor = z.string().regex(/^#[0-9a-f]{6}$/);
/** A build-menu subcategory of the MKI Packs category. */
const buildSubCategory = z.object({ path: projectClassPath, displayName: z.string().min(1), menuPriority: z.number() }).strict();
/** The MKI Roads content plugin that owns every project asset in the manifest. */
const packageSchema = z.object({
  plugin: z.literal("MKIRoads"),
  version: z.string().regex(/^\d+\.\d+\.\d+$/),
  mountPoint: z.literal(PROJECT_ASSET_PREFIX),
  /** The runtime module: the spline road classes and the root game-world module's native parent. */
  nativeModule: z.object({
    name: z.literal("MKIRoads"),
    /** Native parent of the root game-world module Blueprint (`/Script/MKIRoads.MKIRoadsGameWorldModule`). */
    rootModuleParentClass: z.literal("/Script/MKIRoads.MKIRoadsGameWorldModule"),
    evidence: z.string().min(1),
  }).strict(),
  rootModulePath: projectClassPath,
  /** The MKI Roads milestone: it unlocks every spline road and road pattern recipe. */
  schematic: z.object({
    path: projectClassPath, displayName: z.string().min(1), type: z.enum(["milestone", "custom"]), techTier: z.number().int().min(0),
    /** What the HUB charges to complete it. */
    cost: z.array(z.object({ item: baseGameClassPath, amount: z.number().int().positive() }).strict()),
    /** The vanilla milestone, found by display name in this folder, whose build time it takes (and whose tier it must share). */
    timeLike: z.object({ folder: z.string().startsWith("/Game/FactoryGame/"), displayName: z.string().min(1) }).strict(),
    evidence: z.string().min(1),
  }).strict(),
  /** The project-owned build-menu category and its subcategories (the spline roads name theirs, package.splineRoad.subCategory). */
  buildMenu: z.object({
    category: z.object({ path: projectClassPath, displayName: z.string().min(1), menuPriority: z.number() }).strict(),
    /** In menu order; every one lists at least one spline road entry. */
    subCategories: z.array(buildSubCategory).min(1),
    /** Each spline road entry is categorized and its recipe has no material customization key, so every material filter lists it (#159). */
    listing: z.object({ listed: z.literal("always"), evidence: z.string().min(1) }).strict(),
    /** Icon of the MKI Packs category and the schematic (base-game, by reference). */
    categoryIcon: baseGamePath,
    menuPriority: z.number(),
    /** Every spline road entry and road pattern has its own generated icon (road-pack-icons.ts) in this folder. */
    icons: z.object({ folder: projectFolder, sizePx: z.number().int().min(64).max(1024), description: z.string().min(1) }).strict(),
  }).strict(),
  /** The vanilla asphalt foundation the spline road follows: its swatch group and default swatch, and its icon color. */
  asphalt: z.object({
    displayName: z.string().min(1),
    /** The vanilla 8 m x 4 m asphalt foundation recipe whose buildable's swatch settings the editor verify compares. */
    vanillaFoundationRecipe: baseGamePath,
    swatchGroup: scriptClassPath,
    defaultSwatch: baseGamePath,
    iconColor: hexColor,
    evidence: z.string().min(1),
  }).strict(),
  /**
   * Roads are asphalt only (#159): no recipe sets mMaterialCustomizationRecipe, so a road's material cannot be swapped.
   * The five-material version is recorded in the git history (last at a77661e).
   */
  materialCustomization: z.object({ recipeSource: z.literal("none"), evidence: z.string().min(1) }).strict(),
  /**
   * North American lane markings (#159): a solid double yellow line between the directions, dashed white lines between
   * same-direction lanes, and a solid white line inside each outside edge, in the yellow and white paint materials. The
   * road marking patterns carry these dimensions, and the spline road draws its lines liftCm above its deck top.
   */
  markings: z.object({
    liftCm: z.number().positive().max(5),
    lineWidthCm: z.number().positive().max(50),
    centerGapCm: z.number().positive().max(50),
    edgeInsetCm: z.number().positive().max(100),
    dash: z.object({ lengthCm: z.number().positive(), gapCm: z.number().positive() }).strict(),
    paints: z.object({
      yellow: z.object({ material: projectPath, color: hexColor, roughness: z.number().min(0).max(1) }).strict(),
      white: z.object({ material: projectPath, color: hexColor, roughness: z.number().min(0).max(1) }).strict(),
      /** #246: the depot station pads' factory-zone stripes. */
      orange: z.object({ material: projectPath, color: hexColor, roughness: z.number().min(0).max(1) }).strict(),
    }).strict(),
    evidence: z.string().min(1),
  }).strict(),
  /**
   * The spline road (MKIRoadSplineRoad). Its deck is a procedural mesh with the vanilla foundation UV layout, drawn with the
   * vanilla asphalt foundation material by reference (surfaceMaterial), so it looks, colors, and paints like an asphalt
   * foundation.
   */
  splineRoad: z.object({
    surfaceMaterial: baseGamePath,
    /** Native classes: the spline road (the Blueprints' parent) and its build-gun hologram. */
    nativeClass: z.literal("/Script/MKIRoads.MKIRoadSplineRoad"),
    hologramClass: z.literal("/Script/MKIRoads.MKIRoadSplineRoadHologram"),
    /** Folder of the build-menu Blueprints: one buildable, descriptor, and recipe per deck thickness. */
    folder: projectFolder,
    description: z.string().min(1),
    /** The MKI Packs subcategory the entries are listed in: one of package.buildMenu.subCategories. */
    subCategory: projectClassPath,
    /** One build-menu entry per deck thickness, like the vanilla 8x1, 8x2, and 8x4 foundations, each with its own icon (road-pack-icons.ts). */
    /** listed false (#233): no longer in the build menu, kept so roads already built with it load. */
    variants: z.array(z.object({ thicknessCm: z.union([z.literal(100), z.literal(200), z.literal(400)]), stem: z.string().regex(/^[A-Za-z0-9]+$/), displayName: z.string().min(1), listed: z.boolean().optional() }).strict()).min(1),
    /**
     * Road intersections (#227): one build-menu entry per arm count (3 or 4), each a Blueprint of the native MKIRoadJunction
     * (built with MKIRoadJunctionHologram) with its arms and deck thickness, its width set with Plus and Minus (1, 2, or 4
     * lanes), in the same folder and subcategory as the roads.
     */
    junctions: z.object({
      nativeClass: z.literal("/Script/MKIRoads.MKIRoadJunction"),
      hologramClass: z.literal("/Script/MKIRoads.MKIRoadJunctionHologram"),
      thicknessCm: z.union([z.literal(100), z.literal(200), z.literal(400)]),
      description: z.string().min(1),
      /** arms 1 (#236) is a turnaround, 7 and 9 (#237, #240, #251) the truck depot on one side or both (the small depots, 6 and 8, are gone); an entry's own description replaces the intersections'. */
      entries: z.array(z.object({ stem: z.string().regex(/^[A-Za-z0-9]+$/), displayName: z.string().min(1), arms: z.union([z.literal(1), z.literal(2), z.literal(3), z.literal(4), z.literal(7), z.literal(9)]), description: z.string().min(1).optional() }).strict()).min(1),
      evidence: z.string().min(1),
    }).strict(),
    /**
     * Road marking patterns in the build-menu customizer (MKI Patterns > Roads): vanilla pattern descriptors of the native
     * MKIRoadMarkingPattern, which the road draws itself with package.markings' dimensions and paints. IDs stay clear of the
     * vanilla patterns' 0-64; recipeBasis lends the vanilla pattern recipe's producer (the build gun).
     */
    patterns: z.object({
      folder: projectFolder,
      nativeClass: z.literal("/Script/MKIRoads.MKIRoadMarkingPattern"),
      recipeBasis: baseGamePath,
      categoryIcon: baseGamePath,
      /**
       * MKI Patterns (the maintainer: keep it). #202: as a plain FGCustomizerCategory it showed the pattern as a color swatch
       * circle, while the vanilla Patterns category (CC_Pattern) shows its patterns' icons; parent makes MKI Patterns a
       * Blueprint child of CC_Pattern.
       */
      category: z.object({ stem: z.string().regex(/^[A-Za-z0-9_]+$/), displayName: z.string().min(1), menuPriority: z.number(), parent: baseGamePath }).strict(),
      subCategory: z.object({ stem: z.string().regex(/^[A-Za-z0-9_]+$/), displayName: z.string().min(1), menuPriority: z.number() }).strict(),
      entries: z.array(z.object({
        stem: z.string().regex(/^[A-Za-z0-9]+$/), displayName: z.string().min(1), description: z.string().min(1),
        id: z.number().int().min(65).max(255), layout: z.literal("north-american"),
      }).strict()).min(1),
      evidence: z.string().min(1),
    }).strict(),
  }).strict(),
  /**
   * Road supports (#194, #199): one concrete pillar column per click under the aimed point of a raised spline road, laid
   * as the vanilla pillar meshes (base and stacked middles) down to the ground; the cost is the vanilla recipe per piece.
   */
  supports: z.object({
    nativeClass: z.literal("/Script/MKIRoads.MKIRoadSupports"),
    hologramClass: z.literal("/Script/MKIRoads.MKIRoadSupportsHologram"),
    folder: projectFolder,
    subCategory: projectClassPath,
    entries: z.array(z.object({
      stem: z.string().regex(/^[A-Za-z0-9]+$/), displayName: z.string().min(1), description: z.string().min(1),
      vanillaDescriptor: baseGamePath, vanillaRecipe: baseGamePath, baseMesh: baseGamePath, middleMesh: baseGamePath,
    }).strict()).min(1),
    evidence: z.string().min(1),
  }).strict(),
  cost: z.object({
    rule: z.literal("equivalent-foundation-area"),
    foundationAreaCm2: z.literal(FOUNDATION_CELL_AREA_CM2),
    rounding: z.literal("ceil"),
    description: z.string().min(1),
    basisRecipe: baseGamePath,
    /** The basis recipe's ingredients as read from the SDK; the headless import fails if they differ. */
    perFoundation: z.array(z.object({ item: baseGameClassPath, amount: z.number().int().positive() }).strict()).min(1),
    sdkEvidence: z.string().min(1),
    dismantleRefund: z.literal("built-with-recipe"),
    dismantleEvidence: z.string().min(1),
  }).strict(),
  producedIn: baseGamePath,
  unlockRecipeClass: baseGamePath,
}).strict();
const documentSchema = z.object({
  schema: z.literal(ROAD_KIT_SCHEMA),
  schemaVersion: z.literal(ROAD_KIT_SCHEMA_VERSION),
  units: z.object({ length: z.literal("cm"), angle: z.literal("deg") }).strict(),
  package: packageSchema,
  placeholders: z.array(z.object({ path: baseGamePath, assetType: z.string().min(1), usage: z.string().min(1), sdkEvidence: z.string().min(1) }).strict()),
}).strict();

export type RoadKitDocument = z.infer<typeof documentSchema>;
export type RoadKitPackage = z.infer<typeof packageSchema>;

const round = (value: number, digits = 9): number => { const scale = 10 ** digits; return Math.round(value * scale) / scale + 0; };
const assetBaseName = (path: string) => path.slice(path.lastIndexOf("/") + 1, path.indexOf("."));

function semanticErrors(document: RoadKitDocument): string[] {
  const errors: string[] = [];
  const placeholders = new Map<string, RoadKitDocument["placeholders"][number]>(), referenced = new Set<string>();
  for (const item of document.placeholders) {
    if (placeholders.has(item.path)) errors.push(`placeholder ${item.path}: duplicate path`);
    if (!/^\/Game\/FactoryGame\/[^.]+\/([A-Za-z0-9_]+)\.\1$/.test(item.path)) errors.push(`placeholder ${item.path}: must be a full object path /Game/FactoryGame/.../Name.Name`);
    placeholders.set(item.path, item);
  }
  const reference = (where: string, path: string) => {
    referenced.add(path);
    if (!placeholders.has(path)) errors.push(`${where}: base-game asset ${path} has no registered placeholder`);
  };
  const { buildMenu, cost, producedIn, unlockRecipeClass, asphalt, markings, splineRoad, supports } = document.package;
  for (const [key, path] of Object.entries({
    categoryIcon: buildMenu.categoryIcon, costBasisRecipe: cost.basisRecipe, producedIn, unlockRecipeClass,
    asphaltFoundationRecipe: asphalt.vanillaFoundationRecipe, asphaltDefaultSwatch: asphalt.defaultSwatch, splineRoadSurfaceMaterial: splineRoad.surfaceMaterial,
  })) reference(`package.${key}`, path);
  supports.entries.forEach((entry, index) => {
    for (const key of ["vanillaDescriptor", "vanillaRecipe", "baseMesh", "middleMesh"] as const) reference(`package.supports.entries[${index}].${key}`, entry[key]);
  });
  if (new Set(cost.perFoundation.map((item) => item.item)).size !== cost.perFoundation.length) errors.push("package.cost.perFoundation: duplicate item");
  // Every project asset the import creates has its own path.
  const projectAssets = new Set<string>();
  const claim = (where: string, path: string) => {
    if (projectAssets.has(path)) errors.push(`${where}: project asset ${path} is already used`);
    projectAssets.add(path);
  };
  claim("package.rootModulePath", document.package.rootModulePath);
  claim("package.schematic.path", document.package.schematic.path);
  claim("package.buildMenu.category.path", buildMenu.category.path);
  buildMenu.subCategories.forEach((item, index) => claim(`package.buildMenu.subCategories[${index}].path`, item.path));
  // Lane markings (#159): two distinct paint materials, and every line inside the deck.
  for (const paint of [markings.paints.yellow.material, markings.paints.white.material, markings.paints.orange.material]) {
    claim("package.markings.paints", paint);
    if (!/^\/MKIRoads\/[A-Za-z0-9_/]+\/(M_[A-Za-z0-9_]+)\.\1$/.test(paint)) errors.push(`package.markings.paints: ${paint} must name an M_ material asset`);
  }
  if (markings.edgeInsetCm - markings.lineWidthCm / 2 <= 1.5) errors.push(`package.markings: the edge line (inset ${markings.edgeInsetCm} cm, ${markings.lineWidthCm} cm wide) must clear the deck's 1.5 cm top chamfer`);
  if (!assetBaseName(buildMenu.category.path).startsWith("BC_")) errors.push("package.buildMenu.category.path must name a BC_ asset");
  buildMenu.subCategories.forEach((item, index) => { if (!assetBaseName(item.path).startsWith("SC_")) errors.push(`package.buildMenu.subCategories[${index}].path must name an SC_ asset`); });
  if (new Set(buildMenu.subCategories.map((item) => item.displayName)).size !== buildMenu.subCategories.length) errors.push("package.buildMenu.subCategories: duplicate displayName");
  // The spline road entries are listed in one declared subcategory, and no subcategory is empty.
  if (!buildMenu.subCategories.some((item) => item.path === splineRoad.subCategory)) errors.push(`package.splineRoad.subCategory ${splineRoad.subCategory} is not one of package.buildMenu.subCategories`);
  if (!buildMenu.subCategories.some((item) => item.path === supports.subCategory)) errors.push(`package.supports.subCategory ${supports.subCategory} is not one of package.buildMenu.subCategories`);
  for (const item of buildMenu.subCategories) if (![splineRoad.subCategory, supports.subCategory].includes(item.path)) errors.push(`package.buildMenu.subCategories: ${item.displayName} lists nothing`);
  // One entry per deck thickness, each with its own assets.
  for (const key of ["thicknessCm", "stem", "displayName"] as const) if (new Set(splineRoad.variants.map((item) => item[key])).size !== splineRoad.variants.length) errors.push(`package.splineRoad.variants: duplicate ${key}`);
  for (const key of ["id", "stem", "displayName"] as const) if (new Set(splineRoad.patterns.entries.map((item) => item[key])).size !== splineRoad.patterns.entries.length) errors.push(`package.splineRoad.patterns.entries: duplicate ${key}`);
  for (const item of splineRoadVariants(document)) for (const path of [item.classPath, item.descriptorPath, ...(item.listed ? [item.recipePath] : []), item.iconPath]) claim(`package.splineRoad.variants ${item.displayName}`, path);
  for (const key of ["stem", "displayName", "arms"] as const) if (new Set(splineRoad.junctions.entries.map((item) => item[key])).size !== splineRoad.junctions.entries.length) errors.push(`package.splineRoad.junctions.entries: duplicate ${key}`);
  for (const item of roadJunctionEntries(document)) for (const path of [item.classPath, item.descriptorPath, item.recipePath, item.iconPath]) claim(`package.splineRoad.junctions ${item.displayName}`, path);
  const patterns = splineRoadPatterns(document);
  claim("package.splineRoad.patterns.category", patterns.categoryPath);
  claim("package.splineRoad.patterns.subCategory", patterns.subCategoryPath);
  for (const item of patterns.entries) for (const path of [item.descriptorPath, item.recipePath, item.iconPath]) claim(`package.splineRoad.patterns ${item.displayName}`, path);
  if (new Set(supports.entries.map((item) => item.stem)).size !== supports.entries.length) errors.push("package.supports.entries: duplicate stem");
  for (const item of roadSupportEntries(document)) for (const path of [item.classPath, item.descriptorPath, item.recipePath]) claim(`package.supports ${item.displayName}`, path);
  for (const path of placeholders.keys()) if (!referenced.has(path)) errors.push(`placeholder ${path}: not referenced by any package setting`);
  return errors;
}

/** Parse and validate the MKI Roads manifest. */
export function parseRoadKitDocument(value: unknown): RoadKitDocument {
  const parsed = documentSchema.safeParse(value);
  if (!parsed.success) throw new Error(`Invalid road kit manifest: ${parsed.error.issues.map((issue) => `${issue.path.join(".")}: ${issue.message}`).join("; ")}`);
  const errors = semanticErrors(parsed.data);
  if (errors.length > 0) throw new Error(`Invalid road kit manifest: ${errors.join("; ")}`);
  return parsed.data;
}
export function loadRoadKit(path = roadKitModulesPath): RoadKitDocument {
  return parseRoadKitDocument(JSON.parse(readFileSync(path, "utf8")));
}
let cachedKit: RoadKitDocument | undefined;
export function roadKit(): RoadKitDocument { return cachedKit ??= loadRoadKit(); }

function classPath(folder: string, name: string): string { return `${folder}/${name}.${name}_C`; }

/** A spline road build-menu entry (one per deck thickness): Blueprints of the native MKIRoadSplineRoad. */
export interface SplineRoadVariant {
  thicknessCm: number; displayName: string; description: string; menuPriority: number;
  /** In the build menu; a retired variant (#233) has no recipe, its buildable and descriptor kept for saved roads only. */
  listed: boolean;
  classPath: string; descriptorPath: string; recipePath: string; iconPath: string;
  parentClass: string; hologramClass: string;
  buildMenu: { category: string; subCategory: string };
}

/** The icon of a spline road entry or road pattern (drawn by renderSplineRoadIcon): `<icons folder>/Icon_MKIR_<stem>`. */
export function splineRoadIconPath(kit: RoadKitDocument, stem: string): string {
  return `${kit.package.buildMenu.icons.folder}/Icon_MKIR_${stem}.Icon_MKIR_${stem}`;
}

/** The spline road entries, thinnest first, listed in their MKI Packs subcategory (package.splineRoad.subCategory). */
export function splineRoadVariants(kit: RoadKitDocument): SplineRoadVariant[] {
  const settings = kit.package.splineRoad, menu = kit.package.buildMenu;
  return settings.variants.map((variant, index) => {
    const stem = `MKIR_${variant.stem}`;
    return {
      thicknessCm: variant.thicknessCm, displayName: variant.displayName, listed: variant.listed ?? true,
      description: `${settings.description} Deck: ${variant.thicknessCm / 100} m thick.`,
      menuPriority: round(menu.menuPriority - 2 + index * 0.01, 6),
      classPath: classPath(settings.folder, `Build_${stem}`),
      descriptorPath: classPath(settings.folder, `Desc_${stem}`),
      recipePath: classPath(settings.folder, `Recipe_${stem}`),
      iconPath: splineRoadIconPath(kit, variant.stem),
      parentClass: settings.nativeClass, hologramClass: settings.hologramClass,
      buildMenu: { category: menu.category.path, subCategory: settings.subCategory },
    };
  });
}

/** A road intersection build-menu entry (#227): Blueprints of the native MKIRoadJunction, one per arm count. */
export interface RoadJunctionEntry {
  arms: 1 | 2 | 3 | 4 | 7 | 9; thicknessCm: number; displayName: string; description: string; menuPriority: number;
  classPath: string; descriptorPath: string; recipePath: string; iconPath: string;
  parentClass: string; hologramClass: string;
  buildMenu: { category: string; subCategory: string };
}

/** The intersection entries, after the roads in their MKI Packs subcategory (package.splineRoad.subCategory). */
export function roadJunctionEntries(kit: RoadKitDocument): RoadJunctionEntry[] {
  const settings = kit.package.splineRoad, junctions = settings.junctions, menu = kit.package.buildMenu;
  return junctions.entries.map((entry, index) => {
    const stem = `MKIR_${entry.stem}`;
    return {
      arms: entry.arms, thicknessCm: junctions.thicknessCm, displayName: entry.displayName, description: entry.description ?? junctions.description,
      menuPriority: round(menu.menuPriority - 2 + 0.1 + index * 0.01, 6),
      classPath: classPath(settings.folder, `Build_${stem}`),
      descriptorPath: classPath(settings.folder, `Desc_${stem}`),
      recipePath: classPath(settings.folder, `Recipe_${stem}`),
      iconPath: splineRoadIconPath(kit, entry.stem),
      parentClass: junctions.nativeClass, hologramClass: junctions.hologramClass,
      buildMenu: { category: menu.category.path, subCategory: settings.subCategory },
    };
  });
}

export interface SplineRoadPatternEntry {
  id: number; displayName: string; description: string; layout: "north-american";
  descriptorPath: string; recipePath: string; iconPath: string;
  lineWidthCm: number; centerGapCm: number; edgeInsetCm: number; dashLengthCm: number; dashGapCm: number; liftCm: number;
  yellowMaterial: string; whiteMaterial: string;
  /** The painted lines per lane count (MKIRoadMarkingLines, which the road draws): lateral offset from the centerline. */
  lines: Record<string, RoadMarkingPatternLine[]>;
}
/** The spline road's lane counts (#256): 2 or 4 lanes of 8 m (AMKIRoadSplineRoad::MinLanes, MaxLanes). */
export const SPLINE_ROAD_MIN_LANES = 2, SPLINE_ROAD_MAX_LANES = 4, SPLINE_ROAD_LANE_COUNTS = [2, 4] as const;
export interface RoadMarkingPatternLine { lateralCm: number; color: "yellow" | "white"; dashed: boolean; /** The edge trims' width (#316); others are lineWidthCm. */ widthCm?: number }
/** How far each edge trim reaches in from the deck's edge (#316, native MKIRoadEdgeTrimCm): the old edge line's inner side. */
export function roadEdgeTrimCm(markings: { lineWidthCm: number; edgeInsetCm: number }): number { return markings.edgeInsetCm + markings.lineWidthCm / 2; }

/**
 * The lines of a North American road marking pattern on a deck of `lanes` 8 m lanes (2 or 4), as the native
 * MKIRoadMarkingLines lays them: the double solid yellow center line (the pair centerGapCm apart) and a dashed white line
 * on every other lane boundary. An odd count from 3 is marked as one lane fewer, each wider, so it has the same pattern.
 * Then a solid white trim runs along each edge, from the edge in to roadEdgeTrimCm (#316). One lane has its edge trims only.
 */
export function roadMarkingPatternLines(markings: { lineWidthCm: number; centerGapCm: number; edgeInsetCm: number }, lanes: number, laneWidthCm = 800): RoadMarkingPatternLine[] {
  const lines: RoadMarkingPatternLine[] = [], half = lanes * laneWidthCm / 2, pair = (markings.centerGapCm + markings.lineWidthCm) / 2;
  const laneWidth = 2 * half / lanes;
  for (let boundary = 1; boundary < lanes; boundary++) {
    if (boundary * 2 === lanes) for (const side of [-1, 1]) lines.push({ lateralCm: round(side * pair, 6), color: "yellow", dashed: false });
    else lines.push({ lateralCm: round(-half + boundary * laneWidth, 6), color: "white", dashed: true });
  }
  const trim = roadEdgeTrimCm(markings);
  for (const side of [-1, 1]) lines.push({ lateralCm: round(side * (half - trim / 2), 6), color: "white", dashed: false, widthCm: trim });
  return lines;
}
export interface SplineRoadPatterns {
  categoryPath: string; subCategoryPath: string; categoryIcon: string; recipeBasis: string; validBuildables: string[]; nativeClass: string;
  category: { displayName: string; menuPriority: number; parent: string }; subCategory: { displayName: string; menuPriority: number };
  entries: SplineRoadPatternEntry[];
}

/** The customizer's MKI Patterns > Roads category, subcategory, and road marking patterns, with package.markings' dimensions. */
export function splineRoadPatterns(kit: RoadKitDocument): SplineRoadPatterns {
  const settings = kit.package.splineRoad.patterns, markings = kit.package.markings;
  return {
    categoryPath: classPath(settings.folder, `CC_MKIR_${settings.category.stem}`),
    subCategoryPath: classPath(settings.folder, `CSC_MKIR_${settings.subCategory.stem}`),
    categoryIcon: settings.categoryIcon, recipeBasis: settings.recipeBasis, validBuildables: [kit.package.splineRoad.nativeClass, kit.package.splineRoad.junctions.nativeClass, "/Script/MKIRoads.MKIRoadTile"], nativeClass: settings.nativeClass,
    category: { displayName: settings.category.displayName, menuPriority: settings.category.menuPriority, parent: `${settings.category.parent}_C` },
    subCategory: { displayName: settings.subCategory.displayName, menuPriority: settings.subCategory.menuPriority },
    entries: settings.entries.map((entry) => ({
      id: entry.id, displayName: entry.displayName, description: entry.description, layout: entry.layout,
      descriptorPath: classPath(settings.folder, `PatternDesc_MKIR_${entry.stem}`),
      recipePath: classPath(settings.folder, `Recipe_Pattern_MKIR_${entry.stem}`),
      iconPath: splineRoadIconPath(kit, `Pattern_${entry.stem}`),
      lineWidthCm: markings.lineWidthCm, centerGapCm: markings.centerGapCm, edgeInsetCm: markings.edgeInsetCm,
      dashLengthCm: markings.dash.lengthCm, dashGapCm: markings.dash.gapCm, liftCm: markings.liftCm,
      yellowMaterial: markings.paints.yellow.material, whiteMaterial: markings.paints.white.material,
      lines: Object.fromEntries(SPLINE_ROAD_LANE_COUNTS.map((lanes) => [String(lanes), roadMarkingPatternLines(markings, lanes)])),
    })),
  };
}

/** A road supports build-menu entry: a Blueprint of the native MKIRoadSupports laying vanilla pillar meshes. */
export interface RoadSupportEntry {
  displayName: string; description: string; menuPriority: number;
  classPath: string; descriptorPath: string; recipePath: string;
  parentClass: string; hologramClass: string;
  vanillaDescriptor: string; vanillaRecipe: string; baseMesh: string; middleMesh: string;
  buildMenu: { category: string; subCategory: string };
}

/** The road supports entries, listed after the roads in their MKI Packs subcategory (package.supports.subCategory). */
export function roadSupportEntries(kit: RoadKitDocument): RoadSupportEntry[] {
  const settings = kit.package.supports, menu = kit.package.buildMenu;
  return settings.entries.map((entry, index) => {
    const stem = `MKIR_${entry.stem}`;
    return {
      displayName: entry.displayName, description: entry.description, menuPriority: round(menu.menuPriority - 0.4 + index * 0.01, 6),
      classPath: classPath(settings.folder, `Build_${stem}`), descriptorPath: classPath(settings.folder, `Desc_${stem}`), recipePath: classPath(settings.folder, `Recipe_${stem}`),
      parentClass: settings.nativeClass, hologramClass: settings.hologramClass,
      vanillaDescriptor: entry.vanillaDescriptor, vanillaRecipe: entry.vanillaRecipe, baseMesh: entry.baseMesh, middleMesh: entry.middleMesh,
      buildMenu: { category: menu.category.path, subCategory: settings.subCategory },
    };
  });
}

/** Every package the MKI Roads import step creates (`/MKIRoads/.../Name`), sorted. */
export function roadPackAssetPackages(kit: RoadKitDocument): string[] {
  const { buildMenu, schematic, rootModulePath, markings } = kit.package, patterns = splineRoadPatterns(kit);
  return [...new Set([
    schematic.path, rootModulePath, buildMenu.category.path, ...buildMenu.subCategories.map((item) => item.path),
    markings.paints.yellow.material, markings.paints.white.material, markings.paints.orange.material,
    // A retired road (#233) has no recipe (the maintainer: remove Road (2 m) and (4 m); unlocked recipes stay unlocked in a save).
    ...splineRoadVariants(kit).flatMap((item) => [item.classPath, item.descriptorPath, ...(item.listed ? [item.recipePath] : []), item.iconPath]),
    ...roadJunctionEntries(kit).flatMap((item) => [item.classPath, item.descriptorPath, item.recipePath, item.iconPath]),
    patterns.categoryPath, patterns.subCategoryPath, ...patterns.entries.flatMap((item) => [item.descriptorPath, item.recipePath, item.iconPath]),
    ...roadSupportEntries(kit).flatMap((item) => [item.classPath, item.descriptorPath, item.recipePath]),
  ].map((path) => path.split(".")[0]))].sort();
}

/** Everything the editor import and verify steps build from (the Python step reads this instead of re-deriving it). */
export function roadPackEditorPlan(kit: RoadKitDocument) {
  return {
    schema: "mki-roads.editor-plan", schemaVersion: 16,
    assetPackages: roadPackAssetPackages(kit),
    /** The two paint materials the road marking patterns draw with (#159). */
    paints: kit.package.markings.paints,
    /** The spline road: its deck material, one build-menu entry per deck thickness, and its marking patterns. */
    splineRoad: { ...kit.package.splineRoad, entries: splineRoadVariants(kit), junctions: roadJunctionEntries(kit), patterns: splineRoadPatterns(kit) },
    /** Road supports (#194). */
    supports: { ...kit.package.supports, entries: roadSupportEntries(kit) },
  };
}
export type RoadPackEditorPlan = ReturnType<typeof roadPackEditorPlan>;

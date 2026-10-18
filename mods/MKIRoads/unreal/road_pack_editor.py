"""Build or verify MKI Roads content in the SML editor (Unreal Python, headless).

Run through `npm run road-pack -- import` / `verify`, which start

    UnrealEditor-Cmd.exe <SML>/FactoryGame.uproject -run=pythonscript -script=<this file> -unattended -nullrhi

with MKIR_EDITOR_CONFIG pointing to a JSON file:

    {"mode": "import" | "verify", "manifest": ".../modules.json", "plan": ".../editor-plan.json",
     "iconDir": ".../icons", "report": ".../report.json"}

The plan is written by road-kit.ts `roadPackEditorPlan`: the spline road entries, the road marking patterns, the paints,
and the asset list. This script builds and checks exactly that, so the TypeScript contract and its tests stay the single
source of derived values.

`import` deletes and recreates every MKI Roads asset, so the plugin content is a reproducible build output and is
never committed:

- the generated build-menu icons (one per spline road entry and road pattern);
- the two lane-marking paint materials (#159);
- the "MKI Packs" build-menu category and its "Roads" subcategory (#157);
- per deck thickness, a spline road buildable (a Blueprint of the native MKIRoadSplineRoad, built with the native
  MKIRoadSplineRoadHologram), its building descriptor, and a build-gun recipe of one vanilla asphalt 8x4 foundation (the
  hologram multiplies it by the deck area). The recipes carry no mMaterialCustomizationRecipe (#159), so every material
  filter lists them and the customizer swaps no material;
- the customizer's MKI Patterns > Roads (the category a child of the vanilla Patterns category, #202) and each road marking pattern (a Blueprint of the native MKIRoadMarkingPattern)
  with its customization recipe;
- the schematic that unlocks every recipe, and the root game-world module (native parent MKIRoadsGameWorldModule)
  that registers it.

The modular road pieces (meshes, decoration templates, attachment-point types, zoop and adjustable holograms, and their
buildables) were removed in MKI Roads 0.7.0 (#180).

`verify` loads the saved assets in a fresh editor process and checks them against the manifest and the plan, including
the vanilla evidence the manifest relies on. Base-game assets are only referenced through the starter project's same-path
placeholders; nothing is copied into the plugin. Both modes write a JSON report and exit non-zero on any failure.
"""

import json
import os
import traceback

import unreal

CONFIG = json.load(open(os.environ["MKIR_EDITOR_CONFIG"], encoding="utf-8"))
MANIFEST = json.load(open(CONFIG["manifest"], encoding="utf-8"))
PLAN = json.load(open(CONFIG["plan"], encoding="utf-8"))
PACKAGE = MANIFEST["package"]
MOUNT = PACKAGE["mountPoint"].rstrip("/")
COST = PACKAGE["cost"]
MENU = PACKAGE["buildMenu"]
ASPHALT = PACKAGE["asphalt"]
# Functions a road Blueprint must not implement so dismantling refunds exactly the built-with recipe (times the road's multiplier).
REFUND_OVERRIDES = ("GetDismantleRefund", "GetDismantleBlueprintReturns")

report = {"mode": CONFIG["mode"], "plugin": PACKAGE["plugin"], "version": PACKAGE["version"], "assets": {}, "checks": [], "errors": []}


def check(condition, message):
    report["checks"].append({"ok": bool(condition), "check": message})
    if not condition:
        report["errors"].append(message)
    return bool(condition)


def package_name(object_path):
    return object_path.split(".")[0]


def asset_name(object_path):
    return package_name(object_path).rsplit("/", 1)[1]


def asset_folder(object_path):
    return package_name(object_path).rsplit("/", 1)[0]


def load_class(path):
    """`/Script/Module.Class`, `/Mount/.../Name.Name_C`, or a Blueprint asset path `/Game/.../Name.Name`."""
    if path.startswith("/Script/") or path.endswith("_C"):
        found = unreal.load_class(None, path)
    else:
        blueprint = unreal.load_asset(path)
        found = blueprint.generated_class() if blueprint else None
    if found is None:
        raise RuntimeError("Unable to load class %s" % path)
    return found


def load_object(path):
    found = unreal.load_asset(path)
    if found is None:
        raise RuntimeError("Unable to load asset %s" % path)
    return found


def class_path_of(path):
    """The generated class path of a Blueprint asset path (`/Game/X/Name.Name` -> `/Game/X/Name.Name_C`)."""
    return path if path.startswith("/Script/") or path.endswith("_C") else path + "_C"


def soft_path(value):
    """The path of a soft object property value, loaded (an object) or not (a soft path)."""
    if value is None:
        return None
    return value.get_path_name() if hasattr(value, "get_path_name") else str(value.export_text() if hasattr(value, "export_text") else value)


def text(value):
    return unreal.Text(value)


def path_of(value):
    return value.get_path_name() if value else None


def defaults_of(generated):
    return unreal.get_default_object(generated)


def close(actual, expected, tolerance):
    return abs(float(actual) - float(expected)) <= tolerance


def enum_name(value):
    return str(value).split(".")[-1].split(":")[0].strip("<> ").upper()


def ingredients_of(recipe_class):
    return [(path_of(item.get_editor_property("item_class")), int(item.get_editor_property("amount"))) for item in defaults_of(recipe_class).get_editor_property("mIngredients")]


def manifest_basis_ingredients():
    return [(item["item"], int(item["amount"])) for item in COST["perFoundation"]]


def brush(texture_path):
    icon = load_object(texture_path)
    value = unreal.SlateBrush()
    value.import_text("(ImageSize=(X=256.000000,Y=256.000000),ResourceObject=\"%s\")" % icon.get_path_name())
    return value


def blueprint_parent_tags(object_path):
    """The Blueprint's direct and native parent classes from its asset-registry tags (UBlueprint.ParentClass is not exposed to Python)."""
    data = unreal.AssetRegistryHelpers.get_asset_registry().get_asset_by_object_path(package_name(object_path) + "." + asset_name(object_path))
    return {key: (str(data.get_tag_value(key)) if data else None) for key in ("ParentClass", "NativeParentClass")}


def refund_overrides(blueprint):
    """Refund functions the Blueprint overrides. Both have output parameters, so an override is a function graph."""
    return sorted(name for name in REFUND_OVERRIDES if unreal.BlueprintEditorLibrary.find_graph(blueprint, name))


def spline_road_entries():
    return PLAN["splineRoad"]["entries"]


def junction_entries():
    return PLAN["splineRoad"]["junctions"]


def support_entries():
    return PLAN["supports"]["entries"]


def menu_entries():
    """Every build-menu entry the MKI Roads lists: the listed spline roads (#233: Road, 1 m), the intersections, then the
    supports. The retired 2 m and 4 m roads are built for saved roads only, never unlocked."""
    return [entry for entry in spline_road_entries() if entry["listed"]] + junction_entries() + support_entries()



def spline_pattern_recipes():
    """The road marking patterns' customization recipes, which the schematic also unlocks (no build-menu product)."""
    return [entry["recipePath"] for entry in PLAN["splineRoad"]["patterns"]["entries"]]


# Import --------------------------------------------------------------------------------------------


def scan_mount():
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([MOUNT], True)


def require_empty_mount():
    # The CLI empties <SML>/Mods/MKIRoads/Content before an import, so every asset is rebuilt from source.
    scan_mount()
    existing = unreal.EditorAssetLibrary.list_assets(MOUNT, recursive=True, include_folder=False)
    if existing:
        raise RuntimeError("Import needs an empty %s mount; found %s" % (MOUNT, existing))


def srgb_to_linear(hex_color):
    """A #rrggbb sRGB color as a linear LinearColor (material constants are linear)."""
    channels = [int(hex_color[index:index + 2], 16) / 255.0 for index in (1, 3, 5)]
    linear = [value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4 for value in channels]
    return unreal.LinearColor(r=linear[0], g=linear[1], b=linear[2], a=1.0)


def build_paint_material(settings):
    """A lane-marking paint (#159): a project-owned opaque lit material of one flat color and roughness."""
    path = settings["material"]
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(asset_name(path), asset_folder(path), unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("could not create paint material %s" % path)
    library = unreal.MaterialEditingLibrary
    color = library.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 0)
    color.set_editor_property("constant", srgb_to_linear(settings["color"]))
    library.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = library.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 200)
    roughness.set_editor_property("r", float(settings["roughness"]))
    library.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    library.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material, False)
    return material


def import_icon(icon_path):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", os.path.join(CONFIG["iconDir"], asset_name(icon_path) + ".png"))
    task.set_editor_property("destination_path", asset_folder(icon_path))
    task.set_editor_property("destination_name", asset_name(icon_path))
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(package_name(icon_path))
    if texture is None:
        raise RuntimeError("PNG import produced no texture for %s" % icon_path)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property("srgb", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture, False)
    return texture


def create_blueprint(object_path, parent):
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent)
    blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(asset_name(object_path), asset_folder(object_path), unreal.Blueprint, factory)
    if blueprint is None:
        raise RuntimeError("Unable to create Blueprint %s" % object_path)
    return blueprint


def compile_and_save(blueprint):
    unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
    unreal.EditorAssetLibrary.save_loaded_asset(blueprint, False)


def menu_categories():
    """(settings, native parent) of the "MKI Packs" build category and each of its subcategories, in menu order."""
    return [(MENU["category"], unreal.FGBuildCategory)] + [(item, unreal.FGBuildSubCategory) for item in MENU["subCategories"]]


def build_categories():
    """The project-owned "MKI Packs" build category and its subcategories, with the asphalt icon by reference. Keyed by path."""
    created = {}
    for settings, parent in menu_categories():
        blueprint = create_blueprint(settings["path"], parent)
        defaults = defaults_of(blueprint.generated_class())
        defaults.set_editor_property("mDisplayName", text(settings["displayName"]))
        defaults.set_editor_property("mMenuPriority", float(settings["menuPriority"]))
        defaults.set_editor_property("mCategoryIcon", brush(MENU["categoryIcon"]))
        compile_and_save(blueprint)
        created[settings["path"]] = blueprint.generated_class()
    return created


def build_spline_roads(shared):
    """The spline road's build-menu entries, one per deck thickness: a Blueprint of the native MKIRoadSplineRoad (whose
    native hologram is MKIRoadSplineRoadHologram), its descriptor in MKI Packs > Roads, and a recipe of one asphalt 8x4
    foundation, which the hologram multiplies by the deck area. Returns the recipe classes for the schematic."""
    recipes = []
    vanilla = defaults_of(load_class(COST["basisRecipe"]))
    for entry in spline_road_entries():
        buildable = create_blueprint(entry["classPath"], load_class(entry["parentClass"]))
        unreal.BlueprintEditorLibrary.compile_blueprint(buildable)
        defaults = defaults_of(buildable.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("mDeckThicknessCm", float(entry["thicknessCm"]))
        # The deck thickness: holograms aimed at the deck see a vanilla-shaped foundation volume whose top is the deck.
        defaults.set_editor_property("mHeight", float(entry["thicknessCm"]))
        # The vanilla asphalt foundation material by reference, colored and painted like an asphalt foundation.
        defaults.set_editor_property("mDeckMaterial", load_object(PLAN["splineRoad"]["surfaceMaterial"]))
        defaults.set_editor_property("mAllowColoring", True)
        defaults.set_editor_property("mAllowPatterning", True)
        defaults.set_editor_property("mSwatchGroup", load_class(ASPHALT["swatchGroup"]))
        defaults.set_editor_property("mDefaultSwatchCustomizationOverride", load_class(class_path_of(ASPHALT["defaultSwatch"])))
        compile_and_save(buildable)

        # Each spline road entry draws its own icon (road-pack-icons.ts renderSplineRoadIcon).
        if entry["iconPath"] not in shared["icons"]:
            shared["icons"][entry["iconPath"]] = import_icon(entry["iconPath"])
        icon = shared["icons"][entry["iconPath"]]
        descriptor = create_blueprint(entry["descriptorPath"], unreal.FGBuildingDescriptor)
        defaults = defaults_of(descriptor.generated_class())
        defaults.set_editor_property("mBuildableClass", buildable.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("mCategory", shared["categories"][entry["buildMenu"]["category"]])
        defaults.set_editor_property("mSubCategories", [shared["categories"][entry["buildMenu"]["subCategory"]]])
        defaults.set_editor_property("mMenuPriority", float(entry["menuPriority"]))
        defaults.set_editor_property("mSmallIcon", icon)
        defaults.set_editor_property("mPersistentBigIcon", icon)
        compile_and_save(descriptor)

        # A retired road (#233: the maintainer, remove Road (2 m) and (4 m)) has no recipe: a save keeps the recipes it unlocked, so
        # only a missing recipe takes it out of the build menu. Its roads load, and refund through the road's own recipe.
        if not entry["listed"]:
            continue
        recipe = create_blueprint(entry["recipePath"], unreal.FGRecipe)
        defaults = defaults_of(recipe.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mIngredients", [unreal.ItemAmount(item_class=load_class(item), amount=amount) for item, amount in manifest_basis_ingredients()])
        defaults.set_editor_property("mProduct", [unreal.ItemAmount(item_class=descriptor.generated_class(), amount=1)])
        defaults.set_editor_property("mProducedIn", vanilla.get_editor_property("mProducedIn"))
        # Roads are asphalt only (#159): no mMaterialCustomizationRecipe, so the build menu lists every road under any
        # material filter and the customizer refuses material swaps (UFGCDInvalidCustomizationTarget). Colors and
        # patterns stay. The five-material version is recorded in the git history (last at a77661e).
        defaults.set_editor_property("mMaterialCustomizationRecipe", None)
        compile_and_save(recipe)
        recipes.append(recipe.generated_class())
    return recipes


def build_junctions(shared):
    """The road intersections (#227), one per arm count: a Blueprint of the native MKIRoadJunction (built with
    MKIRoadJunctionHologram) with its arms and deck thickness, colored and painted as the road; its descriptor in MKI Packs >
    Roads with its own icon; and a recipe of one asphalt 8x4 foundation, which the hologram multiplies by the deck area.
    Returns the recipe classes for the schematic."""
    recipes = []
    vanilla = defaults_of(load_class(COST["basisRecipe"]))
    for entry in junction_entries():
        buildable = create_blueprint(entry["classPath"], load_class(entry["parentClass"]))
        unreal.BlueprintEditorLibrary.compile_blueprint(buildable)
        defaults = defaults_of(buildable.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("mArms", int(entry["arms"]))
        defaults.set_editor_property("mDeckThicknessCm", float(entry["thicknessCm"]))
        defaults.set_editor_property("mHeight", float(entry["thicknessCm"]))
        defaults.set_editor_property("mDeckMaterial", load_object(PLAN["splineRoad"]["surfaceMaterial"]))
        defaults.set_editor_property("mAllowColoring", True)
        defaults.set_editor_property("mAllowPatterning", True)
        defaults.set_editor_property("mSwatchGroup", load_class(ASPHALT["swatchGroup"]))
        defaults.set_editor_property("mDefaultSwatchCustomizationOverride", load_class(class_path_of(ASPHALT["defaultSwatch"])))
        compile_and_save(buildable)

        if entry["iconPath"] not in shared["icons"]:
            shared["icons"][entry["iconPath"]] = import_icon(entry["iconPath"])
        icon = shared["icons"][entry["iconPath"]]
        descriptor = create_blueprint(entry["descriptorPath"], unreal.FGBuildingDescriptor)
        defaults = defaults_of(descriptor.generated_class())
        defaults.set_editor_property("mBuildableClass", buildable.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("mCategory", shared["categories"][entry["buildMenu"]["category"]])
        defaults.set_editor_property("mSubCategories", [shared["categories"][entry["buildMenu"]["subCategory"]]])
        defaults.set_editor_property("mMenuPriority", float(entry["menuPriority"]))
        defaults.set_editor_property("mSmallIcon", icon)
        defaults.set_editor_property("mPersistentBigIcon", icon)
        compile_and_save(descriptor)

        recipe = create_blueprint(entry["recipePath"], unreal.FGRecipe)
        defaults = defaults_of(recipe.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mIngredients", [unreal.ItemAmount(item_class=load_class(item), amount=amount) for item, amount in manifest_basis_ingredients()])
        defaults.set_editor_property("mProduct", [unreal.ItemAmount(item_class=descriptor.generated_class(), amount=1)])
        defaults.set_editor_property("mProducedIn", vanilla.get_editor_property("mProducedIn"))
        defaults.set_editor_property("mMaterialCustomizationRecipe", None)
        compile_and_save(recipe)
        recipes.append(recipe.generated_class())
    return recipes


def build_spline_patterns(shared):
    """The customizer's MKI Patterns > Roads (a Blueprint child of the vanilla Patterns category, which shows its patterns'
    icons where a plain FGCustomizerCategory showed a swatch circle, #202, and a FGCustomizerSubCategory, both with the
    vanilla pattern category icon by reference) and each road marking pattern: a Blueprint of the native MKIRoadMarkingPattern (a vanilla
    pattern descriptor the road draws itself) and its FGCustomizationRecipe, produced in the build gun as the vanilla
    pattern recipes are. Returns the recipe classes for the schematic."""
    settings = PLAN["splineRoad"]["patterns"]
    categories = {}
    for key, parent in (("category", load_class(settings["category"]["parent"])), ("subCategory", unreal.FGCustomizerSubCategory)):
        blueprint = create_blueprint(settings[key + "Path"], parent)
        defaults = defaults_of(blueprint.generated_class())
        defaults.set_editor_property("mDisplayName", text(settings[key]["displayName"]))
        defaults.set_editor_property("mMenuPriority", float(settings[key]["menuPriority"]))
        defaults.set_editor_property("mCategoryIcon", brush(settings["categoryIcon"]))
        compile_and_save(blueprint)
        categories[key] = blueprint.generated_class()
    basis = defaults_of(load_class(class_path_of(settings["recipeBasis"])))
    recipes = []
    for entry in settings["entries"]:
        # The pattern draws its own 4-lane icon (road-pack-icons.ts renderSplineRoadIcon), its dashed lane lines included.
        if entry["iconPath"] not in shared["icons"]:
            shared["icons"][entry["iconPath"]] = import_icon(entry["iconPath"])
        icon = shared["icons"][entry["iconPath"]]
        descriptor = create_blueprint(entry["descriptorPath"], load_class(settings["nativeClass"]))
        defaults = defaults_of(descriptor.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("ID", int(entry["id"]))
        # The roads and the intersections (#229: the maintainer could not apply the pattern to an intersection) draw it.
        defaults.set_editor_property("mValidBuildables", [load_class(path) for path in settings["validBuildables"]])
        defaults.set_editor_property("mIcon", icon)
        defaults.set_editor_property("mSmallIcon", icon)
        defaults.set_editor_property("mPersistentBigIcon", icon)
        defaults.set_editor_property("mCategory", categories["category"])
        defaults.set_editor_property("mSubCategories", [categories["subCategory"]])
        defaults.set_editor_property("mMenuPriority", 1.0)
        for name, key in (("LineWidthCm", "lineWidthCm"), ("CenterGapCm", "centerGapCm"), ("EdgeInsetCm", "edgeInsetCm"),
                          ("DashLengthCm", "dashLengthCm"), ("DashGapCm", "dashGapCm"), ("LiftCm", "liftCm")):
            defaults.set_editor_property(name, float(entry[key]))
        defaults.set_editor_property("YellowMaterial", load_object(entry["yellowMaterial"]))
        defaults.set_editor_property("WhiteMaterial", load_object(entry["whiteMaterial"]))
        compile_and_save(descriptor)

        recipe = create_blueprint(entry["recipePath"], unreal.FGCustomizationRecipe)
        defaults = defaults_of(recipe.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mCustomizationDesc", descriptor.generated_class())
        defaults.set_editor_property("mProducedIn", basis.get_editor_property("mProducedIn"))
        compile_and_save(recipe)
        recipes.append(recipe.generated_class())
    return recipes



def build_supports(shared):
    """Road supports (#194): a Blueprint of the native MKIRoadSupports laying the vanilla pillar meshes; its descriptor with
    the vanilla concrete pillar's icons; a recipe of the vanilla concrete pillar's ingredients (per piece)."""
    recipes = []
    for entry in support_entries():
        vanilla_descriptor = defaults_of(load_class(class_path_of(entry["vanillaDescriptor"])))
        vanilla_recipe = load_class(class_path_of(entry["vanillaRecipe"]))
        buildable = create_blueprint(entry["classPath"], load_class(entry["parentClass"]))
        unreal.BlueprintEditorLibrary.compile_blueprint(buildable)
        defaults = defaults_of(buildable.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("mBaseMesh", load_object(entry["baseMesh"]))
        defaults.set_editor_property("mMiddleMesh", load_object(entry["middleMesh"]))
        vanilla_buildable = defaults_of(vanilla_descriptor.get_editor_property("mBuildableClass"))
        defaults.set_editor_property("mAllowColoring", True)
        defaults.set_editor_property("mSwatchGroup", vanilla_buildable.get_editor_property("mSwatchGroup"))
        compile_and_save(buildable)

        descriptor = create_blueprint(entry["descriptorPath"], unreal.FGBuildingDescriptor)
        defaults = defaults_of(descriptor.generated_class())
        defaults.set_editor_property("mBuildableClass", buildable.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mDescription", text(entry["description"]))
        defaults.set_editor_property("mCategory", shared["categories"][entry["buildMenu"]["category"]])
        defaults.set_editor_property("mSubCategories", [shared["categories"][entry["buildMenu"]["subCategory"]]])
        defaults.set_editor_property("mMenuPriority", float(entry["menuPriority"]))
        defaults.set_editor_property("mSmallIcon", vanilla_descriptor.get_editor_property("mSmallIcon"))
        defaults.set_editor_property("mPersistentBigIcon", vanilla_descriptor.get_editor_property("mPersistentBigIcon"))
        compile_and_save(descriptor)

        recipe = create_blueprint(entry["recipePath"], unreal.FGRecipe)
        defaults = defaults_of(recipe.generated_class())
        defaults.set_editor_property("mDisplayName", text(entry["displayName"]))
        defaults.set_editor_property("mIngredients", list(defaults_of(vanilla_recipe).get_editor_property("mIngredients")))
        defaults.set_editor_property("mProduct", [unreal.ItemAmount(item_class=descriptor.generated_class(), amount=1)])
        defaults.set_editor_property("mProducedIn", defaults_of(vanilla_recipe).get_editor_property("mProducedIn"))
        defaults.set_editor_property("mMaterialCustomizationRecipe", None)
        compile_and_save(recipe)
        recipes.append(recipe.generated_class())
    return recipes


def build_registration(recipes):
    """The MKI Roads milestone, which unlocks every spline road and road pattern recipe, and the root game-world module
    that registers it."""
    schematic_settings = PACKAGE["schematic"]
    schematic = create_blueprint(schematic_settings["path"], unreal.FGSchematic)
    defaults = defaults_of(schematic.generated_class())
    unlock = unreal.new_object(load_class(PACKAGE["unlockRecipeClass"]), outer=defaults, name="UnlockRoadKitRecipes")
    unlock.set_editor_property("mRecipes", list(recipes))
    defaults.set_editor_property("mType", unreal.SchematicType.EST_MILESTONE if schematic_settings["type"] == "milestone" else unreal.SchematicType.EST_CUSTOM)
    defaults.set_editor_property("mTechTier", int(schematic_settings["techTier"]))
    defaults.set_editor_property("mDisplayName", text(schematic_settings["displayName"]))
    defaults.set_editor_property("mCost", [])
    defaults.set_editor_property("mTimeToComplete", 1.0)
    defaults.set_editor_property("mUnlocks", [unlock])
    # The HUB needs an icon to show a milestone's unlocks; reuse the vanilla asphalt foundation icon by reference.
    defaults.set_editor_property("mSchematicIcon", brush(MENU["categoryIcon"]))
    defaults.set_editor_property("mSmallSchematicIcon", load_object(MENU["categoryIcon"]))
    compile_and_save(schematic)

    # The root module registers the schematic; roads are asphalt only (#159), so it joins no material descriptors.
    module = create_blueprint(PACKAGE["rootModulePath"], load_class(PACKAGE["nativeModule"]["rootModuleParentClass"]))
    defaults = defaults_of(module.generated_class())
    defaults.set_editor_property("bRootModule", True)
    defaults.set_editor_property("mSchematics", [schematic.generated_class()])
    compile_and_save(module)


def run_import():
    require_empty_mount()
    # Costs are the manifest's per-foundation ingredients; refuse to build if the vanilla asphalt 8x4 recipe disagrees.
    for path in [COST["basisRecipe"], ASPHALT["vanillaFoundationRecipe"]]:
        if ingredients_of(load_class(path)) != manifest_basis_ingredients():
            raise RuntimeError("SDK %s ingredients %s differ from package.cost.perFoundation %s" % (path, ingredients_of(load_class(path)), manifest_basis_ingredients()))
    shared = {"categories": build_categories(), "icons": {}}
    # Lane markings (#159): the two paint materials the road marking patterns draw with.
    for settings in PLAN["paints"].values():
        build_paint_material(settings)
    build_registration(build_spline_roads(shared) + build_junctions(shared) + build_spline_patterns(shared) + build_supports(shared))
    unreal.EditorAssetLibrary.save_directory(MOUNT, only_if_is_dirty=False, recursive=True)
    report["assets"] = sorted(unreal.EditorAssetLibrary.list_assets(MOUNT, recursive=True, include_folder=False))


# Verify --------------------------------------------------------------------------------------------


def menu_settings(path):
    return next(settings for settings, _ in menu_categories() if settings["path"] == path)


def resolve_menu(descriptor):
    """Build-menu placement of a descriptor class: its category and subcategories with their display names."""
    defaults = defaults_of(descriptor)
    category = defaults.get_editor_property("mCategory")
    subcategories = list(defaults.get_editor_property("mSubCategories"))

    def name(item):
        return str(defaults_of(item).get_editor_property("mDisplayName")) if item else None

    return {"category": path_of(category), "categoryName": name(category), "subCategories": [path_of(item) for item in subcategories], "subCategoryNames": [name(item) for item in subcategories]}


def verify_categories():
    for settings, parent in menu_categories():
        key = "category" if parent is unreal.FGBuildCategory else "subCategory"
        try:
            found = load_class(settings["path"])
        except RuntimeError as error:
            check(False, str(error))
            continue
        defaults = defaults_of(found)
        check(isinstance(defaults, parent) and (key != "category" or not isinstance(defaults, unreal.FGBuildSubCategory)), "%s is an %s" % (asset_name(settings["path"]), parent.__name__))
        check(str(defaults.get_editor_property("mDisplayName")) == settings["displayName"] and close(defaults.get_editor_property("mMenuPriority"), settings["menuPriority"], 1e-6),
              "%s is %s (priority %s)" % (asset_name(settings["path"]), defaults.get_editor_property("mDisplayName"), settings["menuPriority"]))
        icon = defaults.get_editor_property("mCategoryIcon").get_editor_property("resource_object")
        check(path_of(icon) == MENU["categoryIcon"], "%s icon %s references the base-game placeholder" % (asset_name(settings["path"]), path_of(icon)))
    vanilla = defaults_of(load_class(COST["basisRecipe"])).get_editor_property("mProduct")
    report["vanillaBuildMenu"] = resolve_menu(vanilla[0].get_editor_property("item_class")) if vanilla else None


def verify_vanilla_asphalt():
    """The vanilla evidence behind package.asphalt and package.cost: the asphalt 8x4 foundation's cost, swatch group, and
    default swatch, which the spline road buildables keep."""
    recipe = load_class(ASPHALT["vanillaFoundationRecipe"])
    products = defaults_of(recipe).get_editor_property("mProduct")
    buildable = defaults_of(defaults_of(products[0].get_editor_property("item_class")).get_editor_property("mBuildableClass")) if products else None
    swatch_group = path_of(buildable.get_editor_property("mSwatchGroup")) if buildable else None
    default_swatch = path_of(buildable.get_editor_property("mDefaultSwatchCustomizationOverride")) if buildable else None
    check(swatch_group == ASPHALT["swatchGroup"] and default_swatch == class_path_of(ASPHALT["defaultSwatch"]),
          "vanilla asphalt foundation swatch group %s, default swatch %s" % (swatch_group, default_swatch))
    check(ingredients_of(recipe) == manifest_basis_ingredients(), "vanilla asphalt 8x4 recipe costs %s == package.cost.perFoundation" % ingredients_of(recipe))
    report["vanillaAsphalt"] = {"recipe": path_of(recipe), "swatchGroup": swatch_group, "defaultSwatch": default_swatch, "ingredients": ingredients_of(recipe)}


def verify_paints():
    """Lane markings (#159): both paint materials exist as project materials."""
    for key, settings in PLAN["paints"].items():
        material = unreal.load_asset(package_name(settings["material"]))
        check(isinstance(material, unreal.Material), "paint %s %s is a project material" % (key, settings["material"]))


def verify_native_registration():
    """The root module derives from the native module class and carries no material registration (#159)."""
    root = defaults_of(load_class(PACKAGE["rootModulePath"]))
    check(isinstance(root, unreal.MKIRoadsGameWorldModule), "%s derives from %s" % (asset_name(PACKAGE["rootModulePath"]), PACKAGE["nativeModule"]["rootModuleParentClass"]))
    retired = []
    for name in ("MaterialGroups", "RoadBuildCategory", "RoadBuildSubCategories", "DefaultRoadMaterial"):
        try:
            root.get_editor_property(name)
            retired.append(name)
        except Exception:
            pass
    check(not retired, "the root module has no material registration properties left (%s)" % retired)


def verify_registration():
    schematic = defaults_of(load_class(PACKAGE["schematic"]["path"]))
    recipes = sorted(path_of(recipe) for unlock in schematic.get_editor_property("mUnlocks") if isinstance(unlock, unreal.FGUnlockRecipe) for recipe in unlock.get_editor_property("mRecipes"))
    check(recipes == sorted([entry["recipePath"] for entry in menu_entries()] + spline_pattern_recipes()),
          "schematic unlocks every spline road, support, and road pattern recipe, and nothing else (%d)" % len(recipes))
    check(schematic.get_editor_property("mTechTier") == PACKAGE["schematic"]["techTier"], "schematic tech tier")
    module = defaults_of(load_class(PACKAGE["rootModulePath"]))
    check(module.get_editor_property("bRootModule") is True, "root game-world module is flagged bRootModule")
    check([path_of(item) for item in module.get_editor_property("mSchematics")] == [PACKAGE["schematic"]["path"]], "root module registers the schematic")
    data = unreal.AssetRegistryHelpers.get_asset_registry().get_asset_by_object_path(package_name(PACKAGE["rootModulePath"]) + "." + asset_name(PACKAGE["rootModulePath"]))
    tag = data.get_tag_value("bRootModule") if data else None
    check(str(tag) == "True", "asset registry tag bRootModule=%s (SML discovers root modules by it)" % tag)
    assets = sorted(unreal.EditorAssetLibrary.list_assets(MOUNT, recursive=True, include_folder=False))
    check(not any("/FactoryGame/" in item for item in assets), "plugin content contains no base-game copies")
    check([package_name(item) for item in assets] == PLAN["assetPackages"], "plugin content is exactly the planned %d assets: %s" % (len(PLAN["assetPackages"]), assets))
    report["contents"] = assets


def discovered_root_modules():
    """Root modules as SML finds them: Blueprint assets under the plugin mount whose registry tag is bRootModule=True."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    return sorted("%s.%s_C" % (data.package_name, data.asset_name) for data in registry.get_assets_by_path(MOUNT, recursive=True) if str(data.get_tag_value("bRootModule")) == "True")


def verify_registration_chain():
    """Walk the native registration chain from the discovered root module through object references only:
    RootGameWorld -> mSchematics -> FGUnlockRecipe.mRecipes -> mProduct descriptor -> mCategory / mSubCategories and
    mBuildableClass -> mHologramClass / mDecoratorClass. The plan is used only to compare the result, never to find a link."""
    roots = discovered_root_modules()
    check(roots == [PACKAGE["rootModulePath"]], "SML discovers exactly one root module %s (expected %s)" % (roots, PACKAGE["rootModulePath"]))
    chain = []
    for root_path in roots:
        root = defaults_of(load_class(root_path))
        check(isinstance(root, unreal.GameWorldModule) and root.get_editor_property("bRootModule") is True, "%s is a root GameWorldModule" % asset_name(root_path))
        for schematic in list(root.get_editor_property("mSchematics")):
            schematic_defaults = defaults_of(schematic)
            check(isinstance(schematic_defaults, unreal.FGSchematic) and path_of(schematic).startswith(MOUNT + "/") and enum_name(schematic_defaults.get_editor_property("mType")) == "EST_MILESTONE"
                  and schematic_defaults.get_editor_property("mTechTier") == PACKAGE["schematic"]["techTier"] and len(schematic_defaults.get_editor_property("mCost")) == 0,
                  "%s is a free tier %s MKI Roads milestone" % (asset_name(path_of(schematic)), schematic_defaults.get_editor_property("mTechTier")))
            unlocks = [unlock for unlock in schematic_defaults.get_editor_property("mUnlocks") if isinstance(unlock, unreal.FGUnlockRecipe)]
            for recipe in [recipe for unlock in unlocks for recipe in unlock.get_editor_property("mRecipes")]:
                products = defaults_of(recipe).get_editor_property("mProduct")
                descriptor = products[0].get_editor_property("item_class") if len(products) == 1 else None
                buildable = defaults_of(descriptor).get_editor_property("mBuildableClass") if descriptor else None
                menu = resolve_menu(descriptor) if descriptor else {}
                chain.append({"root": root_path, "schematic": path_of(schematic), "recipe": path_of(recipe), "descriptor": path_of(descriptor), "buildable": path_of(buildable),
                              "hologram": path_of(defaults_of(buildable).get_editor_property("mHologramClass")) if buildable else None,
                              "decorator": path_of(defaults_of(buildable).get_editor_property("mDecoratorClass")) if buildable else None,
                              "category": menu.get("category"), "subCategories": menu.get("subCategories"), "producedIn": [str(item) for item in defaults_of(recipe).get_editor_property("mProducedIn")]})
    splines = {entry["recipePath"]: entry for entry in menu_entries()}
    patterns = {entry["recipePath"]: entry for entry in PLAN["splineRoad"]["patterns"]["entries"]}
    check(sorted(item["recipe"] for item in chain) == sorted(list(splines) + list(patterns)), "the chain reaches exactly the %d planned recipes" % (len(splines) + len(patterns)))
    for item in chain:
        if item["recipe"] in patterns:
            # A customization recipe: no product, its pattern descriptor, produced in the build gun as vanilla patterns are.
            entry = patterns[item["recipe"]]
            check(path_of(defaults_of(load_class(item["recipe"])).get_editor_property("mCustomizationDesc")) == entry["descriptorPath"] and item["descriptor"] is None
                  and any(package_name(PACKAGE["producedIn"]) in produced for produced in item["producedIn"]),
                  "%s -> %s in the customizer, produced in the build gun (%s)" % (asset_name(item["recipe"]), asset_name(entry["descriptorPath"]), item["producedIn"]))
            continue
        if item["recipe"] not in splines:
            continue
        entry = splines[item["recipe"]]
        menu = menu_settings(entry["buildMenu"]["subCategory"])
        expected = {"descriptor": entry["descriptorPath"], "buildable": entry["classPath"], "hologram": entry["hologramClass"], "decorator": None,
                    "category": entry["buildMenu"]["category"], "subCategories": [entry["buildMenu"]["subCategory"]]}
        check({key: item[key] for key in expected} == expected and any(package_name(PACKAGE["producedIn"]) in produced for produced in item["producedIn"]),
              "%s -> %s -> %s -> %s -> %s (%s) in %s > %s" % (asset_name(item["root"]), asset_name(item["schematic"]), asset_name(item["recipe"]), asset_name(item["descriptor"] or "/?"),
                                                           asset_name(item["buildable"] or "/?"), asset_name(item["hologram"] or "/?"), MENU["category"]["displayName"], menu["displayName"]))
    report["registrationChain"] = chain
    verify_build_menu(chain)


def menu_priority(recipe):
    return defaults_of(defaults_of(load_class(recipe)).get_editor_property("mProduct")[0].get_editor_property("item_class")).get_editor_property("mMenuPriority")


def verify_build_menu(chain):
    """The build menu as GetAvailableRecipesInSubCategory builds it from the unlocked recipes (product category and
    subcategory, then menu priority): the spline road entries, thinnest first. Their recipes have no material
    customization key, so the shipped material filter (GetAvailableRecipesForMaterialDescriptorInSubCategory) keeps them
    under every material (#159)."""
    listed = {}
    for item in chain:
        if item["category"] == MENU["category"]["path"]:
            for subcategory in item["subCategories"] or []:
                listed.setdefault(subcategory, []).append(item["recipe"])
    menu = {}
    for settings in MENU["subCategories"]:
        actual = sorted(listed.get(settings["path"], []), key=menu_priority)
        expected = sorted([entry["recipePath"] for entry in menu_entries() if entry["buildMenu"]["subCategory"] == settings["path"]], key=menu_priority)
        keyed = [recipe for recipe in actual if defaults_of(load_class(recipe)).get_editor_property("mMaterialCustomizationRecipe")]
        check(actual == expected and len(set(actual)) == len(actual) and not keyed,
              "%s > %s categorizes %d roads and attachments once each, in plan order, none with a customization key so every material filter lists them (expected %d; keyed: %d)" % (
                  MENU["category"]["displayName"], settings["displayName"], len(actual), len(expected), len(keyed)))
        menu[settings["displayName"]] = [asset_name(item) for item in actual]
    occupied = sorted(path for path in (settings["path"] for settings in MENU["subCategories"]) if any(entry["buildMenu"]["subCategory"] == path for entry in menu_entries()))
    check(sorted(listed) == occupied, "only the planned subcategories hold MKI Roads roads and attachments (%s)" % sorted(listed))
    unlisted = sorted(item["recipe"] for item in chain if not item["category"] and not item["subCategories"] and item["recipe"] not in spline_pattern_recipes())
    check(not unlisted, "every unlocked road recipe is categorized (uncategorized: %s)" % unlisted)
    report["buildMenu"] = menu


def verify_spline_roads():
    """Each spline road entry is a Blueprint of the native road with its deck thickness, listed in the Roads menu, whose
    recipe is exactly one asphalt 8x4 foundation (the hologram multiplies it by the deck area) with no material
    customization key, and whose dismantle refund stays the road's own (no Blueprint refund override)."""
    for entry in spline_road_entries():
        buildable = load_class(entry["classPath"])
        if not check(buildable is not None, "%s exists" % entry["classPath"]):
            continue
        label = asset_name(entry["classPath"])
        check(unreal.MathLibrary.class_is_child_of(buildable, load_class(entry["parentClass"])), "%s derives from %s" % (label, entry["parentClass"]))
        defaults = defaults_of(buildable)
        check(close(defaults.get_editor_property("mDeckThicknessCm"), entry["thicknessCm"], 0.01), "%s deck is %s cm thick" % (label, entry["thicknessCm"]))
        check(path_of(defaults.get_editor_property("mHologramClass")) == entry["hologramClass"], "%s builds with %s" % (label, entry["hologramClass"]))
        check(str(defaults.get_editor_property("mDisplayName")) == entry["displayName"], "%s is named %s" % (label, entry["displayName"]))
        swatch_group, default_swatch = path_of(defaults.get_editor_property("mSwatchGroup")), path_of(defaults.get_editor_property("mDefaultSwatchCustomizationOverride"))
        check(swatch_group == ASPHALT["swatchGroup"] and default_swatch == class_path_of(ASPHALT["defaultSwatch"]),
              "%s swatch group %s and default swatch %s are the vanilla asphalt foundation's" % (label, swatch_group, default_swatch))
        overrides = refund_overrides(unreal.load_asset(package_name(entry["classPath"])))
        check(not overrides, "%s does not override %s (overrides: %s)" % (label, "/".join(REFUND_OVERRIDES), overrides))
        if not entry["listed"]:
            check(unreal.EditorAssetLibrary.does_asset_exist(package_name(entry["recipePath"])) is False, "%s has no recipe (retired, #233)" % label)
            continue
        recipe = load_class(entry["recipePath"])
        check(recipe is not None and ingredients_of(recipe) == manifest_basis_ingredients(), "%s costs one asphalt 8x4 foundation (%s)" % (asset_name(entry["recipePath"]), ingredients_of(recipe) if recipe else None))
        customization = defaults_of(recipe).get_editor_property("mMaterialCustomizationRecipe") if recipe else None
        check(not customization, "%s has no material customization recipe, so its material cannot be swapped (#159; found %s)" % (asset_name(entry["recipePath"]), path_of(customization) if customization else None))
        descriptor = defaults_of(load_class(entry["descriptorPath"]))
        icons = [path_of(descriptor.get_editor_property(key)) for key in ("mSmallIcon", "mPersistentBigIcon")]
        check(icons == [entry["iconPath"]] * 2, "%s icons %s are the generated %s icon" % (asset_name(entry["descriptorPath"]), icons, asset_name(entry["iconPath"])))
        verify_icon(entry["iconPath"])
        report["assets"][entry["classPath"]] = {"swatchGroup": swatch_group, "defaultSwatch": default_swatch, "icon": icons[0], "parent": blueprint_parent_tags(entry["classPath"])}



def verify_junctions():
    """Each intersection (#227) is a Blueprint of the native junction with its arms and deck thickness, the asphalt deck
    material, color and pattern customization, the asphalt swatches, its own icon, and a recipe of one asphalt 8x4
    foundation with no material customization key and no Blueprint refund override."""
    for entry in junction_entries():
        buildable = load_class(entry["classPath"])
        if not check(buildable is not None, "%s exists" % entry["classPath"]):
            continue
        label = asset_name(entry["classPath"])
        check(unreal.MathLibrary.class_is_child_of(buildable, load_class(entry["parentClass"])), "%s derives from %s" % (label, entry["parentClass"]))
        defaults = defaults_of(buildable)
        check(defaults.get_editor_property("mArms") == entry["arms"], "%s has %s arms" % (label, entry["arms"]))
        check(close(defaults.get_editor_property("mDeckThicknessCm"), entry["thicknessCm"], 0.01), "%s deck is %s cm thick" % (label, entry["thicknessCm"]))
        check(path_of(defaults.get_editor_property("mHologramClass")) == entry["hologramClass"], "%s builds with %s" % (label, entry["hologramClass"]))
        check(soft_path(defaults.get_editor_property("mDeckMaterial")) == PLAN["splineRoad"]["surfaceMaterial"], "%s deck uses %s" % (label, PLAN["splineRoad"]["surfaceMaterial"]))
        check(defaults.get_editor_property("mAllowColoring") is True and defaults.get_editor_property("mAllowPatterning") is True, "%s allows color and pattern customization" % label)
        swatch_group, default_swatch = path_of(defaults.get_editor_property("mSwatchGroup")), path_of(defaults.get_editor_property("mDefaultSwatchCustomizationOverride"))
        check(swatch_group == ASPHALT["swatchGroup"] and default_swatch == class_path_of(ASPHALT["defaultSwatch"]), "%s has the asphalt swatches" % label)
        overrides = refund_overrides(unreal.load_asset(package_name(entry["classPath"])))
        check(not overrides, "%s does not override %s (overrides: %s)" % (label, "/".join(REFUND_OVERRIDES), overrides))
        recipe = load_class(entry["recipePath"])
        check(recipe is not None and ingredients_of(recipe) == manifest_basis_ingredients(), "%s costs one asphalt 8x4 foundation" % asset_name(entry["recipePath"]))
        customization = defaults_of(recipe).get_editor_property("mMaterialCustomizationRecipe") if recipe else None
        check(not customization, "%s has no material customization recipe" % asset_name(entry["recipePath"]))
        descriptor = defaults_of(load_class(entry["descriptorPath"]))
        icons = [path_of(descriptor.get_editor_property(key)) for key in ("mSmallIcon", "mPersistentBigIcon")]
        check(icons == [entry["iconPath"]] * 2, "%s icons %s are the generated %s icon" % (asset_name(entry["descriptorPath"]), icons, asset_name(entry["iconPath"])))
        verify_icon(entry["iconPath"])
        report["assets"][entry["classPath"]] = {"arms": entry["arms"], "icon": icons[0], "parent": blueprint_parent_tags(entry["classPath"])}


def verify_supports():
    """Each road supports entry (#194) is a Blueprint of the native supports with the vanilla pillar meshes, the vanilla
    concrete pillar's icons, and its recipe's ingredients."""
    for entry in support_entries():
        buildable = load_class(entry["classPath"])
        if not check(buildable is not None, "%s exists" % entry["classPath"]):
            continue
        label = asset_name(entry["classPath"])
        check(unreal.MathLibrary.class_is_child_of(buildable, load_class(entry["parentClass"])), "%s derives from %s" % (label, entry["parentClass"]))
        defaults = defaults_of(buildable)
        check(soft_path(defaults.get_editor_property("mBaseMesh")) == entry["baseMesh"] and soft_path(defaults.get_editor_property("mMiddleMesh")) == entry["middleMesh"], "%s lays %s and %s" % (label, entry["baseMesh"], entry["middleMesh"]))
        check(path_of(defaults.get_editor_property("mHologramClass")) == entry["hologramClass"], "%s builds with %s" % (label, entry["hologramClass"]))
        recipe = load_class(entry["recipePath"])
        vanilla_cost = ingredients_of(load_class(class_path_of(entry["vanillaRecipe"])))
        check(recipe is not None and ingredients_of(recipe) == vanilla_cost and len(vanilla_cost) > 0, "%s costs the vanilla concrete pillar per piece (%s)" % (asset_name(entry["recipePath"]), vanilla_cost))
        descriptor = defaults_of(load_class(entry["descriptorPath"]))
        vanilla = defaults_of(load_class(class_path_of(entry["vanillaDescriptor"])))
        icons = [path_of(descriptor.get_editor_property(key)) for key in ("mSmallIcon", "mPersistentBigIcon")]
        check(icons == [path_of(vanilla.get_editor_property(key)) for key in ("mSmallIcon", "mPersistentBigIcon")] and all(icons), "%s icons %s are the vanilla concrete pillar's" % (asset_name(entry["descriptorPath"]), icons))


def verify_icon(icon_path):
    icon = unreal.load_asset(package_name(icon_path))
    if check(icon is not None, "icon %s exists" % icon_path):
        size = MENU["icons"]["sizePx"]
        check(icon.blueprint_get_size_x() == size and icon.blueprint_get_size_y() == size and enum_name(icon.get_editor_property("compression_settings")) == "TC_EDITOR_ICON"
              and enum_name(icon.get_editor_property("mip_gen_settings")) == "TMGS_NO_MIPMAPS" and icon.get_editor_property("srgb"),
              "icon %s is a %dx%d sRGB UI texture without mips" % (asset_name(icon_path), size, size))


def verify_spline_patterns():
    """MKI Patterns > Roads: the category (a child of the vanilla Patterns category) and subcategory exist with their names, and each road marking pattern is a
    Blueprint of the native pattern with its ID, dimensions, paints, categories, and only the spline road as a valid
    buildable, whose recipe customizes with it in the build gun and is unlocked by the MKI Roads schematic."""
    settings = PLAN["splineRoad"]["patterns"]
    for key in ("category", "subCategory"):
        found = load_class(settings[key + "Path"])
        if check(found is not None, "%s exists" % settings[key + "Path"]):
            check(str(defaults_of(found).get_editor_property("mDisplayName")) == settings[key]["displayName"], "%s is named %s" % (asset_name(settings[key + "Path"]), settings[key]["displayName"]))
    category = load_class(settings["categoryPath"])
    check(category is not None and unreal.MathLibrary.class_is_child_of(category, load_class(settings["category"]["parent"])), "%s is a child of the vanilla Patterns category %s" % (asset_name(settings["categoryPath"]), settings["category"]["parent"]))
    unlocked = [path_of(recipe) for unlock in defaults_of(load_class(class_path_of(PACKAGE["schematic"]["path"]))).get_editor_property("mUnlocks") for recipe in unlock.get_editor_property("mRecipes")]
    for entry in settings["entries"]:
        descriptor = load_class(entry["descriptorPath"])
        if not check(descriptor is not None, "%s exists" % entry["descriptorPath"]):
            continue
        name = asset_name(entry["descriptorPath"])
        check(unreal.MathLibrary.class_is_child_of(descriptor, load_class(settings["nativeClass"])), "%s derives from %s" % (name, settings["nativeClass"]))
        defaults = defaults_of(descriptor)
        check(defaults.get_editor_property("ID") == entry["id"], "%s has pattern ID %s" % (name, entry["id"]))
        check([path_of(item) for item in defaults.get_editor_property("mValidBuildables")] == settings["validBuildables"], "%s applies only to %s" % (name, settings["validBuildables"]))
        check(path_of(defaults.get_editor_property("mCategory")) == settings["categoryPath"] and [path_of(item) for item in defaults.get_editor_property("mSubCategories")] == [settings["subCategoryPath"]],
              "%s is listed in %s > %s" % (name, settings["category"]["displayName"], settings["subCategory"]["displayName"]))
        for prop, key in (("LineWidthCm", "lineWidthCm"), ("CenterGapCm", "centerGapCm"), ("EdgeInsetCm", "edgeInsetCm"), ("DashLengthCm", "dashLengthCm"), ("DashGapCm", "dashGapCm"), ("LiftCm", "liftCm")):
            check(close(defaults.get_editor_property(prop), entry[key], 0.001), "%s %s is %s" % (name, prop, entry[key]))
        verify_icon(entry["iconPath"])
        recipe = load_class(entry["recipePath"])
        if check(recipe is not None, "%s exists" % entry["recipePath"]):
            check(path_of(defaults_of(recipe).get_editor_property("mCustomizationDesc")) == entry["descriptorPath"], "%s customizes with %s" % (asset_name(entry["recipePath"]), name))
            check(entry["recipePath"] in unlocked, "%s is unlocked by the MKI Roads schematic" % asset_name(entry["recipePath"]))
    for entry in spline_road_entries():
        defaults = defaults_of(load_class(entry["classPath"]))
        check(soft_path(defaults.get_editor_property("mDeckMaterial")) == PLAN["splineRoad"]["surfaceMaterial"], "%s deck uses %s" % (asset_name(entry["classPath"]), PLAN["splineRoad"]["surfaceMaterial"]))
        check(defaults.get_editor_property("mAllowColoring") is True and defaults.get_editor_property("mAllowPatterning") is True, "%s allows color and pattern customization" % asset_name(entry["classPath"]))


def run_verify():
    scan_mount()
    verify_spline_roads()
    verify_junctions()
    verify_spline_patterns()
    verify_supports()
    verify_categories()
    verify_vanilla_asphalt()
    verify_paints()
    verify_registration()
    verify_registration_chain()
    verify_native_registration()


try:
    if CONFIG["mode"] == "import":
        run_import()
    elif CONFIG["mode"] == "verify":
        run_verify()
    else:
        raise RuntimeError("Unknown mode %s" % CONFIG["mode"])
except Exception:  # noqa: BLE001 - every failure is reported, then the commandlet exits non-zero
    report["errors"].append(traceback.format_exc())

report["ok"] = not report["errors"]
with open(CONFIG["report"], "w", encoding="utf-8", newline="\n") as handle:
    json.dump(report, handle, indent=2, sort_keys=True, default=str)
    handle.write("\n")
print("MKIR_EDITOR_%s %s" % ("OK" if report["ok"] else "FAILED", CONFIG["report"]))

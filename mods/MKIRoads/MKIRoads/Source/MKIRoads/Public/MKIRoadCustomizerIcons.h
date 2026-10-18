#pragma once

#include "CoreMinimal.h"

/**
 * Shows MKI Patterns' patterns with their icons in the customizer (#220). The maintainer: the North American Road tile showed a
 * swatch circle, then whatever icon the tile had shown in the category opened before.
 *
 * Read from the game's Blueprints: the customizer's tile (BPW_CustomiserIcon) takes its item's category with the native
 * UFGItemDescriptor::GetCategory, then asks BPHUDHelpers' GetCustomiserTypeFromCategory, a fixed map of exactly the four
 * vanilla categories (CC_Swatch color, CC_Material, CC_Pattern, CC_Skin). Any other category falls to color, whose
 * tile draws a swatch and never sets an image. MKI Patterns (a Blueprint child of CC_Pattern) is not a key there.
 *
 * Install() wraps the native thunk of UFGItemDescriptor::GetCategory: called from the customizer's tile, a category
 * derived from CC_Pattern reads as CC_Pattern, so the tile draws a pattern and its icon. Every other caller, the tabs
 * included (the pattern stays in MKI Patterns), sees the category unchanged. Game builds only.
 */
struct MKIROADS_API FMKIRoadCustomizerIcons
{
	/** Wraps UFGItemDescriptor::GetCategory's thunk once; false if the function is missing. */
	static bool Install();
	/** True when an object running script is the customizer's tile widget. */
	static bool IsCustomizerTile(const UObject* ScriptObject);
	/** Category as the tile should see it: a child of the vanilla Patterns category reads as that category. */
	static UClass* CategoryForTile(UClass* Category);
};

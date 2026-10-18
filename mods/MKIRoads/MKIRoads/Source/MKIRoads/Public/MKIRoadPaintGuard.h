#pragma once

#include "CoreMinimal.h"

class UFGBuildGunStatePaint;
class UFGFactoryCustomizationDescriptor;

/**
 * Keeps vanilla materials and vanilla patterns off MKI Roads spline roads in the customizer (#208). The maintainer: the
 * customizer let them be applied, and the road did not change. A road is a foundation to the vanilla checks, so they
 * accepted both; the road draws neither (its deck is its own mesh, its markings its own patterns), and since #190 it
 * reverted a vanilla pattern only after it was applied.
 *
 * The SML hooks in the runtime module (game builds only) ask Refuses() on each paint-state tick, clearing the state's
 * own can-apply flags and adding the invalid-target disqualifier, and before a click, which is cancelled. Before each
 * tick the aimed road's own coloring and patterning flags are set for the active customization, so the vanilla checks
 * show a refused aim invalid as they do a vanilla material (#225). Since #225 vanilla colours and paint
 * finishes (swatches) are refused too; the road marking patterns and the vanilla pattern removers (ID 0) still apply.
 */
struct MKIROADS_API FMKIRoadPaintGuard
{
	/** True when Customization is a vanilla material, swatch or paint finish, or vanilla stencil pattern (what a road refuses). */
	static bool IsRefusedOnRoad(TSubclassOf<UFGFactoryCustomizationDescriptor> Customization);
	/** True when Paint is aimed at a spline road with a customization the road refuses active. */
	static bool Refuses(UFGBuildGunStatePaint* Paint);
	/** Before the paint tick: the aimed road's coloring and patterning flags allow only what it takes (#225). */
	static void PrepareAim(UFGBuildGunStatePaint* Paint);
	/** Clears Paint's can-apply flags and adds the invalid-target disqualifier, as its own checks would for an invalid aim. */
	static void MarkInvalid(UFGBuildGunStatePaint* Paint);
};

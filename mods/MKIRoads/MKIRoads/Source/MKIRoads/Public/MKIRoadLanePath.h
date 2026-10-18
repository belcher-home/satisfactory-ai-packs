#pragma once

#include "CoreMinimal.h"
#include "FGSplineBuildableInterface.h"

class AMKIRoadSplineRoad;
class AFGVehiclePathSegmentHologram;

/**
 * Vanilla vehicle paths that follow road lanes (#192). The maintainer: auto paths along lanes, since hand-laying paths along
 * curves is the hardest part of using roads. The vanilla path tool routes one arc from the start's facing to the aim;
 * along a curve it cut across the bend, off the road and into barriers (seen live, FGCD_StaticObstacle).
 *
 * With the vanilla truck, tractor, explorer, or universal path tool on a road:
 * - the start snaps to the center of the lane under the aim;
 * - when the aim is on the same road, the planned path is that lane's own line from the start to the aim: points every
 *   PointStepCm on the lane center, 1 cm above the deck (as vanilla projects), with tangents along the lane, so it follows
 *   curves and grades exactly.
 * The vanilla hologram then rebuilds its preview, validation, and construction (nodes included) from those points. Any
 * other aim keeps the vanilla routing. Applied by an SML hook in the runtime module.
 */
struct MKIROADS_API FMKIRoadLanePath
{
	/** Spacing of the planned points along the lane (the vanilla path's own spacing, seen live). */
	static constexpr float PointStepCm = 250.f;
	/** Height of the path above the deck top (vanilla projects paths 1 cm above the floor). */
	static constexpr float LiftCm = 1.f;
	/** Distance over which a start off the lane center (an existing path node) eases onto it. */
	static constexpr float EaseCm = 1000.f;
	/** How far past a road's end a point may be and still count as on it (a node exactly at the end). */
	static constexpr float EndToleranceCm = 50.f;
	/**
	 * A path's start or end aimed within this of a road's end goes exactly to that end (#210: the maintainer), so a path laid to
	 * the end of one road and one started at the start of the next meet at one point and vanilla joins their nodes.
	 */
	static constexpr float RoadEndSnapCm = 400.f;
	/** Along, moved to 0 or Length when within RoadEndSnapCm of that end of a road Length long. */
	static float SnapToRoadEnd(float Along, float Length);
	/** A start on an existing node follows the lane only if the node already faces within this of the lane's travel. */
	static constexpr float MaxNodeTurnDeg = 30.f;

	/** Lateral offset (right positive) of the center of the lane nearest LateralCm on a deck of Lanes lanes. */
	static float LaneCenterCm(int32 Lanes, float LateralCm);

	/**
	 * World points of the lane through Start on Road, from Start's distance along the road to End's, at most PointStepCm
	 * apart: false when either is off the road (beyond an edge, or more than a deck thickness from the deck).
	 */
	static bool PlanLane(const AMKIRoadSplineRoad& Road, const FVector& Start, const FVector& End, TArray<FSplinePointData>& OutWorldPoints);

	/** The SML hook body: after the vanilla path hologram placed itself for HitResult, follow the lane when on a road. */
	static void AfterSetHologramLocation(AFGVehiclePathSegmentHologram* Hologram, const FHitResult& HitResult);
};

#pragma once

#include "CoreMinimal.h"
#include "FGSplineBuildableInterface.h"

class AMKIRoadJunction;
class AMKIRoadSplineRoad;
class AFGVehiclePathNode;
class AFGVehiclePathSegment;

/**
 * Vehicle paths laid by the roads themselves (#235). The maintainer: every road and intersection gets a vehicle path, and they
 * connect to each other as sections are added: Universal paths (every vehicle), and at an intersection every move,
 * straight, right, and left.
 *
 * - A road: one path down the center of each lane, in that lane's direction of travel (traffic keeps right: the lanes
 *   right of the centerline run from the road's start to its end, the lanes left of it back); a one-lane road has a path
 *   each way along its center.
 * - An intersection: from each arm's arriving lanes to every other arm's leaving lanes (lane k to lane k, counted from the
 *   center), a straight line or a circular-arc turn.
 * Every path ends exactly on its lane's center at the road's end or the arm's end, where the joining road's or
 * intersection's own paths start: a path end within JoinRadiusCm of an existing path node uses that node, so vanilla joins
 * them into one network. The paths are the vanilla Universal path (Build_VehiclePath_Universal) with no recipe, so they
 * cost and refund nothing; the road or intersection lists them as dismantle children.
 */
struct MKIROADS_API FMKIRoadAutoPaths
{
	/** Spacing of a road path's points along its lane (as the lane paths of #192). */
	static constexpr float PointStepCm = 250.f;
	/** Height of the paths above the deck top (vanilla projects paths 1 cm above the floor). */
	static constexpr float LiftCm = 1.f;
	/** Version of the laid paths; a road or intersection with older ones lays them again (2: ends join existing nodes; 3: old paths all removed before any are laid; 4: a depot's bay path on arcs; 5: the smaller depot, #239; 6: straight dock paths, #247; 7: the large depot's merges inside it, #250; 8: 7 m straights and 17 m bends, #250; 9: the large depot 48 m again, #250). */
	static constexpr int32 Version = 9;
	/** A path end this close to an existing node uses it. */
	static constexpr float JoinRadiusCm = 50.f;

	/** World points of each lane path of Road (in its direction of travel). */
	static void RoadPaths(const AMKIRoadSplineRoad& Road, TArray<TArray<FSplinePointData>>& OutPaths);
	/** World points of each move through Junction: from every arm's arriving lanes to every other arm's leaving lanes. */
	static void JunctionPaths(const AMKIRoadJunction& Junction, TArray<TArray<FSplinePointData>>& OutPaths);
	/**
	 * A Hermite path from Start leaving along StartDir to End arriving along EndDir (both horizontal, unit), shaped as a
	 * circular arc between the headings (a straight line when they agree).
	 */
	static TArray<FSplinePointData> MovePath(const FVector& Start, const FVector& StartDir, const FVector& End, const FVector& EndDir);
	/** World points of a truck depot's paths with an end on its dock DockIndex (#239, #240). */
	static void DepotDockPaths(const AMKIRoadJunction& Junction, int32 DockIndex, TArray<TArray<FSplinePointData>>& OutPaths);
	/**
	 * Drops every path node's links to paths that are gone or going (#239). Live: path nodes kept links to removed paths, and
	 * a save made after one of those nodes changed crashed the vehicle subsystem on load. Run when a world begins play and
	 * after the MKI Roads removes paths; returns how many links were dropped.
	 */
	static int32 CleanStaleNodeLinks(UWorld* World);
	/** Local points and headings in Junction's frame as path points on its deck top. */
	static void LocalToWorld(const AMKIRoadJunction& Junction, const TArray<TArray<TPair<FVector2D, FVector2D>>>& Local, TArray<TArray<FSplinePointData>>& OutPaths);
	/** Whether a docking station that is not being removed stands at Location (#241). */
	static bool HasStandingStation(UWorld* World, const FVector& Location);
	/** Spawns a vanilla Universal path segment along each of Paths, joining ends to existing nodes; the segments built. */
	static void Spawn(UWorld* World, const TArray<TArray<FSplinePointData>>& Paths, TArray<AFGVehiclePathSegment*>& OutSegments);
};

#pragma once

#include "CoreMinimal.h"

class AMKIRoadSplineRoad;
class AFGBuildable;
class AFGHologram;

/** The side of a road a piece goes on: its left edge, its centerline, or its right edge (seen along the road). */
enum class EMKIRoadSide : uint8 { Left, Center, Right };

/** Where a piece snaps on a spline road: on the deck top, against an edge, or past an end. */
enum class EMKIRoadSnapMode : uint8 { Top, Edge, End };

/** A level square-footprint piece to snap: its length along its X axis, width along Y, height, and pivot. */
struct FMKIRoadSnapPiece
{
	float LengthCm = 800.f;
	float WidthCm = 800.f;
	float HeightCm = 400.f;
	/** True when the pivot is the walking surface (walkways, catwalks); false when it is the center (foundations). */
	bool bPivotOnFloor = false;
};

/**
 * Snapping vanilla foundations, walkways, and catwalks to MKI Roads spline roads (#188). The maintainer: they should snap on
 * top of a road, along its edges, and at its ends, in 2 m steps along the road.
 *
 * - Top: aimed at the deck, the piece rests on it, turned with the road, its ends on the road's 2 m marks (counted from
 *   the start) and its sides on 2 m marks across the deck (counted from the left edge), kept inside the deck.
 * - Edge: aimed at the outer metre of the deck or at its side, the piece sits outside that edge against it, flush with
 *   the deck top (a foundation's top, a walkway's floor), its ends on the 2 m marks.
 * - End: aimed within a metre of an end, the piece sits past it against the end, flush, its sides on the 2 m marks.
 *
 * Pieces stay level (yaw only): on a grade they take the deck height at their center. Pure geometry, so it is unit-shaped:
 * the SML hook in the runtime module applies it to the vanilla holograms.
 */
struct MKIROADS_API FMKIRoadBuildSnap
{
	/** Distance between snap marks along and across the road. */
	static constexpr float StepCm = 200.f;
	/** The outer band of the deck (and the ends) that selects Edge and End. */
	static constexpr float BandCm = 100.f;

	/**
	 * The snapped world transform of Piece aimed at Hit (surface normal HitNormal) on Road, rotated by ExtraYawDeg; false if
	 * the road has no spline. A side or end face is told by its normal: live, the build gun's hit location on the road's
	 * side was not the point on that face.
	 */
	static bool Snap(const AMKIRoadSplineRoad& Road, const FVector& Hit, const FVector& HitNormal, const FMKIRoadSnapPiece& Piece, float ExtraYawDeg, FTransform& Out, EMKIRoadSnapMode* OutMode = nullptr);

	/** Snaps a vanilla foundation or straight walkway/catwalk hologram aimed at a spline road; false leaves it to vanilla. */
	static bool SnapHologram(AFGHologram* Hologram, const FHitResult& HitResult);
	/**
	 * A vanilla truck station at a spline road's edge (#201). The maintainer: a truck stop should go directly on the road's edge.
	 * The station's docking point is its pivot, a truck docks heading along its +X, and its building stands on its -Y side
	 * (read from Build_TruckStation: belt ports at y -1005, the ladder at -1100). Aimed at a road, the pivot goes on the
	 * center of the lane along the nearer edge (or the side face aimed at), on the deck, snapped to StepCm along the road,
	 * turned so the building stands outside the edge: the truck bay is that lane. False if the road has no spline.
	 */
	static bool SnapDockingStation(const AMKIRoadSplineRoad& Road, const FVector& Hit, const FVector& HitNormal, FTransform& Out);

	/** Snaps a vanilla truck station hologram aimed at a spline road (not at a vehicle path); false leaves it to vanilla. */
	static bool SnapDockingStationHologram(AFGHologram* Hologram, const FHitResult& HitResult);

	/** A street light pole's distance inside the deck edge, and its snap step along the road. */
	static constexpr float LightEdgeInsetCm = 50.f;
	static constexpr float LightStepCm = 400.f;

	/**
	 * A vanilla street light on a spline road (#222; the placement of the removed Road Street Light, #193, #200, #203).
	 * The maintainer: no separate road light; the vanilla street light works on roads in its stead. The third of the deck aimed at
	 * picks the left edge, centerline, or right edge. The pole stands LightEdgeInsetCm inside that edge (or on the
	 * centerline), snapped to LightStepCm along the road, its arm (the mesh's +Y) across the road toward the center (from
	 * the centerline, toward the right), then turned ExtraYawDeg about the pole. False if the road has no spline.
	 */
	static bool SnapStreetLight(const AMKIRoadSplineRoad& Road, const FVector& Hit, float ExtraYawDeg, FTransform& Out);

	/** Snaps a vanilla street light hologram aimed at a spline road; false leaves it to vanilla. */
	static bool SnapStreetLightHologram(AFGHologram* Hologram, const FHitResult& HitResult);

	/** True for the vanilla street light (Build_StreetLight). */
	static bool IsStreetLight(const UClass* BuildClass);

	static constexpr float WallEdgeMarginCm = 5.f;

	/**
	 * Vanilla barriers and railings on a spline road (#223). The maintainer: no separate road barriers; the vanilla ones
	 * snap to a road and zoop along it, straight, up ramps, and round curves, in steps of their own length (the modern
	 * railing and the barriers 4 m; the industrial railing 8 m: AFGBuildableWall::mWidth, read from the paks).
	 * True for Build_Concrete_Barrier_01, Build_Barrier_Low_01, Build_Barrier_Tall_01, Build_Railing_01 and Build_Fence_01;
	 * chain link and tarp fences block the truck path beside them and stay vanilla (#312). OutStepCm is the piece's length.
	 */
	static bool IsRoadWall(const UClass* BuildClass, float* OutStepCm = nullptr);

	/**
	 * Vanilla walkways and catwalks along a road's edge (#224). The maintainer: as the barriers, for straights, corners,
	 * T-junctions, and crosses. True for a level walkway or catwalk (every one is a square of mSize, 4 m; ramps and stairs
	 * are not); OutStepCm is its size.
	 */
	static bool IsRoadWalkway(const UClass* BuildClass, float* OutStepCm = nullptr);

	/** A piece that runs along a road (IsRoadWall or IsRoadWalkway); OutStepCm is its length. */
	static bool IsRoadRun(const UClass* BuildClass, float* OutStepCm = nullptr);

	/**
	 * Snaps a vanilla barrier, fence, or railing hologram aimed at a spline road: the third of the deck aimed at picks the
	 * left edge, centerline, or right edge (the piece's middle on the edge, straddling it as vanilla does, #312), and the
	 * piece's ends go on the road's marks every step from its start. Its length axis (+Y, as vanilla walls zoop) lies along
	 * the chord of its stretch of road, pitched with the grade. False leaves it to vanilla.
	 */
	static bool SnapRoadWallHologram(AFGHologram* Hologram, const FHitResult& HitResult);

	/**
	 * A vanilla walkway or catwalk aimed at a road's edge (FMKIRoadBuildSnap::Snap's Edge mode): outside that edge, against it,
	 * its floor flush with the deck top, on the road's marks every piece from its start, turned with the chord of its
	 * stretch of road and by the scroll rotation in quarter turns. False leaves it to Snap.
	 */
	static bool SnapRoadWalkwayEdge(AFGHologram* Hologram, const AMKIRoadSplineRoad& Road, const FVector& Hit, const FVector& HitNormal);

	/**
	 * The zoop of a road run hologram on a road: whole pieces along the road toward the aim, on the local axis vanilla lets it
	 * zoop along that lies along the road. False: not on a road, or no such axis (vanilla's zoop stands).
	 */
	static bool RoadRunZoopFromHit(AFGHologram* Hologram, const FHitResult& HitResult, FIntVector& OutZoop);

	/**
	 * A zoop instance of a road run hologram on a road: vanilla's instance k pieces along the zoop axis (relative to the
	 * hologram) moved onto the road's k-th stretch from the hologram, in the hologram's pose on that stretch, so the zoop
	 * follows the road. False: not on a road.
	 */
	static bool RoadRunZoopInstance(const AFGHologram* Hologram, const FTransform& VanillaInstance, FTransform& OutInstance);

	/** Start of a span of Length on a StepCm grid nearest to centering it on Center (1D, the grid counted from 0). */
	static float SnapSpanStart(float Center, float Length);
};

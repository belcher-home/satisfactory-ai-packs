#pragma once

#include "CoreMinimal.h"
#include "MKIRoadSplineRoad.h"
#include "Buildables/FGBuildableFoundation.h"
#include "MKIRoadJunction.generated.h"

class UMKIRoadMarkingPattern;
class AFGVehiclePathSegment;
class UInstancedStaticMeshComponent;
class UProceduralMeshComponent;
class AMKIRoadTile;

/** A truck depot's dock (#237, #240), in the junction's local space: the docking point on the deck top, and the yaw a truck
 * station takes there, relative to the junction, so its building (its -Y) stands outside the deck. */
struct FMKIRoadDepotDock
{
	FVector2D Point = FVector2D::ZeroVector;
	float StationYawDeg = 0.f;
};

/** One arm of a junction, in its local space: the middle of the arm's end on the deck top, and the direction a road leaves along. */
struct FMKIRoadJunctionArm
{
	FVector2D Center = FVector2D::ZeroVector;
	FVector2D Outward = FVector2D(1.0, 0.0);
};

/** A painted rectangle on an arm (#229): along the arm from the crossing square's edge, lateral to the right of the arm's
 * outward direction, its length along the arm and width across, and its colour. */
struct FMKIRoadJunctionStripe
{
	float AlongCm = 0.f;
	float LateralCm = 0.f;
	float LengthCm = 0.f;
	float WidthCm = 0.f;
	bool bYellow = false;
};

/**
 * A junction's footprint in its local space (X along the through road, the deck top at Z 0): convex cells, each inside
 * one 8 m tile (TileOrigins, its tile's lower corner), so each takes the vanilla foundation's per-tile UVs; and its arms.
 */
struct FMKIRoadJunctionLayout
{
	TArray<TArray<FVector2D>> Cells;
	TArray<FVector2D> TileOrigins;
	TArray<FMKIRoadJunctionArm> Arms;
	/** Outline edges (each a cell edge no other cell shares), and the cell each belongs to; arm ends included. */
	TArray<TPair<FVector2D, FVector2D>> OutlineEdges;
	TArray<int32> OutlineCells;
	/** True for an outline edge that is an arm's end, where a road joins (no edge line is drawn there). */
	TArray<bool> OutlineIsArmEnd;
	/** A turnaround (#236, one arm): whole 8 m tiles, the least grid square that holds its loop (#314). */
	bool bTurnaround = false;
	/** A truck depot (#237): a straight piece with a bay. */
	bool bTruckDepot = false;
	/** Per cell: under a depot station (#241); drawn as its own mesh with the factory zone pattern (#246). */
	TArray<bool> CellIsPad;
	/**
	 * Per cell: the whole 8 m tile between two arms (#314). Its outer edges get no edge line; the curb return is painted
	 * across it instead, so the outline stays square like every foundation and the corner still reads as a road.
	 */
	TArray<bool> CellIsCorner;
};

/**
 * A road intersection (#227). The maintainer: intersections as their own pieces, a 3-way (T) and a 4-way, one entry each, with
 * Plus and Minus setting 1, 2, or 4 lanes (every arm the same width), shaped as the intersection he built from foundations
 * so a path can turn right from the outer lane into the nearest lane of the cross road: a square of the road's width, an
 * ApronCm apron on each arm, and the tile between two arms paved whole (#314; the maintainer: every foundation makes squares) with a
 * curb return of CurbRadiusCm painted across it. The right turn is about 12 m in radius, inside that tile.
 *
 * The 4-way has four arms (+X, -X, +Y, -Y); the 3-way has the through road (+X, -X) and the stem (-Y), the +Y side a
 * straight edge; the 2-way (#228, the maintainer: for tight corners) is a corner, +X and -Y, with one cut corner between them. Each arm ends ApronCm past the square; a road end snaps to the middle of an arm's end, heading out
 * (AMKIRoadSplineRoadHologram::ResolveEnd).
 *
 * The deck is drawn as the spline road's is: the vanilla asphalt foundation material by reference, one patch per 8 m tile
 * with the vanilla per-tile UVs, a level top at the actor's height and the deck thickness below. Collision is hidden
 * instances of the vanilla 8x4 foundation mesh scaled to each square tile. A road marking pattern draws a white edge line
 * inside every edge but the arm ends and the corner tiles' outer edges, the curb return across each corner tile, and, on each arm (#229), a crosswalk next to the
 * square, a stop line across the incoming lanes, and the road's center and lane lines out to the arm's end. Cost and refund: one vanilla asphalt 8x4 foundation per
 * started 64 m2 of deck, as the road.
 */
UCLASS()
class MKIROADS_API AMKIRoadJunction : public AFGBuildableFoundation
{
	GENERATED_BODY()

public:
	AMKIRoadJunction(const FObjectInitializer& ObjectInitializer);

	/** How far each arm reaches past the square, and the legs of the cut corners: the maintainer's intersection's 8 m. */
	static constexpr float ApronCm = 800.f;
	/** Radius of each corner's painted curb return about its outer corner: the apron's 8 m (#232 tried 4 m; the maintainer: it looked odd). */
	static constexpr float CurbRadiusCm = 800.f;
	/** Segments of each corner's painted curb return (#230, #314). */
	static constexpr int32 CornerArcSteps = 12;

	/**
	 * A turnaround (#236, one arm; the maintainer: the least space that loops a 1, 2, or 4 lane road's incoming traffic back to its
	 * outgoing lanes, with markings). Its bulb is centered on the local origin and its arm ends at +X (TurnaroundArmEndCm).
	 * Each arriving lane k veers right on a TurnVeerRadiusCm arc onto a loop of TurnLoopRadiusCm + k lanes about the center,
	 * runs round it (the center on its left), and veers right again into its leaving lane: a light-bulb loop. The deck is
	 * the least square of whole 8 m tiles, from the arm end back, that holds the outer loop's lane edge (#314; it was the
	 * round hull, which no foundation could meet).
	 */
	static constexpr float TurnLoopRadiusCm = 1200.f;
	static constexpr float TurnVeerRadiusCm = 800.f;
	/** The x of a turnaround's arm end: just past where its outer lane starts to veer. */
	static float TurnaroundArmEndCm(int32 Lanes);
	/**
	 * A dock's paths run straight this far each side of it, each its own path, before any curve (#247; the maintainer: a path out
	 * of a station that went straight into a curve made the truck stutter and jump ahead, vanilla stations too; #250: 5 m is
	 * enough; then 7 m to try with 17 m bends).
	 */
	static constexpr float DockStraightCm = 700.f;
	/** The stall box's inset from the bay line and from the pad's yellow trim (#318). */
	static constexpr float StallInsetCm = 60.f;
	/** The stall box's half length along the bay: two foundations long (#318, the maintainer). */
	static constexpr float StallHalfLengthCm = 800.f;
	/** The bay legend's margin to the stall box's lines (#318). */
	static constexpr float LegendMarginCm = 40.f;
	/** The legend's Roboto Black letters are stretched this many times taller than the face, as road words are (#318). */
	static constexpr float LegendStretch = 1.6f;
	/** The radius of a turnaround's round end: the outer loop's lane edge and a little more. */
	static float TurnaroundDeckRadiusCm(int32 Lanes);
	/** Radius of the painted island inside the inner loop: the inner lane's inside edge. */
	static constexpr float TurnaroundIslandRadiusCm() { return TurnLoopRadiusCm - AMKIRoadSplineRoad::LaneWidthCm * 0.5f; }
	/**
	 * The local loop of each arriving lane of a Lanes-lane turnaround, as two halves (the arm end to the far side of the
	 * loop, and back to the arm end): points on the deck top and their unit headings.
	 */
	static void TurnaroundLoops(int32 Lanes, TArray<TArray<TPair<FVector2D, FVector2D>>>& OutHalves, double SplitFraction = 0.5, int32 OnlyLane = INDEX_NONE);
	/**
	 * Truck depots (#237, #240, #250, #251), by their shape code in mArms: a 48 m piece of the road's 2 or 4 lanes, joined by
	 * roads at each end, with an 8 m bay on +Y (TruckDepotShape), and on -Y for the -X lanes too (TruckDepotBothShape). A
	 * path leaves the outer lane at the depot's end on a TruckBayBendRunCm bend of two arcs, runs DockStraightCm straight
	 * into and out of the dock on the bay's center, and rejoins the lane at the other end (the maintainer's working depot). The
	 * vanilla truck or fluid truck station snaps to the nearest dock, its building outside the deck; once one stands, the
	 * paths ending at that dock are laid again to end on its docking node. #251: the small depots (6 and 8) are gone.
	 */
	static constexpr int32 TruckDepotShape = 7;
	static constexpr int32 TruckDepotBothShape = 9;
	static bool IsDepotShape(int32 Arms) { return Arms == TruckDepotShape || Arms == TruckDepotBothShape; }
	static bool IsTurnaroundShape(int32 Arms) { return Arms == 1; }
	static bool IsBothSidesShape(int32 Arms) { return Arms == TruckDepotBothShape; }
	/** Half the length of a depot: its two bends and the dock's straights. */
	static constexpr float DepotHalfLengthCm = 2400.f;
	/** How far along the road a bay's bend runs (the maintainer, #250: 17 m bends with 7 m straights at the dock). */
	static constexpr float TruckBayBendRunCm = 1700.f;
	/** The radius and sweep of each of the two arcs of a bend running Run along while stepping Offset across. */
	static void BayBendArcs(double Run, double Offset, double& OutRadius, double& OutTheta)
	{
		OutRadius = (Run * Run + Offset * Offset) / (4.0 * Offset);
		OutTheta = FMath::Asin(FMath::Clamp(Run / (2.0 * OutRadius), -1.0, 1.0));
	}

	/** A depot's docks, local. */
	static void DepotDocks(int32 Arms, int32 Lanes, TArray<FMKIRoadDepotDock>& OutDocks);
	/**
	 * The marking pattern's paint on a junction of Arms and Lanes, local (section 0 yellow, 1 white); the lines drawn (#241:
	 * the hologram shows them too). DockModes, per depot dock in DepotDocks order: 1 a station set to load, 0 to unload, -1
	 * none (#318: the bay's LOADING or UNLOADING legend); none given, no legend is drawn.
	 */
	static int32 MarkingGeometry(int32 Arms, int32 Lanes, const UMKIRoadMarkingPattern* Pattern, FMKIRoadDeckGeometry (&Paint)[2], const TArray<int8>* DockModes = nullptr);
	/** How often a depot checks its stations' load or unload mode, to repaint the bay legend when one changes (#318). */
	static constexpr float DockModePollSeconds = 2.f;
	/** The station mode at each dock, in DepotDocks order: 1 load, 0 unload, -1 no station (#318). */
	TArray<int8> GetDockModes() const;
	/** Width of a pad's factory-zone stripes, and of the gaps between them (#246). */
	static constexpr float PadStripeWidthCm = 70.f;
	/** The orange paint the stripes take (package.markings.paints.orange). */
	static constexpr const TCHAR* PadStripeMaterialPath = TEXT("/MKIRoads/Materials/M_MKIR_RoadPaint_Orange.M_MKIR_RoadPaint_Orange");
	/** A depot's pads' stripes, local, on the deck top; the bands drawn. */
	static int32 PadStripeGeometry(int32 Arms, int32 Lanes, FMKIRoadDeckGeometry& Out);
	/** The corners of a dock's pad (#241): asphalt under the station's footprint, beyond the dock lane, local. */
	static TArray<FVector2D> DepotPadCorners(const FMKIRoadDepotDock& Dock);
	/** A depot's local paths (points and headings): its through lanes, and each bay's bends and dock straights. */
	static void DepotPaths(int32 Arms, int32 Lanes, TArray<TArray<TPair<FVector2D, FVector2D>>>& OutPaths);
	/** The local through lanes of a straight piece: each lane both ways as a road's (keep right), points and headings. */
	static void StraightLanePaths(int32 Lanes, float HalfLengthCm, TArray<TArray<TPair<FVector2D, FVector2D>>>& OutPaths);
	/** The local extent of a layout's cells. */
	static FBox2D LayoutBounds(const FMKIRoadJunctionLayout& Layout);

	/** The footprint of a junction of Arms arms (1, a turnaround; 2, 3, or 4) and Lanes lanes per arm. */
	static FMKIRoadJunctionLayout Layout(int32 Arms, int32 Lanes);
	/** Deck area of a layout, in cm2. */
	static double LayoutAreaCm2(const FMKIRoadJunctionLayout& Layout);
	/** Equivalent vanilla asphalt 8x4 foundations of a junction: ceil(area / 64 m2), at least one. */
	static int32 FoundationEquivalents(int32 Arms, int32 Lanes);
	/** The deck of a layout: top, bottom, and outline sides, Thickness deep, in the vanilla foundation's per-tile UVs. */
	static FMKIRoadDeckGeometry BuildGeometry(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, int32 Part = -1);
	/** Collision boxes of a layout (local, the deck top at Z 0): center, half extent, and yaw in degrees. */
	static void CollisionBoxes(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, TArray<FTransform>& OutBoxes);
	/** Soft clearance of a layout: one box per square tile, inset as the road's. */
	static void MakeClearanceData(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, TArray<FFGClearanceData>& Out);
	/** Lanes every road piece takes (#255, #256): 2 or 4. */
	static bool IsSupportedLaneCount(int32 Lanes) { return Lanes == 2 || Lanes == 4; }

	/** North American intersection markings on each arm (#229): a crosswalk next to the square, a stop line, the road's lines. */
	static constexpr float CrosswalkInsetCm = 50.f;
	static constexpr float CrosswalkDepthCm = 300.f;
	static constexpr float CrosswalkSideInsetCm = 50.f;
	static constexpr float BarWidthCm = 60.f;
	static constexpr float StopLineGapCm = 120.f;
	/** STOP before each stop line (#318, the maintainer): its letters' cap height, and its gap to the stop line and to the arm's end. */
	static constexpr float StopWordHeightCm = 240.f;
	static constexpr float StopWordGapCm = 25.f;
	static constexpr float StopLineWidthCm = 30.f;
	/** The stripes of one arm of Lanes lanes, in the arm's frame (FMKIRoadJunctionStripe); the road pattern's line sizes. */
	static void ArmMarkings(int32 Lanes, float LineWidthCm, float CenterGapCm, TArray<FMKIRoadJunctionStripe>& Out);

	// Begin AActor interface
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// End AActor interface

	// Begin AFGBuildable interface
	virtual int32 GetDismantleRefundReturnsMultiplier() const override;
	/**
	 * Its vehicle paths go with it, removed by it as the bridge removes children (each dismantled, then the piece), not as
	 * dismantle children (#244). Live: dismantled by the build gun as children of the piece, the vehicle network crashed
	 * rebuilding (AFGVehicleSubsystem::CleanNetworkIDOnElements) when the maintainer removed a turnaround.
	 */
	virtual void Dismantle_Implementation() override;
	/** While the dismantle tool aims at it, its paths show the dismantle highlight with it (#241). */
	virtual void StartIsLookedAtForDismantle_Implementation(class AFGCharacterPlayer* ByCharacter) override;
	virtual void StopIsLookedAtForDismantle_Implementation(class AFGCharacterPlayer* ByCharacter) override;
	virtual void GetChildDismantleActors_Implementation(TArray<AActor*>& OutChildDismantleActors) const override;
	virtual FBox CalculateBounds() const override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& CustomizationData) override;
	// End AFGBuildable interface

	/** The piece Actor is, or the piece a square (AMKIRoadTile) belongs to; null for anything else. */
	static AMKIRoadJunction* FromActor(AActor* Actor);
	static const AMKIRoadJunction* FromActor(const AActor* Actor) { return FromActor(const_cast<AActor*>(Actor)); }
	/** Adds this piece and its squares (a hologram ignores what it joins or holds up). */
	void AddSelfAndTiles(TSet<AActor*>& Actors) const;
	const TArray<TObjectPtr<AMKIRoadTile>>& GetTiles() const { return mTiles; }
	/**
	 * Its 8 m squares (the maintainer, 2026-10-18): each layout cell split into foundation squares, local to the piece (the
	 * deck top at Z 0): a square's middle, and its size (X, Y, the deck's thickness).
	 */
	static void TileBoxes(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, TArray<TPair<FVector, FVector>>& OutTiles);

	/** As the road (#225): what the customizer may put on this junction while it is aimed at. */
	void SetCustomizerAllows(bool bColoring, bool bPatterning) { mAllowColoring = bColoring; mAllowPatterning = bPatterning; }

	/** Sets the lanes before the junction finishes spawning; unsupported counts are ignored. */
	UFUNCTION(BlueprintCallable, Category = "MKI Roads|Junction")
	void SetLanes(int32 Lanes);
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Junction")
	int32 GetLanes() const { return mLanes; }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Junction")
	int32 GetArmCount() const { return mArms; }
	bool IsTruckDepot() const { return IsDepotShape(mArms); }
	/** A depot's docking points in world space, each with the yaw a truck station takes there. */
	TArray<FTransform> GetDepotDocks() const;
	/** The index of the depot dock nearest Location within MaxDistanceCm (horizontally), or INDEX_NONE. */
	int32 FindDepotDock(const FVector& Location, float MaxDistanceCm) const;
	/**
	 * A depot's dock (#237, #239): once a truck station's docking node is on its docking point, the dock lane's middle node
	 * moves onto it, so the lane runs through the dock. Once only (the maintainer: paths he deleted came back).
	 */
	void LayBayPaths();
	/** A station removed near Location (#239): the dock there has its paths laid again on a node of their own. */
	void OnDockStationRemovedAt(const FVector& Location);
	/**
	 * A depot whose station was dismantled (#239). Live: the dock lane's halves kept the station's docking node, which went
	 * with the station, and the save then crashed the vehicle subsystem on load. The halves touching the dock are removed
	 * and laid again on a node of their own; a station built there later takes it again.
	 */
	/** Removes its paths with an end on dock Dock and lays them again, ending on whatever node is there (a station's, or a new one). */
	void RelayDockLane(int32 Dock);
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Junction")
	float GetDeckThicknessCm() const { return mDeckThicknessCm; }
	UMaterialInterface* GetDeckMaterial() const { return mDeckMaterial.LoadSynchronous(); }
	/** The arms' ends in world space: X out along the arm, at the deck top. */
	TArray<FTransform> GetArmEnds() const;
	/**
	 * The side of this piece that Aim is within AMKIRoadSplineRoad::SnapBandCm of (the maintainer,
	 * 2026-10-18, as vanilla foundations snap): OutPoint on that side at the deck top (an arm's end when the side has one,
	 * bOutArm, else the aim on the side's 1 m grid) and OutOutward straight out of it. False when the aim is further in.
	 */
	bool NearSide(const FVector& Aim, FVector& OutPoint, FVector& OutOutward, bool& bOutArm) const;
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Junction")
	int32 GetCollisionBoxCount() const;
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Junction")
	int32 GetMarkingLineCount() const { return mMarkingLineCount; }
	/** Whether Path is one of the vehicle paths this piece laid (#235); dismantling one selects the piece (#241). */
	bool OwnsAutoPath(const AActor* Path) const;

protected:
	/** Arms: 1 (a turnaround, #236), 2 (a corner, #228), 3 (a T), 4, or a depot shape (6-10, #237, #240). The build-menu entry's. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Junction")
	int32 mArms = 4;

	/** Lanes per arm: 1, 2, or 4. */
	UPROPERTY(SaveGame, Replicated, EditAnywhere, BlueprintReadOnly, Category = "MKI Roads|Junction")
	int32 mLanes = AMKIRoadSplineRoad::DefaultLanes;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Junction")
	float mDeckThicknessCm = 100.f;

	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Junction")
	TObjectPtr<UProceduralMeshComponent> mDeck;

	/**
	 * A depot's station pads (#246; the maintainer: the pad under a station should look like the vanilla Full Factory Zone pattern):
	 * the pad cells' tops as their own mesh in the deck's material, painted with that pattern in FICSIT orange.
	 */
	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Junction")
	TObjectPtr<UProceduralMeshComponent> mPad;

	/** The pads' factory-zone stripes (#246; the vanilla pattern did not show on the procedural pad): orange paint bands. */
	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Junction")
	TObjectPtr<UProceduralMeshComponent> mPadStripes;

	UPROPERTY(EditDefaultsOnly, Category = "MKI Roads|Junction")
	TSoftObjectPtr<UMaterialInterface> mDeckMaterial;

	/** Its squares: real foundations under the deck that carry its collision and the game's snapping (the maintainer, 2026-10-18). */
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AMKIRoadTile>> mTiles;

	/** The applied road marking pattern's edge lines. Rendering only. */
	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Junction")
	TObjectPtr<UProceduralMeshComponent> mMarkings;

	int32 mMarkingLineCount = 0;

	/** The vehicle paths of every move through it (#235, FMKIRoadAutoPaths), and whether it has laid them (once). */
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGVehiclePathSegment>> mAutoPaths;
	UPROPERTY(SaveGame)
	bool mAutoPathsLaid = false;
	/** The FMKIRoadAutoPaths::Version its paths were laid with: older ones are laid again (#235: the first did not join). */
	UPROPERTY(SaveGame)
	int32 mAutoPathsVersion = 0;
	/** A depot's bay paths (#237, before #239's smaller depot); none are laid any more. */
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGVehiclePathSegment>> mBayPaths;
	/** Whether its dock lane has been joined to a station's docking node: once only, so paths the player deletes stay deleted. */
	UPROPERTY(SaveGame)
	bool mBayPathsLaid = false;
	/** One bit per dock (#240): its paths have been laid onto a station's docking node, once, so paths the player deletes stay deleted. */
	UPROPERTY(SaveGame)
	int32 mDocksJoined = 0;

private:
	void LayAutoPaths();
	void BuildDeck();
	/** Spawns its squares, or fits the saved ones to the layout (a piece saved before squares gets them on load). */
	void SyncTiles();
	void BuildMarkings(const UMKIRoadMarkingPattern* Pattern);
	void BuildPadStripes();
	/** Repaints the markings when a dock's station changes mode (#318). */
	void PollDockModes();

	TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> mRoadPattern;
	uint8 mRoadPatternRotation = 0;
	/** The pattern last painted and the dock modes it was painted with (#318). */
	const UMKIRoadMarkingPattern* mPaintedPattern = nullptr;
	TArray<int8> mPaintedDockModes;
	FTimerHandle mDockModeTimer;
};

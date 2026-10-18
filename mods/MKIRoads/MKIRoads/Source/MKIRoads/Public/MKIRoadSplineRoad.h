#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildableFoundation.h"
#include "FGSplineBuildableInterface.h"
#include "MKIRoadSplineRoad.generated.h"

class UMKIRoadMarkingPattern;
class UProceduralMeshComponent;
class AFGVehiclePathSegment;
class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UStaticMeshComponent;

/** One cross-section of a swept deck, in actor-local space: the deck top centerline point and its frame. */
struct FMKIRoadDeckSlice
{
	FVector Center = FVector::ZeroVector;
	/** Unit tangent along the spline (it climbs with the grade). */
	FVector Forward = FVector::ForwardVector;
	/** Horizontal unit vector to the right of Forward: the deck stays level crosswise. */
	FVector Right = FVector::RightVector;
	/** Unit normal of the deck top, Forward x Right. */
	FVector Up = FVector::UpVector;
	/** Distance along the spline (the centerline), which places the slice in its 8 m UV tile. */
	float Distance = 0.f;
};

/** Geometry of a swept deck, ready for UProceduralMeshComponent::CreateMeshSection. */
struct FMKIRoadDeckGeometry
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	/** The stencil channel (UV1): top-down, one unit per 8 m lane tile, as on a vanilla foundation. */
	TArray<FVector2D> UV1s;
	TArray<FVector> Tangents;
};

/**
 * Prototype spline road (step 1 of reports/spline-road-investigation-2026-10-11.md): a road deck laid along saved
 * spline points, like vanilla railway track, at a width of 2 or 4 lanes of 8 m. The build gun places it with
 * AMKIRoadSplineRoadHologram; MKI Bridge can also place it from explicit points.
 *
 * The spline is the deck top centerline, in actor-local space. The deck is swept procedurally along it: a cross-section
 * at most DeckStepCm apart, level crosswise, the full width and thickness, with vertices every DeckColumnCm across the
 * top. The deck is laid out as one patch per 8 m tile (per lane across, per 8 m of centerline distance along), each with
 * the vanilla 8 m foundation's UVs (modules.json package.uvLayout), so the vanilla asphalt foundation material (by
 * reference), which picks its top, bottom, and side textures by UV0 range, draws each tile as a foundation top: UV0 on the
 * top and bottom spans the vanilla top's range, the sides and end caps hang the side band from the deck top, and UV1 is
 * the top-down stencil channel, 0-1 per tile. Deforming the
 * vanilla foundation mesh instead, the first build, bent only at its 8 m segment ends: live, curves and ramps read as
 * facets. The swatch and paint finish reach the deck as the vanilla material's custom primitive data.
 *
 * A road marking pattern (UMKIRoadMarkingPattern, the customizer's MKI Patterns > Roads) is drawn by the road itself, as
 * strips swept along its spline for its own lane count (BuildMarkings).
 *
 * Collision is a chain of hidden instances of one instanced static mesh component, the vanilla 8x4 foundation mesh (its box)
 * scaled to oriented slabs at most 4 m long, shorter where the heading or grade turns, at the full deck width and
 * thickness. The vehicle hologram accepts only static mesh (and landscape) hits as a floor: live, with the deck's own
 * procedural triangles or with box components as collision, the truck hologram hovered 2 m above the deck and the build
 * gun placed it on the ground below, while on a static-mesh foundation it rests on the hit.
 *
 * It is an AFGBuildableFoundation, as the retired MKI Roads modules were, because the vehicle hologram and the build gun accept
 * foundations as a surface to place vehicles on: live, a plain AFGBuildable deck made the build gun's vehicle placement
 * pass through it to the ground below. Foundation edge and attachment snapping onto it is disabled on every side.
 *
 * The road is built with the vanilla asphalt 8x4 foundation recipe and costs, and refunds, one of those per started
 * 64 m2 of deck area.
 */
/**
 * Puts paint geometry on Mesh as the built pieces do: section 0 yellow, section 1 white, in Pattern's paints. A hologram's
 * preview draws its markings this way (#241; the maintainer: without the pattern a hologram's orientation is hard to tell).
 */
MKIROADS_API void MKIRoadApplyPaint(UProceduralMeshComponent* Mesh, const UMKIRoadMarkingPattern& Pattern, const FMKIRoadDeckGeometry (&Paint)[2]);

UCLASS()
class MKIROADS_API AMKIRoadSplineRoad : public AFGBuildableFoundation, public IFGSplineBuildableInterface
{
	GENERATED_BODY()

public:
	AMKIRoadSplineRoad(const FObjectInitializer& ObjectInitializer);

	static constexpr int32 MinLanes = 2;
	static constexpr int32 MaxLanes = 4;
	/** Width of a new road, and of a saved one with an unsupported width. */
	static constexpr int32 DefaultLanes = 2;
	static constexpr float LaneWidthCm = 800.f;
	static constexpr float DefaultDeckThicknessCm = 400.f;
	/**
	 * Length of one vanilla foundation: the interface's mesh length, and one UV tile. Each 8 m lane tile of the deck top
	 * spans one whole UV0 unit (a vanilla top spans 0.974 of one, its chamfers the rest), so tiles meet seamlessly at lane
	 * and 8 m boundaries, and UV1 is one unit per tile, as on a vanilla foundation.
	 */
	static constexpr float UvTileCm = 800.f;
	/**
	 * Side band scale of the vanilla foundations (modules.json package.uvLayout): a side's V falls from 1 at the deck top
	 * by one unit per 818.6 cm of depth, so the 1, 2, and 4 m decks show the band's top part as the 8x1, 8x2, and 8x4 do.
	 */
	static constexpr float SideBandCm = 818.6f;
	/** Longest step between deck cross-sections. */
	static constexpr float DeckStepCm = 25.f;
	/** Spacing of the top and bottom vertices across the deck, so U follows each lateral offset's own arc length. */
	static constexpr float DeckColumnCm = 100.f;
	/** Deck area of one vanilla 8 m x 8 m foundation, the cost unit. */
	static constexpr double FoundationAreaCm2 = 800.0 * 800.0;
	/** Longest collision slab; slabs get shorter where the spline bends. */
	static constexpr float MaxCollisionStepCm = 400.f;
	/** Largest heading or grade change inside one collision slab. */
	static constexpr float MaxCollisionStepAngleDeg = 2.f;
	/** How far each slab reaches past its sample points into its neighbours, so the deck has no gaps. */
	static constexpr float CollisionOverlapCm = 5.f;

	/** How far the clearance boxes stay inside the deck's sides, ends, and bottom. */
	static constexpr float ClearanceInsetCm = 10.f;
	/**
	 * How far below the deck top the clearance boxes end, at most half the deck thickness. A vehicle path's clearance dips
	 * below a sloped floor; live, on a 14 % grade it overlapped boxes that ended 10 cm down, and the path was refused.
	 */
	static constexpr float ClearanceTopInsetCm = 50.f;

	/**
	 * Soft clearance boxes of a deck along Spline (in its owner's space): oriented boxes at most MaxCollisionStepCm long,
	 * the deck's width and thickness inset by ClearanceInsetCm. Soft, as vanilla railway track's clearance is: a hologram
	 * overlapping them warns (yellow) rather than blocks.
	 */
	static void MakeClearanceData(const USplineComponent* Spline, float WidthCm, float ThicknessCm, TArray<FFGClearanceData>& Out);

	/**
	 * The construct disqualifier a road piece's hologram (road, intersection, turnaround, depot, support) takes for a
	 * clearance overlap with Other (#378; the maintainer: road pieces were built through other buildings, and a road cannot
	 * be placed through a foundation). Vehicle paths are no obstacle; anything else it overlaps (a foundation or ramp, a
	 * machine, a wall, a walkway, storage, another road piece's deck) blocks it. A deck resting on a foundation's top or
	 * against its edge only touches it (the clearance boxes are inset), and the pieces a hologram joins are left out of the
	 * check before this is asked.
	 */
	static TSubclassOf<class UFGConstructDisqualifier> PieceClearanceDisqualifier(EClearanceOverlapResult Result, const AActor* Other, TSubclassOf<class UFGConstructDisqualifier> Vanilla);
	/**
	 * A hologram's copy of a piece's clearance boxes, hardened for placement (#378): two soft boxes ignore each other (live:
	 * a road crossed a foundation's body unchallenged), so the hologram's boxes are default (hard) ones. A hard box over a
	 * soft one still reports a soft overlap, which PieceClearanceDisqualifier turns into a block. The built piece keeps soft
	 * boxes, so lights, barriers, stations and paths can still be placed on and along it.
	 */
	static void HardenForPlacement(TArray<FFGClearanceData>& Clearance) { for (FFGClearanceData& Data : Clearance) Data.Type = EClearanceType::CT_Default; }
	/** The world grid Ctrl locks road pieces to (#379), as foundations: centers and road starts on 4 m, heights on 1 m. */
	static constexpr float WorldGridCm = 400.f;
	static constexpr float WorldGridHeightCm = 100.f;
	/** Location on the world grid (#379). */
	static FVector WorldGridLocation(const FVector& Location)
	{
		return FVector(FMath::GridSnap(Location.X, static_cast<double>(WorldGridCm)), FMath::GridSnap(Location.Y, static_cast<double>(WorldGridCm)), FMath::GridSnap(Location.Z, static_cast<double>(WorldGridHeightCm)));
	}
	/** Yaw on the world grid (#379): the nearest world axis. */
	static float WorldGridYaw(float Yaw) { return 90.f * FMath::RoundToFloat(FRotator::NormalizeAxis(Yaw) / 90.f); }
	/**
	 * Every road piece snaps against a foundation's or another road piece's edge when aimed within this of the edge (the
	 * maintainer, 2026-10-18): 2 m, a quarter of an 8 m foundation, as vanilla foundations snap to each other, whatever the
	 * size of what is aimed at.
	 */
	static constexpr float SnapBandCm = 200.f;
	/**
	 * The end of Road that Aim is within SnapBandCm of (measured back along the road from the end,
	 * anywhere across the deck), the nearer if both: OutEnd its deck-top point, OutOutward its outward direction (unit,
	 * with the road's grade). False when the aim is further along the road.
	 */
	static bool NearEnd(const AMKIRoadSplineRoad& Road, const FVector& Aim, FVector& OutEnd, FVector& OutOutward);
	/** How far one press of the build gun's nudge moves a locked road piece (#377): a metre, the foundation grid's step. */
	static constexpr float NudgeStepCm = 100.f;

	/** True for the supported lane counts (#256): 2 or 4; any other loaded reopens at the default width. */
	static bool IsSupportedLaneCount(int32 Lanes) { return Lanes == 2 || Lanes == 4; }
	/** Equivalent vanilla asphalt 8x4 foundations of a deck: ceil(length x width / 64 m2), at least one. */
	static int32 FoundationEquivalents(double LengthCm, int32 Lanes);
	/**
	 * Sweeps a deck of WidthCm and ThicknessCm through Slices (at least two): top, bottom, both sides, and end caps, as one
	 * patch per 8 m tile (along the centerline, and per lane across), so each tile's UVs stay inside the vanilla
	 * foundation's ranges: the vanilla foundation material picks its top, bottom, and side textures by UV0 range.
	 */
	static FMKIRoadDeckGeometry SweepDeck(const TArray<FMKIRoadDeckSlice>& Slices, float WidthCm, float ThicknessCm);

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
	/** True for a vanilla stencil pattern (not a road marking pattern, not a remover): the road refuses it (#190). */
	static bool IsVanillaPattern(TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> PatternDesc);
	/**
	 * What the customizer may put on this road while it is aimed at (#225): the vanilla checks read these AFGBuildable
	 * flags, and show a refused aim as an invalid target. FMKIRoadPaintGuard sets them for the active customization.
	 */
	void SetCustomizerAllows(bool bColoring, bool bPatterning) { mAllowColoring = bColoring; mAllowPatterning = bPatterning; }
	/** Applies the swatch and paint finish to the deck and draws a road marking pattern (UMKIRoadMarkingPattern) on it. */
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& CustomizationData) override;
	/**
	 * The frame holograms snap to when aimed at this foundation (FHologramHelpers::SnapToFloor, used by the wheeled vehicle
	 * hologram, takes the top of mHeight in this frame). While the local player's build gun holds any hologram but the
	 * road's own aimed at the deck, it is a vanilla-shaped foundation volume (mHeight the deck thickness) centered half the
	 * thickness below the deck point under the aim, turned to the road's heading, so its top is the deck and a truck, a
	 * foundation, or a wall rests on the deck there, on any grade; otherwise the actor's. Live (bridge probe), the actor
	 * frame put the truck mHeight / 2 above the deck: the actor sits at the deck start, not at a volume center.
	 */
	virtual FTransform GetLightweightInstanceActorTransform_Implementation() const override;
	// End AFGBuildable interface

	// Begin IFGSplineBuildableInterface
	virtual UFGConnectionComponent* GetSplineConnection0() const override { return nullptr; }
	virtual UFGConnectionComponent* GetSplineConnection1() const override { return nullptr; }
	virtual USplineComponent* GetSplineComponent() const override { return mSplineComponent; }
	virtual const TArray<FSplinePointData>& GetSplinePointData() const override { return mSplineData; }
	virtual TArray<FSplinePointData>* GetMutableSplinePointData() override { return &mSplineData; }
	virtual float GetMeshLength() const override { return UvTileCm; }
	virtual UStaticMesh* GetSplineMesh() const override;
	// End IFGSplineBuildableInterface

	/** Sets the width before the road finishes spawning; unsupported counts are ignored. */
	UFUNCTION(BlueprintCallable, Category = "MKI Roads|Spline Road")
	void SetLanes(int32 Lanes);
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetLanes() const { return mLanes; }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	float GetDeckWidthCm() const { return mLanes * LaneWidthCm; }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	float GetDeckThicknessCm() const { return mDeckThicknessCm; }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	float GetSplineLengthCm() const;
	/** The deck's material (the vanilla asphalt foundation's), loaded: the hologram previews a valid road with it. */
	UMaterialInterface* GetDeckMaterial() const { return mDeckMaterial.LoadSynchronous(); }
	/** Number of collision slabs built at BeginPlay. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetCollisionSlabCount() const;
	/** Number of deck cross-sections swept at BeginPlay. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetDeckSliceCount() const { return mDeckSliceCount; }
	/** Painted lines of the applied road marking pattern (0 without one), and their vertices. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetMarkingLineCount() const { return mMarkingLineCount; }
	/** Whether Path is one of the vehicle paths this road laid (#235); dismantling one selects the road (#241). */
	bool OwnsAutoPath(const AActor* Path) const;
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetMarkingVertexCount() const { return mMarkingVertexCount; }
	/** The pattern ID the deck material receives (custom primitive data), so it is readable: 0 under a road marking pattern. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	float GetDeckPatternId() const;

	/**
	 * A painted strip along Spline (in its owner's space): LateralCm from the centerline, WidthCm wide, LiftCm above the
	 * deck top (the spline), from distance FromCm to ToCm, with cross-sections at most DeckStepCm apart.
	 */
	static void SweepStrip(const USplineComponent& Spline, float LateralCm, float WidthCm, float LiftCm, float FromCm, float ToCm, FMKIRoadDeckGeometry& Out);
	/** The marking pattern's lines along Spline for Lanes lanes (section 0 yellow, 1 white); the lines drawn (#241: the hologram too). */
	static int32 MarkingGeometry(const USplineComponent& Spline, int32 Lanes, const UMKIRoadMarkingPattern& Pattern, FMKIRoadDeckGeometry (&Paint)[2]);

protected:
	/** Saved spline points of the deck top centerline, in actor-local space. */
	UPROPERTY(SaveGame, Replicated, Meta = (NoAutoJson = true))
	TArray<FSplinePointData> mSplineData;

	/** Width in 8 m lanes: 2 or 4. */
	UPROPERTY(SaveGame, Replicated, EditAnywhere, BlueprintReadOnly, Category = "MKI Roads|Spline Road")
	int32 mLanes = DefaultLanes;

	/** Deck thickness below the spline (the vanilla foundation heights: 100, 200, or 400 cm). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Spline Road")
	float mDeckThicknessCm = DefaultDeckThicknessCm;

	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Spline Road")
	TObjectPtr<USplineComponent> mSplineComponent;

	/** The swept deck: rendering only (collision is the instances of mCollisionSlabs). UV0 and UV1 follow the vanilla foundation layout. */
	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Spline Road")
	TObjectPtr<UProceduralMeshComponent> mDeck;

	/** Material of the deck: the vanilla asphalt foundation's (package.splineRoad.surfaceMaterial in modules.json). */
	UPROPERTY(EditDefaultsOnly, Category = "MKI Roads|Spline Road")
	TSoftObjectPtr<UMaterialInterface> mDeckMaterial;

	/** Vanilla foundation mesh: scaled, hidden, as the collision slabs, and returned to the spline buildable interface. */
	UPROPERTY(EditDefaultsOnly, Category = "MKI Roads|Spline Road")
	TSoftObjectPtr<UStaticMesh> mInterfaceMesh;

	/**
	 * The collision slabs, one instance each, in a single hidden component: a vehicle path's obstacle check ignores only
	 * the floor component under it, so live, separate slab components on a grade were each other's obstacles.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> mCollisionSlabs;

	UPROPERTY(Transient)
	int32 mDeckSliceCount = 0;

	/** The applied road marking pattern's lines: section 0 yellow, section 1 white. Rendering only. */
	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Spline Road")
	TObjectPtr<UProceduralMeshComponent> mMarkings;

	int32 mMarkingLineCount = 0;
	int32 mMarkingVertexCount = 0;

	/** The vehicle paths this road laid along its lanes (#235, FMKIRoadAutoPaths), and whether it has laid them (once). */
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGVehiclePathSegment>> mAutoPaths;
	UPROPERTY(SaveGame)
	bool mAutoPathsLaid = false;
	/** The FMKIRoadAutoPaths::Version its paths were laid with: older ones are laid again (#235: the first did not join). */
	UPROPERTY(SaveGame)
	int32 mAutoPathsVersion = 0;
	/**
	 * Whether the road has had the default North American Road pattern offered once (#316). The build gun paints a new road
	 * (#234), but roads spawned without it (the bridge's /v1/place-spline-road) had none; a road painted over later keeps
	 * whatever it was given.
	 */
	UPROPERTY(SaveGame)
	bool mPatternDefaulted = false;

private:
	void PopulateSplineComponent();
	void BuildDeck();
	void BuildCollision();
	/** The road marking pattern last applied (none after a remover), restored when a vanilla pattern is applied (#190). */
	TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> mRoadPattern;
	uint8 mRoadPatternRotation = 0;
	/**
	 * Creates the deck collision's physics state if it is missing (#185). Built with the build gun, the slabs were there,
	 * collision enabled, but without a physics state until the save was reloaded, so nothing could hit the road: not the
	 * build gun (dismantle), vehicles, or traces. Called after the slabs are built and again once the spawn has settled.
	 */
	void EnsureCollisionPhysics();
	/** Rebuilds mMarkings for Pattern (none clears them). */
	void BuildMarkings(const UMKIRoadMarkingPattern* Pattern);
	/** Lays the lane paths once (#235): on the authority, a tick after BeginPlay, so roads loaded with it have their nodes. */
	void LayAutoPaths();
};

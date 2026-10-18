#pragma once

class AFGBuildableFoundation;

#include "CoreMinimal.h"
#include "MKIRoadSplineRoad.h"
#include "Hologram/FGSplineHologram.h"
#include "MKIRoadSplineRoadHologram.generated.h"

class AMKIRoadSplineRoad;
class UInputAction;
class UInputMappingContext;
class UProceduralMeshComponent;
struct FInputActionValue;

/**
 * Where a spline road may start or end: a point on the deck top and, when snapped to a spline road end or rotated by the
 * player, the heading the road leaves that point along.
 */
struct FMKIRoadSplineEnd
{
	/** Deck top centerline point, world space. */
	FVector Location = FVector::ZeroVector;
	/** Snapped to a spline road end: the location, heading, and grade are that end's. */
	bool bSnapped = false;
	/** The heading is that end's and the rotate input cannot turn it: only spline road ends lock it. */
	bool bHeadingLocked = false;
	/** The heading is fixed: locked to a road end, or rotated by the player. */
	bool bFixedHeading = false;
	/** On a foundation's top: the point is on its 1 m grid. */
	bool bOnFoundation = false;
	/** On a foundation only: its yaw. */
	float FoundationYaw = 0.f;
	/**
	 * At a foundation's edge: the point on the edge and the edge's outward direction, which the end's quarter turns start
	 * from (the maintainer, 2026-10-18: a road should be able to run along a foundation's edge).
	 */
	bool bOnEdge = false;
	FVector EdgePoint = FVector::ZeroVector;
	FVector EdgeOutward = FVector::ForwardVector;
	/** Fixed heading only: the unit horizontal direction a road leaving that point travels (the end's outward direction). */
	FVector Outward = FVector::ForwardVector;
	/** Snapped only: the grade of that outward direction, rise over run. */
	float OutwardGrade = 0.f;
	/** Snapped only: the road whose end it is. */
	TWeakObjectPtr<AMKIRoadSplineRoad> Road;
	/** Snapped to a junction's arm (#227): the junction, and its lanes. */
	TWeakObjectPtr<AActor> Junction;
	int32 JunctionLanes = 0;
};

/** Facts of a planned road: what validation and the cost read. */
struct FMKIRoadSplinePlan
{
	TArray<FSplinePointData> Points;
	double LengthCm = 0.0;
	double MaxGradePercent = 0.0;
	double MinRadiusCm = TNumericLimits<double>::Max();
	/** The end lies behind the start's fixed direction, so no arc from it reaches the end. */
	bool bTurnTooSharp = false;
};

/**
 * Build-gun hologram of the spline road: placed like railway track. The first click sets the start (snapped to a free
 * spline road end within reach, else resting on the aimed surface); the end then follows the aim, and the second click
 * builds. From a snapped start the road continues that end's heading and bends in a circular arc to the aimed end, like
 * rail; from a free start it is straight. Snapping the end to another road end makes it meet that end's heading too.
 * Grades ease between the ends' own grades. Holding the build gun's snap modifier (Ctrl), as for vanilla rail, is straight
 * mode: the road runs straight ahead along the start's heading, as far as the aim reaches along it, whatever the aim's
 * sideways offset.
 *
 * On a foundation's top a free start or end snaps to the foundation's 1 m grid, in its own frame, and a free start's
 * heading to the foundation's axes (and, rotated, to the rotation step from them).
 *
 * Plus and Minus (the literal keys and the numpad) step between 2 and 4 lanes while placing; the deck thickness is the
 * built class's (Road 1 m, 2 m, and 4 m). Validation turns the hologram red for a length outside 4-100 m, a grade above
 * 25 degrees (as rail), a horizontal radius tighter than the deck width, or an end behind the start; overlapping another buildable's
 * clearance turns it yellow, as rail does (soft clearance along the deck). Cost is one recipe (one asphalt 8x4
 * foundation) per started 64 m2 of deck.
 */
UCLASS(Blueprintable)
class MKIROADS_API AMKIRoadSplineRoadHologram : public AFGSplineHologram
{
	GENERATED_BODY()

public:
	AMKIRoadSplineRoadHologram();

	static constexpr float MinLengthCm = 400.f;
	static constexpr float MaxLengthCm = 10000.f;
	/** Steepest grade, as vanilla railway track allows: 25 degrees (its mMaxGrade), tan(25 deg) = 46.63 %. */
	static constexpr float MaxGradeDeg = 25.f;
	static constexpr float MaxGradePercent = 46.630766f;
	/** The widest arc from a fixed start (#206: a full half circle, and past it); its radius diverges toward 360 degrees. */
	static constexpr double MaxArcTurnDeg = 270.0;
	/** Grid a point on a foundation's top snaps to, counted from the foundation's edges. */
	static constexpr float FoundationGridCm = 100.f;
	/**
	 * A foundation top aimed within this share of the foundation's size from an edge (or its side face) puts the end on that
	 * edge (#207): 2 m, the snap band every piece shares (the maintainer, 2026-10-18: a 50 cm strip was too thin to hit).
	 */
	static constexpr float FoundationEdgeBandCm = AMKIRoadSplineRoad::SnapBandCm;
	/**
	 * A road end aimed at Foundation's side face, or at its top within FoundationEdgeBandCm of an edge nothing continues past
	 * (#207): OutLocation on that edge, on the 1 m grid along it and flush with the top, OutOutward straight out from the
	 * edge. False when not at an edge. Shared with the intersections and turnarounds (#319), which snap as roads do.
	 */
	static bool FoundationEdgeEnd(const AFGBuildableFoundation& Foundation, const FHitResult& HitResult, const AActor* Ignore, FVector& OutLocation, FVector& OutOutward);
	/** Cross-section spacing of the preview deck (the built deck uses AMKIRoadSplineRoad::DeckStepCm). */
	static constexpr float PreviewStepCm = 50.f;
	/** Priority of the transient Plus/Minus mapping context, above the build gun's. */
	static constexpr int32 LaneInputPriority = 100;

	/** The spline points (local to Origin) and facts of a road from Start to End. bStraight ignores fixed headings. */
	static FMKIRoadSplinePlan PlanRoad(const FMKIRoadSplineEnd& Start, const FMKIRoadSplineEnd& End, const FVector& Origin, bool bStraight, int32 Lanes);
	/** Length, grade, and radius facts of saved spline points (what validation and the cost read). */
	static FMKIRoadSplinePlan FactsOf(const TArray<FSplinePointData>& Points);
	/** Rotation step when the build gun passes none, in degrees; otherwise the game's own step, everywhere. */
	static constexpr int32 RotationStepDeg = 10;
	/** On a foundation, a road's heading turns in these steps from the foundation's axes (#253). */
	static constexpr int32 FoundationRotationStepDeg = 45;
	/** Length of the start preview before the first click: a stub of the real width and thickness. */
	static constexpr float StartStubLengthCm = 800.f;
	/**
	 * Straight mode's end: the aim projected onto the line from Start along Facing (a unit horizontal heading), never
	 * behind the start, rounded to whole FoundationGridCm when bOnGrid; the aim's height is kept.
	 */
	static FVector ProjectStraightEnd(const FVector& Start, const FVector& Facing, const FVector& Aim, bool bOnGrid);
	/** Yaw on a foundation: the nearest of its axes (Step 90) or rotation steps from them, in degrees. */
	static float AlignYawToFoundation(float Yaw, float FoundationYaw, float Step);
	/** The lane counts +/- steps through (#226): 2 and 4 (#255: no 1-lane roads, trucks behaved oddly on them). */
	static constexpr int32 StepLaneCounts[] = { 2, 4 };
	/** The next offered lane count from Current in Delta's direction (StepLaneCounts), kept at the ends of the list. */
	static int32 StepLanes(int32 Current, int32 Delta);

	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetLanes() const { return mLanes; }
	UFUNCTION(BlueprintCallable, Category = "MKI Roads|Spline Road")
	void SetLanes(int32 Lanes);
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	float GetPlannedLengthCm() const { return static_cast<float>(mPlan.LengthCm); }
	UFUNCTION(BlueprintCallable, Category = "MKI Roads|Spline Road")
	void SetStraightMode(bool bStraight);
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	bool IsStraightMode() const { return mStraightMode; }
	/** The heading the preview points along before the first click (the start's), or the end's arrival heading after it. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	float GetPreviewHeadingYaw() const;
	/**
	 * Clears the construct disqualifiers and runs the placement checks (clearance and the road's own length, grade, and
	 * radius). A hologram driven without the build gun never gets past its initializing sentinel in
	 * ValidatePlacementAndCost, so headless callers (the bridge) run the checks with this instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "MKI Roads|Spline Road")
	void RunPlacementChecks();
	/** The custom-depth stencil the preview deck renders with: its visible colour state (OK, warning, or error). */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetPreviewStencilValue() const;
	/** True while the preview shows the deck's own (asphalt) material: a valid placement. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	bool IsPreviewShowingDeck() const;
	/** Number of clearance boxes the hologram currently checks. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Spline Road")
	int32 GetClearanceBoxCount() const { return mClearanceData.Num(); }

	// Begin AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// End AActor interface

	// Begin AFGHologram interface
	virtual void SetHologramLocationAndRotation(const FHitResult& HitResult) override;
	virtual bool DoMultiStepPlacement(bool bIsInputFromARelease) override;
	virtual int32 GetBaseCostMultiplier() const override;
	virtual void SetMaterialState(EHologramMaterialState State) override;
	virtual void ResetBuildSteps() override;
	virtual bool TrySnapToActor(const FHitResult& HitResult) override { return false; }
	virtual void ScrollRotate(int32 Delta, int32 Step) override;
	virtual int32 GetRotationStep() const override { return RotationStepDeg; }
	virtual void PostHologramPlacement(const FHitResult& HitResult, bool bCallForChildren) override;
	virtual void SerializeConstructMessage(FArchive& Ar, FNetConstructionID Id) override;
	virtual void PostConstructMessageDeserialization() override;
	virtual void SetupPendingConstructionHologram(AFGHologram* FromHologram) override;
	/** The build gun's snap modifier (Ctrl): straight mode while held, as for vanilla rail. */
	virtual void SetSnapToGuideLines(bool bIsEnabled) override;
	virtual void GetIgnoredClearanceActors(TSet<AActor*>& IgnoredActors) const override;
	/** Locked (H), the nudge moves the point being placed (#377): the start before the first click, the end after it. */
	virtual bool CanNudgeHologram() const override { return true; }
	virtual float GetNudgeDistance() const override { return AMKIRoadSplineRoad::NudgeStepCm; }
	virtual void SetHologramNudgeLocation() override;
	/** Nudged along the current point's heading, so forward is along the road. */
	virtual FTransform GetNudgeSpaceTransform() const override;
	// End AFGHologram interface

protected:
	// Begin AFGHologram interface
	virtual void CheckValidPlacement() override;
	/** Overlaps that are not foundations or vehicle paths block the road (#378). */
	virtual TSubclassOf<class UFGConstructDisqualifier> GetConstructDisqualifierFromClearanceOverlap(const EClearanceOverlapResult& OverlapResult, AActor* OtherActor) const override;
	// End AFGHologram interface

	// Begin AFGBuildableHologram interface
	virtual void CheckValidFloor() override {}
	virtual void ConfigureActor(AFGBuildable* InBuildable) const override;
	// End AFGBuildableHologram interface

	// Begin AFGSplineHologram interface
	/** Soft clearance along the planned deck (AMKIRoadSplineRoad::MakeClearanceData); none before the start is set. */
	virtual void UpdateClearanceData() override;
	// End AFGSplineHologram interface

	UPROPERTY(Replicated, CustomSerialization)
	int32 mLanes = 2;
	/** The piece whose lanes the start last took (#238): taken once per piece, so Plus and Minus still change them after. */
	TWeakObjectPtr<AActor> mLanesTakenFrom;

	UPROPERTY(CustomSerialization)
	bool mStraightMode = false;

	/** The planned end lies behind a fixed start heading (carried in the construct message). */
	bool mTurnTooSharp = false;

	/** Preview of the deck being placed. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> mPreview;
	/** The default pattern's markings on the preview (#241), so the road's lanes and direction show. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> mPreviewMarkings;
	/** A spline along the start stub, for its markings before the start is set. Never registered. */
	UPROPERTY(Transient)
	TObjectPtr<USplineComponent> mStubSpline;

private:
	FMKIRoadSplineEnd mStart;
	FMKIRoadSplineEnd mEnd;
	/** The point being placed as the aim last set it: what a nudge offsets while the hologram is locked (#377). */
	FMKIRoadSplineEnd mNudgeBase;
	FMKIRoadSplinePlan mPlan;
	/** Player-rotated headings of a free start and a free end, in degrees (the end's is the heading it arrives along). */
	float mStartYaw = 0.f;
	float mEndYaw = 0.f;
	bool mStartRotated = false;
	bool mEndRotated = false;
	/** Quarter turns the scroll gave a start or an end at a foundation's edge, from straight out of the edge. */
	int32 mStartEdgeTurns = 0;
	int32 mEndEdgeTurns = 0;
	/** The preview shows the deck's own material (and carries the default swatch's primitive data). */
	bool mPreviewShowsDeck = false;
	/** The preview's last swept deck, and planar UVs for it under the hologram material. */
	FMKIRoadDeckGeometry mPreviewGeometry;
	TArray<FVector2D> mPreviewPlanarUVs;
	/** Rebuilds the preview mesh from mPreviewGeometry with the deck's vanilla UVs or the planar ones. */
	void ApplyPreviewMesh(bool bDeckUVs);
	/** The start's heading when the first click fixed it (straight mode runs along it), in degrees. */
	float mStartFacingYaw = 0.f;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> mPlusAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> mMinusAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> mLaneMappingContext;
	TWeakObjectPtr<APlayerController> mLaneInputController;
	/**
	 * Lightweight buildables (foundations, walls) only exist as actors, and so only meet clearance boxes, within a radius of
	 * an instance converter; the build gun's sits at the aim point, so a long road missed a foundation 18 m from it (live,
	 * #378). The hologram keeps its own converter over the planned road's bounds, re-added when its radius bucket changes.
	 */
	void UpdateLightweightConverter();
	void RemoveLightweightConverter();
	TWeakObjectPtr<AActor> mLightweightConverter;
	float mLightweightConverterRadius = 0.f;
	static constexpr float LightweightConverterStepCm = 1000.f;

	FMKIRoadSplineEnd ResolveEnd(const FHitResult& HitResult, float DeckThicknessCm) const;
	float DeckThicknessCm() const;
	void Replan();
	/** Applies the player's rotations to free ends. */
	void ApplyRotations();
	/** Straight mode (Ctrl) on a free start (#379): the start on the world grid, its heading along the nearest world axis. */
	void SnapStartToWorldGrid();
	/** The player's view yaw (the heading of a free, unrotated start). */
	float ViewYaw() const;
	/** The yaw of an end's current heading: its fixed heading, else the view (start) or the planned arrival (end). */
	float CurrentHeadingYaw(bool bStartEnd) const;
	/** Rebuilds the spline component and plan facts from mSplineData (a deserialized or copied hologram). */
	void SyncFromSplineData();
	void UpdatePreview();
	void RegisterLaneInput();
	void UnregisterLaneInput();
	void OnPlus(const FInputActionValue& Value);
	void OnMinus(const FInputActionValue& Value);
};

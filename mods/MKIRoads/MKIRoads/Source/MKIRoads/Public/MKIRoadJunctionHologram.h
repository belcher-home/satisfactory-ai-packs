#pragma once

#include "CoreMinimal.h"
#include "MKIRoadSplineRoad.h"
#include "Hologram/FGBuildableHologram.h"
#include "MKIRoadJunctionHologram.generated.h"

class AMKIRoadJunction;
class UInputAction;
class UInputMappingContext;
class UProceduralMeshComponent;
struct FInputActionValue;

/**
 * Build-gun hologram of a road intersection (#227). One click places it:
 * - Aimed at a spline road within reach of its end (as a road end snaps), one arm joins that end: the arm's end on the
 *   road's end, heading back along it. The scroll rotation picks which arm joins; the width follows the road's (1, 2, or 4
 *   lanes).
 * - On a foundation's top, its center goes on the foundation's half-tile grid (4 m), turned along the foundation's axes,
 *   and the scroll rotation turns it in 45 degree steps (#253).
 * - Anywhere else it rests on the aimed point, along the view's heading, turned freely by the scroll rotation (#253).
 * Plus and Minus (the literal keys and the numpad) step its width between 2 and 4 lanes (#255). The preview is the deck in
 * asphalt, its state shown by the coloured outline, as the road's. Cost: one asphalt 8x4 foundation per started 64 m2.
 */
UCLASS(Blueprintable)
class MKIROADS_API AMKIRoadJunctionHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	AMKIRoadJunctionHologram();

	/** Grid a junction's center snaps to on a foundation's top, counted from the foundation's edges: half a tile. */
	static constexpr float FoundationGridCm = 400.f;

	/**
	 * The junction transform that puts side QuarterTurn (0 +X, 1 +Y, 2 -X, 3 -Y, in the piece's frame) of a junction of Arms
	 * arms and Lanes lanes against a road end, an arm end or a foundation edge at EndLocation (the deck top) whose outward
	 * heading is EndOutward (horizontal, unit). An arm on that side joins it, its end on the point; a side with no arm lies
	 * flush against it, centred on the point, so the piece can run alongside a foundation (the maintainer, 2026-10-18: only
	 * the turns that pointed an arm into what was aimed at were offered).
	 */
	static FTransform FaceSideToEnd(int32 Arms, int32 Lanes, int32 QuarterTurn, const FVector& EndLocation, const FVector& EndOutward);

	UFUNCTION(BlueprintPure, Category = "MKI Roads|Junction")
	int32 GetLanes() const { return mLanes; }
	UFUNCTION(BlueprintCallable, Category = "MKI Roads|Junction")
	void SetLanes(int32 Lanes);

	// Begin AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	// End AActor interface

	// Begin AFGHologram interface
	virtual void SetHologramLocationAndRotation(const FHitResult& HitResult) override;
	virtual bool TrySnapToActor(const FHitResult& HitResult) override { return false; }
	virtual void ScrollRotate(int32 Delta, int32 Step) override;
	virtual int32 GetBaseCostMultiplier() const override;
	virtual void SetMaterialState(EHologramMaterialState State) override;
	virtual void SerializeConstructMessage(FArchive& Ar, FNetConstructionID Id) override;
	virtual void SetupPendingConstructionHologram(AFGHologram* FromHologram) override;
	virtual void PostHologramPlacement(const FHitResult& HitResult, bool bCallForChildren) override;
	/** Locked (H), nudged as a vanilla building (#377), along its own axes. */
	virtual bool CanNudgeHologram() const override { return true; }
	virtual float GetNudgeDistance() const override { return AMKIRoadSplineRoad::NudgeStepCm; }
	virtual void GetIgnoredClearanceActors(TSet<AActor*>& IgnoredActors) const override;
	/** The build gun's snap modifier (Ctrl): a free piece locks to the world grid (#379), as a foundation does. */
	virtual void SetSnapToGuideLines(bool bIsEnabled) override;
	// End AFGHologram interface

protected:
	// Begin AFGBuildableHologram interface
	virtual void CheckValidFloor() override {}
	virtual void ConfigureActor(AFGBuildable* InBuildable) const override;
	// End AFGBuildableHologram interface

	// Begin AFGHologram interface
	/** Overlaps that are not foundations or vehicle paths block the piece (#378). */
	virtual TSubclassOf<class UFGConstructDisqualifier> GetConstructDisqualifierFromClearanceOverlap(const EClearanceOverlapResult& OverlapResult, AActor* OtherActor) const override;
	// End AFGHologram interface

	UPROPERTY(Replicated, CustomSerialization)
	int32 mLanes = AMKIRoadSplineRoad::DefaultLanes;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> mPreview;
	/** The default pattern's markings on the preview (#241), so its arms and orientation show. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> mPreviewMarkings;

private:
	/** Quarter turns from the scroll rotation: the yaw on a foundation or the ground, the joining arm at a road end. */
	int32 mQuarterTurns = 0;
	/**
	 * The scroll rotation as notches (#253; the maintainer: road pieces turn in 45 degree steps on foundations and freely on open
	 * ground): eighth turns on a foundation, and on the ground the game's rotation step each.
	 */
	int32 mRotateNotches = 0;
	/** The game's rotation step of the last scroll, in degrees. */
	int32 mRotateStepDeg = 10;
	/** The road or piece whose lanes it last took (#238): taken once per piece, so Plus and Minus still change them after. */
	TWeakObjectPtr<const AActor> mLanesTakenFrom;
	/** Ctrl held (#379). */
	bool mWorldGrid = false;
	/** The road or piece an arm joins, left out of the clearance check (#378). */
	TWeakObjectPtr<AActor> mJoinedTo;
	/** Rebuilds the clearance boxes from the layout (#378): the built piece's, which its hologram lacked. */
	void UpdateClearance();
	/** The previewed layout's lanes, so the preview is rebuilt only when the width changes. */
	int32 mPreviewLanes = 0;
	bool mPreviewShowsDeck = false;
	int32 ArmCount() const;
	/** Takes Source's lanes the first time it is aimed at (#238). */
	void TakeLanesFrom(const AActor* Source, int32 Lanes);
	float DeckThicknessCm() const;
	void UpdatePreview();

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> mPlusAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> mMinusAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> mLaneMappingContext;
	TWeakObjectPtr<APlayerController> mLaneInputController;
	void RegisterLaneInput();
	void UnregisterLaneInput();
	void OnPlus(const FInputActionValue& Value);
	void OnMinus(const FInputActionValue& Value);
};

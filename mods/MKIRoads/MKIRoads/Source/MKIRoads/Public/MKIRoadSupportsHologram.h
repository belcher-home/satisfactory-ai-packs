#pragma once

#include "CoreMinimal.h"
#include "MKIRoadSupports.h"
#include "MKIRoadSplineRoad.h"
#include "Hologram/FGBuildableHologram.h"
#include "MKIRoadSupportsHologram.generated.h"

class AMKIRoadSplineRoad;
class AMKIRoadJunction;
class AFGBuildableFoundation;
class UStaticMeshComponent;

/**
 * Build-gun hologram of a road support (#194, #199, #200, #203, #216, #219), placed as a zoop. Aimed at a raised spline
 * road it previews one concrete pillar column of the shortest height under the aimed point (snapped 4 m along and across
 * the road); the first click fixes its top there. The aim then sets where the base goes (the point on the column's line
 * nearest the view, or the ground aimed at), in 2 m steps, nudged up or down 1 m at a time, and the second click builds it;
 * a reset goes back to the first step. Where it goes is the player's call: it is invalid only aimed off a road. The
 * cost is the recipe per pillar piece.
 */
UCLASS(Blueprintable)
class MKIROADS_API AMKIRoadSupportsHologram : public AFGBuildableHologram
{
	GENERATED_BODY()

public:
	// Begin AFGHologram interface
	virtual bool IsValidHitResult(const FHitResult& HitResult) const override;
	virtual bool TrySnapToActor(const FHitResult& HitResult) override { return false; }
	virtual void SetHologramLocationAndRotation(const FHitResult& HitResult) override;
	virtual int32 GetBaseCostMultiplier() const override;
	virtual void SetMaterialState(EHologramMaterialState State) override;
	virtual void SerializeConstructMessage(FArchive& Ar, FNetConstructionID Id) override;
	virtual void SetupPendingConstructionHologram(AFGHologram* FromHologram) override;
	virtual bool DoMultiStepPlacement(bool bIsInputFromARelease) override;
	virtual bool HandleReset() override;
	virtual bool IsChanged() const override;
	/** Locked (H), the nudge moves the column's top (first step) or its base, in height steps (second step) (#377). */
	virtual bool CanNudgeHologram() const override { return true; }
	virtual float GetNudgeDistance() const override { return AMKIRoadSplineRoad::NudgeStepCm; }
	virtual void SetHologramNudgeLocation() override;
	virtual FTransform GetNudgeSpaceTransform() const override;
	virtual void GetIgnoredClearanceActors(TSet<AActor*>& IgnoredActors) const override;
	// End AFGHologram interface

	AMKIRoadSupportsHologram();

	/** Where the base of a column whose top is Top goes for a view along TraceStart..TraceEnd that hit (or not) Hit. */
	static float BaseZFromView(const FVector& Top, const FVector& TraceStart, const FVector& TraceEnd, const FHitResult& Hit);

	UFUNCTION(BlueprintPure, Category = "MKI Roads|Supports")
	int32 GetPreviewColumnCount() const { return mColumns.Num(); }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Supports")
	int32 GetPreviewPieceCount() const { return AMKIRoadSupports::CountPieces(mColumns); }
	/** True after the first click, while the aim sets the base. */
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Supports")
	bool IsPlacingBase() const { return mPlacingBase; }

protected:
	// Begin AFGHologram interface
	virtual void CheckValidPlacement() override;
	/** Columns may stand on foundations and under their deck, but not through other buildings (#378). */
	virtual TSubclassOf<class UFGConstructDisqualifier> GetConstructDisqualifierFromClearanceOverlap(const EClearanceOverlapResult& OverlapResult, AActor* OtherActor) const override;
	// End AFGHologram interface

	// Begin AFGBuildableHologram interface
	virtual void CheckValidFloor() override {}
	virtual void ConfigureActor(AFGBuildable* InBuildable) const override;
	// End AFGBuildableHologram interface

	UPROPERTY(Transient)
	TObjectPtr<AMKIRoadSplineRoad> mRoad;
	/** Or the intersection, turnaround, or depot aimed at (#320). */
	UPROPERTY(Transient)
	TObjectPtr<AMKIRoadJunction> mJunction;
	/** The deck the support stands under: the road or the piece. */
	const AActor* Deck() const;
	/** A vanilla foundation it goes under (the maintainer, 2026-10-18); a lightweight one's temporary actor, so held weakly. */
	TWeakObjectPtr<AFGBuildableFoundation> mFoundation;
	/** Under a vanilla foundation: kept for the construct message, which cannot name a lightweight foundation's actor. */
	bool mUnderFoundation = false;
	/** Under a road, a road piece, or a vanilla foundation. */
	bool HasDeck() const { return mRoad || mJunction || mUnderFoundation; }
	UPROPERTY(Transient)
	TArray<FMKIRoadSupportColumn> mColumns;
	/** The column the aim last set, which a nudge offsets while the hologram is locked (#377): its top, heading, height. */
	FVector mNudgeTop = FVector::ZeroVector;
	FVector mNudgeForward = FVector::ForwardVector;
	float mNudgeHeight = 0.f;
	/** Clearance boxes round the previewed columns (#378). */
	void UpdateClearance();
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> mPreviewPieces;

private:
	EMKIRoadSupportFit mFit = EMKIRoadSupportFit::NoGround;
	/** The second step: the top (mTop, mForward) is fixed and the aim sets the base. */
	bool mPlacingBase = false;
	FVector mTop = FVector::ZeroVector;
	FVector mForward = FVector::ForwardVector;
	void UpdatePreview();
};

#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildable.h"
#include "MKIRoadSupports.generated.h"

class AMKIRoadSplineRoad;
class AMKIRoadJunction;
class UStaticMesh;
class UStaticMeshComponent;

/** One concrete pillar column under a road: its base and its stacked middles (world transforms of the meshes). */
USTRUCT()
struct MKIROADS_API FMKIRoadSupportColumn
{
	GENERATED_BODY()
	UPROPERTY(SaveGame)
	FTransform Base;
	UPROPERTY(SaveGame)
	TArray<FTransform> Middles;
};

/** Why a support column cannot stand where it is aimed. */
enum class EMKIRoadSupportFit : uint8 { Fits, NoGround };

/**
 * A support under a raised spline road (#194; by hand since #199, as a zoop since #200). The maintainer: concrete pillars; then,
 * after supports laid every 24 m stood in the lanes of a highway under an overpass, let the player place each one, and
 * set its length, as a zoop. The first click fixes the column's top under the road, snapped to StepCm along it and
 * (#216: the maintainer, two under a 4-lane road or one, as the player wishes) across it from the centerline; the aim then sets where its base goes, in HeightStepCm steps, and the second click builds it: the vanilla
 * concrete pillar, a Pillar Base at the bottom and Concrete Pillar Middles stacked to the deck's underside, the top one
 * half height (scaled) when the height is an odd number of 2 m steps. Where it stands is the player's call: nothing is
 * refused but an aim off a road (#215 removed the on-road check, #216 the spacing and ground-gap checks).
 *
 * Vanilla pillars become lightweight instances once built, so a road could not take real ones with it; a support is the
 * vanilla pillar meshes by reference (material, paint, collision), as the barriers are, saved with this actor and
 * dismantled with the road. The stack's top meets the deck; a base the player sets into the ground sinks into it.
 * The cost is the Concrete Pillar Middle recipe per piece (the base counted as one).
 */
UCLASS()
class MKIROADS_API AMKIRoadSupports : public AFGBuildable
{
	GENERATED_BODY()

public:
	AMKIRoadSupports();

	/** Snap step of a column along and across the road. */
	static constexpr float StepCm = 400.f;
	/** Half the footprint of the vanilla pillar base (8 x 8 m). */
	static constexpr float HalfFootprintCm = 400.f;
	/** Half the width of a Concrete Pillar Middle: a column's top stays this far inside the deck's edges. */
	static constexpr float MiddleHalfCm = 200.f;
	/** Height of a vanilla pillar piece. */
	static constexpr float PieceHeightCm = 400.f;

	/** Most pieces in a column. */
	static constexpr int32 MaxPieces = 64;
	/** A column's height steps (#203: the maintainer); a half-height top piece makes up the odd step. */
	static constexpr float HeightStepCm = 200.f;
	/**
	 * The shortest column (#217: the maintainer, shorter ones don't look right; 6 m, #218): with less room under the deck, the
	 * base sinks into what is below.
	 */
	static constexpr float MinHeightCm = 600.f;
	/** The shortest top piece; less than this is added to the piece below instead. */
	static constexpr float MinPieceCm = 100.f;

	/**
	 * The top of the column under Road at the point Aim selects (the deck's underside, snapped to StepCm along the road and
	 * across it from the centerline, kept MiddleHalfCm inside the edges) and the road's direction there; false if the road
	 * has no spline.
	 */
	static bool FindTop(const AMKIRoadSplineRoad& Road, const FVector& Aim, FVector& OutTop, FVector& OutForward);
	/**
	 * The same under an intersection, turnaround, or depot (#320, the maintainer): the aim snapped to StepCm on the piece's own
	 * axes, nudged to the nearest step whose Concrete Pillar Middle lies wholly under the deck; false if none does nearby.
	 * Forward is the piece's +X.
	 */
	static bool FindTop(const AMKIRoadJunction& Junction, const FVector& Aim, FVector& OutTop, FVector& OutForward);
	/**
	 * The same under a level vanilla foundation (the maintainer, 2026-10-18): the aim snapped to StepCm from the foundation's
	 * centre on its own axes, the column's middle kept under it, the top on its underside. Forward is the foundation's +X.
	 * False for a ramp.
	 */
	static bool FindTop(const class AFGBuildableFoundation& Foundation, const FVector& Aim, FVector& OutTop, FVector& OutForward);
	/** The column height reaching DepthCm below the top (rounded up to HeightStepCm, MinHeightCm to MaxPieces pieces). */
	static float HeightForDepth(float DepthCm);
	/**
	 * The column HeightCm tall under Top: where the vanilla pillar pieces' centers go (their pivots are centered), the top
	 * middle scaled to half height when HeightCm leaves half a piece.
	 */
	static FMKIRoadSupportColumn MakeColumn(const FVector& Top, const FVector& Forward, float HeightCm);
	/** Number of pillar pieces in Columns. */
	static int32 CountPieces(const TArray<FMKIRoadSupportColumn>& Columns);

	// Begin AActor interface
	virtual void BeginPlay() override;
	// End AActor interface

	// Begin AFGBuildable interface
	virtual int32 GetDismantleRefundReturnsMultiplier() const override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& CustomizationData) override;
	// End AFGBuildable interface

	/** Sets the road or road piece and the columns (world) before the supports finish spawning. */
	void Configure(AMKIRoadSplineRoad* Road, AMKIRoadJunction* Junction, const TArray<FMKIRoadSupportColumn>& WorldColumns);

	UFUNCTION(BlueprintPure, Category = "MKI Roads|Supports")
	AMKIRoadSplineRoad* GetRoad() const { return mRoad; }
	/** The intersection, turnaround, or depot it stands under (#320); null under a road. */
	AMKIRoadJunction* GetJunction() const { return mJunction; }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Supports")
	int32 GetColumnCount() const { return mColumns.Num(); }
	const TArray<FMKIRoadSupportColumn>& GetColumns() const { return mColumns; }
	UFUNCTION(BlueprintPure, Category = "MKI Roads|Supports")
	int32 GetPieceCount() const { return CountPieces(mColumns); }
	UStaticMesh* GetBaseMesh() const;
	UStaticMesh* GetMiddleMesh() const;
	/** The mesh transform that puts Mesh's bounds center at PieceCenter (pivots of the vanilla pillar meshes are not centered). */
	static FTransform MeshTransform(const UStaticMesh* Mesh, const FTransform& PieceCenter);

protected:
	UPROPERTY(EditDefaultsOnly, Category = "MKI Roads|Supports")
	TSoftObjectPtr<UStaticMesh> mBaseMesh;
	UPROPERTY(EditDefaultsOnly, Category = "MKI Roads|Supports")
	TSoftObjectPtr<UStaticMesh> mMiddleMesh;

	UPROPERTY(SaveGame)
	TObjectPtr<AMKIRoadSplineRoad> mRoad;
	UPROPERTY(SaveGame)
	TObjectPtr<AMKIRoadJunction> mJunction;
	/** The columns, in world space. */
	UPROPERTY(SaveGame)
	TArray<FMKIRoadSupportColumn> mColumns;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> mPieceComponents;

private:
	void BuildPieces();
};

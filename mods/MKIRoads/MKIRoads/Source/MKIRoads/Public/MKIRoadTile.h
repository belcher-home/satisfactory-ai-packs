#pragma once

#include "CoreMinimal.h"
#include "Buildables/FGBuildableFoundation.h"
#include "MKIRoadTile.generated.h"

class AMKIRoadJunction;
class UBoxComponent;

/**
 * One 8 m square of an intersection, turnaround, or truck depot (the maintainer, 2026-10-18: build road pieces from real
 * foundation squares, as a blueprint lays them, so the game's own snapping works on them). A square is a plain foundation
 * box (mWidth, mDepth, mHeight, every snap allowed) with a collision box and nothing drawn: the piece it belongs to still
 * draws the deck and its markings whole. Vanilla foundations, walls, walkways, attachments, poles, and supports snap to it
 * as to any foundation. It belongs to its piece: dismantling a square dismantles the piece and all its squares (one
 * refund, the piece's), and paint aimed at it goes to the whole piece, so no square is ever painted on its own. It is never
 * a lightweight instance (that would drop its link to the piece).
 */
UCLASS(NotPlaceable)
class MKIROADS_API AMKIRoadTile : public AFGBuildableFoundation
{
	GENERATED_BODY()

public:
	AMKIRoadTile();

	/** Sets the piece and the square's size before it finishes spawning (Depth along X, Width along Y, Height along Z). */
	void Setup(AMKIRoadJunction* Junction, const FVector& Size);
	AMKIRoadJunction* GetJunction() const { return mJunction; }

	// Begin AActor interface
	virtual void BeginPlay() override;
	// End AActor interface

	// Begin IFGDismantleInterface
	virtual void GetChildDismantleActors_Implementation(TArray<AActor*>& OutChildDismantleActors) const override;
	virtual void StartIsLookedAtForDismantle_Implementation(class AFGCharacterPlayer* ByCharacter) override;
	virtual void StopIsLookedAtForDismantle_Implementation(class AFGCharacterPlayer* ByCharacter) override;
	virtual void GetDismantleRefund_Implementation(TArray<FInventoryStack>& OutRefund, bool bNoBuildCostEnabled) const override;
	// End IFGDismantleInterface

	// Begin IFGUseableInterface
	virtual FText GetLookAtDecription_Implementation(class AFGCharacterPlayer* ByCharacter, const FUseState& State) const override;
	// End IFGUseableInterface

	/** The customizer's allowed kinds, as its piece takes (the paint guard sets both together). */
	void SetCustomizerAllows(bool bColoring, bool bPatterning) { mAllowColoring = bColoring; mAllowPatterning = bPatterning; }

	/** Paint aimed at a square goes to its whole piece. */
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& CustomizationData) override;

private:
	void ApplySize();

	/** The piece it belongs to. */
	UPROPERTY(SaveGame)
	TObjectPtr<AMKIRoadJunction> mJunction;

	/** Its size, saved: Depth (X), Width (Y), Height (Z). */
	UPROPERTY(SaveGame)
	FVector mSize = FVector(800.f, 800.f, 100.f);

	UPROPERTY(VisibleAnywhere, Category = "MKI Roads|Tile")
	TObjectPtr<UBoxComponent> mCollision;
};

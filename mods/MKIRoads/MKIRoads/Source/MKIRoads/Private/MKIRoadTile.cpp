#include "MKIRoadTile.h"

#include "MKIRoadJunction.h"
#include "MKIRoadSplineRoad.h"
#include "Components/BoxComponent.h"

AMKIRoadTile::AMKIRoadTile()
{
	// The collision the build gun, the player, and wheels hit, as the road's slabs: the foundations' profile.
	mCollision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
	mCollision->SetupAttachment(RootComponent);
	mCollision->SetMobility(EComponentMobility::Static);
	mCollision->SetCollisionProfileName(TEXT("BuildingMesh"));
	mCollision->SetCanEverAffectNavigation(true);
	mCollision->SetHiddenInGame(true);
	// Never a lightweight instance: it must keep its piece.
	mCanContainLightweightInstances = false;
	mManagedByLightweightBuildableSubsystem = false;
	// A plain foundation: every snap allowed (the defaults), an 8 m square a metre thick until set up.
	mDepth = mWidth = 800.f;
	mHeight = 100.f;
	ApplySize();
}

void AMKIRoadTile::Setup(AMKIRoadJunction* Junction, const FVector& Size)
{
	mJunction = Junction;
	if (Junction) SetBuiltWithRecipe(Junction->GetBuiltWithRecipe());
	mSize = Size;
	ApplySize();
}

void AMKIRoadTile::ApplySize()
{
	mDepth = mSize.X;
	mWidth = mSize.Y;
	mHeight = mSize.Z;
	// #264: squares meeting edge to edge leave seams a downward trace slips through, so the collision overlaps its
	// neighbours by the road's overlap; the foundation box the game snaps to stays exact. The actor sits at its middle,
	// as a vanilla foundation does.
	if (mCollision) mCollision->SetBoxExtent(FVector(mSize.X * 0.5f + AMKIRoadSplineRoad::CollisionOverlapCm, mSize.Y * 0.5f + AMKIRoadSplineRoad::CollisionOverlapCm, mSize.Z * 0.5f), false);
}

void AMKIRoadTile::BeginPlay()
{
	ApplySize();
	// Its piece's recipe, so the build gun's sample (middle click) aimed at a square picks the piece (the maintainer, 2026-10-18:
	// sampling did nothing). Its refund stays empty (GetDismantleRefund): the piece refunds itself.
	if (mJunction) SetBuiltWithRecipe(mJunction->GetBuiltWithRecipe());
	Super::BeginPlay();
}

void AMKIRoadTile::GetChildDismantleActors_Implementation(TArray<AActor*>& OutChildDismantleActors) const
{
	Super::GetChildDismantleActors_Implementation(OutChildDismantleActors);
	// The whole piece goes: the piece itself, which brings its other squares and supports.
	if (mJunction && !mJunction->IsActorBeingDestroyed())
	{
		OutChildDismantleActors.AddUnique(mJunction);
		IFGDismantleInterface::Execute_GetChildDismantleActors(mJunction, OutChildDismantleActors);
		OutChildDismantleActors.Remove(const_cast<AMKIRoadTile*>(this));
	}
}

void AMKIRoadTile::StartIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* ByCharacter)
{
	// Nothing of its own is drawn: the piece shows it is marked.
	Super::StartIsLookedAtForDismantle_Implementation(ByCharacter);
	if (mJunction) IFGDismantleInterface::Execute_StartIsLookedAtForDismantle(mJunction, ByCharacter);
}

void AMKIRoadTile::StopIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* ByCharacter)
{
	Super::StopIsLookedAtForDismantle_Implementation(ByCharacter);
	if (mJunction) IFGDismantleInterface::Execute_StopIsLookedAtForDismantle(mJunction, ByCharacter);
}

void AMKIRoadTile::GetDismantleRefund_Implementation(TArray<FInventoryStack>& OutRefund, bool bNoBuildCostEnabled) const
{
	// The piece refunds itself; a square costs nothing.
}

FText AMKIRoadTile::GetLookAtDecription_Implementation(AFGCharacterPlayer* ByCharacter, const FUseState& State) const
{
	return mJunction ? IFGUseableInterface::Execute_GetLookAtDecription(mJunction, ByCharacter, State) : Super::GetLookAtDecription_Implementation(ByCharacter, State);
}

void AMKIRoadTile::ApplyCustomizationData_Native(const FFactoryCustomizationData& CustomizationData)
{
	// Paint aimed at a square repaints its whole piece (as aimed at the piece before); the square keeps none of its own.
	if (mJunction && !mJunction->IsActorBeingDestroyed()) mJunction->SetCustomizationData_Native(CustomizationData);
}

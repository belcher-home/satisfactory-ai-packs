#include "MKITerrainRules.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "FGCharacterPlayer.h"
#include "FGDismantleInterface.h"
#include "FGInventoryComponent.h"
#include "FGPlayerController.h"
#include "ItemAmount.h"
#include "MKIToolsRemoteCallObject.h"
#include "UI/FGGameUI.h"
#include "FGItemPickup_Spawnable.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "GameFramework/Actor.h"
#include "InstancedFoliageActor.h"
#include "MKITools.h"
#include "TimerManager.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	/**
	 * Mesh folders Terrain mode never touches even on a plain static mesh actor: resource art (nodes, wells, deposits), and
	 * anything built, carried or driven.
	 */
	const TCHAR* const ReservedMeshFolders[] = {
		TEXT("/Resource/"), TEXT("/Buildable/"), TEXT("/Equipment/"), TEXT("/Vehicle/"), TEXT("/Character/"), TEXT("/Interface/"),
	};

	const TCHAR* const LimestoneClass = TEXT("/Game/FactoryGame/Resource/RawResources/Stone/Desc_Stone.Desc_Stone_C");

	/** Cubic metres of a box. */
	double VolumeM3(const FVector& Size) { return FMath::Max(Size.X, 0.0) * FMath::Max(Size.Y, 0.0) * FMath::Max(Size.Z, 0.0) / 1.0e6; }
}

UStaticMeshComponent* FMKITerrainRules::RockMesh(const AActor* Actor)
{
	const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
	UStaticMeshComponent* Mesh = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
	return Mesh && Mesh->GetStaticMesh() ? Mesh : nullptr;
}

FString FMKITerrainRules::RefusalReason(const AActor* Actor)
{
	if (!IsValid(Actor) || Actor->IsActorBeingDestroyed()) return TEXT("missing");
	// Exactly the engine's static mesh actor: Blueprint children (the resource nodes' NodeMeshActor), buildables,
	// destructible rocks, deposits, foliage and landscape are all other classes.
	if (Actor->GetClass() != AStaticMeshActor::StaticClass()) return TEXT("not_static_mesh_actor");
	if (Actor->Implements<UFGDismantleInterface>()) return TEXT("dismantlable");
	// Placed in a level, not spawned while playing.
	if (!Actor->GetLevel() || !(Actor->HasAnyFlags(RF_WasLoaded) || Actor->IsNetStartupActor())) return TEXT("not_level_placed");
	const UStaticMeshComponent* Mesh = RockMesh(Actor);
	if (!Mesh) return TEXT("no_static_mesh");
	if (Mesh->Mobility != EComponentMobility::Static) return TEXT("not_static");
	const FString MeshPath = Mesh->GetStaticMesh()->GetPathName();
	for (const TCHAR* Folder : ReservedMeshFolders) if (MeshPath.Contains(Folder)) return TEXT("reserved_mesh");
	if (Actor->IsHidden() || !Actor->GetActorEnableCollision()) return TEXT("hidden");
	return FString();
}

bool FMKITerrainRules::IsRockFoliageMesh(const UStaticMesh* Mesh)
{
	return Mesh && Mesh->GetPathName().StartsWith(RockMeshFolder);
}

bool FMKITerrainRules::IsRockFoliageType(const UFoliageType* Type)
{
	const UFoliageType_InstancedStaticMesh* MeshType = Cast<UFoliageType_InstancedStaticMesh>(Type);
	return MeshType && IsRockFoliageMesh(MeshType->GetStaticMesh());
}

bool FMKITerrainRules::RockFoliageHit(const FHitResult& Hit, UHierarchicalInstancedStaticMeshComponent*& OutComponent, int32& OutInstance)
{
	UHierarchicalInstancedStaticMeshComponent* Foliage = Cast<UHierarchicalInstancedStaticMeshComponent>(Hit.GetComponent());
	if (!Foliage || !Cast<AInstancedFoliageActor>(Foliage->GetOwner()) || !IsRockFoliageMesh(Foliage->GetStaticMesh())) return false;
	if (Hit.Item == INDEX_NONE || Hit.Item >= Foliage->GetInstanceCount()) return false;
	OutComponent = Foliage;
	OutInstance = Hit.Item;
	return true;
}

int32 FMKITerrainRules::FindRockFoliageInstance(const UHierarchicalInstancedStaticMeshComponent* Component, const FVector& Location, double ToleranceCm)
{
	if (!Component || !IsRockFoliageMesh(Component->GetStaticMesh())) return INDEX_NONE;
	int32 Best = INDEX_NONE; double BestDistance = ToleranceCm;
	for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
	{
		FTransform Instance;
		if (!Component->GetInstanceTransform(Index, Instance, true)) continue;
		const double Distance = FVector::Dist(Instance.GetLocation(), Location);
		if (Distance <= BestDistance) { Best = Index; BestDistance = Distance; }
	}
	return Best;
}

int32 FMKITerrainRules::LimestoneForBoulder(const UStaticMesh* Mesh, const FVector& Scale)
{
	// The maintainer (2026-10-20): boulders give limestone by their size, 5 to 50. Live: at 0.5 per cubic metre of bounds most
	// desert boulders (under ~10 m3) gave the minimum 5; at 2 a 2 m boulder gives about 16 and large ones reach 50.
	const FVector Size = Mesh ? Mesh->GetBoundingBox().GetSize() * Scale.GetAbs() : FVector::ZeroVector;
	return FMath::Clamp(FMath::RoundToInt(VolumeM3(Size) * 2.0), 5, 50);
}

int32 FMKITerrainRules::LimestoneForRock(const FBox& Bounds)
{
	// The maintainer (2026-10-20): level rocks give limestone by their size, 50 to 500.
	return FMath::Clamp(FMath::RoundToInt(VolumeM3(Bounds.GetSize()) * 0.01), 50, 500);
}

void FMKITerrainRules::GrantLimestone(AFGCharacterPlayer* Character, int32 Amount, const FVector& At)
{
	if (!Character || !Character->HasAuthority() || Amount <= 0) return;
	const TSubclassOf<UFGItemDescriptor> Limestone = LoadClass<UFGItemDescriptor>(nullptr, LimestoneClass);
	if (!Limestone) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: limestone (%s) is missing; nothing granted"), LimestoneClass); return; }
	// The maintainer (2026-10-20): the game's own "+N Limestone" popup, as for a pickup. Live: adding the stack to the
	// inventory and calling the HUD's PushInventoryNotification showed nothing, so the limestone is handed over the way
	// the game hands over any dropped item: it is dropped at the player as a world item and picked up by the player
	// (AFGCharacterPlayer::PickUpItem, what E on a dropped item runs through the item's OnUse; live, the item's own
	// PickUpByCharacter alone gave the items but no popup), with the game's own feedback. What does not fit
	// stays on the ground as a normal item drop.
	// The maintainer (2026-10-20): the drop at the player's feet showed the item-drop rings and looked wrong. The items go
	// straight into the inventory and the character's own "picked up an item" (OnPickUpItem) runs, which the game's
	// pickup ran when the popup did show; only what does not fit is dropped (and picked up as below).
	if (UFGInventoryComponent* Inventory = Character->GetInventory())
	{
		const int32 Added = Inventory->AddStack(FInventoryStack(Amount, Limestone), true);
		if (Added > 0) Character->OnPickUpItem();
		UE_LOG(LogMKITools, Log, TEXT("Terrain: %d limestone into the inventory, then OnPickUpItem (%d to drop)"), Added, Amount - Added);
		Amount -= Added;
		if (Amount <= 0) return;
	}
	TArray<AFGItemPickup_Spawnable*> Drops;
	const TArray<FInventoryStack> Stacks{ FInventoryStack(Amount, Limestone) };
	const TArray<AActor*> Ignore{ Character };
	AFGItemPickup_Spawnable::CreateItemDropsInCylinder(Character->GetWorld(), Stacks, Character->GetActorLocation(), 50.f, Ignore, Drops);
	TArray<TWeakObjectPtr<AFGItemPickup_Spawnable>> Waiting;
	for (AFGItemPickup_Spawnable* Drop : Drops)
	{
		if (!Drop) continue;
		Character->PickUpItem(Drop);
		if (IsValid(Drop) && !Drop->IsPickedUp()) Waiting.Add(Drop);
	}
	if (Drops.Num() == 0)
	{
		// No drop could be made (no room around the player): straight into the inventory, without the popup.
		UFGInventoryComponent* Inventory = Character->GetInventory();
		const int32 Added = Inventory ? Inventory->AddStack(FInventoryStack(Amount, Limestone), true) : 0;
		UE_LOG(LogMKITools, Log, TEXT("Terrain: %d limestone straight into the inventory (%d added)"), Amount, Added);
		return;
	}
	UE_LOG(LogMKITools, Log, TEXT("Terrain: %d limestone through the character's pickup (%d drop(s), %d not taken yet)"), Amount, Drops.Num(), Waiting.Num());
	if (Waiting.Num() == 0) return;
	// Live (2026-10-20): from the chainsaw the character's pickup took the drop (with the popup); from Terrain mode (the
	// build gun out) it left it at the player's feet. Once the removal is done the character's pickup is tried again,
	// and whatever it still refuses is taken by the item's own pickup (the items, without the popup).
	TWeakObjectPtr<AFGCharacterPlayer> WeakCharacter(Character);
	FTimerHandle Retry;
	Character->GetWorldTimerManager().SetTimer(Retry, FTimerDelegate::CreateWeakLambda(Character, [WeakCharacter, Waiting]()
	{
		AFGCharacterPlayer* Player = WeakCharacter.Get();
		if (!Player) return;
		int32 ByCharacter = 0, ByItem = 0, Left = 0;
		for (const TWeakObjectPtr<AFGItemPickup_Spawnable>& Weak : Waiting)
		{
			AFGItemPickup_Spawnable* Drop = Weak.Get();
			if (!IsValid(Drop) || Drop->IsPickedUp()) continue;
			Player->PickUpItem(Drop);
			if (!IsValid(Drop) || Drop->IsPickedUp()) { ++ByCharacter; continue; }
			Drop->PickUpByCharacter(Player);
			if (!IsValid(Drop) || Drop->IsPickedUp()) ++ByItem;
			else Left += Drop->GetPickupItems().NumItems;
		}
		UE_LOG(LogMKITools, Log, TEXT("Terrain: limestone retry: %d by the character's pickup, %d by the item's pickup, %d left on the ground"), ByCharacter, ByItem, Left);
	}), 0.25f, false);
}

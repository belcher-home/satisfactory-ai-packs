#include "MKICollectNearby.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGCharacterPlayer.h"
#include "FGCrate.h"
#include "FGFoliageIdentifier.h"
#include "FGFoliagePickup.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FGFoliageResourceUserData.h"
#include "FGInventoryComponent.h"
#include "FGItemPickup.h"
#include "GameFramework/PlayerController.h"
#include "MKITools.h"
#include "MKIToolsConfiguration.h"

bool UMKICollectNearbySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

TStatId UMKICollectNearbySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMKICollectNearbySubsystem, STATGROUP_Tickables);
}

bool UMKICollectNearbySubsystem::IsCollectibleFoliage(UStaticMesh* Mesh)
{
	if (!Mesh) return false;
	if (const bool* Known = CollectibleMeshes.Find(Mesh)) return *Known;
	bool bCollectible = false;
	if (const UFGFoliageResourceUserData* Data = Mesh->GetAssetUserData<UFGFoliageResourceUserData>())
	{
		// The maintainer (2026-10-21): berries, nuts and bacon agaric (Desc_Berry, Desc_Nut, Desc_Shroom).
		for (const FItemDropWithChance& Drop : Data->GetPickupItems())
		{
			const FString Item = GetNameSafe(Drop.Drop.ItemClass.Get());
			if (Item.Contains(TEXT("Berry")) || Item.Contains(TEXT("Nut")) || Item.Contains(TEXT("Shroom"))) { bCollectible = true; break; }
		}
	}
	CollectibleMeshes.Add(Mesh, bCollectible);
	if (bCollectible) UE_LOG(LogMKITools, Log, TEXT("Collect: collects %s"), *Mesh->GetName());
	return bCollectible;
}

void UMKICollectNearbySubsystem::Tick(float DeltaTime)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	AFGCharacterPlayer* Character = Controller ? Cast<AFGCharacterPlayer>(Controller->GetPawn()) : nullptr;
	if (!Character || !Character->IsLocallyControlled() || !UMKIToolsConfiguration::GetCollectNearby(Character)) return;
	AFGFoliagePickup* Proxy = Character->GetFoliagePickupProxy();
	const float RadiusCm = BaseRadiusCm * UMKIToolsConfiguration::GetCollectRadiusMultiplier(Character);
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now >= NextScanTime)
	{
		NextScanTime = Now + ScanIntervalSeconds;
		Scan(Character, RadiusCm);
		if (Character->HasAuthority()) EmptyCrates(Character, RadiusCm);
	}
	if (Proxy && Now >= NextPickupTime && PickUpNext(Character, Proxy, RadiusCm)) NextPickupTime = Now + PickupIntervalSeconds;
}

void UMKICollectNearbySubsystem::Scan(AFGCharacterPlayer* Character, float RadiusCm)
{
	NearItems.Reset();
	NearCrates.Reset();
	const FVector Center = Character->GetActorLocation();
	const double RadiusSq = FMath::Square(static_cast<double>(RadiusCm));
	for (TActorIterator<AFGItemPickup> It(GetWorld()); It; ++It)
	{
		if (!It->IsPickedUp() && FVector::DistSquared(It->GetActorLocation(), Center) <= RadiusSq) NearItems.Add(*It);
	}
	for (TActorIterator<AFGCrate> It(GetWorld()); It; ++It)
	{
		// The maintainer (2026-10-21): dismantle crates and death crates.
		const EFGCrateType Type = It->GetCrateType();
		if ((Type == EFGCrateType::CT_DismantleCrate || Type == EFGCrateType::CT_DeathCrate) && FVector::DistSquared(It->GetActorLocation(), Center) <= RadiusSq) NearCrates.Add(*It);
	}
}

bool UMKICollectNearbySubsystem::PickUpNext(AFGCharacterPlayer* Character, AFGFoliagePickup* Proxy, float RadiusCm)
{
	// Ground items first, one per pickup, through the proxy as a press of E on them.
	while (NearItems.Num() > 0)
	{
		AFGItemPickup* Item = NearItems.Pop().Get();
		if (!Item || Item->IsPickedUp() || !Item->CanBePickedUp(Character)) continue;
		Proxy->SetItemToPickup(Item);
		Proxy->DoPickup();
		return true;
	}
	// Then the nearest collectible foliage instance.
	AFGFoliageRemovalSubsystem* Removal = AFGFoliageRemovalSubsystem::Get(GetWorld());
	if (!Removal) return false;
	const FVector Center = Character->GetActorLocation();
	TMap<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>> Near;
	Removal->GetFoliageWithinRadius(Center, RadiusCm, Near);
	UHierarchicalInstancedStaticMeshComponent* Best = nullptr;
	int32 BestInstance = INDEX_NONE;
	double BestDistSq = TNumericLimits<double>::Max();
	for (const TPair<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>& Entry : Near)
	{
		if (!IsValid(Entry.Key) || !IsCollectibleFoliage(Entry.Key->GetStaticMesh())) continue;
		if (!AFGFoliageRemovalSubsystem::HasIdentifier(Entry.Key, UFGFoliageIdentifier_Pickupable::StaticClass())) continue;
		for (const int32 Instance : Entry.Value)
		{
			FTransform Transform;
			if (!Entry.Key->GetInstanceTransform(Instance, Transform, true)) continue;
			const double DistSq = FVector::DistSquared(Transform.GetLocation(), Center);
			if (DistSq < BestDistSq) { BestDistSq = DistSq; Best = Entry.Key; BestInstance = Instance; }
		}
	}
	if (!Best) return false;
	Proxy->SetPickupData(Best, BestInstance, Character);
	Proxy->DoPickup();
	return true;
}

void UMKICollectNearbySubsystem::EmptyCrates(AFGCharacterPlayer* Character, float RadiusCm)
{
	UFGInventoryComponent* Inventory = Character->GetInventory();
	if (!Inventory) return;
	for (const TWeakObjectPtr<AFGCrate>& Weak : NearCrates)
	{
		AFGCrate* Crate = Weak.Get();
		UFGInventoryComponent* Contents = Crate ? Crate->GetInventory() : nullptr;
		if (!Contents) continue;
		int32 Moved = 0;
		for (int32 Index = 0; Index < Contents->GetSizeLinear(); ++Index)
		{
			FInventoryStack Stack;
			if (!Contents->GetStackFromIndex(Index, Stack) || !Stack.HasItems()) continue;
			const int32 Added = Inventory->AddStack(Stack, true);
			if (Added <= 0) continue;
			// Removing the items lets the crate notice it is empty and go, as when the player takes them.
			Contents->RemoveFromIndex(Index, Added);
			Moved += Added;
		}
		if (Moved > 0)
		{
			UE_LOG(LogMKITools, Log, TEXT("Collect: took %d items from %s"), Moved, *Crate->GetName());
			Character->OnPickUpItem();
		}
	}
}

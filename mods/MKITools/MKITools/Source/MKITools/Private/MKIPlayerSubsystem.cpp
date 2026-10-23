#include "MKIPlayerSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "MKITools.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

AMKIPlayerSubsystem::AMKIPlayerSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
	PrimaryActorTick.bCanEverTick = false;
}

void AMKIPlayerSubsystem::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority()) GetWorldTimerManager().SetTimer(ApplyTimer, this, &AMKIPlayerSubsystem::Apply, 1.f, true);
}

void AMKIPlayerSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(ApplyTimer);
	Super::EndPlay(EndPlayReason);
}

AMKIPlayerSubsystem* AMKIPlayerSubsystem::Get(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	USubsystemActorManager* Manager = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
	return Manager ? Manager->GetSubsystemActor<AMKIPlayerSubsystem>() : nullptr;
}

int32 AMKIPlayerSubsystem::GetExtraInventorySlots(const UObject* WorldContext)
{
	const AMKIPlayerSubsystem* Self = Get(WorldContext);
	return Self ? Self->ExtraInventorySlots : 0;
}

bool AMKIPlayerSubsystem::CanAddInventorySlots(const UObject* WorldContext)
{
	// The subsystem spawns on the host only, so a multiplayer client finds none here.
	const AMKIPlayerSubsystem* Self = Get(WorldContext);
	return Self && Self->HasAuthority() && Self->ExtraInventorySlots < MaximumExtraInventorySlots;
}

void AMKIPlayerSubsystem::AddInventorySlots(const UObject* WorldContext)
{
	AMKIPlayerSubsystem* Self = Get(WorldContext);
	if (!Self || !Self->HasAuthority() || Self->ExtraInventorySlots >= MaximumExtraInventorySlots) return;
	const int32 Before = Self->ExtraInventorySlots;
	Self->ExtraInventorySlots = FMath::Min(Before + InventorySlotsPerPress, MaximumExtraInventorySlots);
	UE_LOG(LogMKITools, Log, TEXT("Inventory: extra slots %d -> %d"), Before, Self->ExtraInventorySlots);
	Self->Apply();
}

void AMKIPlayerSubsystem::Apply()
{
	for (TActorIterator<AFGCharacterPlayer> It(GetWorld()); It; ++It)
	{
		UFGInventoryComponent* Inventory = It->GetInventory();
		// GetTotalPlayerInventorySlots includes the extra slots (the module's hook).
		const int32 Total = It->GetTotalPlayerInventorySlots();
		if (!Inventory || Inventory->GetSizeLinear() >= Total) continue;
		UE_LOG(LogMKITools, Log, TEXT("Inventory: %s grows from %d to %d slots"), *It->GetName(), Inventory->GetSizeLinear(), Total);
		Inventory->Resize(Total);
	}
}

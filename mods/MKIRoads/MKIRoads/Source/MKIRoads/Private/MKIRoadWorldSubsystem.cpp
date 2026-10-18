#include "MKIRoadWorldSubsystem.h"

#include "MKIRoadAutoPaths.h"
#include "MKIRoadJunction.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "EngineUtils.h"
#include "FGBuildableSubsystem.h"
#include "TimerManager.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "MKIRoadsGameWorldModule.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehiclePathPreset.h"

bool UMKIRoadWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMKIRoadWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (InWorld.GetNetMode() == NM_Client) return;
	// Before the vehicle subsystem begins play (#239): its path networks drop nodes that no longer exist. Live: paths and
	// nodes the MKI Roads once removed with Destroy() stayed listed in a saved network, and the load crashed rebuilding it
	// (UFGVehiclePathNetwork::RegenerateCachedDataOnServer).
	if (AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(&InWorld))
	{
		int32 Dropped = 0;
		TArray<UFGVehiclePathNetwork*>& Networks = const_cast<TArray<UFGVehiclePathNetwork*>&>(Vehicles->GetAllPathNetworks());
		Dropped += Networks.RemoveAll([](const UFGVehiclePathNetwork* Network) { return !IsValid(Network); });
		for (UFGVehiclePathNetwork* Network : Networks)
		{
			Dropped += const_cast<TArray<AFGVehiclePathNode*>&>(Network->GetNetworkElementsOnServer()).RemoveAll([](const AFGVehiclePathNode* Node) { return !IsValid(Node); });
		}
		if (Dropped > 0) UE_LOG(LogMKIRoads, Log, TEXT("Road paths: dropped %d missing entries from the vehicle path networks"), Dropped);
	}
	// And path nodes' links to paths that no longer exist, which the rebuild walks too.
	FMKIRoadAutoPaths::CleanStaleNodeLinks(&InWorld);
	if (AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(&InWorld)) { Buildables->BuildableConstructedGlobalDelegate.AddDynamic(this, &UMKIRoadWorldSubsystem::OnBuildableConstructed); Buildables->mBuildableRemovedDelegate.AddDynamic(this, &UMKIRoadWorldSubsystem::OnBuildableRemoved); }
	// Path nodes' links to removed paths (#239), before anything changes them.
	InWorld.GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]() { FMKIRoadAutoPaths::CleanStaleNodeLinks(GetWorld()); }));
	// Every vehicle path the game marks invalid, and why (#239), in the log.
	FTimerHandle Report;
	InWorld.GetTimerManager().SetTimer(Report, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		int32 Paths = 0, Unchecked = 0;
		for (TActorIterator<AFGVehiclePathSegment> It(GetWorld()); It; ++It)
		{
			++Paths;
			if (It->GetPathSegmentValidationData().Num() == 0) ++Unchecked;
			for (const FVehiclePathSegmentValidationData& Data : It->GetPathSegmentValidationData())
			{
				if (Data.ValidationInfo.Result == EVehiclePathValidationResult::Valid) continue;
				UE_LOG(LogMKIRoads, Log, TEXT("Path %s at %s: %s invalid (%d) at %.0f cm"), *It->GetName(), *It->GetActorLocation().ToString(), Data.PathPreset ? *Data.PathPreset->GetName() : TEXT("?"), static_cast<int32>(Data.ValidationInfo.Result), Data.ValidationInfo.SplineValidationFailDistance);
			}
		}
		UE_LOG(LogMKIRoads, Log, TEXT("Vehicle paths checked: %d, not yet validated: %d"), Paths, Unchecked);
	}), 15.f, false);
}

void UMKIRoadWorldSubsystem::OnBuildableRemoved(AFGBuildable* Buildable)
{
	const AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(Buildable);
	UWorld* World = GetWorld();
	if (!Station || !World || World->GetNetMode() == NM_Client) return;
	// A second later, once the station, its docking node, and the vehicle network's rebuild are done (live, #241: re-laid
	// the next tick, the paths joined the dying docking node and the network crashed rebuilding).
	const FVector Location = Station->GetActorLocation();
	FTimerHandle Handle;
	World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, [this, Location]()
	{
		for (TActorIterator<AMKIRoadJunction> It(GetWorld()); It; ++It)
		{
			if (It->IsTruckDepot() && It->FindDepotDock(Location, 200.f) != INDEX_NONE) It->OnDockStationRemovedAt(Location);
		}
	}), 1.f, false);
}

void UMKIRoadWorldSubsystem::OnBuildableConstructed(AFGBuildable* Buildable)
{
	const AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(Buildable);
	UWorld* World = GetWorld();
	if (!Station || !World) return;
	// Once the station's docking node stands: tried a few times shortly after the build; a depot lays its bay path once only.
	const FVector Location = Station->GetActorLocation();
	auto Try = FTimerDelegate::CreateWeakLambda(this, [this, Location]()
	{
		for (TActorIterator<AMKIRoadJunction> It(GetWorld()); It; ++It)
		{
			if (It->IsTruckDepot() && It->FindDepotDock(Location, 200.f) != INDEX_NONE) It->LayBayPaths();
		}
	});
	World->GetTimerManager().SetTimerForNextTick(Try);
	for (const float Delay : { 0.5f, 2.f })
	{
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, Try, Delay, false);
	}
}

void UMKIRoadWorldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client) return;
	AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(World);
	if (!Vehicles) return;
	int32 Dropped = 0;
	for (UFGVehiclePathNetwork* Network : Vehicles->GetAllPathNetworks())
	{
		if (!IsValid(Network)) continue;
		Dropped += const_cast<TArray<AFGVehiclePathNode*>&>(Network->GetNetworkElementsOnServer()).RemoveAll([](const AFGVehiclePathNode* Node) { return !IsValid(Node); });
	}
	if (Dropped > 0) UE_LOG(LogMKIRoads, Log, TEXT("Road paths: dropped %d removed nodes from the vehicle path networks"), Dropped);
}

TStatId UMKIRoadWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMKIRoadWorldSubsystem, STATGROUP_Tickables);
}

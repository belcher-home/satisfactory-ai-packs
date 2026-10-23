#include "MKITerrainSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGCharacterPlayer.h"
#include "FGFoliageRemovalSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "MKITerrainRules.h"
#include "MKITools.h"
#include "Net/UnrealNetwork.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WorldPartition/HLOD/HLODRuntimeSubsystem.h"
#include "WorldPartition/HLOD/IWorldPartitionHLODObject.h"

namespace
{
	/** A streamed-in actor with a removed rock's name is that rock only if its mesh sits where the rock's did. */
	constexpr double SameRockToleranceCm = 100.0;
	/** An HLOD instance stands for a removed rock when it has the rock's mesh and transform, within these tolerances. */
	constexpr double HlodLocationToleranceCm = 25.0;
	constexpr double HlodScaleTolerance = 0.01;
	/** A merged HLOD mesh within a removed rock's bounds grown by this much is that rock's stand-in. */
	constexpr double HlodProxySlackCm = 500.0;

	FString JsonText(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	FString ErrorText(const TCHAR* Code, const FString& Message)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), false);
		Object->SetStringField(TEXT("error"), Code);
		Object->SetStringField(TEXT("message"), Message);
		return JsonText(Object);
	}

	TSharedRef<FJsonObject> VectorJson(const FVector& Vector)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X); Object->SetNumberField(TEXT("y"), Vector.Y); Object->SetNumberField(TEXT("z"), Vector.Z);
		return Object;
	}

	TSharedRef<FJsonObject> RecordJson(const FMKITerrainRemovedRock& Record)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("actorName"), Record.ActorName.ToString());
		Object->SetStringField(TEXT("actorPath"), Record.ActorPath);
		Object->SetStringField(TEXT("mesh"), Record.MeshPath);
		Object->SetObjectField(TEXT("location"), VectorJson(Record.MeshTransform.GetLocation()));
		const TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetObjectField(TEXT("min"), VectorJson(Record.BoundsMin)); Bounds->SetObjectField(TEXT("max"), VectorJson(Record.BoundsMax));
		Object->SetObjectField(TEXT("bounds"), Bounds);
		Object->SetStringField(TEXT("removedBy"), Record.RemovedBy);
		Object->SetStringField(TEXT("removedAt"), Record.RemovedAt);
		return Object;
	}

	bool IsHlodActor(const AActor* Actor)
	{
		return Actor && Actor->GetClass()->GetName().Contains(TEXT("WorldPartitionHLOD"));
	}
}

AMKITerrainSubsystem::AMKITerrainSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = false;
}

AMKITerrainSubsystem* AMKITerrainSubsystem::Get(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World) return nullptr;
	for (TActorIterator<AMKITerrainSubsystem> It(World); It; ++It) if (IsValid(*It)) return *It;
	return nullptr;
}

void AMKITerrainSubsystem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMKITerrainSubsystem, RemovedRocks);
}

void AMKITerrainSubsystem::BeginPlay()
{
	Super::BeginPlay();
	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &AMKITerrainSubsystem::OnLevelAdded);
	// Live (2026-10-19): HLOD actors stream in and out on their own, without a level being added, so a removed rock showed
	// again from 1 km. Each one is checked as the HLOD runtime registers it.
	if (UWorldPartitionHLODRuntimeSubsystem* Hlod = GetWorld()->GetSubsystem<UWorldPartitionHLODRuntimeSubsystem>())
	{
		HlodRegisteredHandle = Hlod->OnHLODObjectRegisteredEvent().AddUObject(this, &AMKITerrainSubsystem::OnHlodRegistered);
	}
	ApplyAll();
}

void AMKITerrainSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	if (UWorldPartitionHLODRuntimeSubsystem* Hlod = GetWorld() ? GetWorld()->GetSubsystem<UWorldPartitionHLODRuntimeSubsystem>() : nullptr)
	{
		Hlod->OnHLODObjectRegisteredEvent().Remove(HlodRegisteredHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void AMKITerrainSubsystem::PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
	UE_LOG(LogMKITools, Log, TEXT("Terrain: %d removed rocks loaded from the save"), RemovedRocks.Num());
	ApplyAll();
}

void AMKITerrainSubsystem::OnRep_RemovedRocks()
{
	ApplyAll();
}

void AMKITerrainSubsystem::OnLevelAdded(ULevel* Level, UWorld* World)
{
	if (World == GetWorld()) ApplyToLevel(Level);
}

void AMKITerrainSubsystem::OnHlodRegistered(IWorldPartitionHLODObject* HlodObject)
{
	if (RemovedRocks.Num() == 0 || !HlodObject) return;
	if (AActor* Actor = Cast<AActor>(HlodObject->GetUObject())) HideHlodInstances(Actor);
}

const FMKITerrainRemovedRock* AMKITerrainSubsystem::FindRecord(const AActor* Actor) const
{
	if (!Actor || Actor->GetClass() != AStaticMeshActor::StaticClass()) return nullptr;
	const FName Name = Actor->GetFName();
	for (const FMKITerrainRemovedRock& Record : RemovedRocks)
	{
		if (Record.ActorName != Name) continue;
		const UStaticMeshComponent* Mesh = FMKITerrainRules::RockMesh(Actor);
		const FVector Location = Mesh ? Mesh->GetComponentLocation() : Actor->GetActorLocation();
		if (FVector::Dist(Location, Record.MeshTransform.GetLocation()) <= SameRockToleranceCm) return &Record;
	}
	return nullptr;
}

void AMKITerrainSubsystem::SetRockRemoved(AActor* Rock, bool bRemoved)
{
	if (!IsValid(Rock)) return;
	Rock->SetActorHiddenInGame(bRemoved);
	Rock->SetActorEnableCollision(!bRemoved);
}

void AMKITerrainSubsystem::ApplyToLevel(ULevel* Level)
{
	if (!Level || RemovedRocks.Num() == 0) return;
	for (AActor* Actor : Level->Actors)
	{
		if (FindRecord(Actor))
		{
			SetRockRemoved(Actor, true);
			AppliedNames.Add(Actor->GetFName());
		}
	}
	HideHlodInstances(Level);
}

void AMKITerrainSubsystem::ApplyAll()
{
	UWorld* World = GetWorld();
	if (!World) return;
	// Shown again: every rock this machine hid that is no longer on the list (a restore, here or replicated in).
	for (const FName Name : AppliedNames.Array())
	{
		if (RemovedRocks.ContainsByPredicate([Name](const FMKITerrainRemovedRock& Record) { return Record.ActorName == Name; })) continue;
		SetRockRemoved(FindLoadedActor(Name), false);
		RestoreHlodInstances(Name);
		AppliedNames.Remove(Name);
	}
	for (ULevel* Level : World->GetLevels()) ApplyToLevel(Level);
}

AActor* AMKITerrainSubsystem::FindLoadedActor(FName ActorName) const
{
	const UWorld* World = GetWorld();
	if (!World || ActorName.IsNone()) return nullptr;
	for (const ULevel* Level : World->GetLevels())
	{
		if (!Level) continue;
		for (AActor* Actor : Level->Actors) if (Actor && Actor->GetFName() == ActorName && !Actor->IsActorBeingDestroyed()) return Actor;
	}
	return nullptr;
}

void AMKITerrainSubsystem::HideHlodInstances(ULevel* Level)
{
	for (AActor* Actor : Level->Actors) if (IsHlodActor(Actor)) HideHlodInstances(Actor);
}

void AMKITerrainSubsystem::HideHlodInstances(AActor* HlodActor)
{
	if (!IsValid(HlodActor) || RemovedRocks.Num() == 0) return;
	// Instances of components that went away with an unloaded HLOD actor; a reloaded one has new components.
	HiddenHlodInstances.RemoveAll([](const FHiddenHlodInstance& Hidden) { return !Hidden.Component.IsValid(); });
	HiddenHlodProxies.RemoveAll([](const FHiddenHlodProxy& Hidden) { return !Hidden.Component.IsValid(); });
	TInlineComponentArray<UStaticMeshComponent*> Components(HlodActor);
	for (UStaticMeshComponent* Component : Components)
	{
		const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
		if (!Mesh) continue;
		UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
		if (!Instanced)
		{
			// A merged or simplified HLOD mesh. Live (2026-10-19): from 1 km the arch was drawn by a WPHLOD_Landscape mesh
			// whose bounds lay inside the arch's own, its simplified stand-in. A merged mesh that fits inside a removed rock's
			// bounds draws only that rock and is hidden with it; a larger one (a stretch of terrain with the rock in it)
			// cannot lose the rock alone, so it stays and is logged.
			const FBox Covered = Component->Bounds.GetBox();
			for (const FMKITerrainRemovedRock& Record : RemovedRocks)
			{
				const FBox Rock(Record.BoundsMin, Record.BoundsMax);
				if (!Covered.Intersect(Rock)) continue;
				if (Rock.ExpandBy(HlodProxySlackCm).IsInside(Covered))
				{
					if (!HiddenHlodProxies.ContainsByPredicate([Component](const FHiddenHlodProxy& Hidden) { return Hidden.Component == Component; }))
					{
						Component->SetHiddenInGame(true);
						HiddenHlodProxies.Add({ Component, Record.ActorName });
						UE_LOG(LogMKITools, Log, TEXT("Terrain: hid HLOD stand-in %s (%s) for %s"), *HlodActor->GetName(), *Mesh->GetPathName(), *Record.ActorName.ToString());
					}
					break;
				}
				UE_LOG(LogMKITools, Log, TEXT("Terrain: HLOD %s draws %s inside a larger merged mesh (%s); it stays visible from afar"), *HlodActor->GetName(), *Record.ActorName.ToString(), *Mesh->GetPathName());
			}
			continue;
		}
		const FString MeshPath = Mesh->GetPathName();
		for (const FMKITerrainRemovedRock& Record : RemovedRocks)
		{
			if (Record.MeshPath != MeshPath) continue;
			for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
			{
				if (HiddenHlodInstances.ContainsByPredicate([Instanced, Index](const FHiddenHlodInstance& Hidden) { return Hidden.Component == Instanced && Hidden.Index == Index; })) continue;
				FTransform Instance;
				if (!Instanced->GetInstanceTransform(Index, Instance, true)) continue;
				if (FVector::Dist(Instance.GetLocation(), Record.MeshTransform.GetLocation()) > HlodLocationToleranceCm) continue;
				if (!Instance.GetScale3D().Equals(Record.MeshTransform.GetScale3D(), HlodScaleTolerance)) continue;
				FTransform Collapsed = Instance;
				Collapsed.SetScale3D(FVector::ZeroVector);
				if (!Instanced->UpdateInstanceTransform(Index, Collapsed, true, true, true)) continue;
				HiddenHlodInstances.Add({ Instanced, Index, Instance, Record.ActorName });
				UE_LOG(LogMKITools, Log, TEXT("Terrain: hid HLOD instance %d of %s for %s"), Index, *HlodActor->GetName(), *Record.ActorName.ToString());
			}
		}
	}
}

void AMKITerrainSubsystem::RestoreHlodInstances(FName ActorName)
{
	HiddenHlodInstances.RemoveAll([ActorName](const FHiddenHlodInstance& Hidden)
	{
		if (Hidden.Rock != ActorName) return false;
		if (UInstancedStaticMeshComponent* Component = Hidden.Component.Get()) Component->UpdateInstanceTransform(Hidden.Index, Hidden.Original, true, true, true);
		return true;
	});
	HiddenHlodProxies.RemoveAll([ActorName](const FHiddenHlodProxy& Hidden)
	{
		if (Hidden.Rock != ActorName) return false;
		if (UStaticMeshComponent* Component = Hidden.Component.Get()) Component->SetHiddenInGame(false);
		return true;
	});
}

bool AMKITerrainSubsystem::RemoveRock(AActor* Rock, const FString& RemovedBy, FString& OutReason)
{
	if (!HasAuthority()) { OutReason = TEXT("server_authority_required"); return false; }
	OutReason = FMKITerrainRules::RefusalReason(Rock);
	if (!OutReason.IsEmpty()) return false;
	const UStaticMeshComponent* Mesh = FMKITerrainRules::RockMesh(Rock);
	const FBox Bounds = Rock->GetComponentsBoundingBox(true);
	FMKITerrainRemovedRock Record;
	Record.ActorName = Rock->GetFName();
	Record.ActorPath = Rock->GetPathName();
	Record.MeshPath = Mesh->GetStaticMesh()->GetPathName();
	Record.MeshTransform = Mesh->GetComponentTransform();
	Record.BoundsMin = Bounds.Min;
	Record.BoundsMax = Bounds.Max;
	Record.RemovedBy = RemovedBy;
	Record.RemovedAt = FDateTime::UtcNow().ToIso8601();
	RemovedRocks.Add(Record);
	ApplyAll();
	ForceNetUpdate();
	UE_LOG(LogMKITools, Log, TEXT("Terrain: %s removed %s (%s)"), *RemovedBy, *Record.ActorName.ToString(), *Record.MeshPath);
	return true;
}

bool AMKITerrainSubsystem::RemoveRockForPlayer(AActor* Rock, APlayerController* Player, FString& OutReason)
{
	const APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (!Pawn || !IsValid(Rock)) { OutReason = TEXT("missing"); return false; }
	// The player must stand within reach of the rock: the nearest point of its bounds, so a long arch counts from any part.
	const FBox Bounds = Rock->GetComponentsBoundingBox(true);
	const double Distance = Bounds.IsValid ? FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(Pawn->GetActorLocation())) : FVector::Dist(Rock->GetActorLocation(), Pawn->GetActorLocation());
	if (Distance > FMKITerrainRules::MaximumReachCm) { OutReason = FString::Printf(TEXT("out_of_reach (%.0f cm)"), Distance); return false; }
	const APlayerState* State = Player->GetPlayerState<APlayerState>();
	if (!RemoveRock(Rock, State ? State->GetPlayerName() : FString(TEXT("player")), OutReason)) return false;
	// The maintainer (2026-10-20): a removed level rock gives limestone by its size, once (a rock already removed is refused).
	FMKITerrainRules::GrantLimestone(Cast<AFGCharacterPlayer>(Player->GetPawn()), FMKITerrainRules::LimestoneForRock(Bounds), Pawn->GetActorLocation());
	return true;
}

bool AMKITerrainSubsystem::RemoveRockFoliageForPlayer(UHierarchicalInstancedStaticMeshComponent* Foliage, const FVector& Location, APlayerController* Player, FString& OutReason)
{
	if (!HasAuthority()) { OutReason = TEXT("server_authority_required"); return false; }
	const APawn* Pawn = Player ? Player->GetPawn() : nullptr;
	if (!Pawn || !IsValid(Foliage)) { OutReason = TEXT("missing"); return false; }
	// The instance by its location: indices are the server's own and change as instances are removed.
	const int32 Instance = FMKITerrainRules::FindRockFoliageInstance(Foliage, Location, 100.0);
	if (Instance == INDEX_NONE) { OutReason = TEXT("not_rock_foliage"); return false; }
	FTransform Transform;
	Foliage->GetInstanceTransform(Instance, Transform, true);
	const double Distance = FVector::Dist(Transform.GetLocation(), Pawn->GetActorLocation());
	if (Distance > FMKITerrainRules::MaximumReachCm) { OutReason = FString::Printf(TEXT("out_of_reach (%.0f cm)"), Distance); return false; }
	AFGFoliageRemovalSubsystem* Removal = AFGFoliageRemovalSubsystem::Get(GetWorld());
	// The game's own foliage removal, as the chainsaw's: saved with the game and replicated, and permanent.
	if (!Removal || !Removal->RemoveFoliageInstance(Foliage, Instance)) { OutReason = TEXT("foliage_removal_failed"); return false; }
	UE_LOG(LogMKITools, Log, TEXT("Terrain: removed rock foliage %s at %s"), *Foliage->GetStaticMesh()->GetName(), *Transform.GetLocation().ToString());
	FMKITerrainRules::GrantLimestone(Cast<AFGCharacterPlayer>(Player->GetPawn()), FMKITerrainRules::LimestoneForBoulder(Foliage->GetStaticMesh(), Transform.GetScale3D()), Pawn->GetActorLocation());
	return true;
}

bool AMKITerrainSubsystem::RestoreRock(FName ActorName, FString& OutReason)
{
	if (!HasAuthority()) { OutReason = TEXT("server_authority_required"); return false; }
	if (RemovedRocks.RemoveAll([ActorName](const FMKITerrainRemovedRock& Record) { return Record.ActorName == ActorName; }) == 0)
	{
		OutReason = TEXT("not_removed");
		return false;
	}
	ApplyAll();
	ForceNetUpdate();
	UE_LOG(LogMKITools, Log, TEXT("Terrain: restored %s"), *ActorName.ToString());
	return true;
}

FString AMKITerrainSubsystem::BridgeDescribeRock(const FString& ActorName) const
{
	const FName Name(*ActorName);
	AActor* Actor = FindLoadedActor(Name);
	const FMKITerrainRemovedRock* Record = RemovedRocks.FindByPredicate([Name](const FMKITerrainRemovedRock& Item) { return Item.ActorName == Name; });
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("actorName"), ActorName);
	Object->SetBoolField(TEXT("loaded"), Actor != nullptr);
	Object->SetBoolField(TEXT("removed"), Record != nullptr);
	if (Record) Object->SetObjectField(TEXT("record"), RecordJson(*Record));
	if (Actor)
	{
		const FString Refusal = FMKITerrainRules::RefusalReason(Actor);
		Object->SetBoolField(TEXT("removable"), Refusal.IsEmpty());
		if (!Refusal.IsEmpty()) Object->SetStringField(TEXT("refusal"), Refusal);
		Object->SetStringField(TEXT("actorClass"), Actor->GetClass()->GetName());
		Object->SetStringField(TEXT("actorPath"), Actor->GetPathName());
		if (const UStaticMeshComponent* Mesh = FMKITerrainRules::RockMesh(Actor)) Object->SetStringField(TEXT("mesh"), Mesh->GetStaticMesh()->GetPathName());
		Object->SetBoolField(TEXT("hidden"), Actor->IsHidden());
		Object->SetBoolField(TEXT("collision"), Actor->GetActorEnableCollision());
	}
	return JsonText(Object);
}

FString AMKITerrainSubsystem::BridgeRemoveRock(const FString& ActorName, bool bConfirm)
{
	if (!HasAuthority()) return ErrorText(TEXT("server_authority_required"), TEXT("Only the host can remove rocks."));
	AActor* Actor = FindLoadedActor(FName(*ActorName));
	if (!Actor) return ErrorText(TEXT("rock_not_loaded"), TEXT("No loaded actor has that exact name; stand near it so its cell streams in."));
	const FString Refusal = FMKITerrainRules::RefusalReason(Actor);
	if (!Refusal.IsEmpty()) return ErrorText(*Refusal, FString::Printf(TEXT("%s cannot be removed in Terrain mode (%s)."), *ActorName, *Refusal));
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("actorName"), ActorName);
	Object->SetBoolField(TEXT("dryRun"), !bConfirm);
	if (!bConfirm) { Object->SetBoolField(TEXT("removable"), true); return JsonText(Object); }
	FString Reason;
	if (!RemoveRock(Actor, TEXT("bridge"), Reason)) return ErrorText(*Reason, FString::Printf(TEXT("%s was not removed (%s)."), *ActorName, *Reason));
	Object->SetBoolField(TEXT("removed"), true);
	Object->SetBoolField(TEXT("verified"), Actor->IsHidden() && !Actor->GetActorEnableCollision());
	Object->SetNumberField(TEXT("hlodInstancesHidden"), HiddenHlodInstances.FilterByPredicate([&](const FHiddenHlodInstance& Hidden) { return Hidden.Rock == Actor->GetFName(); }).Num());
	return JsonText(Object);
}

FString AMKITerrainSubsystem::BridgeRestoreRock(const FString& ActorName, bool bConfirm)
{
	if (!HasAuthority()) return ErrorText(TEXT("server_authority_required"), TEXT("Only the host can restore rocks."));
	const FName Name(*ActorName);
	if (!RemovedRocks.ContainsByPredicate([Name](const FMKITerrainRemovedRock& Record) { return Record.ActorName == Name; }))
	{
		return ErrorText(TEXT("not_removed"), FString::Printf(TEXT("%s is not on the removed-rock list."), *ActorName));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("actorName"), ActorName);
	Object->SetBoolField(TEXT("dryRun"), !bConfirm);
	if (!bConfirm) return JsonText(Object);
	FString Reason;
	if (!RestoreRock(Name, Reason)) return ErrorText(*Reason, FString::Printf(TEXT("%s was not restored (%s)."), *ActorName, *Reason));
	AActor* Actor = FindLoadedActor(Name);
	Object->SetBoolField(TEXT("restored"), true);
	Object->SetBoolField(TEXT("loaded"), Actor != nullptr);
	// An unloaded rock streams back in as the level has it: shown and collidable.
	Object->SetBoolField(TEXT("verified"), !Actor || (!Actor->IsHidden() && Actor->GetActorEnableCollision()));
	return JsonText(Object);
}

FString AMKITerrainSubsystem::BridgeListRemovedRocks() const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	TArray<TSharedPtr<FJsonValue>> Rocks;
	for (const FMKITerrainRemovedRock& Record : RemovedRocks)
	{
		const TSharedRef<FJsonObject> Entry = RecordJson(Record);
		const AActor* Actor = FindLoadedActor(Record.ActorName);
		Entry->SetBoolField(TEXT("loaded"), Actor != nullptr);
		if (Actor) Entry->SetBoolField(TEXT("hidden"), Actor->IsHidden() && !Actor->GetActorEnableCollision());
		Rocks.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Object->SetArrayField(TEXT("rocks"), Rocks);
	Object->SetNumberField(TEXT("hlodInstancesHidden"), HiddenHlodInstances.Num());
	Object->SetNumberField(TEXT("hlodStandInsHidden"), HiddenHlodProxies.Num());
	return JsonText(Object);
}

#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "Subsystem/ModSubsystem.h"
#include "MKITerrainSubsystem.generated.h"

class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMeshComponent;

/** One rock removed in Terrain mode, saved with the game and replicated to every client. */
USTRUCT(BlueprintType)
struct MKITOOLS_API FMKITerrainRemovedRock
{
	GENERATED_BODY()

	/** The level actor's name (StaticMeshActor_8744): stable across loads and unique in the world partition. */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FName ActorName;

	/** Its full path when removed, for diagnostics only (a world partition cell path can change between game builds). */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FString ActorPath;

	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FString MeshPath;

	/** The mesh component's world transform: confirms a streamed-in actor is the same rock, and finds its HLOD instances. */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FTransform MeshTransform;

	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FVector BoundsMin = FVector::ZeroVector;

	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FVector BoundsMax = FVector::ZeroVector;

	/** Who removed it: a player name, or "bridge". */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FString RemovedBy;

	/** UTC, ISO 8601. */
	UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Terrain")
	FString RemovedAt;
};

/**
 * Keeps the rocks removed in Terrain mode removed. The server owns the list; it is saved with the game and replicated, and
 * every machine applies it locally: a removed rock is hidden and loses its collision, never destroyed, so restoring it is
 * exact. World partition cells stream rocks out and back in, so the list is applied again to every level added to the
 * world. From afar the world partition draws HLOD meshes instead of the cells; where an HLOD keeps a removed rock as an
 * instance of the same mesh, that instance is hidden too (best effort: a merged or simplified HLOD mesh cannot be edited,
 * and the rock then still shows from far away).
 */
UCLASS()
class MKITOOLS_API AMKITerrainSubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	AMKITerrainSubsystem();

	/** The world's terrain subsystem, or null before SML spawned it (or on a client before it replicated). */
	static AMKITerrainSubsystem* Get(const UObject* WorldContext);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// IFGSaveInterface
	virtual bool ShouldSave_Implementation() const override { return true; }
	virtual void PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override;

	/** Server only. Removes `Rock` when FMKITerrainRules allows it; otherwise returns false with the reason. */
	bool RemoveRock(AActor* Rock, const FString& RemovedBy, FString& OutReason);
	/** Server only. A player's Terrain mode removal: the player must be within reach of the rock, then RemoveRock. */
	bool RemoveRockForPlayer(AActor* Rock, class APlayerController* Player, FString& OutReason);
	/** Server only. A player's Terrain mode removal of a rock foliage instance (a boulder), found by its location, through the game's own foliage removal: permanent. */
	bool RemoveRockFoliageForPlayer(UHierarchicalInstancedStaticMeshComponent* Foliage, const FVector& Location, class APlayerController* Player, FString& OutReason);
	/** Server only. Brings a removed rock back (shown and collidable again, and its HLOD instances); false when it was not removed. */
	bool RestoreRock(FName ActorName, FString& OutReason);

	const TArray<FMKITerrainRemovedRock>& GetRemovedRocks() const { return RemovedRocks; }
	/** A loaded actor with this exact name, in any level of the world. */
	AActor* FindLoadedActor(FName ActorName) const;
	/** How many HLOD instances this machine hides for removed rocks. */
	int32 GetHiddenHlodInstanceCount() const { return HiddenHlodInstances.Num(); }

	// Called by MKI Bridge through reflection (it does not link against MKI Tools); each returns a JSON object as text.
	UFUNCTION(BlueprintCallable, Category = "MKI Tools|Terrain")
	FString BridgeDescribeRock(const FString& ActorName) const;
	UFUNCTION(BlueprintCallable, Category = "MKI Tools|Terrain")
	FString BridgeRemoveRock(const FString& ActorName, bool bConfirm);
	UFUNCTION(BlueprintCallable, Category = "MKI Tools|Terrain")
	FString BridgeRestoreRock(const FString& ActorName, bool bConfirm);
	UFUNCTION(BlueprintCallable, Category = "MKI Tools|Terrain")
	FString BridgeListRemovedRocks() const;

private:
	UFUNCTION()
	void OnRep_RemovedRocks();

	void OnLevelAdded(ULevel* Level, UWorld* World);
	/** Hides every loaded rock on the list and shows every rock this machine hid that is no longer on it. */
	void ApplyAll();
	void ApplyToLevel(ULevel* Level);
	const FMKITerrainRemovedRock* FindRecord(const AActor* Actor) const;
	static void SetRockRemoved(AActor* Rock, bool bRemoved);
	void HideHlodInstances(ULevel* Level);
	void HideHlodInstances(AActor* HlodActor);
	void OnHlodRegistered(class IWorldPartitionHLODObject* HlodObject);
	void RestoreHlodInstances(FName ActorName);

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_RemovedRocks)
	TArray<FMKITerrainRemovedRock> RemovedRocks;

	/** Rocks this machine has hidden, by name, so a restore that replicates in shows them again. */
	TSet<FName> AppliedNames;

	struct FHiddenHlodInstance
	{
		TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
		int32 Index = INDEX_NONE;
		FTransform Original;
		FName Rock;
	};
	TArray<FHiddenHlodInstance> HiddenHlodInstances;

	/** Merged HLOD meshes that stood in for a removed rock alone, hidden with it. */
	struct FHiddenHlodProxy
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		FName Rock;
	};
	TArray<FHiddenHlodProxy> HiddenHlodProxies;

	FDelegateHandle LevelAddedHandle;
	FDelegateHandle HlodRegisteredHandle;
};

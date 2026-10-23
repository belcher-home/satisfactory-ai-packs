#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"

class AActor;
class AFGCharacterPlayer;
class UFoliageType;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FHitResult;

/**
 * Which world actors Terrain mode may remove: plain level-placed rocks. Everything the game already removes another way
 * (buildables, nobelisk-destructible rocks, resource deposits, chainsawed foliage), resource node meshes, landscape, and
 * anything spawned at runtime is refused. Rock foliage (boulders the game places as foliage instances, which neither the
 * chainsaw nor a nobelisk removes) is removed through the game's own foliage removal.
 */
struct MKITOOLS_API FMKITerrainRules
{
	/** How far from the player's view a rock may be aimed at, and how far from the player the server accepts a removal (cm). */
	static constexpr double MaximumReachCm = 15000.0;
	/** The game's rock meshes: arches, boulders, cliffs, desert rocks. */
	static constexpr const TCHAR* RockMeshFolder = TEXT("/Game/FactoryGame/World/Environment/Rock/");

	/** The rock's mesh component: the root static mesh component of a plain static mesh actor, or null. */
	static UStaticMeshComponent* RockMesh(const AActor* Actor);

	/**
	 * Why `Actor` cannot be removed, as a short code (missing, not_static_mesh_actor, dismantlable, not_level_placed,
	 * no_static_mesh, not_static, reserved_mesh, hidden), or empty when it can. A rock already removed reports `hidden`.
	 */
	static FString RefusalReason(const AActor* Actor);

	/** Whether a foliage mesh is one of the game's rocks (never a bush, tree, or anything pickable). */
	static bool IsRockFoliageMesh(const UStaticMesh* Mesh);
	static bool IsRockFoliageType(const UFoliageType* Type);
	/** The rock foliage instance a trace hit, if it hit one. */
	static bool RockFoliageHit(const FHitResult& Hit, UHierarchicalInstancedStaticMeshComponent*& OutComponent, int32& OutInstance);
	/** The instance of rock foliage in `Component` whose location is nearest `Location`, within `ToleranceCm`, or INDEX_NONE. */
	static int32 FindRockFoliageInstance(const UHierarchicalInstancedStaticMeshComponent* Component, const FVector& Location, double ToleranceCm);

	/** Limestone for a removed boulder: 5 to 50, by the size of its mesh at its scale. */
	static int32 LimestoneForBoulder(const UStaticMesh* Mesh, const FVector& Scale);
	/** Limestone for a removed level rock: 50 to 500, by the size of its bounds. */
	static int32 LimestoneForRock(const FBox& Bounds);
	/** Server only: gives `Amount` limestone to the player; through the game's own item pickup (its popup); what does not fit stays on the ground as an item drop. */
	static void GrantLimestone(AFGCharacterPlayer* Character, int32 Amount, const FVector& At);
};

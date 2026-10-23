#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MKICollectNearby.generated.h"

class AFGCharacterPlayer;
class AFGFoliagePickup;
class UStaticMesh;

/**
 * Collect nearby items (Mods > MKI Tools, the maintainer 2026-10-21): with the option on, the local player picks up, within
 * the collect radius (1x-10x the chainsaw's 5 m):
 * - berries, nuts and bacon agaric (foliage whose pickup gives them; not leaves, wood, flowers or mycelia);
 * - items lying on the ground (AFGItemPickup: crash-site materials, power slugs, somersloops, mercer spheres, creature
 *   remains);
 * - the contents of dismantle and death crates (moved into the inventory as far as it has room; the game removes an empty crate).
 * Foliage and ground items go through the game's own foliage pickup proxy (SetPickupData / SetItemToPickup, then DoPickup),
 * as a press of E does, one every PickupIntervalSeconds, so the game's popup, sound and replication apply. Crates are
 * emptied on the host only.
 */
UCLASS()
class MKITOOLS_API UMKICollectNearbySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** The collect radius at 1x: the chainsaw's own clearance radius (cm). */
	static constexpr float BaseRadiusCm = 500.f;
	/** Seconds between two pickups, and between two scans for what is near. */
	static constexpr float PickupIntervalSeconds = 0.1f;
	static constexpr float ScanIntervalSeconds = 0.5f;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Whether a foliage mesh's pickup gives berries, nuts or bacon agaric. */
	bool IsCollectibleFoliage(UStaticMesh* Mesh);

private:
	void Scan(AFGCharacterPlayer* Character, float RadiusCm);
	bool PickUpNext(AFGCharacterPlayer* Character, AFGFoliagePickup* Proxy, float RadiusCm);
	void EmptyCrates(AFGCharacterPlayer* Character, float RadiusCm);

	TMap<TObjectPtr<UStaticMesh>, bool> CollectibleMeshes;
	TArray<TWeakObjectPtr<class AFGItemPickup>> NearItems;
	TArray<TWeakObjectPtr<class AFGCrate>> NearCrates;
	double NextPickupTime = 0.0;
	double NextScanTime = 0.0;
};

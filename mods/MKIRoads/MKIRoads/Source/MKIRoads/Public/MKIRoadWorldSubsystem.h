#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Buildables/FGBuildable.h"
#include "MKIRoadWorldSubsystem.generated.h"

/**
 * MKI Roads' game-world services: it keeps the vehicle path networks clean of removed paths and nodes (on load, #239, and
 * each frame, #245), and tells a truck depot (#237, #239) when a truck station is built on or removed from its bay.
 */
UCLASS()
class MKIROADS_API UMKIRoadWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	/**
	 * Each frame (#245): the vehicle path networks drop nodes that are gone or going. Live: removing a road piece while a
	 * truck was on autodrive crashed the network rebuild on a node already collected (CleanNetworkIDOnElements); a removed
	 * node still listed in its old network until the rebuild is dropped first.
	 */
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	/** A truck station built on a truck depot's bay (#237): the depot lays its bay path through the station's docking node. */
	UFUNCTION()
	void OnBuildableConstructed(AFGBuildable* Buildable);
	/** A truck station on a truck depot's dock dismantled (#239): the depot lays its dock lane again. */
	UFUNCTION()
	void OnBuildableRemoved(AFGBuildable* Buildable);
};

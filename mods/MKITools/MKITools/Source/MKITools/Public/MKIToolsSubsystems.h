#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "MKIToolsSubsystems.generated.h"

/** Registers MKI Tools' remote call object with SML (which adds it to every game mode, so a client's click reaches the server) and its options (UMKIToolsConfiguration). */
UCLASS()
class MKITOOLS_API UMKIToolsGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
};

/**
 * Registers the terrain subsystem actor with SML's subsystem actor manager once the world's actors are initialized; SML
 * then reuses the one a save loaded, or spawns it (on the server, replicated).
 */
UCLASS()
class MKITOOLS_API UMKIToolsWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void OnActorsInitialized(const UWorld::FActorsInitializedParams& Params);
	FDelegateHandle ActorsInitializedHandle;
};

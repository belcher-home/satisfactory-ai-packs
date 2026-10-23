#include "MKIToolsSubsystems.h"

#include "Configuration/ConfigManager.h"
#include "Engine/GameInstance.h"
#include "MKIToolsConfiguration.h"
#include "MKIPlayerSubsystem.h"
#include "MKIRegrowthSubsystem.h"
#include "MKITerrainSubsystem.h"
#include "MKIToolsRemoteCallObject.h"
#include "Registry/RemoteCallObjectRegistry.h"
#include "Subsystem/SubsystemActorManager.h"

void UMKIToolsGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (URemoteCallObjectRegistry* Registry = Collection.InitializeDependency<URemoteCallObjectRegistry>())
	{
		Registry->RegisterRemoteCallObject(UMKIToolsRemoteCallObject::StaticClass());
	}
	// MKI Tools' options under Mods > MKI Tools, loaded from disk as SML registers them at startup.
	if (UConfigManager* Config = Collection.InitializeDependency<UConfigManager>())
	{
		UMKIToolsConfiguration::BuildOptions();
		Config->RegisterModConfiguration(UMKIToolsConfiguration::StaticClass());
		UMKIToolsConfiguration::AdoptOptions(Config);
	}
}

bool UMKIToolsWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UMKIToolsWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USubsystemActorManager>();
	ActorsInitializedHandle = GetWorld()->OnActorsInitialized.AddUObject(this, &UMKIToolsWorldSubsystem::OnActorsInitialized);
}

void UMKIToolsWorldSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld()) World->OnActorsInitialized.Remove(ActorsInitializedHandle);
	Super::Deinitialize();
}

void UMKIToolsWorldSubsystem::OnActorsInitialized(const UWorld::FActorsInitializedParams& Params)
{
	// SML's manager exists only in worlds that load mod modules (not the main menu).
	if (USubsystemActorManager* Manager = GetWorld()->GetSubsystem<USubsystemActorManager>())
	{
		Manager->RegisterSubsystemActor(AMKITerrainSubsystem::StaticClass());
		Manager->RegisterSubsystemActor(AMKIRegrowthSubsystem::StaticClass());
		Manager->RegisterSubsystemActor(AMKIPlayerSubsystem::StaticClass());
	}
}

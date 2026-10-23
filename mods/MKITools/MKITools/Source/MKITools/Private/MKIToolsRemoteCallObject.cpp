#include "MKIToolsRemoteCallObject.h"

#include "FGPlayerController.h"
#include "MKITerrainRules.h"
#include "MKITerrainSubsystem.h"
#include "MKITools.h"
#include "Net/UnrealNetwork.h"

void UMKIToolsRemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UMKIToolsRemoteCallObject, mForceNetField_UMKIToolsRemoteCallObject);
}

void UMKIToolsRemoteCallObject::Server_RemoveRock_Implementation(AActor* Rock)
{
	AFGPlayerController* Controller = GetOwnerPlayerController();
	AMKITerrainSubsystem* Terrain = AMKITerrainSubsystem::Get(Controller);
	if (!Terrain) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: no terrain subsystem on the server; rock not removed")); return; }
	// The server checks the rules and the player's reach again.
	FString Reason;
	if (!Terrain->RemoveRockForPlayer(Rock, Controller, Reason))
	{
		UE_LOG(LogMKITools, Warning, TEXT("Terrain: %s not removed: %s"), Rock ? *Rock->GetName() : TEXT("(no rock)"), *Reason);
	}
}

void UMKIToolsRemoteCallObject::Server_RemoveRockFoliage_Implementation(UHierarchicalInstancedStaticMeshComponent* Foliage, FVector Location)
{
	AFGPlayerController* Controller = GetOwnerPlayerController();
	AMKITerrainSubsystem* Terrain = AMKITerrainSubsystem::Get(Controller);
	if (!Terrain) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: no terrain subsystem on the server; boulder not removed")); return; }
	FString Reason;
	if (!Terrain->RemoveRockFoliageForPlayer(Foliage, Location, Controller, Reason)) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: boulder at %s not removed: %s"), *Location.ToString(), *Reason); }
}

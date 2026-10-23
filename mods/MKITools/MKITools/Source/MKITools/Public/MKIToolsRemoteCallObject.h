#pragma once

#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "MKIToolsRemoteCallObject.generated.h"

/** A client's Terrain mode click, sent to the server through the player's controller (registered by UMKIToolsGameInstanceSubsystem). */
UCLASS()
class MKITOOLS_API UMKIToolsRemoteCallObject : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Removes a level rock the player aimed at in Terrain mode; the server checks the rules and the player's reach again. */
	UFUNCTION(Server, Reliable)
	void Server_RemoveRock(AActor* Rock);

	/** Removes a rock foliage instance (a boulder) the player aimed at in Terrain mode, found by its location on the server. */
	UFUNCTION(Server, Reliable)
	void Server_RemoveRockFoliage(class UHierarchicalInstancedStaticMeshComponent* Foliage, FVector Location);

	/** The game requires a replicated property on every remote call object. */
	UPROPERTY(Replicated)
	bool mForceNetField_UMKIToolsRemoteCallObject = false;
};

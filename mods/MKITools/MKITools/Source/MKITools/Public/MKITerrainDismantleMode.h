#pragma once

#include "CoreMinimal.h"
#include "FGDismantleModeDescriptor.h"
#include "MKITerrainDismantleMode.generated.h"

/**
 * The Terrain dismantle mode: listed after the vanilla Default and Blueprint modes of the dismantle gun (the module adds it
 * to UFGBuildGunStateDismantle::GetSupportedBuildModes), so the player cycles to it the same way. In it the gun aims only
 * at removable level rocks (FMKITerrainRules) and a click removes the one outlined (FMKITerrainDismantle).
 */
UCLASS()
class MKITOOLS_API UMKITerrainDismantleMode : public UFGDismantleModeDescriptor
{
	GENERATED_BODY()

public:
	UMKITerrainDismantleMode();
};

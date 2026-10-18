#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "MKIRoadsGameWorldModule.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMKIRoads, Log, All);

/**
 * MKI Roads' root game-world module: the SML content registration of UGameWorldModule (the schematic that unlocks the
 * spline road and road pattern recipes). Roads are asphalt only since #159, so it registers no material variants; how the
 * five-material version joined the vanilla foundation material descriptors is recorded in
 * the git history (last at a77661e).
 */
UCLASS(Blueprintable)
class MKIROADS_API UMKIRoadsGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()
};
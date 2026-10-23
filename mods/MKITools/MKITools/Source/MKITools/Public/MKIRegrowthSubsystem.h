#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "Subsystem/ModSubsystem.h"
#include "MKIRegrowthSubsystem.generated.h"

/**
 * Vegetation regrowth (Mods > MKI Tools, the maintainer 2026-10-20: cut vegetation regrows at random, more likely the
 * longer it has been gone). The game keeps every cut plant as a removed location per foliage cell and type
 * (AFGFoliageRemovalSubsystem, saved with the game) and removes it again whenever that cell loads. Regrowing a plant is
 * taking its location off that list: the next time its cell loads, the original plant (type, size, rotation) is there.
 * So a plant regrows only in a cell that is not loaded, never in front of the player.
 *
 * MKI Tools remembers when it first saw each removed location (saved here, against its own play-time clock) and, once a
 * minute on the server, gives each one in an unloaded cell a chance to regrow that rises with its age: a Weibull curve
 * (shape 2) whose mean is the "Regrowth time" option. Rock foliage (boulders) never regrows, nor does a location with a
 * building on or next to it (the game records the foliage it clears for building the same way).
 */
UCLASS()
class MKITOOLS_API AMKIRegrowthSubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	AMKIRegrowthSubsystem();

	/** Seconds between regrowth checks. */
	static constexpr float CheckIntervalSeconds = 60.f;
	/** A location within this distance of a building does not regrow (cm). */
	static constexpr float BuildingClearanceCm = 300.f;
	/** A plant in a loaded cell regrows only farther than this from every player (cm)... */
	static constexpr double HiddenDistanceCm = 20000.0;
	/** ...and, nearer than this, only behind them (cm). */
	static constexpr double InViewDistanceCm = 100000.0;

	/** Whether a player is near `Location` or looking towards it from within view distance. */
	static bool IsInPlayerView(const FVector& Location, const TArray<struct FMinimalViewInfo>& Viewers);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// IFGSaveInterface
	virtual bool ShouldSave_Implementation() const override { return true; }

	/** The probability that a location `AgeSeconds` old regrows within the next `StepSeconds`, for a mean regrowth time. */
	static double RegrowChance(double AgeSeconds, double StepSeconds, double MeanSeconds);

private:
	void Check();
	bool IsNearBuilding(const FVector& Location) const;

	/** Play time on the server's own clock, saved (seconds). */
	UPROPERTY(SaveGame)
	double PlaySeconds = 0.0;

	/** When each removed location was first seen, on PlaySeconds, by a key of its foliage type and location hash. */
	UPROPERTY(SaveGame)
	TMap<uint32, double> FirstSeenRemoved;

	FTimerHandle CheckTimer;
	int32 Checks = 0;
};

#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "Subsystem/ModSubsystem.h"
#include "MKIPlayerSubsystem.generated.h"

/**
 * Extra inventory slots (Mods > MKI Tools, the maintainer 2026-10-21): a "+10 slots" button, up to 500 added, that can only
 * grow, not subtract. The slots added in this save are kept here (saved, server side). The module adds them to the game's
 * own slot count (AFGCharacterPlayer::GetTotalPlayerInventorySlots), and once a second every player's inventory that is
 * smaller than that count is grown to it.
 */
UCLASS()
class MKITOOLS_API AMKIPlayerSubsystem : public AModSubsystem, public IFGSaveInterface
{
	GENERATED_BODY()

public:
	AMKIPlayerSubsystem();

	/** Slots one press of the button adds, and the most a save can have added. */
	static constexpr int32 InventorySlotsPerPress = 10;
	static constexpr int32 MaximumExtraInventorySlots = 500;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// IFGSaveInterface
	virtual bool ShouldSave_Implementation() const override { return true; }

	/** The extra inventory slots added in this save; 0 without the subsystem. */
	static int32 GetExtraInventorySlots(const UObject* WorldContext);
	/** Whether the button can add slots now: on the host, in a game, below the maximum. */
	static bool CanAddInventorySlots(const UObject* WorldContext);
	/** Adds InventorySlotsPerPress slots (up to the maximum) and grows every player's inventory now. */
	static void AddInventorySlots(const UObject* WorldContext);

private:
	static AMKIPlayerSubsystem* Get(const UObject* WorldContext);
	void Apply();

	/** The slots added in this save; only ever grows. */
	UPROPERTY(SaveGame)
	int32 ExtraInventorySlots = 0;

	FTimerHandle ApplyTimer;
};

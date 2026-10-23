#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "MKIAutoPickup.generated.h"

class AFGCharacterPlayer;
class AFGFoliagePickup;
class SWidget;

/**
 * Auto pickup on the keyboard. The game has its own auto pickup in its foliage pickup proxy (AFGFoliagePickup: toggled
 * on, it picks up whatever pickable the player looks at and shows "Auto-Pickup Active" on the HUD), but on keyboard and
 * mouse nothing turns it on: its toggle action is bound only on a gamepad, and mapping E to it did nothing (live,
 * 2026-10-20). MKI Tools times the hold itself: E held for ToggleHoldSeconds where E has nothing to use calls the
 * proxy's own ToggleAutoPickUp, with a progress bar under the crosshair while the hold runs. Everything else is the
 * game's own. A tap of E is untouched.
 */
UCLASS()
class MKITOOLS_API UMKIAutoPickupSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** How long E is held to toggle auto pickup. */
	static constexpr float ToggleHoldSeconds = 1.0f;
	/** A hold shorter than this shows no bar, so a tap of E never flashes it (the game's own toggle prompt waits as long). */
	static constexpr float BarDelaySeconds = 0.3f;
	/** A foliage instance is aimed at when it lies this close to the view ray, within the player's use distance plus FoliageReachSlackCm. */
	static constexpr double FoliageAimToleranceCm = 200.0;
	static constexpr double FoliageReachSlackCm = 100.0;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	/** Whether E is aimed at something it uses, so the hold is the game's own. */
	static bool IsAimedAtUsable(const AFGCharacterPlayer* Character, const AFGFoliagePickup* Proxy);
	/** Whether the player looks at a pickable now: an item pickup, or the proxy's foliage instance on the view ray. */
	static bool IsLookingAtPickable(const AFGCharacterPlayer* Character, const AFGFoliagePickup* Proxy);
	/** While auto pickup is on: the game's own pickup on the pickable looked at, at the game's repeat interval. */
	void PickUpAimed(AFGCharacterPlayer* Character, AFGFoliagePickup* Proxy);
	void UpdateHold(AFGCharacterPlayer* Character, AFGFoliagePickup* Proxy, bool bEHeld, float DeltaTime);
	void EnsureOverlay();
	void RemoveOverlay();

	float HeldSeconds = 0.f;
	/** After a toggle, or a press aimed at something E uses, nothing happens until E is released. */
	bool bWaitForRelease = false;
	/** The overlay reads it: the hold's progress (0-1), negative when no bar shows. */
	float BarProgress = -1.f;
	/** World time of the next auto pickup. */
	double NextPickupTime = 0.0;
	TSharedPtr<SWidget> Overlay;
};

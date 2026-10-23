#include "MKIAutoPickup.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "FGCharacterPlayer.h"
#include "FGFoliagePickup.h"
#include "FGItemPickup.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "MKITools.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SOverlay.h"

bool UMKIAutoPickupSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	// Input and HUD belong to a local player: never on a dedicated server.
	return World && World->IsGameWorld() && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UMKIAutoPickupSubsystem::Deinitialize()
{
	RemoveOverlay();
	Super::Deinitialize();
}

TStatId UMKIAutoPickupSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMKIAutoPickupSubsystem, STATGROUP_Tickables);
}

void UMKIAutoPickupSubsystem::Tick(float DeltaTime)
{
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	AFGCharacterPlayer* Character = Controller ? Cast<AFGCharacterPlayer>(Controller->GetPawn()) : nullptr;
	AFGFoliagePickup* Proxy = Character ? Character->GetFoliagePickupProxy() : nullptr;
	if (!Controller || !Character || !Proxy || !Character->IsLocallyControlled())
	{
		HeldSeconds = 0.f; BarProgress = -1.f;
		return;
	}
	EnsureOverlay();
	// A menu or a machine opened with E shows the cursor: that E is not a hold.
	const bool bEHeld = !Controller->bShowMouseCursor && Controller->IsInputKeyDown(EKeys::E);
	UpdateHold(Character, Proxy, bEHeld, DeltaTime);
	// One state: the game's own flag (the hold flips it; its indicator follows it).
	if (Proxy->IsActive()) PickUpAimed(Character, Proxy);
}

bool UMKIAutoPickupSubsystem::IsLookingAtPickable(const AFGCharacterPlayer* Character, const AFGFoliagePickup* Proxy)
{
	const AFGItemPickup* Item = Cast<AFGItemPickup>(Character->GetBestUsableActor());
	if (Item && !Item->IsPickedUp()) return true;
	// Live (2026-10-20): the proxy keeps its last foliage target after the player looks away or picks it, so the target
	// alone says nothing. A foliage instance counts only while it lies on the player's view ray, within reach.
	const UHierarchicalInstancedStaticMeshComponent* Foliage = Proxy->GetPickupComponent().Get();
	FTransform Instance;
	if (!Foliage || Proxy->GetPickupInstance() == INDEX_NONE || !Foliage->GetInstanceTransform(Proxy->GetPickupInstance(), Instance, true)) return false;
	const AController* Controller = Character->GetController();
	if (!Controller) return false;
	FVector Eye; FRotator View;
	Controller->GetPlayerViewPoint(Eye, View);
	const FVector ToInstance = Instance.GetLocation() - Eye;
	const double Along = FVector::DotProduct(ToInstance, View.Vector());
	if (Along < 0.0 || Along > Character->GetUseDistance() + FoliageReachSlackCm) return false;
	return (ToInstance - View.Vector() * Along).Size() <= FoliageAimToleranceCm;
}

bool UMKIAutoPickupSubsystem::IsAimedAtUsable(const AFGCharacterPlayer* Character, const AFGFoliagePickup* Proxy)
{
	const AActor* Aimed = Character->GetBestUsableActor();
	// The game's proxy stands in for foliage and stays the usable actor while auto pickup is on, so it is no interactable
	// itself (live: it blocked turning auto pickup off, and back on after a manual pickup).
	const bool bInteractable = Aimed && Aimed != Proxy && !Aimed->IsA<AFGItemPickup>();
	// On, only a real interactable (a machine, container, vehicle, door) keeps E the game's own: pickables are picked anyway.
	if (Proxy->IsActive()) return bInteractable;
	// Off, the maintainer's rule: the hold toggles only where E has nothing to use, pickups and foliage included.
	return bInteractable || IsLookingAtPickable(Character, Proxy);
}

void UMKIAutoPickupSubsystem::PickUpAimed(AFGCharacterPlayer* Character, AFGFoliagePickup* Proxy)
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now < NextPickupTime || !IsLookingAtPickable(Character, Proxy)) return;
	// Live (2026-10-20): with the game's auto pickup flag on, the game itself picked nothing (it picks only while E is
	// held), so MKI Tools runs the proxy's own pickup, as a press of E: its rules, inventory checks, animation and server
	// call. Only on a pickable the player looks at (live: on a stale target the pickup animation played every half second).
	if (AFGItemPickup* Item = Cast<AFGItemPickup>(Character->GetBestUsableActor())) Proxy->SetItemToPickup(Item);
	Proxy->DoPickup();
	NextPickupTime = Now + FMath::Max(Proxy->mPickupRepeatInterval, 0.1f);
}

void UMKIAutoPickupSubsystem::UpdateHold(AFGCharacterPlayer* Character, AFGFoliagePickup* Proxy, bool bEHeld, float DeltaTime)
{
	if (!bEHeld) { HeldSeconds = 0.f; bWaitForRelease = false; BarProgress = -1.f; return; }
	if (bWaitForRelease) return;
	if (HeldSeconds <= 0.f && IsAimedAtUsable(Character, Proxy)) { bWaitForRelease = true; return; }
	HeldSeconds += DeltaTime;
	BarProgress = HeldSeconds >= BarDelaySeconds ? FMath::Clamp((HeldSeconds - BarDelaySeconds) / (ToggleHoldSeconds - BarDelaySeconds), 0.f, 1.f) : -1.f;
	if (HeldSeconds < ToggleHoldSeconds) return;
	bWaitForRelease = true;
	BarProgress = -1.f;
	// The game's own toggle: it does the picking (only what the player looks at, its own animation and rules) and shows
	// its own "Auto-Pickup Active" indicator. Live (2026-10-20): MKI Tools picking on top of it played the pickup
	// animation every half second with nothing to pick, so it no longer picks anything itself.
	const bool bWasActive = Proxy->IsActive();
	Proxy->ToggleAutoPickUp();
	UE_LOG(LogMKITools, Log, TEXT("Auto pickup: %s"), Proxy->IsActive() ? TEXT("on") : TEXT("off"));
	if (Proxy->IsActive() == bWasActive) UE_LOG(LogMKITools, Warning, TEXT("Auto pickup: the game's toggle did not change the state"));
}

void UMKIAutoPickupSubsystem::EnsureOverlay()
{
	if (Overlay.IsValid()) return;
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	if (!Viewport) return;
	TWeakObjectPtr<UMKIAutoPickupSubsystem> Weak(this);
	// The hold's progress bar, under the crosshair (the game shows none for this hold on keyboard and mouse).
	Overlay = SNew(SOverlay).Visibility(EVisibility::HitTestInvisible)
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(FMargin(0.f, 160.f, 0.f, 0.f))
		[
			SNew(SBox).WidthOverride(220.f).HeightOverride(8.f)
			.Visibility_Lambda([Weak]() { return Weak.IsValid() && Weak->BarProgress >= 0.f ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			[
				SNew(SProgressBar).FillColorAndOpacity(FLinearColor::White)
				.Percent_Lambda([Weak]() { return TOptional<float>(Weak.IsValid() ? FMath::Max(Weak->BarProgress, 0.f) : 0.f); })
			]
		];
	Viewport->AddViewportWidgetContent(Overlay.ToSharedRef(), 5);
}

void UMKIAutoPickupSubsystem::RemoveOverlay()
{
	if (Overlay.IsValid())
	{
		if (UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr) Viewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
		Overlay.Reset();
	}
}

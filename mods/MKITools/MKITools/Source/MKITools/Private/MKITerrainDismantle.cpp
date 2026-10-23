#include "MKITerrainDismantle.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunDismantle.h"
#include "FGCharacterPlayer.h"
#include "FGOutlineComponent.h"
#include "FGPlayerController.h"
#include "InputCoreTypes.h"
#include "MKITerrainDismantleMode.h"
#include "MKITerrainRules.h"
#include "MKITerrainSubsystem.h"
#include "MKITools.h"
#include "MKIToolsConfiguration.h"
#include "MKIToolsRemoteCallObject.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MKITools"

namespace
{
	/** How long the fire button is held on a rock before it is removed, like the vanilla hold to dismantle. */
	constexpr float HoldToRemoveSeconds = 0.5f;

	struct FTerrainAim
	{
		/** A level rock (an actor), or */
		TWeakObjectPtr<AActor> Rock;
		TWeakObjectPtr<UStaticMeshComponent> Mesh;
		/** a rock foliage instance (a boulder), outlined through a mesh of its own (Proxy) at the instance. */
		TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Foliage;
		int32 Instance = INDEX_NONE;
		FVector InstanceLocation = FVector::ZeroVector;
		TWeakObjectPtr<UStaticMeshComponent> Proxy;
		TWeakObjectPtr<UFGOutlineComponent> Outline;
		/** Fire is held on this rock (the HUD shows HoldProgress as its progress bar). */
		bool bHolding = false;
		/** Seconds the fire button has been held on this rock. */
		float HeldSeconds = 0.f;
		/** After a removal (or a press that began off any rock), nothing more happens until the button is released. */
		bool bWaitForRelease = false;

		bool HasTarget() const { return Rock.IsValid() || (Foliage.IsValid() && Instance != INDEX_NONE); }
	};
	/** The rock each local dismantle state aims at in Terrain mode. */
	TMap<TWeakObjectPtr<UFGBuildGunStateDismantle>, FTerrainAim> Aims;

	/** "Boulder: removal is permanent", under the crosshair while a boulder is aimed at (the game's removal cannot be undone). */
	TSharedPtr<SWidget> PermanentNote;
	bool bShowPermanentNote = false;

	void EnsurePermanentNote(UWorld* World)
	{
		if (PermanentNote.IsValid() || !World) return;
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport) return;
		PermanentNote = SNew(SOverlay).Visibility(EVisibility::HitTestInvisible)
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(FMargin(0.f, 200.f, 0.f, 0.f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BoulderPermanent", "Boulder: removal is permanent"))
				.ColorAndOpacity(FLinearColor(1.f, 0.62f, 0.2f))
				.ShadowOffset(FVector2D(1.f, 1.f))
				.Visibility_Lambda([]() { return bShowPermanentNote ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
			];
		Viewport->AddViewportWidgetContent(PermanentNote.ToSharedRef(), 5);
	}

	AFGCharacterPlayer* LocalCharacter(const UFGBuildGunStateDismantle* State)
	{
		const AFGBuildGun* Gun = State ? State->GetBuildGun() : nullptr;
		AFGCharacterPlayer* Character = Gun ? Gun->GetInstigatorCharacter() : nullptr;
		return Character && Character->IsLocallyControlled() ? Character : nullptr;
	}

	void HideOutline(FTerrainAim& Aim)
	{
		UFGOutlineComponent* Outline = Aim.Outline.Get();
		if (AActor* Rock = Aim.Rock.Get())
		{
			if (Outline)
			{
				if (UStaticMeshComponent* Mesh = Aim.Mesh.Get()) Outline->HideOutlineForStaticMeshComponent(Rock, Mesh);
				else Outline->HideOutline(Rock);
			}
		}
		if (UStaticMeshComponent* Proxy = Aim.Proxy.Get())
		{
			if (Outline) Outline->HideOutlineForStaticMeshComponent(Proxy->GetOwner(), Proxy);
			Proxy->DestroyComponent();
		}
		bShowPermanentNote = false;
		const bool bWaitForRelease = Aim.bWaitForRelease;
		Aim = FTerrainAim();
		Aim.bWaitForRelease = bWaitForRelease;
	}

	/** Outlines a boulder: the game's outline works on a mesh component, so a mesh of its own stands at the instance. */
	void OutlineBoulder(AFGCharacterPlayer* Character, FTerrainAim& Aim)
	{
		UHierarchicalInstancedStaticMeshComponent* Foliage = Aim.Foliage.Get();
		FTransform Instance;
		if (!Foliage || !Foliage->GetInstanceTransform(Aim.Instance, Instance, true)) return;
		Aim.InstanceLocation = Instance.GetLocation();
		UStaticMeshComponent* Proxy = NewObject<UStaticMeshComponent>(Character, NAME_None, RF_Transient);
		Proxy->SetStaticMesh(Foliage->GetStaticMesh());
		Proxy->SetMobility(EComponentMobility::Movable);
		Proxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Proxy->SetCastShadow(false);
		Proxy->SetRenderInMainPass(false);
		Proxy->RegisterComponent();
		Proxy->SetWorldTransform(Instance);
		Aim.Proxy = Proxy;
		if (UFGOutlineComponent* Outline = Aim.Outline.Get()) Outline->ShowOutlineForStaticMeshComponent(Character, Proxy, EOutlineColor::OC_DISMANTLE);
		bShowPermanentNote = true;
	}

	/** Ends the hold (and with it the progress bar). */
	void StopHold(UFGBuildGunStateDismantle* State, FTerrainAim& Aim)
	{
		Aim.bHolding = false;
		Aim.HeldSeconds = 0.f;
	}

	/** The fire button, held: the left mouse button or a gamepad's right trigger, while no menu has the cursor. */
	bool FireHeld(const APlayerController* Controller)
	{
		return Controller && !Controller->bShowMouseCursor && (Controller->IsInputKeyDown(EKeys::LeftMouseButton) || Controller->IsInputKeyDown(EKeys::Gamepad_RightTrigger));
	}

	/** Removes the aimed rock or boulder: directly on the host (and in single-player), through the remote call object on a client. */
	void RequestRemoval(UFGBuildGunStateDismantle* State, FTerrainAim& Aim)
	{
		AFGCharacterPlayer* Character = LocalCharacter(State);
		AFGPlayerController* Controller = Character ? Cast<AFGPlayerController>(Character->GetController()) : nullptr;
		if (!Controller || !Aim.HasTarget()) return;
		AActor* Rock = Aim.Rock.Get();
		UHierarchicalInstancedStaticMeshComponent* Foliage = Aim.Foliage.Get();
		const FString Name = Rock ? Rock->GetName() : FString::Printf(TEXT("boulder %s"), *Aim.InstanceLocation.ToString());
		if (Rock)
		{
			const FString Refusal = FMKITerrainRules::RefusalReason(Rock);
			if (!Refusal.IsEmpty()) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: %s cannot be removed (%s)"), *Name, *Refusal); return; }
		}
		Aim.bWaitForRelease = true;
		if (Controller->HasAuthority())
		{
			AMKITerrainSubsystem* Terrain = AMKITerrainSubsystem::Get(Controller);
			FString Reason;
			const bool bRemoved = Terrain && (Rock ? Terrain->RemoveRockForPlayer(Rock, Controller, Reason) : Terrain->RemoveRockFoliageForPlayer(Foliage, Aim.InstanceLocation, Controller, Reason));
			if (!Terrain) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: no terrain subsystem in this world; %s not removed"), *Name); }
			else if (!bRemoved) { UE_LOG(LogMKITools, Warning, TEXT("Terrain: %s not removed: %s"), *Name, *Reason); }
		}
		else if (UMKIToolsRemoteCallObject* Remote = Controller->GetRemoteCallObjectOfClass<UMKIToolsRemoteCallObject>())
		{
			UE_LOG(LogMKITools, Log, TEXT("Terrain: asking the server to remove %s"), *Name);
			if (Rock) Remote->Server_RemoveRock(Rock);
			else Remote->Server_RemoveRockFoliage(Foliage, Aim.InstanceLocation);
		}
		else
		{
			UE_LOG(LogMKITools, Warning, TEXT("Terrain: the server has no MKI Tools remote call object (is MKI Tools installed on the host?); %s not removed"), *Name);
		}
		HideOutline(Aim);
	}
}

bool FMKITerrainDismantle::IsTerrainMode(const UFGBuildGunStateDismantle* State)
{
	return State && State->IsCurrentBuildGunMode(UMKITerrainDismantleMode::StaticClass());
}

void FMKITerrainDismantle::Tick(UFGBuildGunStateDismantle* State, float DeltaTime)
{
	// Nothing vanilla stays aimed at or queued while Terrain mode has the gun.
	if (State->mCurrentlySelectedActor || State->mPendingDismantleActors.Num() > 0)
	{
		State->ClearPendingSelectedActors();
		State->SetAimedAtActor(nullptr);
	}
	AFGCharacterPlayer* Character = LocalCharacter(State);
	FTerrainAim& Aim = Aims.FindOrAdd(State);
	APlayerController* Controller = Character ? Cast<APlayerController>(Character->GetController()) : nullptr;
	if (!Controller) { StopHold(State, Aim); HideOutline(Aim); return; }
	EnsurePermanentNote(Character->GetWorld());
	FVector ViewLocation; FRotator ViewRotation;
	Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(MKITerrainAim), true, Character);
	Query.AddIgnoredActor(State->GetBuildGun());
	FHitResult Hit;
	AActor* TargetRock = nullptr;
	UHierarchicalInstancedStaticMeshComponent* TargetFoliage = nullptr;
	int32 TargetInstance = INDEX_NONE;
	if (Character->GetWorld()->LineTraceSingleByChannel(Hit, ViewLocation, ViewLocation + ViewRotation.Vector() * FMKITerrainRules::MaximumReachCm, ECC_Visibility, Query))
	{
		AActor* HitActor = Hit.GetActor();
		if (HitActor && FMKITerrainRules::RefusalReason(HitActor).IsEmpty()) TargetRock = HitActor;
		// Boulders the game places as foliage. The maintainer (2026-10-20): they are the chainsaw's ("Chainsaw cuts boulders",
		// with the game's own pickup popup); Terrain mode takes them only while that option is off, so none is ever left
		// without a way to remove it.
		else if (!UMKIToolsConfiguration::GetChainsawCutsBoulders(Character)) FMKITerrainRules::RockFoliageHit(Hit, TargetFoliage, TargetInstance);
	}
	if (Aim.Rock.Get() != TargetRock || Aim.Foliage.Get() != TargetFoliage || Aim.Instance != TargetInstance)
	{
		StopHold(State, Aim);
		HideOutline(Aim);
		Aim.Outline = Character->GetOutline();
		if (TargetRock)
		{
			Aim.Rock = TargetRock;
			Aim.Mesh = FMKITerrainRules::RockMesh(TargetRock);
			if (UFGOutlineComponent* Outline = Aim.Outline.Get()) Outline->ShowOutlineForStaticMeshComponent(TargetRock, Aim.Mesh.Get(), EOutlineColor::OC_DISMANTLE);
		}
		else if (TargetFoliage)
		{
			Aim.Foliage = TargetFoliage;
			Aim.Instance = TargetInstance;
			OutlineBoulder(Character, Aim);
		}
	}
	// Hold to remove: the vanilla gun calls PrimaryFire only after its own hold delay, which needs a vanilla target, so
	// Terrain mode reads the fire button and times the hold itself. Live (2026-10-19): starting the state's own delay
	// instead did nothing (it does not start without a vanilla target), so removal depends on this timer alone; the HUD's
	// progress bar only reads it (HoldProgress, through the delay-percentage hooks).
	if (!FireHeld(Controller)) { StopHold(State, Aim); Aim.bWaitForRelease = false; return; }
	if (Aim.bWaitForRelease) return;
	if (!Aim.HasTarget()) { StopHold(State, Aim); return; }
	Aim.bHolding = true;
	Aim.HeldSeconds += DeltaTime;
	if (Aim.HeldSeconds >= HoldToRemoveSeconds)
	{
		StopHold(State, Aim);
		RequestRemoval(State, Aim);
	}
}

float FMKITerrainDismantle::HoldProgress(const UFGBuildGunState* State)
{
	const FTerrainAim* Aim = Aims.Find(const_cast<UFGBuildGunStateDismantle*>(Cast<UFGBuildGunStateDismantle>(State)));
	return Aim && Aim->bHolding ? FMath::Clamp(Aim->HeldSeconds / HoldToRemoveSeconds, 0.f, 1.f) : -1.f;
}

void FMKITerrainDismantle::PrimaryFire(UFGBuildGunStateDismantle* State)
{
	// A gun that does call PrimaryFire in Terrain mode removes at once, unless the hold already did.
	FTerrainAim* Aim = Aims.Find(State);
	if (Aim && !Aim->bWaitForRelease) RequestRemoval(State, *Aim);
}

void FMKITerrainDismantle::Clear(UFGBuildGunStateDismantle* State)
{
	if (FTerrainAim* Aim = Aims.Find(State)) { StopHold(State, *Aim); HideOutline(*Aim); }
	Aims.Remove(State);
	// Weak keys of states that are gone.
	for (auto It = Aims.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
}

#undef LOCTEXT_NAMESPACE

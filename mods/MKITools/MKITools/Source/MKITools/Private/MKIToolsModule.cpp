#include "MKITools.h"

#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunDismantle.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Equipment/FGChainsaw.h"
#include "Equipment/FGResourceMiner.h"
#include "FGCharacterMovementComponent.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "MKIPlayerSubsystem.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FGFoliageIdentifier.h"
#include "FGWorkBench.h"
#include "Hologram/FGConveyorLiftHologram.h"
#include "FoliageType.h"
#include "GameFramework/PlayerController.h"
#include "MKITerrainDismantle.h"
#include "MKITerrainDismantleMode.h"
#include "MKITerrainRules.h"
#include "MKIToolsConfiguration.h"
#include "Modules/ModuleManager.h"
#if !WITH_EDITOR
#include "Patching/NativeHookManager.h"
#endif

DEFINE_LOG_CATEGORY(LogMKITools);

/** Foliage instances by component (a type alias: a comma inside a hook macro's lambda would split the macro's arguments). */
using FFoliageAround = TMap<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>;

#if !WITH_EDITOR
/** The movement speed multiplier for a player's movement component; 1 for anything else (creatures, vehicles). */
static float MovementSpeedFor(const UCharacterMovementComponent* Movement)
{
	const AActor* Owner = Movement ? Movement->GetOwner() : nullptr;
	if (!Owner || !Owner->IsA<AFGCharacterPlayer>()) return 1.f;
	// Live (2026-10-22): crouching at speed starts the game's slide, which carried the multiplied speed and threw the
	// player half across the map. Sliding and the game's own movement modes (ladders, hypertubes, ziplines, parachute)
	// keep the game's speed.
	const UFGCharacterMovementComponent* Player = Cast<UFGCharacterMovementComponent>(Movement);
	if (Movement->MovementMode == MOVE_Custom || (Player && Player->IsSliding())) return 1.f;
	return UMKIToolsConfiguration::GetMovementSpeed(Owner);
}
#endif

/**
 * MKI Tools' runtime module. In game builds it adds the Terrain mode to the vanilla dismantle gun, after Default and
 * Blueprint, and runs the dismantle state through FMKITerrainDismantle while that mode is selected. The terrain subsystem
 * and the remote call object are registered by UMKIToolsWorldSubsystem and UMKIToolsGameInstanceSubsystem.
 */
class FMKIToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if !WITH_EDITOR
		// SML hooks are game-only (as SML's own).
		SUBSCRIBE_UOBJECT_METHOD_AFTER(UFGBuildGunStateDismantle, GetSupportedBuildModes_Implementation, [](const UFGBuildGunStateDismantle* Self, TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& OutBuildModes)
		{
			OutBuildModes.AddUnique(UMKITerrainDismantleMode::StaticClass());
		});
		SUBSCRIBE_UOBJECT_METHOD(UFGBuildGunStateDismantle, TickState_Implementation, [](auto& Scope, UFGBuildGunStateDismantle* Self, float DeltaTime)
		{
			if (!FMKITerrainDismantle::IsTerrainMode(Self)) return;
			FMKITerrainDismantle::Tick(Self, DeltaTime);
			Scope.Cancel();
		});
		SUBSCRIBE_UOBJECT_METHOD(UFGBuildGunStateDismantle, PrimaryFire_Implementation, [](auto& Scope, UFGBuildGunStateDismantle* Self)
		{
			if (!FMKITerrainDismantle::IsTerrainMode(Self)) return;
			FMKITerrainDismantle::PrimaryFire(Self);
			Scope.Cancel();
		});
		// The HUD's hold progress bar reads the gun's delay percentage; during a Terrain hold it reads the Terrain timer
		// instead. Only the gun's UI getter: live (2026-10-19), also overriding the state's own getter, which the gun's
		// logic reads, made the gun beep in rapid succession through the hold.
		SUBSCRIBE_METHOD(AFGBuildGun::GetCurrentBuildGunDelayPercentage, [](auto& Scope, const AFGBuildGun* Self)
		{
			const float Progress = FMKITerrainDismantle::HoldProgress(Self ? Self->GetCurrentState() : nullptr);
			if (Progress >= 0.f) Scope.Override(Progress);
		});
		// Hand crafting multiplier (Mods > MKI Tools, the work per tick, not the tick rate): each Craft Bench production step runs the configured number of times, so a
		// craft takes that much less time. Live (2026-10-20, the maintainer): one step at N times the length still made at
		// most one item per step, so short recipes did not get faster; N steps can each finish an item. 1 (the default)
		// leaves the game's own step untouched. The bench's own timings are logged once.
		SUBSCRIBE_METHOD(UFGWorkBench::Produce, [](auto& Scope, UFGWorkBench* Self, float DeltaTime)
		{
			const int32 Speed = FMath::RoundToInt(UMKIToolsConfiguration::GetHandCraftingSpeed(Self));
			if (Speed <= 1 || !Self) return;
			static bool bLogged = false;
			if (!bLogged)
			{
				bLogged = true;
				UE_LOG(LogMKITools, Log, TEXT("Crafting: step %.3f s, hold cycle %.3f s, recipe %.3f s, speed %.2f, cooldown delay %.3f s"), DeltaTime, Self->mHoldProduceTime, Self->mRecipeDuration, Self->mManufacturingSpeed, Self->mCooldownDelay);
			}
			for (int32 Step = 0; Step < Speed; ++Step) Scope(Self, DeltaTime);
		});
		// Hand mining multiplier (Mods > MKI Tools, the maintainer 2026-10-21; the ore per swing, not the swing rate): mining a resource node by hand gives ore the
		// configured times as fast. Live: a shorter mining cycle (mMiningCycleDuration, 1.2 s) changed nothing, the swing
		// animation paces the cycles, so each completed cycle gives its reward that many times instead. Server side.
		SUBSCRIBE_UOBJECT_METHOD(AFGResourceMiner, Server_GiveMiningRewards_Implementation, [](auto& Scope, AFGResourceMiner* Self)
		{
			const int32 Speed = Self ? FMath::RoundToInt(UMKIToolsConfiguration::GetHandMiningSpeed(Self)) : 1;
			if (Speed <= 1) return;
			for (int32 Reward = 0; Reward < Speed; ++Reward) Scope(Self);
			static bool bLogged = false;
			if (!bLogged) { bLogged = true; UE_LOG(LogMKITools, Log, TEXT("Mining: a hand mining cycle gave its reward %d times"), Speed); }
		});
		// Health regeneration speed (Mods > MKI Tools, the maintainer 2026-10-21): the game's regeneration step runs as if
		// that many times as much time had passed; its wait after damage and its own health curve (where it stops) stay.
		SUBSCRIBE_METHOD(AFGCharacterPlayer::TickHealthGeneration, [](auto& Scope, AFGCharacterPlayer* Self, float Delta)
		{
			const float Speed = Self ? UMKIToolsConfiguration::GetHealthRegenerationSpeed(Self) : 1.f;
			if (Speed > 1.f) Scope(Self, Delta * Speed);
		});
		// Extra inventory slots (Mods > MKI Tools, the maintainer 2026-10-21: can only grow): the game's own slot count
		// plus the slots granted in this save (AMKIPlayerSubsystem, which grows each inventory to it).
		SUBSCRIBE_METHOD(AFGCharacterPlayer::GetTotalPlayerInventorySlots, [](auto& Scope, const AFGCharacterPlayer* Self)
		{
			const int32 Extra = Self ? AMKIPlayerSubsystem::GetExtraInventorySlots(Self) : 0;
			if (Extra > 0) Scope.Override(Scope(Self) + Extra);
		});
		// Never shrink a player's own inventory: before the save's extra slots are known (or with the mod's slots counted
		// out), the game could otherwise size it back down and lose what is in the extra slots. The game itself only ever
		// grows the player inventory.
		SUBSCRIBE_METHOD(UFGInventoryComponent::Resize, [](auto& Scope, UFGInventoryComponent* Self, int32 NewSize)
		{
			const AFGCharacterPlayer* Owner = Self ? Cast<AFGCharacterPlayer>(Self->GetOwner()) : nullptr;
			if (!Owner || Owner->GetInventory() != Self || NewSize >= Self->GetSizeLinear()) return;
			UE_LOG(LogMKITools, Log, TEXT("Inventory: kept %s at %d slots (asked for %d)"), *Owner->GetName(), Self->GetSizeLinear(), NewSize);
			Scope.Cancel();
		});
		// Movement speed (Mods > MKI Tools, the maintainer 2026-10-21: how fast the player runs or moves in the air, 1-10):
		// the player's top speed in every movement mode (walk, sprint, crouch, swim, falling) and its acceleration (so the
		// higher speed is reached as quickly, and air control scales with it) are multiplied. Players only; creatures keep
		// the game's own.
		SUBSCRIBE_METHOD_VIRTUAL(UFGCharacterMovementComponent::GetMaxSpeed, GetMutableDefault<UFGCharacterMovementComponent>(), [](auto& Scope, const UFGCharacterMovementComponent* Self)
		{
			const float Speed = MovementSpeedFor(Self);
			if (Speed > 1.f) { Scope.Override(Scope(Self) * Speed); return; }
			// A slide entered at a multiplied speed is held to the game's own top speed for it, not carried on.
			if (Self && Self->IsSliding() && UMKIToolsConfiguration::GetMovementSpeed(Self->GetOwner()) > 1.f)
			{
				const float GameMax = Scope(Self);
				UFGCharacterMovementComponent* Mutable = const_cast<UFGCharacterMovementComponent*>(Self);
				if (GameMax > 0.f && Mutable->Velocity.Size2D() > GameMax)
				{
					const double Z = Mutable->Velocity.Z;
					Mutable->Velocity = Mutable->Velocity.GetClampedToMaxSize2D(GameMax);
					Mutable->Velocity.Z = Z;
				}
				Scope.Override(GameMax);
			}
		});
		SUBSCRIBE_METHOD_VIRTUAL(UCharacterMovementComponent::GetMaxAcceleration, GetMutableDefault<UFGCharacterMovementComponent>(), [](auto& Scope, const UCharacterMovementComponent* Self)
		{
			const float Speed = MovementSpeedFor(Self);
			if (Speed > 1.f) Scope.Override(Scope(Self) * Speed);
		});
		// Short conveyor lifts (Mods > MKI Tools, default on; the maintainer 2026-10-22): the game builds a lift shorter than
		// its usual shortest (mMinimumHeight, 4 m) only when it snaps between two connections stacked above each other
		// (down to mMinimumHeightWithVerticalConnection, 1.5 m). With the option on, a lift's shortest everywhere is the
		// first whole step (mStepHeight, 1 m) at or above that lower height: 2 m, the vanilla short lift (live: the one the
		// maintainer stood by measured 200 cm; 1.5 m was not the game's short lift and put every height off the 1 m grid).
		// It still extends in the game's own steps. Set before each update of the lift's top, where the game clamps it.
		SUBSCRIBE_METHOD(AFGConveyorLiftHologram::UpdateTopTransform, [](auto& Scope, AFGConveyorLiftHologram* Self, const FHitResult& HitResult, FRotator Rotation)
		{
			if (!Self || !UMKIToolsConfiguration::GetShortConveyorLifts(Self) || Self->mMinimumHeightWithVerticalConnection <= 0.f) return;
			const float Step = Self->mStepHeight > 0.f ? Self->mStepHeight : 100.f;
			const float Shortest = FMath::CeilToFloat(Self->mMinimumHeightWithVerticalConnection / Step) * Step;
			if (Self->mMinimumHeight > Shortest)
			{
				UE_LOG(LogMKITools, Log, TEXT("Lifts: shortest %.0f cm -> %.0f cm (step %.0f cm)"), Self->mMinimumHeight, Shortest, Step);
				Self->mMinimumHeight = Shortest;
			}
		});
		// Chainsaw cuts boulders (Mods > MKI Tools, default on): the game asks whether a foliage type is removable before it
		// is sawn (and before anything else clears it); the game's rock foliage says yes. Bushes, trees and every other type
		// keep the game's own answer.
		SUBSCRIBE_METHOD(AFGFoliageRemovalSubsystem::IsRemovable, [](auto& Scope, const UFoliageType* Type)
		{
			if (FMKITerrainRules::IsRockFoliageType(Type) && UMKIToolsConfiguration::GetChainsawCutsBoulders(GEngine ? GEngine->GetCurrentPlayWorld() : nullptr)) Scope.Override(true);
		});
		// Live (2026-10-20): the chainsaw did not even aim at boulders. It picks what to saw by foliage identifier (the
		// Chainsawable tag), not by IsRemovable; rock foliage carries that tag too while the option is on (not the
		// Christmas tree subclass).
		SUBSCRIBE_METHOD(AFGFoliageRemovalSubsystem::HasIdentifier, [](auto& Scope, const UHierarchicalInstancedStaticMeshComponent* Component, TSubclassOf<UFGFoliageIdentifier> Identifier)
		{
			if (Identifier == UFGFoliageIdentifier_Chainsawable::StaticClass() && Component && FMKITerrainRules::IsRockFoliageMesh(Component->GetStaticMesh())
				&& UMKIToolsConfiguration::GetChainsawCutsBoulders(GEngine ? GEngine->GetCurrentPlayWorld() : nullptr)) Scope.Override(true);
		});
		// Chainsaw clearance radius (Mods > MKI Tools, the maintainer 2026-10-20): the area around a sawn tree the chainsaw
		// also clears, as a multiple (1x-10x) of the game's own (its class default, 5 m, logged once). Applied each time the
		// chainsaw is equipped.
		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGChainsaw, Equip, [](AFGChainsaw* Self, AFGCharacterPlayer* Character)
		{
			if (!Self) return;
			const AFGChainsaw* GameDefaults = Self->GetClass()->GetDefaultObject<AFGChainsaw>();
			const float GameRadiusCm = GameDefaults ? GameDefaults->mCollateralPickupRadius : Self->mCollateralPickupRadius;
			Self->mCollateralPickupRadius = GameRadiusCm * UMKIToolsConfiguration::GetChainsawClearanceMultiplier(Self);
			static bool bLogged = false;
			if (!bLogged) { bLogged = true; UE_LOG(LogMKITools, Log, TEXT("Chainsaw: the game's clearance radius is %.0f cm"), GameRadiusCm); }
			UE_LOG(LogMKITools, Log, TEXT("Chainsaw: clearance radius %.0f cm"), Self->mCollateralPickupRadius);
		});
		// A boulder sawn down gives limestone by its size (the maintainer, 2026-10-20), once: only when the instance is
		// gone after the game's own removal ran. Server side, where the chainsaw removes foliage.
		SUBSCRIBE_METHOD(AFGChainsaw::RemoveFoliageInstance, [](auto& Scope, AFGChainsaw* Self, const FFoliageInstanceStableId& StableId, const FVector& EffectLocation, const FTransform& InstanceTransform)
		{
			AFGCharacterPlayer* Character = Self ? Self->GetInstigatorCharacter() : nullptr;
			AFGFoliageRemovalSubsystem* Removal = Self ? AFGFoliageRemovalSubsystem::Get(Self->GetWorld()) : nullptr;
			if (!Character || !Character->HasAuthority() || !Removal) return;
			const FVector Location = InstanceTransform.GetLocation();
			FFoliageAround Near;
			Removal->GetFoliageWithinRadius(Location, 50.f, Near);
			UHierarchicalInstancedStaticMeshComponent* Boulder = nullptr;
			for (const auto& Entry : Near)
			{
				if (FMKITerrainRules::FindRockFoliageInstance(Entry.Key, Location, 50.0) != INDEX_NONE) { Boulder = Entry.Key; break; }
			}
			if (!Boulder) return;
			const UStaticMesh* Mesh = Boulder->GetStaticMesh();
			Scope(Self, StableId, EffectLocation, InstanceTransform);
			if (FMKITerrainRules::FindRockFoliageInstance(Boulder, Location, 50.0) != INDEX_NONE) return;
			FMKITerrainRules::GrantLimestone(Character, FMKITerrainRules::LimestoneForBoulder(Mesh, InstanceTransform.GetScale3D()), Character->GetActorLocation());
		});
		// Live (2026-10-20, the maintainer): the chainsaw's area clearing left boulders and short grass standing; the game
		// clears only its pickupable foliage around a sawn object. After the game's own clearing (server side), the rest of
		// the foliage in the clearance radius that buildings may clear goes too, and with "Chainsaw cuts boulders" on, the
		// boulders, for their limestone together. Foliage types it keeps are logged once each; the landscape's own grass is
		// not foliage the game's removal reaches at all.
		SUBSCRIBE_METHOD_AFTER(AFGChainsaw::RemoveCollateralFoliage, [](AFGChainsaw* Self, AFGFoliageRemovalSubsystem* Removal, const FVector& Location)
		{
			AFGCharacterPlayer* Character = Self ? Self->GetInstigatorCharacter() : nullptr;
			if (!Character || !Character->HasAuthority() || !Removal) return;
			const bool bBoulders = UMKIToolsConfiguration::GetChainsawCutsBoulders(Self);
			FFoliageAround Near;
			Removal->GetFoliageWithinRadius(Location, Self->mCollateralPickupRadius, Near);
			// One declaration per line: a comma here would split the hook macro's arguments.
			int32 Limestone = 0;
			int32 Boulders = 0;
			int32 Cleared = 0;
			static TSet<FString> LoggedKept;
			for (auto& Entry : Near)
			{
				UHierarchicalInstancedStaticMeshComponent* Component = Entry.Key;
				if (!IsValid(Component) || Entry.Value.Num() == 0) continue;
				if (!FMKITerrainRules::IsRockFoliageMesh(Component->GetStaticMesh()))
				{
					const FString Mesh = GetNameSafe(Component->GetStaticMesh());
					// Live (2026-10-20): pickable grass the chainsaw cuts stayed inside the clearing; asked by each tag, in case
					// the game matches tags exactly rather than by their parents.
					if (AFGFoliageRemovalSubsystem::HasIdentifier(Component, UFGFoliageIdentifier_RemovableByBuildings::StaticClass())
						|| AFGFoliageRemovalSubsystem::HasIdentifier(Component, UFGFoliageIdentifier_Pickupable::StaticClass()))
					{
						if (Removal->RemoveFoliageInstances(Cast<APlayerController>(Character->GetController()), Component, Entry.Value)) Cleared += Entry.Value.Num();
						if (!LoggedKept.Contains(Mesh))
						{
							LoggedKept.Add(Mesh);
							UE_LOG(LogMKITools, Log, TEXT("Chainsaw: area clearing clears %s"), *Mesh);
						}
					}
					else if (!LoggedKept.Contains(Mesh))
					{
						LoggedKept.Add(Mesh);
						UE_LOG(LogMKITools, Log, TEXT("Chainsaw: area clearing keeps %s (not removable by buildings)"), *Mesh);
					}
					continue;
				}
				if (!bBoulders) continue;
				// Highest index first: removing an instance can move the last one into its slot.
				Entry.Value.Sort(TGreater<int32>());
				for (const int32 Instance : Entry.Value)
				{
					FTransform Transform;
					if (!Component->GetInstanceTransform(Instance, Transform, true)) continue;
					if (!Removal->RemoveFoliageInstance(Component, Instance)) continue;
					Limestone += FMKITerrainRules::LimestoneForBoulder(Component->GetStaticMesh(), Transform.GetScale3D());
					++Boulders;
				}
			}
			UE_LOG(LogMKITools, Log, TEXT("Chainsaw: area clearing (%.0f cm) also cleared %d foliage instances and %d boulders"), Self->mCollateralPickupRadius, Cleared, Boulders);
			if (Boulders > 0) FMKITerrainRules::GrantLimestone(Character, Limestone, Character->GetActorLocation());
		});
		// Leaving Terrain mode, or the dismantle state, drops the rock outline; vanilla handles the rest.
		SUBSCRIBE_UOBJECT_METHOD_AFTER(UFGBuildGunStateDismantle, OnBuildGunModeChanged_Implementation, [](UFGBuildGunStateDismantle* Self, TSubclassOf<UFGBuildGunModeDescriptor> NewMode)
		{
			if (!FMKITerrainDismantle::IsTerrainMode(Self)) FMKITerrainDismantle::Clear(Self);
		});
		SUBSCRIBE_UOBJECT_METHOD_AFTER(UFGBuildGunStateDismantle, EndState_Implementation, [](UFGBuildGunStateDismantle* Self)
		{
			FMKITerrainDismantle::Clear(Self);
		});
#endif
	}
};

IMPLEMENT_MODULE(FMKIToolsModule, MKITools);

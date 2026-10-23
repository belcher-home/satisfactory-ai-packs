#include "MKIRegrowthSubsystem.h"

#include "Buildables/FGBuildable.h"
#include "Camera/CameraTypes.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FoliageType.h"
#include "InstancedFoliageActor.h"
#include "MKITerrainRules.h"
#include "MKITools.h"
#include "MKIToolsConfiguration.h"
#include "TimerManager.h"

namespace
{
	/** The private parts of the game's per-type removal record, by reflection (they are saved properties). */
	struct FRemovalRecordAccess
	{
		FArrayProperty* Locations = nullptr;
		FSetProperty* Lookup = nullptr;
		FArrayProperty* Buckets = nullptr;

		FRemovalRecordAccess()
		{
			const UScriptStruct* Struct = FFoliageRemovalSaveDataForFoliageType::StaticStruct();
			Locations = CastField<FArrayProperty>(Struct->FindPropertyByName(TEXT("RemovedLocations")));
			Lookup = CastField<FSetProperty>(Struct->FindPropertyByName(TEXT("RemovedLocationLookup")));
			Buckets = CastField<FArrayProperty>(Struct->FindPropertyByName(TEXT("RemovalBuckets")));
		}
		bool IsValid() const { return Locations && Lookup && Buckets; }
		TArray<FVector>& LocationsOf(FFoliageRemovalSaveDataForFoliageType& Record) const { return *Locations->ContainerPtrToValuePtr<TArray<FVector>>(&Record); }
		TSet<uint32>& LookupOf(FFoliageRemovalSaveDataForFoliageType& Record) const { return *Lookup->ContainerPtrToValuePtr<TSet<uint32>>(&Record); }
		TArray<FInt32Range>& BucketsOf(FFoliageRemovalSaveDataForFoliageType& Record) const { return *Buckets->ContainerPtrToValuePtr<TArray<FInt32Range>>(&Record); }
	};

	/** The cells whose foliage is loaded now (the game's private map, a saved-property-free UPROPERTY, by reflection). */
	const TMap<FIntVector, TObjectPtr<AInstancedFoliageActor>>* LoadedCells(const AFGFoliageRemovalSubsystem* Removal)
	{
		const FMapProperty* Property = CastField<FMapProperty>(AFGFoliageRemovalSubsystem::StaticClass()->FindPropertyByName(TEXT("mCellToLoadedIFAMap")));
		return Property ? Property->ContainerPtrToValuePtr<TMap<FIntVector, TObjectPtr<AInstancedFoliageActor>>>(Removal) : nullptr;
	}
}

AMKIRegrowthSubsystem::AMKIRegrowthSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
	PrimaryActorTick.bCanEverTick = false;
}

void AMKIRegrowthSubsystem::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority()) GetWorldTimerManager().SetTimer(CheckTimer, this, &AMKIRegrowthSubsystem::Check, CheckIntervalSeconds, true);
}

void AMKIRegrowthSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(CheckTimer);
	Super::EndPlay(EndPlayReason);
}

double AMKIRegrowthSubsystem::RegrowChance(double AgeSeconds, double StepSeconds, double MeanSeconds)
{
	// Weibull, shape 2: P(regrown by age a) = 1 - exp(-(a / L)^2), mean L * Gamma(1.5) = L * 0.8862. The chance over the
	// next step, given not regrown yet, rises with age.
	if (MeanSeconds <= 0.0 || StepSeconds <= 0.0) return 0.0;
	const double Scale = MeanSeconds / 0.886226925;
	const double Age = FMath::Max(AgeSeconds, 0.0), Next = Age + StepSeconds;
	return 1.0 - FMath::Exp(-(Next * Next - Age * Age) / (Scale * Scale));
}

bool AMKIRegrowthSubsystem::IsInPlayerView(const FVector& Location, const TArray<FMinimalViewInfo>& Viewers)
{
	for (const FMinimalViewInfo& View : Viewers)
	{
		const FVector ToPlant = Location - View.Location;
		const double Distance = ToPlant.Size();
		if (Distance < HiddenDistanceCm) return true;
		// In front of the player (within about 70 degrees of where they look) and near enough to be drawn.
		if (Distance < InViewDistanceCm && FVector::DotProduct(ToPlant / Distance, View.Rotation.Vector()) > 0.34) return true;
	}
	return false;
}

bool AMKIRegrowthSubsystem::IsNearBuilding(const FVector& Location) const
{
	TArray<FOverlapResult> Overlaps;
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	GetWorld()->OverlapMultiByObjectType(Overlaps, Location + FVector(0.0, 0.0, 100.0), FQuat::Identity, Objects, FCollisionShape::MakeSphere(BuildingClearanceCm));
	for (const FOverlapResult& Overlap : Overlaps)
	{
		const AActor* Actor = Overlap.GetActor();
		// A buildable, or a lightweight one (foundations, walls) drawn by the game's instance manager.
		if (Actor && (Actor->IsA<AFGBuildable>() || Actor->GetClass()->GetName().Contains(TEXT("AbstractInstanceManager")))) return true;
	}
	return false;
}

void AMKIRegrowthSubsystem::Check()
{
	PlaySeconds += CheckIntervalSeconds;
	AFGFoliageRemovalSubsystem* Removal = AFGFoliageRemovalSubsystem::Get(GetWorld());
	static const FRemovalRecordAccess Access;
	if (!Removal || !Access.IsValid())
	{
		static bool bWarned = false;
		if (!bWarned) { bWarned = true; UE_LOG(LogMKITools, Warning, TEXT("Regrowth: off, the game's foliage removal record is not readable (subsystem %s, fields %s)"), Removal ? TEXT("found") : TEXT("missing"), Access.IsValid() ? TEXT("found") : TEXT("missing")); }
		return;
	}
	const bool bEnabled = UMKIToolsConfiguration::GetVegetationRegrowth(this);
	const double MeanSeconds = UMKIToolsConfiguration::GetRegrowthHours(this) * 3600.0;
	const TMap<FIntVector, TObjectPtr<AInstancedFoliageActor>>* Loaded = LoadedCells(Removal);
	// The game's record, edited in place: regrown locations leave it and are not removed again when their cell loads.
	auto& SaveData = const_cast<TMap<FIntVector, FFoliageRemovalSaveDataPerCell>&>(Removal->GetSaveData());
	TSet<uint32> Seen;
	int32 Regrown = 0;
	// Diagnostics (live 2026-10-21: nothing regrew in four hours, with no log to tell why).
	int32 Cells = 0;
	int32 LoadedCellCount = 0;
	int32 Eligible = 0;
	int32 ByBuildings = 0;
	int32 InView = 0;
	TArray<FMinimalViewInfo> Viewers;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (const APlayerController* Player = It->Get())
		{
			FMinimalViewInfo View;
			Player->GetPlayerViewPoint(View.Location, View.Rotation);
			Viewers.Add(View);
		}
	}
	for (TPair<FIntVector, FFoliageRemovalSaveDataPerCell>& Cell : SaveData)
	{
		const bool bCellLoaded = !Loaded || Loaded->Contains(Cell.Key);
		++Cells;
		if (bCellLoaded) ++LoadedCellCount;
		for (TPair<TObjectPtr<const UFoliageType>, FFoliageRemovalSaveDataForFoliageType>& Type : Cell.Value.SaveDataMap)
		{
			const UFoliageType* FoliageType = Type.Key;
			if (!FoliageType || FMKITerrainRules::IsRockFoliageType(FoliageType)) continue;
			const uint32 TypeHash = GetTypeHash(FoliageType->GetPathName());
			TArray<FVector>& Locations = Access.LocationsOf(Type.Value);
			// Live (2026-10-21): the game keeps every foliage cell with cut plants loaded (48 of 48), so a regrown plant is
			// put back into its cell's foliage component now, where no player is close or looking.
			UHierarchicalInstancedStaticMeshComponent* Live = bCellLoaded ? Removal->GetHISM(Cell.Key, FoliageType) : nullptr;
			TArray<int32> Regrow;
			for (int32 Index = 0; Index < Locations.Num(); ++Index)
			{
				const uint32 Key = HashCombine(TypeHash, AFGFoliageRemovalSubsystem::HashFoliageInstanceLocation(Locations[Index]));
				Seen.Add(Key);
				const double FirstSeen = FirstSeenRemoved.FindOrAdd(Key, PlaySeconds);
				if (!bEnabled || (bCellLoaded && !Live)) continue;
				++Eligible;
				if (FMath::FRand() >= RegrowChance(PlaySeconds - FirstSeen, CheckIntervalSeconds, MeanSeconds)) continue;
				if (Live && IsInPlayerView(Locations[Index], Viewers)) { ++InView; continue; }
				if (IsNearBuilding(Locations[Index])) { ++ByBuildings; continue; }
				Regrow.Add(Index);
			}
			if (Regrow.Num() == 0) continue;
			TSet<uint32>& Lookup = Access.LookupOf(Type.Value);
			for (int32 Back = Regrow.Num() - 1; Back >= 0; --Back)
			{
				const FVector Location = Locations[Regrow[Back]];
				// The game keeps only where the plant stood; its yaw and size are drawn again the way the foliage type places
				// it. The next time the cell loads from disk, the original plant is there instead.
				if (Live) Live->AddInstance(FTransform(FRotator(0.0, FoliageType->RandomYaw ? FMath::FRandRange(0.0, 360.0) : 0.0, 0.0), Location, FVector(FoliageType->GetRandomScale())), true);
				Lookup.Remove(AFGFoliageRemovalSubsystem::HashFoliageInstanceLocation(Location));
				FirstSeenRemoved.Remove(HashCombine(TypeHash, AFGFoliageRemovalSubsystem::HashFoliageInstanceLocation(Location)));
				Locations.RemoveAt(Regrow[Back]);
			}
			// The game's runtime buckets index into the array; one bucket covers what is left.
			TArray<FInt32Range>& Buckets = Access.BucketsOf(Type.Value);
			Buckets.Reset();
			if (Locations.Num() > 0) Buckets.Add(FInt32Range(0, Locations.Num()));
			Regrown += Regrow.Num();
		}
	}
	// Locations the game no longer lists (regrown, or never removed again) are forgotten.
	for (auto It = FirstSeenRemoved.CreateIterator(); It; ++It) if (!Seen.Contains(It.Key())) It.RemoveCurrent();
	++Checks;
	if (Regrown > 0 || Checks % 10 == 1) UE_LOG(LogMKITools, Log, TEXT("Regrowth (%s, mean %.1f h, play %.2f h): %d cells with cut plants, %d loaded (%s); %d removed locations tracked, %d eligible, %d near or in view of a player, %d kept by buildings, %d regrown"), bEnabled ? TEXT("on") : TEXT("off"), MeanSeconds / 3600.0, PlaySeconds / 3600.0, Cells, LoadedCellCount, Loaded ? TEXT("loaded-cell map read") : TEXT("loaded-cell map missing"), FirstSeenRemoved.Num(), Eligible, InView, ByBuildings, Regrown);
}

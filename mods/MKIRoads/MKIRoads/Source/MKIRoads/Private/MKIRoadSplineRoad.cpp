#include "MKIRoadSplineRoad.h"

#include "MKIRoadAutoPaths.h"
#include "FGDismantleInterface.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "FGConstructDisqualifier.h"
#include "MKIRoadJunction.h"
#include "MKIRoadSupports.h"
#include "MKIRoadMarkingPattern.h"
#include "EngineUtils.h"
#include "MKIRoadSplineRoadHologram.h"
#include "FGRecipe.h"
#include "Components/SplineComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FGCharacterPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Hologram/FGHologram.h"
#include "TimerManager.h"
#include "Misc/ScopeExit.h"

AMKIRoadSplineRoad::AMKIRoadSplineRoad(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	mSplineComponent = CreateDefaultSubobject<USplineComponent>(TEXT("SplineComponent"));
	mSplineComponent->SetMobility(EComponentMobility::Static);
	mSplineComponent->SetupAttachment(RootComponent);

	mDeck = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Deck"));
	mDeck->SetupAttachment(RootComponent);
	// Rendering only: the build gun and the vehicle hologram must hit the static mesh collision slabs (BuildCollision).
	mDeck->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mDeck->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	// The vanilla asphalt foundation's material, by reference (package.splineRoad.surfaceMaterial in modules.json): the
	// deck is a plain procedural mesh, which it renders, with the vanilla UV layout and custom primitive data.
	mDeckMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/FactoryGame/Buildable/Building/Foundation/AsphaltSet/MI_AsphaltFoundation.MI_AsphaltFoundation")));

	mMarkings = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Markings"));
	mMarkings->SetupAttachment(RootComponent);
	mMarkings->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mMarkings->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	mMarkings->SetCastShadow(false);
	mInterfaceMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/FactoryGame/Buildable/Building/Foundation/ConcreteSet/Meshes/SM_Foundation_Concrete_8x4.SM_Foundation_Concrete_8x4")));

	mHologramClass = AMKIRoadSplineRoadHologram::StaticClass();

	// The deck is swept at BeginPlay; it never becomes a lightweight instance.
	mCanContainLightweightInstances = false;
	mManagedByLightweightBuildableSubsystem = false;

	// A foundation, so vehicles can be placed on it, but not a grid cell: no foundation snaps to its edges or faces.
	// A vanilla-shaped volume (mHeight the deck thickness) around the frame GetLightweightInstanceActorTransform gives
	// holograms aimed at the deck: centered half the thickness below the deck top under the aim, so its top is the deck.
	mWidth = LaneWidthCm * DefaultLanes;
	mDepth = UvTileCm;
	mHeight = DefaultDeckThicknessCm;
	for (FFoundationSideSelectionFlags* Flags : { &mDisableSnapOn, &mDisableAttachmentSnapOn })
	{
		Flags->Front = Flags->Right = Flags->Back = Flags->Left = Flags->Top = Flags->Bottom = true;
	}
}

int32 AMKIRoadSplineRoad::FoundationEquivalents(double LengthCm, int32 Lanes)
{
	const double Area = FMath::Max(0.0, LengthCm) * FMath::Max(0, Lanes) * LaneWidthCm;
	// Tolerate float noise so an exact multiple of 64 m2 is not charged one extra foundation.
	return FMath::Max(1, FMath::CeilToInt32(Area / FoundationAreaCm2 - 1e-6));
}

void AMKIRoadSplineRoad::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMKIRoadSplineRoad, mSplineData);
	DOREPLIFETIME(AMKIRoadSplineRoad, mLanes);
}

void AMKIRoadSplineRoad::SetLanes(int32 Lanes)
{
	if (IsSupportedLaneCount(Lanes)) mLanes = Lanes;
}

float AMKIRoadSplineRoad::GetSplineLengthCm() const
{
	return mSplineComponent ? mSplineComponent->GetSplineLength() : 0.f;
}

UStaticMesh* AMKIRoadSplineRoad::GetSplineMesh() const
{
	return mInterfaceMesh.LoadSynchronous();
}

void AMKIRoadSplineRoad::BeginPlay()
{
	// A saved or replicated road may carry a width this build does not support; fall back to the default.
	if (!IsSupportedLaneCount(mLanes)) mLanes = DefaultLanes;
	// A retired road (#233: Road (2 m) and (4 m) have no recipe any more) refunds through the Road's, the same cost basis.
	if (!GetBuiltWithRecipe()) SetBuiltWithRecipe(LoadClass<UFGRecipe>(nullptr, TEXT("/MKIRoads/SplineRoad/Recipe_MKIR_SplineRoad1m.Recipe_MKIR_SplineRoad1m_C")));
	PopulateSplineComponent();
	BuildDeck();
	BuildCollision();
	// Soft clearance along the deck, as vanilla railway track has: a hologram crossing the road warns (yellow).
	ResetClearanceData();
	TArray<FFGClearanceData> Clearance;
	MakeClearanceData(mSplineComponent, GetDeckWidthCm(), mDeckThicknessCm, Clearance);
	AddClearanceData(Clearance);
	const bool bDefaultPattern = !mPatternDefaulted && !GetCustomizationData_Native().PatternDesc;
	if (bDefaultPattern) GetCustomizationData_Native().PatternDesc = MKIRoadDefaultPattern();
	mPatternDefaulted = true;
	Super::BeginPlay();
	if (bDefaultPattern) ApplyCustomizationData_Native(GetCustomizationData_Native());
	// A build-gun build finishes spawning after this; check the collision again once it has (#185).
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]() { EnsureCollisionPhysics(); }));
		FTimerHandle Settled;
		World->GetTimerManager().SetTimer(Settled, FTimerDelegate::CreateWeakLambda(this, [this]() { EnsureCollisionPhysics(); }), 1.f, false);
		// Its vehicle paths (#235), once, after the roads and intersections loaded with it have begun play.
		if (HasAuthority() && mAutoPathsVersion < FMKIRoadAutoPaths::Version)
		{
			// Paths laid by an older version go first, so none is left when any road or intersection lays its new paths (live:
			// one laid first joined a neighbour's old node, which went with the old path, and the neighbour made a second node).
			// Removed as the player's dismantle does, next tick (the vehicle subsystem ready), and laid the tick after, when every
			// road and intersection has removed its own. Live (#239): Destroy() left the path nodes of other pieces and stations
			// linked to the gone paths, and dismantling one of those later left a save that crashed the vehicle subsystem on load.
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				for (AFGVehiclePathSegment* Old : mAutoPaths) if (IsValid(Old) && !Old->IsActorBeingDestroyed()) IFGDismantleInterface::Execute_Dismantle(Old);
				mAutoPaths.Reset();
				FMKIRoadAutoPaths::CleanStaleNodeLinks(GetWorld());
				GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]() { LayAutoPaths(); }));
			}));
		}
	}
}

bool AMKIRoadSplineRoad::OwnsAutoPath(const AActor* Path) const
{
	return Path && mAutoPaths.ContainsByPredicate([Path](const TObjectPtr<AFGVehiclePathSegment>& Own) { return Own.Get() == Path; });
}

void AMKIRoadSplineRoad::LayAutoPaths()
{
	if (mAutoPathsVersion >= FMKIRoadAutoPaths::Version || IsActorBeingDestroyed()) return;
	TArray<TArray<FSplinePointData>> Paths;
	FMKIRoadAutoPaths::RoadPaths(*this, Paths);
	TArray<AFGVehiclePathSegment*> Segments;
	FMKIRoadAutoPaths::Spawn(GetWorld(), Paths, Segments);
	for (AFGVehiclePathSegment* Segment : Segments) mAutoPaths.Add(Segment);
	mAutoPathsLaid = true;
	mAutoPathsVersion = FMKIRoadAutoPaths::Version;
}

void AMKIRoadSplineRoad::EnsureCollisionPhysics()
{
	if (!mCollisionSlabs || !mCollisionSlabs->IsRegistered() || mCollisionSlabs->GetInstanceCount() == 0 || mCollisionSlabs->IsPhysicsStateCreated()) return;
	mCollisionSlabs->RecreatePhysicsState();
}

void AMKIRoadSplineRoad::MakeClearanceData(const USplineComponent* Spline, float WidthCm, float ThicknessCm, TArray<FFGClearanceData>& Out)
{
	const float Length = Spline ? Spline->GetSplineLength() : 0.f;
	const float Span = Length - 2.f * ClearanceInsetCm;
	if (Span < 1.f) return;
	const int32 Pieces = FMath::Clamp(FMath::CeilToInt32(Span / MaxCollisionStepCm), 1, 64);
	for (int32 Index = 0; Index < Pieces; ++Index)
	{
		const FVector C0 = Spline->GetLocationAtDistanceAlongSpline(ClearanceInsetCm + Span * Index / Pieces, ESplineCoordinateSpace::Local);
		const FVector C1 = Spline->GetLocationAtDistanceAlongSpline(ClearanceInsetCm + Span * (Index + 1) / Pieces, ESplineCoordinateSpace::Local);
		const double Run = FVector::Distance(C0, C1);
		if (Run < 1.0) continue;
		const FVector Forward = (C1 - C0) / Run;
		FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal2D();
		if (Right.IsNearlyZero()) Right = FVector::RightVector;
		FFGClearanceData Data;
		Data.Type = EClearanceType::CT_Soft;
		// Inset from the deck's faces, so a foundation under the deck, a road or module joined at its end, and a building
		// standing on its top only touch it.
		const double HalfWidth = WidthCm * 0.5 - ClearanceInsetCm, TopInset = FMath::Min(ClearanceTopInsetCm, ThicknessCm * 0.5f);
		Data.ClearanceBox = FBox(FVector(-Run * 0.5, -HalfWidth, -ThicknessCm + ClearanceInsetCm), FVector(Run * 0.5, HalfWidth, -TopInset));
		Data.RelativeTransform = FTransform(FRotationMatrix::MakeFromXY(Forward, Right).ToQuat(), (C0 + C1) * 0.5);
		Out.Add(Data);
	}
}

void AMKIRoadSplineRoad::PopulateSplineComponent()
{
	mSplineComponent->ClearSplinePoints(false);
	for (int32 Index = 0; Index < mSplineData.Num(); ++Index)
	{
		const FSplinePointData& Point = mSplineData[Index];
		mSplineComponent->AddSplinePoint(Point.Location, ESplineCoordinateSpace::Local, false);
		mSplineComponent->SetTangentsAtSplinePoint(Index, Point.ArriveTangent, Point.LeaveTangent, ESplineCoordinateSpace::Local, false);
	}
	mSplineComponent->UpdateSpline();
}

FMKIRoadDeckGeometry AMKIRoadSplineRoad::SweepDeck(const TArray<FMKIRoadDeckSlice>& Slices, float WidthCm, float ThicknessCm)
{
	FMKIRoadDeckGeometry Out;
	if (Slices.Num() < 2) return Out;
	const int32 Columns = FMath::Max(1, FMath::RoundToInt32(WidthCm / DeckColumnCm));
	const float Half = WidthCm * 0.5f;

	// A procedural mesh triangle faces the side from which (B - A) x (C - A) points away: live, the opposite order drew
	// every deck inside out (the top culled, the inner walls showing). The order is fixed per triangle, so every face
	// shows its outer side.
	auto AddTriangle = [&Out](int32 A, int32 B, int32 C, const FVector& Normal)
	{
		const FVector Face = FVector::CrossProduct(Out.Vertices[B] - Out.Vertices[A], Out.Vertices[C] - Out.Vertices[A]);
		if (FVector::DotProduct(Face, Normal) > 0.f) Swap(B, C);
		Out.Triangles.Append({ A, B, C });
	};
	auto AddVertex = [&Out](const FVector& Position, const FVector& Normal, const FVector2D& UV, const FVector2D& UV1, const FVector& Tangent)
	{
		Out.Vertices.Add(Position); Out.Normals.Add(Normal); Out.UVs.Add(UV); Out.UV1s.Add(UV1); Out.Tangents.Add(Tangent);
		return Out.Vertices.Num() - 1;
	};
	// One patch per 8 m tile: across, per lane (DeckColumnCm columns each); along, per run of slices within one 8 m of
	// centerline distance (a slice on a tile boundary ends one run and starts the next). Each patch has its own vertices,
	// with UVs inside that tile, as a vanilla foundation's: its material picks the top, bottom, and side textures by UV0
	// range (live, UVs running on across lanes and along the road showed the side texture on the top in bands).
	const int32 LaneTiles = FMath::Max(1, FMath::RoundToInt32(WidthCm / UvTileCm));
	const int32 TileColumns = FMath::Max(1, Columns / LaneTiles);
	const float TileWidthCm = WidthCm / LaneTiles;
	TArray<TPair<int32, int32>> Runs;
	for (int32 First = 0, Index = 1; Index < Slices.Num(); ++Index)
	{
		const float TileEnd = (FMath::FloorToFloat(Slices[First].Distance / UvTileCm + 1e-4f) + 1.f) * UvTileCm;
		if (Slices[Index].Distance >= TileEnd - 0.01f || Index == Slices.Num() - 1) { Runs.Add({ First, Index }); First = Index; }
	}
	// The vanilla top spans +-0.487 of a UV unit per 8 m (its chamfers the rest), its sides +-0.489.
	constexpr float TopSpan = 0.974f, SideSpan = 0.978f;
	auto Along = [&Slices](int32 SliceIndex, int32 RunFirst)
	{
		const float TileStart = FMath::FloorToFloat(Slices[RunFirst].Distance / UvTileCm + 1e-4f) * UvTileCm;
		return (Slices[SliceIndex].Distance - TileStart) / UvTileCm;
	};

	// Top and bottom. The top's UV0 runs U across (to the right) and V back; the bottom's U back from 2 and V across; UV1
	// (the stencil channel) is top-down, U forward and V across, 0-1 per tile.
	for (const bool bTop : { true, false })
	{
		const FVector Drop = bTop ? FVector::ZeroVector : FVector(0.f, 0.f, -ThicknessCm);
		for (const TPair<int32, int32>& Run : Runs)
		{
			for (int32 Lane = 0; Lane < LaneTiles; ++Lane)
			{
				const int32 First = Out.Vertices.Num();
				for (int32 SliceIndex = Run.Key; SliceIndex <= Run.Value; ++SliceIndex)
				{
					const FMKIRoadDeckSlice& Slice = Slices[SliceIndex];
					const float A = Along(SliceIndex, Run.Key);
					for (int32 Column = 0; Column <= TileColumns; ++Column)
					{
						const float C = static_cast<float>(Column) / TileColumns;
						const FVector Position = Slice.Center + Slice.Right * (-Half + TileWidthCm * (Lane + C)) + Drop;
						const FVector2D UV = bTop ? FVector2D((C - 0.5f) * TopSpan, (0.5f - A) * TopSpan) : FVector2D(2.f + (0.5f - A) * TopSpan, (C - 0.5f) * TopSpan);
						AddVertex(Position, bTop ? Slice.Up : -Slice.Up, UV, FVector2D(A, C), Slice.Forward);
					}
				}
				for (int32 Row = 0; Row < Run.Value - Run.Key; ++Row)
				{
					for (int32 Column = 0; Column < TileColumns; ++Column)
					{
						const int32 A = First + Row * (TileColumns + 1) + Column, B = A + 1, C = A + TileColumns + 1, D = C + 1;
						const FVector Normal = bTop ? Slices[Run.Key + Row].Up : -Slices[Run.Key + Row].Up;
						AddTriangle(A, C, B, Normal);
						AddTriangle(B, C, D, Normal);
					}
				}
			}
		}
	}

	// Sides, per run: U across the 8 m tile around 2, increasing to the left of a viewer facing the side, as on the vanilla
	// +Y and -Y sides; V from 1 at the deck top down the side band. UV1 collapses onto the stencil's border.
	for (const float Side : { -1.f, 1.f })
	{
		for (const TPair<int32, int32>& Run : Runs)
		{
			const int32 First = Out.Vertices.Num();
			for (int32 SliceIndex = Run.Key; SliceIndex <= Run.Value; ++SliceIndex)
			{
				const FMKIRoadDeckSlice& Slice = Slices[SliceIndex];
				const float A = Along(SliceIndex, Run.Key);
				const FVector Top = Slice.Center + Slice.Right * (Side * Half);
				const FVector Normal = Slice.Right * Side;
				const float U = 2.f + (Side > 0.f ? A - 0.5f : 0.5f - A) * SideSpan;
				const FVector2D UV1(A, Side < 0.f ? 0.f : 1.f);
				AddVertex(Top, Normal, FVector2D(U, 1.f), UV1, Slice.Forward);
				AddVertex(Top - FVector(0.f, 0.f, ThicknessCm), Normal, FVector2D(U, 1.f - ThicknessCm / SideBandCm), UV1, Slice.Forward);
			}
			for (int32 Row = 0; Row < Run.Value - Run.Key; ++Row)
			{
				const int32 A = First + Row * 2, B = A + 1, C = A + 2, D = A + 3;
				const FVector Normal = Slices[Run.Key + Row].Right * Side;
				AddTriangle(A, B, C, Normal);
				AddTriangle(B, D, C, Normal);
			}
		}
	}

	// End caps, per lane tile: the start is the vanilla back side (U around 0), the end its front (around 2).
	for (const bool bStart : { true, false })
	{
		const FMKIRoadDeckSlice& Slice = bStart ? Slices[0] : Slices.Last();
		const FVector Normal = bStart ? -Slice.Forward : Slice.Forward;
		const FVector Down(0.f, 0.f, -ThicknessCm);
		const float Bottom = 1.f - ThicknessCm / SideBandCm, End = bStart ? 0.f : 1.f;
		for (int32 Lane = 0; Lane < LaneTiles; ++Lane)
		{
			const FVector Left = Slice.Center + Slice.Right * (-Half + TileWidthCm * Lane), Right = Left + Slice.Right * TileWidthCm;
			const float ULeft = bStart ? -0.5f * SideSpan : 2.f + 0.5f * SideSpan, URight = bStart ? 0.5f * SideSpan : 2.f - 0.5f * SideSpan;
			const int32 A = AddVertex(Left, Normal, FVector2D(ULeft, 1.f), FVector2D(End, 0.f), Slice.Right), B = AddVertex(Right, Normal, FVector2D(URight, 1.f), FVector2D(End, 1.f), Slice.Right);
			const int32 C = AddVertex(Left + Down, Normal, FVector2D(ULeft, Bottom), FVector2D(End, 0.f), Slice.Right), D = AddVertex(Right + Down, Normal, FVector2D(URight, Bottom), FVector2D(End, 1.f), Slice.Right);
			AddTriangle(A, B, C, Normal);
			AddTriangle(B, D, C, Normal);
		}
	}
	return Out;
}

void AMKIRoadSplineRoad::BuildDeck()
{
	mDeckSliceCount = 0;
	mDeck->ClearAllMeshSections();
	const float Length = mSplineComponent->GetSplineLength();
	if (mSplineData.Num() < 2 || Length <= KINDA_SMALL_NUMBER) return;

	// Cross-sections at most DeckStepCm apart: on the tightest allowed curve (radius = deck width) the outer edge's chord
	// error is then well under a millimetre, and grade changes follow the spline tangents smoothly.
	// Capped at 2 km of deck, a guard against a degenerate spline; real roads are at most 100 m.
	// Every 8 m of centerline distance is a UV tile boundary (SweepDeck), and a cross-section.
	TArray<float> Distances;
	for (float TileStart = 0.f; TileStart < Length && Distances.Num() <= 8000; TileStart += UvTileCm)
	{
		const float TileLength = FMath::Min(UvTileCm, Length - TileStart);
		const int32 TileSteps = FMath::Max(1, FMath::CeilToInt32(TileLength / DeckStepCm));
		for (int32 Index = Distances.Num() == 0 ? 0 : 1; Index <= TileSteps; ++Index) Distances.Add(TileStart + TileLength * Index / TileSteps);
	}
	TArray<FMKIRoadDeckSlice> Slices;
	Slices.Reserve(Distances.Num());
	for (const float Distance : Distances)
	{
		FMKIRoadDeckSlice Slice;
		Slice.Distance = Distance;
		Slice.Center = mSplineComponent->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
		Slice.Forward = mSplineComponent->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
		Slice.Right = FVector::CrossProduct(FVector::UpVector, Slice.Forward).GetSafeNormal2D();
		if (Slice.Right.IsNearlyZero()) Slice.Right = Slices.Num() > 0 ? Slices.Last().Right : FVector::RightVector;
		Slice.Up = FVector::CrossProduct(Slice.Forward, Slice.Right).GetSafeNormal();
		Slices.Add(Slice);
	}
	const FMKIRoadDeckGeometry Geometry = SweepDeck(Slices, GetDeckWidthCm(), mDeckThicknessCm);
	TArray<FProcMeshTangent> Tangents;
	Tangents.Reserve(Geometry.Tangents.Num());
	for (const FVector& Tangent : Geometry.Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
	mDeck->CreateMeshSection_LinearColor(0, Geometry.Vertices, Geometry.Triangles, Geometry.Normals, Geometry.UVs, Geometry.UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, true);
	if (UMaterialInterface* Material = mDeckMaterial.LoadSynchronous()) mDeck->SetMaterial(0, Material);
	mDeckSliceCount = Slices.Num();
}

bool AMKIRoadSplineRoad::IsVanillaPattern(TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> PatternDesc)
{
	// A vanilla stencil pattern; a road marking pattern and the vanilla removers (ID 0) are not.
	return PatternDesc && !PatternDesc->IsChildOf(UMKIRoadMarkingPattern::StaticClass()) && GetDefault<UFGFactoryCustomizationDescriptor_Pattern>(PatternDesc)->ID != 0;
}

void AMKIRoadSplineRoad::ApplyCustomizationData_Native(const FFactoryCustomizationData& InCustomizationData)
{
	// Vanilla patterns are refused (#190): their stencils repeat on every 8 m tile and drew nothing on the deck. The road
	// keeps the road marking pattern it had (or none); a vanilla remover still clears it.
	FFactoryCustomizationData CustomizationData = InCustomizationData;
	if (IsVanillaPattern(CustomizationData.PatternDesc))
	{
		CustomizationData.PatternDesc = mRoadPattern;
		CustomizationData.PatternRotation = mRoadPatternRotation;
		GetCustomizationData_Native().PatternDesc = mRoadPattern;
		GetCustomizationData_Native().PatternRotation = mRoadPatternRotation;
	}
	else
	{
		mRoadPattern = CustomizationData.PatternDesc && CustomizationData.PatternDesc->IsChildOf(UMKIRoadMarkingPattern::StaticClass()) ? CustomizationData.PatternDesc : nullptr;
		mRoadPatternRotation = CustomizationData.PatternRotation;
	}
	Super::ApplyCustomizationData_Native(CustomizationData);
	const UMKIRoadMarkingPattern* Pattern = CustomizationData.PatternDesc ? Cast<UMKIRoadMarkingPattern>(CustomizationData.PatternDesc->GetDefaultObject()) : nullptr;
	// The swatch and paint finish reach the deck as the vanilla material's custom primitive data. A road marking pattern
	// is drawn as strips instead of the vanilla stencil, which has no texture at its ID: the deck gets pattern ID 0.
	FResolvedFactoryCustomizationData Resolved(CustomizationData);
	if (Pattern) Resolved.PatternID = 0.f;
	if (mDeck) Resolved.ApplyToMeshComponent(mDeck);
	BuildMarkings(Pattern);
}

int32 AMKIRoadSplineRoad::GetCollisionSlabCount() const
{
	return mCollisionSlabs ? mCollisionSlabs->GetInstanceCount() : 0;
}

float AMKIRoadSplineRoad::GetDeckPatternId() const
{
	if (!mDeck) return 0.f;
	const TArray<float>& Data = mDeck->GetCustomPrimitiveData().Data;
	const int32 Index = static_cast<int32>(BuildableColorConstants::EColorDataIndices::CDI_PatternID);
	return Data.IsValidIndex(Index) ? Data[Index] : 0.f;
}

void AMKIRoadSplineRoad::SweepStrip(const USplineComponent& Spline, float LateralCm, float WidthCm, float LiftCm, float FromCm, float ToCm, FMKIRoadDeckGeometry& Out)
{
	if (ToCm - FromCm < 1.f) return;
	const int32 Steps = FMath::Clamp(FMath::CeilToInt32((ToCm - FromCm) / DeckStepCm), 1, 8000);
	const int32 First = Out.Vertices.Num();
	FVector LastRight = FVector::RightVector;
	for (int32 Index = 0; Index <= Steps; ++Index)
	{
		const float Distance = FromCm + (ToCm - FromCm) * Index / Steps;
		const FVector Center = Spline.GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
		const FVector Forward = Spline.GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
		FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal2D();
		if (Right.IsNearlyZero()) Right = LastRight;
		LastRight = Right;
		// The deck top is level crosswise (Right is horizontal), as SweepDeck lays it; the strip lifts along its normal.
		const FVector Up = FVector::CrossProduct(Forward, Right).GetSafeNormal();
		for (const float Edge : { -0.5f, 0.5f })
		{
			Out.Vertices.Add(Center + Right * (LateralCm + Edge * WidthCm) + Up * LiftCm);
			Out.Normals.Add(Up);
			Out.UVs.Add(FVector2D(Distance / UvTileCm, Edge + 0.5f));
			Out.UV1s.Add(FVector2D::ZeroVector);
			Out.Tangents.Add(Forward);
		}
	}
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		const int32 A = First + Index * 2, B = A + 1, C = A + 2, D = A + 3;
		// A procedural mesh triangle faces away from (B - A) x (C - A) (see SweepDeck): order each pair to face up.
		const FVector Face = FVector::CrossProduct(Out.Vertices[B] - Out.Vertices[A], Out.Vertices[C] - Out.Vertices[A]);
		if (FVector::DotProduct(Face, Out.Normals[A]) > 0.f) Out.Triangles.Append({ A, C, B, B, C, D });
		else Out.Triangles.Append({ A, B, C, B, D, C });
	}
}

int32 AMKIRoadSplineRoad::MarkingGeometry(const USplineComponent& Spline, int32 Lanes, const UMKIRoadMarkingPattern& Pattern, FMKIRoadDeckGeometry (&Paint)[2])
{
	const float Length = Spline.GetSplineLength();
	if (Length < 1.f) return 0;
	const TArray<FMKIRoadMarkingLine> Lines = MKIRoadMarkingLines(Pattern, Lanes, LaneWidthCm);
	const float Period = FMath::Max(1.f, Pattern.DashLengthCm + Pattern.DashGapCm);
	for (const FMKIRoadMarkingLine& Line : Lines)
	{
		FMKIRoadDeckGeometry& Out = Paint[Line.bYellow ? 0 : 1];
		const float Width = Line.WidthCm > 0.f ? Line.WidthCm : Pattern.LineWidthCm;
		if (!Line.bDashed) { SweepStrip(Spline, Line.LateralCm, Width, Pattern.LiftCm, 0.f, Length, Out); continue; }
		// Dashes from the road's start, along its centerline, so they continue across roads joined end to end.
		for (float From = 0.f; From < Length; From += Period) SweepStrip(Spline, Line.LateralCm, Width, Pattern.LiftCm, From, FMath::Min(Length, From + Pattern.DashLengthCm), Out);
	}
	return Lines.Num();
}

void AMKIRoadSplineRoad::BuildMarkings(const UMKIRoadMarkingPattern* Pattern)
{
	mMarkingLineCount = mMarkingVertexCount = 0;
	if (!mMarkings) return;
	mMarkings->ClearAllMeshSections();
	const float Length = mSplineComponent ? mSplineComponent->GetSplineLength() : 0.f;
	if (!Pattern || Length < 1.f) return;
	FMKIRoadDeckGeometry Paint[2];
	const int32 LineCount = MarkingGeometry(*mSplineComponent, mLanes, *Pattern, Paint);
	const TSoftObjectPtr<UMaterialInterface> Materials[2] = { Pattern->YellowMaterial, Pattern->WhiteMaterial };
	for (int32 Section = 0; Section < 2; ++Section)
	{
		TArray<FProcMeshTangent> Tangents;
		for (const FVector& Tangent : Paint[Section].Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
		mMarkings->CreateMeshSection_LinearColor(Section, Paint[Section].Vertices, Paint[Section].Triangles, Paint[Section].Normals, Paint[Section].UVs, Paint[Section].UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
		if (UMaterialInterface* Material = Materials[Section].LoadSynchronous()) mMarkings->SetMaterial(Section, Material);
		mMarkingVertexCount += Paint[Section].Vertices.Num();
	}
	mMarkingLineCount = LineCount;
}

void AMKIRoadSplineRoad::BuildCollision()
{
	// The mesh and every slab are set before the component registers (#185), as the barrier's pieces are: then its
	// physics bodies are created with it. Registered first and filled afterwards, a road built with the build gun kept no
	// physics state (nothing could hit it) until the save was reloaded, and recreating the state did not bring it back.
	const bool bFresh = !mCollisionSlabs;
	if (bFresh)
	{
		mCollisionSlabs = NewObject<UInstancedStaticMeshComponent>(this, UInstancedStaticMeshComponent::StaticClass(), TEXT("DeckCollision"));
		mCollisionSlabs->SetupAttachment(RootComponent);
		mCollisionSlabs->SetMobility(EComponentMobility::Static);
		// Foundations' profile: it blocks pawns, vehicles, the build gun, and the VehicleWheelQuery channel wheels trace.
		mCollisionSlabs->SetCollisionProfileName(TEXT("BuildingMesh"));
		mCollisionSlabs->SetVisibility(false);
		mCollisionSlabs->SetCastShadow(false);
		mCollisionSlabs->SetCanEverAffectNavigation(true);
	}
	else mCollisionSlabs->ClearInstances();
	ON_SCOPE_EXIT
	{
		if (bFresh) mCollisionSlabs->RegisterComponent();
		EnsureCollisionPhysics();
	};
	UStaticMesh* Mesh = mInterfaceMesh.LoadSynchronous();
	const float Length = mSplineComponent->GetSplineLength();
	if (!Mesh || mSplineData.Num() < 2 || Length <= KINDA_SMALL_NUMBER) return;
	const FBox Bounds = Mesh->GetBoundingBox();
	const FVector MeshSize = Bounds.GetSize(), MeshCenter = Bounds.GetCenter();
	if (MeshSize.X <= KINDA_SMALL_NUMBER || MeshSize.Y <= KINDA_SMALL_NUMBER || MeshSize.Z <= KINDA_SMALL_NUMBER) return;
	mCollisionSlabs->SetStaticMesh(Mesh);

	// Oriented slabs along the spline in actor-local space: level crosswise, the top on the spline, the full deck width
	// and thickness. Steps shorten where heading or grade changes so a slab never strays from the drawn deck, and each
	// overlaps its neighbours so wheels never meet a gap.
	const float CosLimit = FMath::Cos(FMath::DegreesToRadians(MaxCollisionStepAngleDeg));
	const float Width = GetDeckWidthCm(), Thickness = mDeckThicknessCm;
	float Start = 0.f;
	int32 Guard = 0;
	while (Start < Length - KINDA_SMALL_NUMBER && ++Guard < 4000)
	{
		const FVector StartDir = mSplineComponent->GetDirectionAtDistanceAlongSpline(Start, ESplineCoordinateSpace::Local);
		float End = FMath::Min(Length, Start + MaxCollisionStepCm);
		while (End - Start > 25.f && FVector::DotProduct(StartDir, mSplineComponent->GetDirectionAtDistanceAlongSpline(End, ESplineCoordinateSpace::Local)) < CosLimit)
		{
			End = Start + (End - Start) * 0.5f;
		}
		const FVector A = mSplineComponent->GetLocationAtDistanceAlongSpline(Start, ESplineCoordinateSpace::Local);
		const FVector B = mSplineComponent->GetLocationAtDistanceAlongSpline(End, ESplineCoordinateSpace::Local);
		const float Chord = FVector::Distance(A, B);
		Start = End;
		if (Chord <= KINDA_SMALL_NUMBER) continue;
		const FMatrix Frame = FRotationMatrix::MakeFromXZ((B - A) / Chord, FVector::UpVector);
		const FVector Scale((Chord + 2.f * CollisionOverlapCm) / MeshSize.X, Width / MeshSize.Y, Thickness / MeshSize.Z);
		const FVector SlabCenter = (A + B) * 0.5f - Frame.GetUnitAxis(EAxis::Z) * (Thickness * 0.5f);
		const FQuat Rotation = Frame.ToQuat();
		// The mesh's own pivot may be off its bounds center; place the scaled bounds center on the slab center.
		mCollisionSlabs->AddInstance(FTransform(Rotation, SlabCenter - Rotation.RotateVector(MeshCenter * Scale), Scale), false);
	}
}

FTransform AMKIRoadSplineRoad::GetLightweightInstanceActorTransform_Implementation() const
{
	const FTransform Actor = GetActorTransform();
	UWorld* World = GetWorld();
	APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	const AFGCharacterPlayer* Player = Controller ? Cast<AFGCharacterPlayer>(Controller->GetPawn()) : nullptr;
	AFGBuildGun* BuildGun = Player ? Player->GetBuildGun() : nullptr;
	const UFGBuildGunStateBuild* Build = BuildGun ? Cast<UFGBuildGunStateBuild>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD)) : nullptr;
	const AFGHologram* Hologram = Build ? Build->GetHologram() : nullptr;
	// Any hologram but the road's own (it snaps by spline points): vehicles, foundations, walls, and machines snap to the
	// top of mHeight in this frame, so it is the deck under the aim.
	if (!Hologram || Hologram->IsA(AMKIRoadSplineRoadHologram::StaticClass()) || !mSplineComponent) return Actor;
	// The deck point under the local player's aim: the view ray against this road's collision.
	FVector Eye; FRotator View;
	Controller->GetPlayerViewPoint(Eye, View);
	FHitResult Hit;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(MKIRoadVehicleFloor), false);
	bool bHit = false;
	for (UStaticMeshComponent* Slab : { static_cast<UStaticMeshComponent*>(mCollisionSlabs.Get()) })
	{
		FHitResult SlabHit;
		if (Slab && Slab->LineTraceComponent(SlabHit, Eye, Eye + View.Vector() * 20000.f, Query) && (!bHit || SlabHit.Time < Hit.Time)) { Hit = SlabHit; bHit = true; }
	}
	if (!bHit) return Actor;
	// The deck is level crosswise, so its top under the aim is the centerline height there; the frame turns with the road.
	const float Key = mSplineComponent->FindInputKeyClosestToWorldLocation(Hit.Location);
	const FVector Deck = mSplineComponent->GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World);
	const FVector Along = mSplineComponent->GetDirectionAtSplineInputKey(Key, ESplineCoordinateSpace::World);
	// Centered as a vanilla foundation is: its top (the center plus mHeight / 2) is the deck. Live, a zero-height volume
	// at the deck top put walls 4 m below the deck.
	return FTransform(FRotator(0.f, FMath::RadiansToDegrees(FMath::Atan2(Along.Y, Along.X)), 0.f), FVector(Hit.Location.X, Hit.Location.Y, Deck.Z - mHeight * 0.5f));
}

int32 AMKIRoadSplineRoad::GetDismantleRefundReturnsMultiplier() const
{
	return FoundationEquivalents(GetSplineLengthCm(), mLanes);
}

void AMKIRoadSplineRoad::Dismantle_Implementation()
{
	if (HasAuthority())
	{
		for (AFGVehiclePathSegment* Path : mAutoPaths) if (IsValid(Path) && !Path->IsActorBeingDestroyed()) IFGDismantleInterface::Execute_Dismantle(Path);
		mAutoPaths.Reset();
	}
	Super::Dismantle_Implementation();
}

void AMKIRoadSplineRoad::StartIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* ByCharacter)
{
	Super::StartIsLookedAtForDismantle_Implementation(ByCharacter);
	for (AFGVehiclePathSegment* Path : mAutoPaths) if (IsValid(Path)) Path->TogglePendingDismantleMaterial(true);
}

void AMKIRoadSplineRoad::StopIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* ByCharacter)
{
	Super::StopIsLookedAtForDismantle_Implementation(ByCharacter);
	for (AFGVehiclePathSegment* Path : mAutoPaths) if (IsValid(Path)) Path->TogglePendingDismantleMaterial(false);
}

void AMKIRoadSplineRoad::GetChildDismantleActors_Implementation(TArray<AActor*>& OutChildDismantleActors) const
{
	Super::GetChildDismantleActors_Implementation(OutChildDismantleActors);
	if (!GetWorld()) return;
	// Supports (#194).
	for (TActorIterator<AMKIRoadSupports> It(GetWorld()); It; ++It) if (It->GetRoad() == this) OutChildDismantleActors.AddUnique(*It);
	// Its vehicle paths (#235) are not children: it removes them itself (Dismantle_Implementation, #244).
}

FBox AMKIRoadSplineRoad::CalculateBounds() const
{
	FBox Box(ForceInit);
	if (!mSplineComponent) return Box;
	const float Length = mSplineComponent->GetSplineLength();
	const float Pad = GetDeckWidthCm() * 0.5f;
	for (float Distance = 0.f;; Distance = FMath::Min(Length, Distance + 200.f))
	{
		const FVector Point = mSplineComponent->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		Box += Point + FVector(Pad, Pad, 0.f);
		Box += Point - FVector(Pad, Pad, mDeckThicknessCm);
		if (Distance >= Length) break;
	}
	return Box;
}

bool AMKIRoadSplineRoad::NearEnd(const AMKIRoadSplineRoad& Road, const FVector& Aim, FVector& OutEnd, FVector& OutOutward)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Band = SnapBandCm, HalfWidth = Road.GetDeckWidthCm() * 0.5f;
	float Best = TNumericLimits<float>::Max();
	for (const int32 Point : { 0, Spline->GetNumberOfSplinePoints() - 1 })
	{
		const FVector End = Spline->GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World);
		const FVector Along = Spline->GetDirectionAtSplinePoint(Point, ESplineCoordinateSpace::World);
		const FVector Outward = Point == 0 ? -Along : Along;
		const FVector Flat = FVector(Outward.X, Outward.Y, 0.f).GetSafeNormal();
		if (Flat.IsNearlyZero()) continue;
		const FVector ToAim(Aim.X - End.X, Aim.Y - End.Y, 0.f);
		const float In = -FVector::DotProduct(ToAim, Flat);
		const float Across = FMath::Abs(FVector::DotProduct(ToAim, FVector(-Flat.Y, Flat.X, 0.f)));
		if (In > Band || Across > HalfWidth + 100.f) continue;
		const float Distance = FMath::Abs(In);
		if (Distance >= Best) continue;
		Best = Distance;
		OutEnd = End;
		OutOutward = Outward.GetSafeNormal();
	}
	return Best < TNumericLimits<float>::Max();
}

TSubclassOf<UFGConstructDisqualifier> AMKIRoadSplineRoad::PieceClearanceDisqualifier(EClearanceOverlapResult Result, const AActor* Other, TSubclassOf<UFGConstructDisqualifier> Vanilla)
{
	if (Result == EClearanceOverlapResult::COR_None || !Other) return Vanilla;
	// Vehicle paths run on and across roads: never an obstacle.
	if (Other->IsA<AFGVehiclePathSegment>() || Other->IsA<AFGVehiclePathNode>()) return nullptr;
	return UFGCDEncroachingClearance::StaticClass();
}

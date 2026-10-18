#include "MKIRoadBuildSnap.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"

#include "MKIRoadJunction.h"

#include "MKIRoadSplineRoad.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "FGFactoryLegsComponent.h"
#include "Hologram/FGBuildableHologram.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Buildables/FGBuildableWall.h"
#include "Buildables/FGBuildableWalkway.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Hologram/FGBuildableHologram.h"
#include "FGRecipe.h"

namespace
{
/**
 * The side of the road a world location is on: the left or right third of the deck, or its middle third (the centerline),
 * across the road at the spline point nearest it. With bOutsideEdge, only left or right of the centerline.
 */
EMKIRoadSide RoadSideAt(const AMKIRoadSplineRoad& Road, const FVector& WorldLocation, bool bOutsideEdge = false)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return EMKIRoadSide::Right;
	const float Key = Spline->FindInputKeyClosestToWorldLocation(WorldLocation);
	const FVector Center = Spline->GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World);
	const FVector Along = Spline->GetDirectionAtSplineInputKey(Key, ESplineCoordinateSpace::World);
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Along).GetSafeNormal2D();
	const float Lateral = FVector::DotProduct(WorldLocation - Center, Right);
	if (bOutsideEdge) return Lateral < 0.f ? EMKIRoadSide::Left : EMKIRoadSide::Right;
	const float Third = Road.GetDeckWidthCm() / 6.f;
	return Lateral < -Third ? EMKIRoadSide::Left : (Lateral > Third ? EMKIRoadSide::Right : EMKIRoadSide::Center);
}
}
#include "Hologram/FGFoundationHologram.h"
#include "Hologram/FGWalkwayHologram.h"

float FMKIRoadBuildSnap::SnapSpanStart(float Center, float Length)
{
	return FMath::RoundToFloat((Center - Length * 0.5f) / StepCm) * StepCm;
}

namespace
{
	void ForgetSnappedRoad(const AFGHologram* Hologram);
}

bool FMKIRoadBuildSnap::Snap(const AMKIRoadSplineRoad& Road, const FVector& Hit, const FVector& HitNormal, const FMKIRoadSnapPiece& Piece, float ExtraYawDeg, FTransform& Out, EMKIRoadSnapMode* OutMode)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Length = Spline->GetSplineLength(), HalfDeck = Road.GetDeckWidthCm() * 0.5f;
	if (Length < 1.f) return false;
	// The aim in the road's frame: distance along the centerline, lateral offset (right positive), height below the top.
	const float Key = Spline->FindInputKeyClosestToWorldLocation(Hit);
	const float Along = Spline->GetDistanceAlongSplineAtSplineInputKey(Key);
	auto Frame = [Spline](float Distance, FVector& Center, FVector& Forward, FVector& Right)
	{
		Center = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		Forward = Spline->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World).GetSafeNormal2D();
		Right = FVector::CrossProduct(FVector::UpVector, Forward);
	};
	FVector Center, Forward, Right;
	Frame(Along, Center, Forward, Right);
	float Lateral = FVector::DotProduct(Hit - Center, Right);
	const bool bBelowTop = Hit.Z < Center.Z - 10.f;

	// The mode: a side or end face by its normal; on the deck, an end's band, then the outer band, else the top.
	EMKIRoadSnapMode Mode = EMKIRoadSnapMode::Top;
	bool bAtStart = Along <= BandCm, bAtEnd = Along >= Length - BandCm;
	const FVector Normal = HitNormal.GetSafeNormal();
	if (!Normal.IsNearlyZero() && FMath::Abs(Normal.Z) < 0.5f)
	{
		if (FMath::Abs(FVector::DotProduct(Normal, Forward)) > 0.7f) { Mode = EMKIRoadSnapMode::End; bAtStart = Along < Length * 0.5f; bAtEnd = !bAtStart; }
		else { Mode = EMKIRoadSnapMode::Edge; Lateral = FVector::DotProduct(Normal, Right) >= 0.f ? HalfDeck : -HalfDeck; }
	}
	else if ((bAtStart || bAtEnd) && FMath::Abs(Lateral) <= HalfDeck) Mode = EMKIRoadSnapMode::End;
	else if (bBelowTop || FMath::Abs(Lateral) >= HalfDeck - BandCm) Mode = EMKIRoadSnapMode::Edge;
	if (OutMode) *OutMode = Mode;

	// Across the deck: the piece's sides on 2 m marks counted from the left edge, kept on the deck.
	auto Across = [&](float Wanted)
	{
		const float Width = Piece.WidthCm;
		if (Width >= 2.f * HalfDeck) return 0.f;
		const float Left = FMath::Clamp(SnapSpanStart(Wanted + HalfDeck, Width), 0.f, 2.f * HalfDeck - Width);
		return Left + Width * 0.5f - HalfDeck;
	};
	// Along the road: the piece's ends on 2 m marks counted from the start.
	auto AlongSnapped = [&](float Wanted, bool bKeepOnDeck)
	{
		const float Start = SnapSpanStart(Wanted, Piece.LengthCm);
		float Mid = Start + Piece.LengthCm * 0.5f;
		if (bKeepOnDeck && Length >= Piece.LengthCm) Mid = FMath::Clamp(Mid, Piece.LengthCm * 0.5f, Length - Piece.LengthCm * 0.5f);
		return FMath::Clamp(Mid, 0.f, Length);
	};

	FVector Location;
	float Top = 0.f;
	if (Mode == EMKIRoadSnapMode::End)
	{
		const float Distance = bAtStart ? 0.f : Length;
		Frame(Distance, Center, Forward, Right);
		const FVector Outward = bAtStart ? -Forward : Forward;
		Location = Center + Outward * (Piece.LengthCm * 0.5f) + Right * Across(Lateral);
		Top = Center.Z;
	}
	else
	{
		const float Mid = AlongSnapped(Along, Mode == EMKIRoadSnapMode::Top);
		Frame(Mid, Center, Forward, Right);
		const float Side = Lateral >= 0.f ? 1.f : -1.f;
		const float Offset = Mode == EMKIRoadSnapMode::Top ? Across(Lateral) : Side * (HalfDeck + Piece.WidthCm * 0.5f);
		Location = Center + Right * Offset;
		Top = Center.Z;
	}
	// Heights: on the top the piece rests on the deck; at an edge or end its top (a walkway's floor) is flush with it.
	if (Mode == EMKIRoadSnapMode::Top) Location.Z = Piece.bPivotOnFloor ? Top : Top + Piece.HeightCm * 0.5f;
	else Location.Z = Piece.bPivotOnFloor ? Top : Top - Piece.HeightCm * 0.5f;
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Forward.Y, Forward.X)) + FMath::RoundToFloat(ExtraYawDeg / 90.f) * 90.f;
	Out = FTransform(FRotator(0.f, Yaw, 0.f), Location);
	return true;
}

bool FMKIRoadBuildSnap::SnapDockingStation(const AMKIRoadSplineRoad& Road, const FVector& Hit, const FVector& HitNormal, FTransform& Out)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Length = Spline->GetSplineLength(), HalfDeck = Road.GetDeckWidthCm() * 0.5f;
	if (Length < 1.f) return false;
	const float Key = Spline->FindInputKeyClosestToWorldLocation(Hit);
	const float Along = FMath::Clamp(FMath::RoundToFloat(Spline->GetDistanceAlongSplineAtSplineInputKey(Key) / StepCm) * StepCm, 0.f, Length);
	const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
	const FVector Forward = Spline->GetDirectionAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World).GetSafeNormal2D();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
	// The edge: the side face aimed at, else the nearer half of the deck.
	const FVector Normal = HitNormal.GetSafeNormal();
	const bool bSideFace = !Normal.IsNearlyZero() && FMath::Abs(Normal.Z) < 0.5f && FMath::Abs(FVector::DotProduct(Normal, Forward)) <= 0.7f;
	const float Side = (bSideFace ? FVector::DotProduct(Normal, Right) : FVector::DotProduct(Hit - Center, Right)) >= 0.f ? 1.f : -1.f;
	// The bay on that edge's lane and the building (the station's -Y, the truck's left) outside it: at the right edge the truck
	// heads against the road's direction, at the left edge along it.
	const float Lane = FMath::Min(AMKIRoadSplineRoad::LaneWidthCm, 2.f * HalfDeck);
	const FVector Location = Center + Right * (Side * (HalfDeck - Lane * 0.5f));
	const FVector Heading = Side > 0.f ? -Forward : Forward;
	Out = FTransform(FRotator(0.f, FMath::RadiansToDegrees(FMath::Atan2(Heading.Y, Heading.X)), 0.f), Location);
	return true;
}

namespace
{
	/**
	 * A station snapped onto a depot dock or a road stands on the deck, so its legs are cleared (#321, the maintainer: placed by hand,
	 * a station's legs reached down under the platform; the vanilla placement had traced its feet before the snap moved it).
	 * The hologram's legs are protected; they are reached by reflection.
	 */
	void ClearHologramLegs(AFGHologram* Hologram)
	{
		static FObjectProperty* LegsProperty = FindFProperty<FObjectProperty>(AFGBuildableHologram::StaticClass(), TEXT("mLegs"));
		if (!LegsProperty || !Hologram || !Hologram->IsA<AFGBuildableHologram>()) return;
		if (UFGFactoryLegsComponent* Legs = Cast<UFGFactoryLegsComponent>(LegsProperty->GetObjectPropertyValue_InContainer(Hologram))) Legs->SetFeetOffsetsToZero();
	}
}

bool FMKIRoadBuildSnap::SnapDockingStationHologram(AFGHologram* Hologram, const FHitResult& HitResult)
{
	if (!Hologram) return false;
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	// A truck depot (#237): the docking point on its bay's center, the building outside the deck. The station's hologram
	// also hits vehicle paths, so a hit on a depot's own path counts as the depot. Live (#239): a path is hit up to about 2.5 m
	// above the deck (its collision), so a hit 1 m up missed the depot and the vanilla path snap placed the station off the dock.
	const AMKIRoadJunction* Depot = AMKIRoadJunction::FromActor(HitActor);
	if (!Depot && HitActor && HitActor->IsA<AFGVehiclePathSegment>())
	{
		for (TActorIterator<AMKIRoadJunction> It(HitActor->GetWorld()); It && !Depot; ++It)
		{
			if (!It->IsTruckDepot()) continue;
			const FVector Local = It->GetActorTransform().InverseTransformPosition(HitResult.Location);
			const FBox2D Box = AMKIRoadJunction::LayoutBounds(AMKIRoadJunction::Layout(It->GetArmCount(), It->GetLanes()));
			if (Box.ExpandBy(50.0).IsInside(FVector2D(Local.X, Local.Y)) && Local.Z >= -100.0 && Local.Z <= 600.0) Depot = *It;
		}
	}
	if (Depot && Depot->IsTruckDepot())
	{
		// The dock nearest the aim (#240: a depot on both sides has two).
		const int32 Dock = Depot->FindDepotDock(HitResult.Location, TNumericLimits<float>::Max());
		const TArray<FTransform> Docks = Depot->GetDepotDocks();
		if (!Docks.IsValidIndex(Dock)) return false;
		// The station's docking node on the dock (#249): live, a fluid station's node stands 88 cm along it from its pivot, so
		// its node missed the dock and the paths made their own; the pivot is moved back by the node's offset.
		FTransform Placed = Docks[Dock];
		const AFGBuildableDockingStation* Station = Hologram->GetBuildClass() ? Cast<AFGBuildableDockingStation>(Hologram->GetBuildClass()->GetDefaultObject()) : nullptr;
		if (Station && Station->GetDockingTargetNodeLocation())
		{
			const FVector Offset = Station->GetDockingTargetNodeLocation()->GetRelativeLocation();
			Placed.AddToTranslation(-Placed.GetRotation().RotateVector(FVector(Offset.X, Offset.Y, 0.0)));
		}
		Hologram->SetActorTransform(Placed);
		ClearHologramLegs(Hologram);
		return true;
	}
	const AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor);
	FTransform Snapped;
	if (!Road || !SnapDockingStation(*Road, HitResult.Location, HitResult.ImpactNormal, Snapped)) return false;
	Hologram->SetActorTransform(Snapped);
	ClearHologramLegs(Hologram);
	return true;
}

bool FMKIRoadBuildSnap::SnapHologram(AFGHologram* Hologram, const FHitResult& HitResult)
{
	if (!Hologram) return false;
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	const AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor);
	UClass* BuildClass = Hologram->GetBuildClass();
	if (!Road || !BuildClass) return false;
	FMKIRoadSnapPiece Piece;
	if (BuildClass->IsChildOf(AFGBuildableFoundation::StaticClass()))
	{
		// Level foundations only: a vanilla ramp (mElevation) keeps its own snap.
		const AFGBuildableFoundation* Foundation = GetDefault<AFGBuildableFoundation>(BuildClass);
		if (FMath::Abs(Foundation->mElevation) > KINDA_SMALL_NUMBER || Foundation->mDepth <= 0.f || Foundation->mWidth <= 0.f || Foundation->mHeight <= 0.f) return false;
		Piece.LengthCm = Foundation->mDepth; Piece.WidthCm = Foundation->mWidth; Piece.HeightCm = Foundation->mHeight; Piece.bPivotOnFloor = false;
	}
	else if (BuildClass->IsChildOf(AFGBuildableWalkway::StaticClass()))
	{
		// Straight walkways and catwalks (and their turns and crossings, all one square); ramps and stairs keep vanilla.
		const AFGBuildableWalkway* Walkway = GetDefault<AFGBuildableWalkway>(BuildClass);
		if (FMath::Abs(Walkway->mElevation) > KINDA_SMALL_NUMBER || Walkway->mSize <= 0.f) return false;
		Piece.LengthCm = Walkway->mSize; Piece.WidthCm = Walkway->mSize; Piece.HeightCm = 0.f; Piece.bPivotOnFloor = true;
	}
	else return false;
	ForgetSnappedRoad(Hologram);
	FTransform Snapped;
	EMKIRoadSnapMode Mode = EMKIRoadSnapMode::Top;
	if (!Snap(*Road, HitResult.Location, HitResult.ImpactNormal, Piece, static_cast<float>(Hologram->GetScrollRotateValue()), Snapped, &Mode)) return false;
	// The maintainer (#224): walkways and catwalks along a road's edge follow it, on its 4 m marks, and zoop along it.
	if (Mode == EMKIRoadSnapMode::Edge && SnapRoadWalkwayEdge(Hologram, *Road, HitResult.Location, HitResult.ImpactNormal)) return true;
	Hologram->SetActorTransform(Snapped);
	return true;
}

namespace
{
	const TCHAR* StreetLightClassPath = TEXT("/Game/FactoryGame/Buildable/Factory/StreetLight/Build_StreetLight.Build_StreetLight_C");
}

bool FMKIRoadBuildSnap::IsStreetLight(const UClass* BuildClass)
{
	return BuildClass && BuildClass->GetPathName() == StreetLightClassPath;
}

bool FMKIRoadBuildSnap::SnapStreetLight(const AMKIRoadSplineRoad& Road, const FVector& Hit, float ExtraYawDeg, FTransform& Out)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Length = Spline->GetSplineLength();
	if (Length < 1.f) return false;
	const EMKIRoadSide Side = RoadSideAt(Road, Hit);
	const float Half = Road.GetDeckWidthCm() * 0.5f;
	const float Offset = Side == EMKIRoadSide::Center ? 0.f : (Side == EMKIRoadSide::Left ? -(Half - LightEdgeInsetCm) : Half - LightEdgeInsetCm);
	const float Toward = Side == EMKIRoadSide::Right ? -1.f : 1.f;
	const float Key = Spline->FindInputKeyClosestToWorldLocation(Hit);
	const float Distance = FMath::Clamp(FMath::RoundToFloat(Spline->GetDistanceAlongSplineAtSplineInputKey(Key) / LightStepCm) * LightStepCm, 0.f, Length);
	const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Spline->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World)).GetSafeNormal2D();
	const FQuat Facing = FRotationMatrix::MakeFromYZ(Right * Toward, FVector::UpVector).ToQuat();
	Out = FTransform(FQuat(FVector::UpVector, FMath::DegreesToRadians(ExtraYawDeg)) * Facing, Center + Right * Offset);
	return true;
}

bool FMKIRoadBuildSnap::SnapStreetLightHologram(AFGHologram* Hologram, const FHitResult& HitResult)
{
	if (!Hologram) return false;
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	const AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor);
	if (!Road || !IsStreetLight(Hologram->GetBuildClass())) return false;
	FTransform Snapped;
	if (!SnapStreetLight(*Road, HitResult.Location, static_cast<float>(Hologram->GetScrollRotateValue()), Snapped)) return false;
	Hologram->SetActorTransform(Snapped);
	return true;
}

namespace
{
	/** Zoop reach for a road run hologram whose limit is still 0 (a railing on a road, #223). */
	constexpr int32 RoadRunDefaultMaxZoop = 10;

	/**
	 * The vanilla barriers and railings that snap to roads (#223). Chain link and tarp fences are left to vanilla placement:
	 * on a road they block the truck path beside them even straddling the edge (#312).
	 */
	const TCHAR* RoadWallClassPaths[] = {
		TEXT("/Game/FactoryGame/Buildable/Building/Fence/Build_Concrete_Barrier_01.Build_Concrete_Barrier_01_C"),
		TEXT("/Game/FactoryGame/Buildable/Building/Barrier/Build_Barrier_Low_01.Build_Barrier_Low_01_C"),
		TEXT("/Game/FactoryGame/Buildable/Building/Barrier/Build_Barrier_Tall_01.Build_Barrier_Tall_01_C"),
		TEXT("/Game/FactoryGame/Buildable/Building/Fence/Build_Railing_01.Build_Railing_01_C"),
		TEXT("/Game/FactoryGame/Buildable/Building/Fence/Build_Fence_01.Build_Fence_01_C"),
	};

	/** The bounds of the hologram's own (non-instanced) meshes, in its local space. */
	FBox PieceBounds(const AActor& Hologram)
	{
		FBox Box(ForceInit);
		TArray<UStaticMeshComponent*> Meshes;
		Hologram.GetComponents<UStaticMeshComponent>(Meshes);
		const FTransform ToActor = Hologram.GetActorTransform().Inverse();
		for (const UStaticMeshComponent* Mesh : Meshes)
		{
			if (!Mesh || Mesh->IsA<UInstancedStaticMeshComponent>() || !Mesh->GetStaticMesh()) continue;
			Box += Mesh->GetStaticMesh()->GetBoundingBox().TransformBy(Mesh->GetComponentTransform() * ToActor);
		}
		return Box;
	}

	/** The middle of the piece's footprint in its local space (on its pivot's level). */
	FVector FootprintMiddle(const AActor& Hologram)
	{
		const FBox Box = PieceBounds(Hologram);
		return Box.IsValid ? FVector(Box.GetCenter().X, Box.GetCenter().Y, 0.0) : FVector::ZeroVector;
	}

	/** A point LateralCm right of the road's centerline at Along (clamped to the road). */
	FVector OffsetPoint(const USplineComponent& Spline, float Along, float LateralCm)
	{
		const float Clamped = FMath::Clamp(Along, 0.f, Spline.GetSplineLength());
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Spline.GetDirectionAtDistanceAlongSpline(Clamped, ESplineCoordinateSpace::World)).GetSafeNormal2D();
		return Spline.GetLocationAtDistanceAlongSpline(Clamped, ESplineCoordinateSpace::World) + Right * LateralCm;
	}

	/**
	 * The maintainer (#223): on the switchback the pieces stood apart. Spaced a step apart along the centerline, the pieces on the
	 * outside of a bend left gaps (and overlapped on the inside). The piece from From toward Sign ends where its chord
	 * along the offset line is one step long, so pieces meet end to end on curves too.
	 */
	float ChainEnd(const USplineComponent& Spline, float From, float LateralCm, float Step, float Sign)
	{
		const FVector A = OffsetPoint(Spline, From, LateralCm);
		float To = From + Sign * Step;
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			const float Chord = static_cast<float>(FVector::Dist(A, OffsetPoint(Spline, To, LateralCm)));
			if (Chord <= KINDA_SMALL_NUMBER) break;
			To = From + (To - From) * Step / Chord;
		}
		return To;
	}

	/** The span of the piece whose middle is nearest Middle, its chord along the offset line one step long, on the road. */
	void CenteredSpan(const USplineComponent& Spline, float Middle, float LateralCm, float Step, float& OutStart, float& OutEnd)
	{
		float Half = Step * 0.5f;
		for (int32 Iteration = 0; Iteration < 8; ++Iteration)
		{
			const float Chord = static_cast<float>(FVector::Dist(OffsetPoint(Spline, Middle - Half, LateralCm), OffsetPoint(Spline, Middle + Half, LateralCm)));
			if (Chord <= KINDA_SMALL_NUMBER) break;
			Half *= Step / Chord;
		}
		const float Length = Spline.GetSplineLength();
		// Kept on the road: a span past an end moves back onto it.
		const float Shift = Middle - Half < 0.f ? Half - Middle : (Middle + Half > Length ? Length - Middle - Half : 0.f);
		OutStart = FMath::Max(0.f, Middle - Half + Shift);
		OutEnd = FMath::Min(Length, Middle + Half + Shift);
	}

	/** Where a road run hologram stands: its road, step, first piece's span and offset line, and its pose on that chord. */
	struct FRoadRunPlace
	{
		const AMKIRoadSplineRoad* Road = nullptr;
		float Step = 400.f;
		float Start = 0.f;
		float End = 400.f;
		float Lateral = 0.f;
		/** The hologram relative to its first piece's chord frame (pivot offset, turn, and side): every piece keeps it. */
		FTransform OnChord;
		/** The road's direction along the first piece. */
		FVector Forward = FVector::ForwardVector;
	};

	/** The span of piece Index from the first (negative: backward), or false where it would leave the road. */
	bool PieceSpan(const USplineComponent& Spline, const FRoadRunPlace& Place, int32 Index, float& OutStart, float& OutEnd)
	{
		OutStart = Place.Start;
		OutEnd = Place.End;
		const float Length = Spline.GetSplineLength();
		for (int32 Taken = 0; Taken < FMath::Abs(Index); ++Taken)
		{
			if (Index > 0) { OutStart = OutEnd; OutEnd = ChainEnd(Spline, OutStart, Place.Lateral, Place.Step, 1.f); }
			else { OutEnd = OutStart; OutStart = ChainEnd(Spline, OutEnd, Place.Lateral, Place.Step, -1.f); }
			if (OutStart < -1.f || OutEnd > Length + 1.f) return false;
		}
		return true;
	}

	/** The road each road run hologram snapped to: at an intersection, the road it was aimed at, not one beside it (#223). */
	TMap<TWeakObjectPtr<const AFGHologram>, TWeakObjectPtr<const AMKIRoadSplineRoad>>& SnappedRoads()
	{
		static TMap<TWeakObjectPtr<const AFGHologram>, TWeakObjectPtr<const AMKIRoadSplineRoad>> Roads;
		return Roads;
	}

	/** Forgets dead holograms, and Hologram's road until it snaps to one again. */
	void ForgetSnappedRoad(const AFGHologram* Hologram)
	{
		for (auto It = SnappedRoads().CreateIterator(); It; ++It) if (!It.Key().IsValid() || It.Key().Get() == Hologram) It.RemoveCurrent();
	}

	/**
	 * Measures Point against Road: true when it is beside its deck (within the deck and a walkway's width outside it, not past
	 * an end) and within 50 cm of its top.
	 */
	bool PointByRoad(const AMKIRoadSplineRoad& Road, const FVector& Point, float& OutAlong, float& OutLateral, float& OutHeight)
	{
		const USplineComponent* Spline = Road.GetSplineComponent();
		if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
		OutAlong = Spline->GetDistanceAlongSplineAtSplineInputKey(Spline->FindInputKeyClosestToWorldLocation(Point));
		const FVector Center = Spline->GetLocationAtDistanceAlongSpline(OutAlong, ESplineCoordinateSpace::World);
		const FVector Forward = Spline->GetDirectionAtDistanceAlongSpline(OutAlong, ESplineCoordinateSpace::World).GetSafeNormal2D();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal2D();
		OutLateral = static_cast<float>(FVector::DotProduct(Point - Center, Right));
		OutHeight = static_cast<float>(FMath::Abs(Point.Z - Center.Z));
		// Past an end the nearest point is the end itself, and the point lies ahead of it along the road.
		const float Ahead = static_cast<float>(FMath::Abs(FVector::DotProduct(Point - Center, Forward)));
		return FMath::Abs(OutLateral) <= Road.GetDeckWidthCm() * 0.5f + 600.f && OutHeight <= 50.f && Ahead <= 100.f;
	}

	/** The chord frame of a span: X along the chord (pitched with the grade), Z up, at its middle on the offset line. */
	bool ChordFrame(const USplineComponent& Spline, float Start, float End, float LateralCm, FTransform& Out)
	{
		const FVector A = OffsetPoint(Spline, Start, LateralCm), B = OffsetPoint(Spline, End, LateralCm);
		const FVector Forward = (B - A).GetSafeNormal();
		if (Forward.IsNearlyZero()) return false;
		Out = FTransform(FRotationMatrix::MakeFromXZ(Forward, FVector::UpVector).ToQuat(), (A + B) * 0.5f);
		return true;
	}

	/**
	 * Where a road run hologram stands: the road it snapped to, or else (the hologram that constructs) the road beside which
	 * its footprint's middle lies, nearest in height, preferring the road running along the piece where roads meet.
	 */
	bool FindRoadRunPlace(const AFGHologram& Hologram, FRoadRunPlace& Out)
	{
		if (!FMKIRoadBuildSnap::IsRoadRun(Hologram.GetBuildClass(), &Out.Step) || !Hologram.GetWorld()) return false;
		const FVector Middle = Hologram.GetActorTransform().TransformPosition(FootprintMiddle(Hologram));
		float Along = 0.f, Lateral = 0.f, Height = 0.f;
		const AMKIRoadSplineRoad* Snapped = SnappedRoads().FindRef(&Hologram).Get();
		if (Snapped && PointByRoad(*Snapped, Middle, Along, Lateral, Height)) Out.Road = Snapped;
		else
		{
			float Best = TNumericLimits<float>::Max();
			for (TActorIterator<AMKIRoadSplineRoad> It(Hologram.GetWorld()); It; ++It)
			{
				float ItAlong = 0.f, ItLateral = 0.f, ItHeight = 0.f;
				if (!PointByRoad(**It, Middle, ItAlong, ItLateral, ItHeight)) continue;
				const FVector ItForward = It->GetSplineComponent()->GetDirectionAtDistanceAlongSpline(ItAlong, ESplineCoordinateSpace::World).GetSafeNormal2D();
				const float Aligned = static_cast<float>(FMath::Max(FMath::Abs(FVector::DotProduct(Hologram.GetActorForwardVector(), ItForward)), FMath::Abs(FVector::DotProduct(Hologram.GetActorRightVector(), ItForward))));
				const float Score = ItHeight + 100.f * (1.f - Aligned);
				if (Score >= Best) continue;
				Best = Score;
				Out.Road = *It;
				Along = ItAlong;
				Lateral = ItLateral;
			}
		}
		if (!Out.Road) return false;
		const USplineComponent& Spline = *Out.Road->GetSplineComponent();
		Out.Lateral = Lateral;
		// The footprint's middle is the first piece's chord middle: the piece is the one centered there.
		CenteredSpan(Spline, Along, Lateral, Out.Step, Out.Start, Out.End);
		FTransform Chord;
		if (!ChordFrame(Spline, Out.Start, Out.End, Out.Lateral, Chord)) return false;
		Out.OnChord = Hologram.GetActorTransform().GetRelativeTransform(Chord);
		Out.Forward = Chord.GetUnitAxis(EAxis::X);
		return true;
	}

	/** Places Hologram on the chord of [Start, End] along LateralCm, turned YawDeg on it, its footprint's middle on the chord's. */
	bool PlaceOnChord(AFGHologram& Hologram, const AMKIRoadSplineRoad& Road, float Start, float End, float LateralCm, float YawDeg)
	{
		FTransform Chord;
		if (!ChordFrame(*Road.GetSplineComponent(), Start, End, LateralCm, Chord)) return false;
		const FQuat Rotation = Chord.GetRotation() * FQuat(FVector::UpVector, FMath::DegreesToRadians(YawDeg));
		Hologram.SetActorTransform(FTransform(Rotation, Chord.GetLocation() - Rotation.RotateVector(FootprintMiddle(Hologram))));
		SnappedRoads().Add(&Hologram, &Road);
		return true;
	}
}

bool FMKIRoadBuildSnap::IsRoadWall(const UClass* BuildClass, float* OutStepCm)
{
	if (!BuildClass || !BuildClass->IsChildOf(AFGBuildableWall::StaticClass())) return false;
	const FString Path = BuildClass->GetPathName();
	for (const TCHAR* Candidate : RoadWallClassPaths)
	{
		if (Path != Candidate) continue;
		const float Width = GetDefault<AFGBuildableWall>(const_cast<UClass*>(BuildClass))->mWidth;
		if (OutStepCm) *OutStepCm = Width > 1.f ? Width : 400.f;
		return true;
	}
	return false;
}

bool FMKIRoadBuildSnap::IsRoadWalkway(const UClass* BuildClass, float* OutStepCm)
{
	if (!BuildClass || !BuildClass->IsChildOf(AFGBuildableWalkway::StaticClass())) return false;
	const AFGBuildableWalkway* Walkway = GetDefault<AFGBuildableWalkway>(const_cast<UClass*>(BuildClass));
	// Level pieces only (straights, corners, T-junctions, crosses); ramps and stairs keep vanilla.
	if (FMath::Abs(Walkway->mElevation) > KINDA_SMALL_NUMBER || Walkway->mSize <= 1.f) return false;
	if (OutStepCm) *OutStepCm = Walkway->mSize;
	return true;
}

bool FMKIRoadBuildSnap::IsRoadRun(const UClass* BuildClass, float* OutStepCm)
{
	return IsRoadWall(BuildClass, OutStepCm) || IsRoadWalkway(BuildClass, OutStepCm);
}

bool FMKIRoadBuildSnap::SnapRoadWallHologram(AFGHologram* Hologram, const FHitResult& HitResult)
{
	if (!Hologram) return false;
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	const AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor);
	float Step = 400.f;
	const bool bRoadWall = IsRoadWall(Hologram->GetBuildClass(), &Step);
	ForgetSnappedRoad(Hologram);
	if (!Road || !bRoadWall) return false;
	const USplineComponent* Spline = Road->GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Along = Spline->GetDistanceAlongSplineAtSplineInputKey(Spline->FindInputKeyClosestToWorldLocation(HitResult.Location));
	const EMKIRoadSide Side = RoadSideAt(*Road, HitResult.Location);
	// +X away from the centerline (to the right on it); half a turn of scroll rotation turns the piece round.
	const bool bTurned = ((FMath::RoundToInt(Hologram->GetScrollRotateValue() / 180.f) % 2) + 2) % 2 == 1;
	const float XSign = (Side == EMKIRoadSide::Left ? -1.f : 1.f) * (bTurned ? -1.f : 1.f);
	// The piece's middle on the edge, straddling it as vanilla walls straddle a foundation's edge (#312: pulled fully onto
	// the deck, the pieces narrowed the lanes and trucks found no path), or on the centerline.
	const float HalfDeck = Road->GetDeckWidthCm() * 0.5f;
	const float Lateral = Side == EMKIRoadSide::Center ? 0.f : (Side == EMKIRoadSide::Left ? -HalfDeck : HalfDeck);
	// The piece on the road's step marks (from its start) under the aim. Its length axis (+Y, as vanilla walls zoop) lies
	// along the chord: turned 90° on it, +X points to the road's right (XSign 1) or left.
	float Start = 0.f, End = 0.f;
	CenteredSpan(*Spline, (FMath::FloorToFloat(Along / Step) + 0.5f) * Step, Lateral, Step, Start, End);
	return PlaceOnChord(*Hologram, *Road, Start, End, Lateral, 90.f * XSign);
}

bool FMKIRoadBuildSnap::SnapRoadWalkwayEdge(AFGHologram* Hologram, const AMKIRoadSplineRoad& Road, const FVector& Hit, const FVector& HitNormal)
{
	float Step = 400.f;
	if (!Hologram || !IsRoadWalkway(Hologram->GetBuildClass(), &Step)) return false;
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Along = Spline->GetDistanceAlongSplineAtSplineInputKey(Spline->FindInputKeyClosestToWorldLocation(Hit));
	const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Spline->GetDirectionAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World)).GetSafeNormal2D();
	// The edge: the side face aimed at, else the nearer half of the deck (FMKIRoadBuildSnap::Snap chose Edge already).
	const FVector Normal = HitNormal.GetSafeNormal();
	const bool bSideFace = !Normal.IsNearlyZero() && FMath::Abs(Normal.Z) < 0.5f;
	const float Side = (bSideFace ? FVector::DotProduct(Normal, Right) : FVector::DotProduct(Hit - Center, Right)) >= 0.f ? 1.f : -1.f;
	// Outside the edge, against it: the square's middle half a piece out. Its floor (the pivot) is flush with the deck top.
	const float Lateral = Side * (Road.GetDeckWidthCm() * 0.5f + Step * 0.5f);
	float Start = 0.f, End = 0.f;
	CenteredSpan(*Spline, (FMath::FloorToFloat(Along / Step) + 0.5f) * Step, Lateral, Step, Start, End);
	// Turned with the road, and by the scroll rotation in quarter turns (corners and T-junctions face either way).
	return PlaceOnChord(*Hologram, Road, Start, End, Lateral, FMath::RoundToFloat(Hologram->GetScrollRotateValue() / 90.f) * 90.f);
}

bool FMKIRoadBuildSnap::RoadRunZoopFromHit(AFGHologram* Hologram, const FHitResult& HitResult, FIntVector& OutZoop)
{
	FRoadRunPlace Place;
	AFGBuildableHologram* Buildable = Cast<AFGBuildableHologram>(Hologram);
	if (!Buildable || !FindRoadRunPlace(*Buildable, Place)) return false;
	// The local axis the zoop may run along (Holo_Railing and Holo_WalkwayT block X, Holo_WalkwayStraight blocks Y; read
	// from the paks) that lies along the road. Neither: vanilla's zoop stands.
	const FByteProperty* BlockedProperty = FindFProperty<FByteProperty>(AFGBuildableHologram::StaticClass(), TEXT("mDefaultBlockedZoopDirections"));
	const uint8 Blocked = BlockedProperty ? BlockedProperty->GetPropertyValue_InContainer(Buildable) : 0;
	const bool bX = (Blocked & (static_cast<uint8>(EHologramZoopDirections::HZD_Forward) | static_cast<uint8>(EHologramZoopDirections::HZD_Backward))) == 0;
	const bool bY = (Blocked & (static_cast<uint8>(EHologramZoopDirections::HZD_Left) | static_cast<uint8>(EHologramZoopDirections::HZD_Right))) == 0;
	const float AlongX = bX ? static_cast<float>(FVector::DotProduct(Buildable->GetActorForwardVector(), Place.Forward)) : 0.f;
	const float AlongY = bY ? static_cast<float>(FVector::DotProduct(Buildable->GetActorRightVector(), Place.Forward)) : 0.f;
	const bool bUseX = FMath::Abs(AlongX) >= FMath::Abs(AlongY);
	const float Alignment = bUseX ? AlongX : AlongY;
	if (FMath::Abs(Alignment) < 0.7f) return false;
	// A railing's limit may still be 0 here (it never zooped on a road, #223); it gets the barriers' vanilla reach.
	if (Buildable->GetMaxZoopAmount() <= 0) Buildable->SetMaxZoopAmount(RoadRunDefaultMaxZoop);
	const int32 Max = FMath::Max(0, Buildable->GetMaxZoopAmount());
	// Whole pieces toward the aim, chained along the road, each kept on it, and no more than vanilla's zoop limit.
	const USplineComponent& Spline = *Place.Road->GetSplineComponent();
	const FVector Aim = HitResult.bBlockingHit ? HitResult.Location : HitResult.TraceEnd;
	const float AimAlong = Spline.GetDistanceAlongSplineAtSplineInputKey(Spline.FindInputKeyClosestToWorldLocation(Aim));
	const float Sign = AimAlong >= (Place.Start + Place.End) * 0.5f ? 1.f : -1.f;
	int32 Pieces = 0;
	float Start = Place.Start, End = Place.End;
	while (Pieces < Max)
	{
		float NextStart = 0.f, NextEnd = 0.f;
		if (!PieceSpan(Spline, Place, (Pieces + 1) * static_cast<int32>(Sign), NextStart, NextEnd)) break;
		// Stop once the aim is nearer this piece's middle than the next one's.
		if (FMath::Abs(AimAlong - (Start + End) * 0.5f) <= FMath::Abs(AimAlong - (NextStart + NextEnd) * 0.5f)) break;
		Start = NextStart;
		End = NextEnd;
		++Pieces;
	}
	const int32 Local = Pieces * static_cast<int32>(Sign) * (Alignment >= 0.f ? 1 : -1);
	OutZoop = bUseX ? FIntVector(Local, 0, 0) : FIntVector(0, Local, 0);
	return true;
}

bool FMKIRoadBuildSnap::RoadRunZoopInstance(const AFGHologram* Hologram, const FTransform& VanillaInstance, FTransform& OutInstance)
{
	FRoadRunPlace Place;
	if (!Hologram || !FindRoadRunPlace(*Hologram, Place)) return false;
	// Vanilla puts instance k k pieces along the zoop axis; that axis, turned to the world, says which way along the road.
	const FVector Offset = VanillaInstance.GetLocation();
	int32 Index = 0;
	if (Offset.Size() > 1.0)
	{
		const float Along = static_cast<float>(FVector::DotProduct(Hologram->GetActorTransform().TransformVectorNoScale(Offset).GetSafeNormal(), Place.Forward));
		if (FMath::Abs(Along) < 0.7f) return false;
		Index = FMath::RoundToInt(static_cast<float>(Offset.Size()) / Place.Step) * (Along >= 0.f ? 1 : -1);
	}
	const USplineComponent& Spline = *Place.Road->GetSplineComponent();
	float Start = 0.f, End = 0.f;
	FTransform Chord;
	if (!PieceSpan(Spline, Place, Index, Start, End) || !ChordFrame(Spline, Start, End, Place.Lateral, Chord)) return false;
	// Each piece keeps the hologram's pose on its own chord.
	OutInstance = (Place.OnChord * Chord).GetRelativeTransform(Hologram->GetActorTransform());
	return true;
}


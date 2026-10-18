#include "MKIRoadLanePath.h"

#include "MKIRoadSplineRoad.h"
#include "Components/SplineComponent.h"
#include "EngineUtils.h"
#include "Hologram/FGVehiclePathSegmentHologram.h"

namespace
{
	/** Reaches the vanilla path hologram's protected state (its planned points, build step, and their refresh). */
	struct FPathHologramAccess : public AFGVehiclePathSegmentHologram
	{
		static TArray<FSplinePointData>& SplineData(AFGVehiclePathSegmentHologram* Hologram) { return static_cast<FPathHologramAccess*>(Hologram)->mSplineData; }
		static bool IsEndStep(AFGVehiclePathSegmentHologram* Hologram) { return static_cast<FPathHologramAccess*>(Hologram)->mBuildStep == EVehiclePathBuildStep::EndPoint; }
		/** True when the start is on an existing path node (the first of the snapped nodes). */
		static bool IsStartOnNode(AFGVehiclePathSegmentHologram* Hologram) { return static_cast<FPathHologramAccess*>(Hologram)->mSnappedPathNodes[0] != nullptr; }
		static void Refresh(AFGVehiclePathSegmentHologram* Hologram)
		{
			FPathHologramAccess* Access = static_cast<FPathHologramAccess*>(Hologram);
			Access->OnRep_SplineData();
			Access->UpdatePathValidationInfoFromSplineComponent();
			Access->UpdateHologramVisualState();
		}
	};

	AMKIRoadSplineRoad* RoadOf(const FHitResult& HitResult)
	{
		AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
		if (!HitActor) HitActor = HitResult.GetActor();
		return Cast<AMKIRoadSplineRoad>(HitActor);
	}

	/** Distance along Road and lateral offset of a world point, and whether it is on the deck. */
	bool OnDeck(const AMKIRoadSplineRoad& Road, const FVector& Point, float& OutAlong, float& OutLateral)
	{
		const USplineComponent* Spline = Road.GetSplineComponent();
		if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
		const float Key = Spline->FindInputKeyClosestToWorldLocation(Point);
		OutAlong = Spline->GetDistanceAlongSplineAtSplineInputKey(Key);
		const FVector Center = Spline->GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World);
		const FVector Forward = Spline->GetDirectionAtSplineInputKey(Key, ESplineCoordinateSpace::World).GetSafeNormal2D();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
		OutLateral = FVector::DotProduct(Point - Center, Right);
		// Beside the road, not past an end: the closest point clamps to an end, so a point beyond it (live, a path started
		// on the next road) projects ahead of or behind it there.
		const float Beyond = FMath::Abs(FVector::DotProduct(Point - Center, Forward));
		return Beyond <= FMKIRoadLanePath::EndToleranceCm && FMath::Abs(OutLateral) <= Road.GetDeckWidthCm() * 0.5f && FMath::Abs(Point.Z - Center.Z) <= Road.GetDeckThicknessCm() + 100.f;
	}
}

float FMKIRoadLanePath::SnapToRoadEnd(float Along, float Length)
{
	if (Along <= RoadEndSnapCm) return 0.f;
	if (Along >= Length - RoadEndSnapCm) return Length;
	return Along;
}

float FMKIRoadLanePath::LaneCenterCm(int32 Lanes, float LateralCm)
{
	const float Width = AMKIRoadSplineRoad::LaneWidthCm, Half = Lanes * Width * 0.5f;
	const int32 Lane = FMath::Clamp(FMath::FloorToInt32((LateralCm + Half) / Width), 0, FMath::Max(0, Lanes - 1));
	return -Half + (Lane + 0.5f) * Width;
}

bool FMKIRoadLanePath::PlanLane(const AMKIRoadSplineRoad& Road, const FVector& Start, const FVector& End, TArray<FSplinePointData>& OutWorldPoints)
{
	OutWorldPoints.Reset();
	float StartAlong, StartLateral, EndAlong, EndLateral;
	if (!OnDeck(Road, Start, StartAlong, StartLateral) || !OnDeck(Road, End, EndAlong, EndLateral)) return false;
	EndAlong = SnapToRoadEnd(EndAlong, Road.GetSplineComponent()->GetSplineLength());
	const float Span = EndAlong - StartAlong;
	if (FMath::Abs(Span) < PointStepCm) return false;
	const USplineComponent* Spline = Road.GetSplineComponent();
	const float Lane = LaneCenterCm(Road.GetLanes(), StartLateral), Sign = Span >= 0.f ? 1.f : -1.f;
	// The line leaves from the start's own offset and eases onto the lane center over EaseCm (smoothstep), so a start on an
	// existing node beside the center has no kink (live, a 54 cm jump failed the vehicle floor check).
	const float Offset = StartLateral - Lane;
	// Aimed into another lane, the path changes lanes or maneuvers (live, an S-curve across a truck apron): vanilla routes it.
	if (FMath::Abs(EndLateral - Lane) > AMKIRoadSplineRoad::LaneWidthCm * 0.5f) return false;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FMath::Abs(Span) / PointStepCm));
	const float Step = Span / Steps;
	for (int32 Index = 0; Index <= Steps; ++Index)
	{
		const float Along = StartAlong + Step * Index;
		const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
		const FVector Direction = Spline->GetDirectionAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Direction).GetSafeNormal2D();
		// The lane line keeps the road's grade; travel runs from the start toward the end.
		const FVector Tangent = Direction * Sign * FMath::Abs(Step);
		const float Ease = FMath::Clamp(FMath::Abs(Along - StartAlong) / EaseCm, 0.f, 1.f);
		const float Lateral = Lane + Offset * (1.f - Ease * Ease * (3.f - 2.f * Ease));
		OutWorldPoints.Add(FSplinePointData(Center + Right * Lateral + FVector(0.f, 0.f, LiftCm), Tangent));
	}
	return true;
}

void FMKIRoadLanePath::AfterSetHologramLocation(AFGVehiclePathSegmentHologram* Hologram, const FHitResult& HitResult)
{
	if (!Hologram) return;
	AMKIRoadSplineRoad* Road = RoadOf(HitResult);
	if (!Road) return;
	if (!FPathHologramAccess::IsEndStep(Hologram))
	{
		// The start: on the center of the lane under the aim, on the deck.
		float Along, Lateral;
		if (!OnDeck(*Road, Hologram->GetActorLocation(), Along, Lateral)) return;
		const USplineComponent* Spline = Road->GetSplineComponent();
		Along = SnapToRoadEnd(Along, Spline->GetSplineLength());
		const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Spline->GetDirectionAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World)).GetSafeNormal2D();
		Hologram->SetActorLocation(Center + Right * LaneCenterCm(Road->GetLanes(), Lateral) + FVector(0.f, 0.f, LiftCm));
		return;
	}
	// The end on the same road: the lane's own line, in the hologram's space (its actor is at the start).
	TArray<FSplinePointData> World;
	if (!PlanLane(*Road, Hologram->GetActorLocation(), HitResult.Location, World)) return;
	// The line leaves from the start itself (a free start, or an existing node), easing onto the lane.
	World[0].Location = Hologram->GetActorLocation();
	const FTransform Frame = Hologram->GetActorTransform();
	TArray<FSplinePointData>& Points = FPathHologramAccess::SplineData(Hologram);
	// A start on an existing node leaves in that node's direction (vanilla's plan starts along it): the lane is followed
	// only when that direction is within MaxNodeTurnDeg of the lane's travel; otherwise the path is a maneuver (a turn or
	// U-turn on a wide apron, live #192 showcase) and keeps vanilla's routing instead of kinking onto the lane.
	if (FPathHologramAccess::IsStartOnNode(Hologram) && Points.Num() >= 2 && World.Num() >= 2)
	{
		const FVector VanillaStart = Frame.TransformVector(Points[0].LeaveTangent).GetSafeNormal2D();
		const FVector LaneStart = (World[1].Location - World[0].Location).GetSafeNormal2D();
		if (!VanillaStart.IsNearlyZero() && FVector::DotProduct(VanillaStart, LaneStart) < FMath::Cos(FMath::DegreesToRadians(MaxNodeTurnDeg))) return;
	}
	Points.Reset(World.Num());
	for (const FSplinePointData& Point : World)
	{
		const FVector Tangent = Frame.InverseTransformVector(Point.ArriveTangent);
		Points.Add(FSplinePointData(Frame.InverseTransformPosition(Point.Location), Tangent));
	}
	FPathHologramAccess::Refresh(Hologram);
}

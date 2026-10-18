#include "MKIRoadAutoPaths.h"

#include "MKIRoadJunction.h"
#include "MKIRoadsGameWorldModule.h"
#include "MKIRoadSplineRoad.h"
#include "Components/SplineComponent.h"
#include "FGBuildableSubsystem.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"

namespace
{
	const TCHAR* UniversalPathClassPath = TEXT("/Game/FactoryGame/Buildable/Vehicle/VehiclePath/Build_VehiclePath_Universal.Build_VehiclePath_Universal_C");

	/** Points along Spline at Lateral (right positive), from distance From to To (either way), PointStepCm apart. */
	TArray<FSplinePointData> LaneLine(const USplineComponent& Spline, float Lateral, float From, float To)
	{
		TArray<FSplinePointData> Points;
		const float Span = To - From, Sign = Span >= 0.f ? 1.f : -1.f;
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32(FMath::Abs(Span) / FMKIRoadAutoPaths::PointStepCm), 1, 4000);
		const float Step = Span / Steps;
		for (int32 Index = 0; Index <= Steps; ++Index)
		{
			const float Along = From + Step * Index;
			const FVector Center = Spline.GetLocationAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
			const FVector Direction = Spline.GetDirectionAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Direction).GetSafeNormal2D();
			Points.Add(FSplinePointData(Center + Right * Lateral + FVector(0.f, 0.f, FMKIRoadAutoPaths::LiftCm), Direction * Sign * FMath::Abs(Step)));
		}
		return Points;
	}
}

void FMKIRoadAutoPaths::RoadPaths(const AMKIRoadSplineRoad& Road, TArray<TArray<FSplinePointData>>& OutPaths)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return;
	const float Length = Spline->GetSplineLength();
	if (Length < PointStepCm) return;
	const int32 Lanes = Road.GetLanes();
	const float Half = Lanes * AMKIRoadSplineRoad::LaneWidthCm * 0.5f;
	for (int32 Lane = 0; Lane < Lanes; ++Lane)
	{
		const float Lateral = -Half + (Lane + 0.5f) * AMKIRoadSplineRoad::LaneWidthCm;
		// Traffic keeps right: right of the centerline runs forward, left of it back.
		OutPaths.Add(Lateral > 0.f ? LaneLine(*Spline, Lateral, 0.f, Length) : LaneLine(*Spline, Lateral, Length, 0.f));
	}
}

void FMKIRoadAutoPaths::DepotDockPaths(const AMKIRoadJunction& Junction, int32 DockIndex, TArray<TArray<FSplinePointData>>& OutPaths)
{
	TArray<FMKIRoadDepotDock> Docks;
	AMKIRoadJunction::DepotDocks(Junction.GetArmCount(), Junction.GetLanes(), Docks);
	if (!Docks.IsValidIndex(DockIndex)) return;
	const FVector2D Point = Docks[DockIndex].Point;
	TArray<TArray<TPair<FVector2D, FVector2D>>> Local, Dock;
	AMKIRoadJunction::DepotPaths(Junction.GetArmCount(), Junction.GetLanes(), Local);
	for (TArray<TPair<FVector2D, FVector2D>>& Path : Local)
	{
		if (Path.Num() >= 2 && (FVector2D::Distance(Path[0].Key, Point) < 1.0 || FVector2D::Distance(Path.Last().Key, Point) < 1.0)) Dock.Add(MoveTemp(Path));
	}
	const FTransform Frame = Junction.GetActorTransform();
	for (const TArray<TPair<FVector2D, FVector2D>>& Half : Dock)
	{
		TArray<FSplinePointData> Points;
		for (int32 Index = 0; Index < Half.Num(); ++Index)
		{
			const FVector2D At = Half[Index].Key, Heading = Half[Index].Value;
			const double Spacing = (Half[FMath::Min(Index + 1, Half.Num() - 1)].Key - Half[FMath::Max(Index - 1, 0)].Key).Size() / (Index == 0 || Index == Half.Num() - 1 ? 1.0 : 2.0);
			Points.Add(FSplinePointData(Frame.TransformPosition(FVector(At.X, At.Y, LiftCm)), Frame.TransformVectorNoScale(FVector(Heading.X, Heading.Y, 0.0)) * Spacing));
		}
		OutPaths.Add(MoveTemp(Points));
	}
}

int32 FMKIRoadAutoPaths::CleanStaleNodeLinks(UWorld* World)
{
	if (!World) return 0;
	int32 Dropped = 0;
	for (TActorIterator<AFGVehiclePathNode> It(World); It; ++It)
	{
		// The node's own arrays (GetArrivingConnections returns them by reference); only links to paths gone or going drop.
		for (const TArray<AFGVehiclePathSegment*>* Links : { &It->GetArrivingConnections(), &It->GetLeavingConnections() })
		{
			Dropped += const_cast<TArray<AFGVehiclePathSegment*>*>(Links)->RemoveAll([](const AFGVehiclePathSegment* Segment) { return !IsValid(Segment) || Segment->IsActorBeingDestroyed(); });
		}
	}
	if (Dropped > 0) UE_LOG(LogMKIRoads, Log, TEXT("Road paths: dropped %d stale path-node links"), Dropped);
	return Dropped;
}

TArray<FSplinePointData> FMKIRoadAutoPaths::MovePath(const FVector& Start, const FVector& StartDir, const FVector& End, const FVector& EndDir)
{
	const double Chord = FVector::Dist2D(Start, End);
	const double Cos = FMath::Clamp(FVector::DotProduct(StartDir.GetSafeNormal2D(), EndDir.GetSafeNormal2D()), -1.0, 1.0);
	const double Theta = FMath::Acos(Cos);
	// A circular arc between the headings: radius chord / (2 sin(theta / 2)), Hermite tangents 4 tan(theta / 4) R long.
	double Magnitude = Chord;
	if (Theta > 1e-3) { const double Radius = Chord / (2.0 * FMath::Sin(Theta * 0.5)); Magnitude = 4.0 * FMath::Tan(Theta * 0.25) * Radius; }
	const FVector Rise(0.0, 0.0, End.Z - Start.Z);
	return { FSplinePointData(Start, StartDir.GetSafeNormal2D() * Magnitude + Rise), FSplinePointData(End, EndDir.GetSafeNormal2D() * Magnitude + Rise) };
}

void FMKIRoadAutoPaths::JunctionPaths(const AMKIRoadJunction& Junction, TArray<TArray<FSplinePointData>>& OutPaths)
{
	const TArray<FTransform> Arms = Junction.GetArmEnds();
	const int32 Lanes = Junction.GetLanes();
	// Local points and headings in the junction's frame, as path points on its deck top.
	auto ToWorld = [&Junction](const TArray<TArray<TPair<FVector2D, FVector2D>>>& Local, TArray<TArray<FSplinePointData>>& Out)
	{
		const FTransform Frame = Junction.GetActorTransform();
		for (const TArray<TPair<FVector2D, FVector2D>>& Half : Local)
		{
			TArray<FSplinePointData> Points;
			for (int32 Index = 0; Index < Half.Num(); ++Index)
			{
				const FVector2D Point = Half[Index].Key, Heading = Half[Index].Value;
				const double Spacing = (Half[FMath::Min(Index + 1, Half.Num() - 1)].Key - Half[FMath::Max(Index - 1, 0)].Key).Size() / (Index == 0 || Index == Half.Num() - 1 ? 1.0 : 2.0);
				Points.Add(FSplinePointData(Frame.TransformPosition(FVector(Point.X, Point.Y, LiftCm)), Frame.TransformVectorNoScale(FVector(Heading.X, Heading.Y, 0.0)) * Spacing));
			}
			Out.Add(MoveTemp(Points));
		}
	};
	if (Junction.IsTruckDepot())
	{
		// A truck depot (#237, #239, #240): its lanes, bays, or loops, each path through a dock in halves meeting there.
		TArray<TArray<TPair<FVector2D, FVector2D>>> Local;
		AMKIRoadJunction::DepotPaths(Junction.GetArmCount(), Lanes, Local);
		ToWorld(Local, OutPaths);
		return;
	}
	if (Junction.GetArmCount() == 1)
	{
		// A turnaround (#236): each arriving lane's loop back to its leaving lane, in two halves (so no path starts and ends on
		// the same node).
		TArray<TArray<TPair<FVector2D, FVector2D>>> Halves;
		AMKIRoadJunction::TurnaroundLoops(Lanes, Halves);
		ToWorld(Halves, OutPaths);
		return;
	}
	// Lanes each way at an arm, counted from the center out.
	const int32 PerDirection = Lanes / 2;
	const float Lift = LiftCm;
	auto LanePoint = [](const FTransform& Arm, int32 Lane, bool bLeaving)
	{
		// Leaving traffic keeps right of the arm's outward direction, arriving traffic right of its own (left of outward).
		const float Lateral = (Lane + 0.5f) * AMKIRoadSplineRoad::LaneWidthCm * (bLeaving ? 1.f : -1.f);
		return Arm.GetLocation() + Arm.GetUnitAxis(EAxis::Y) * Lateral;
	};
	for (int32 From = 0; From < Arms.Num(); ++From)
	{
		for (int32 To = 0; To < Arms.Num(); ++To)
		{
			if (From == To) continue;
			const FVector In = -Arms[From].GetUnitAxis(EAxis::X), Out = Arms[To].GetUnitAxis(EAxis::X);
			for (int32 Lane = 0; Lane < PerDirection; ++Lane)
			{
				OutPaths.Add(MovePath(LanePoint(Arms[From], Lane, false) + FVector(0.f, 0.f, Lift), In, LanePoint(Arms[To], Lane, true) + FVector(0.f, 0.f, Lift), Out));
			}
		}
	}
}

void FMKIRoadAutoPaths::LocalToWorld(const AMKIRoadJunction& Junction, const TArray<TArray<TPair<FVector2D, FVector2D>>>& Local, TArray<TArray<FSplinePointData>>& OutPaths)
{
	const FTransform Frame = Junction.GetActorTransform();
	for (const TArray<TPair<FVector2D, FVector2D>>& Half : Local)
	{
		TArray<FSplinePointData> Points;
		for (int32 Index = 0; Index < Half.Num(); ++Index)
		{
			const FVector2D At = Half[Index].Key, Heading = Half[Index].Value;
			const double Spacing = (Half[FMath::Min(Index + 1, Half.Num() - 1)].Key - Half[FMath::Max(Index - 1, 0)].Key).Size() / (Index == 0 || Index == Half.Num() - 1 ? 1.0 : 2.0);
			Points.Add(FSplinePointData(Frame.TransformPosition(FVector(At.X, At.Y, LiftCm)), Frame.TransformVectorNoScale(FVector(Heading.X, Heading.Y, 0.0)) * Spacing));
		}
		OutPaths.Add(MoveTemp(Points));
	}
}

bool FMKIRoadAutoPaths::HasStandingStation(UWorld* World, const FVector& Location)
{
	for (TActorIterator<AFGBuildableDockingStation> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed() && FVector::Dist(It->GetActorLocation(), Location) <= 2.f * JoinRadiusCm) return true;
	}
	return false;
}

void FMKIRoadAutoPaths::Spawn(UWorld* World, const TArray<TArray<FSplinePointData>>& Paths, TArray<AFGVehiclePathSegment*>& OutSegments)
{
	AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
	UClass* SegmentClass = LoadClass<AFGVehiclePathSegment>(nullptr, UniversalPathClassPath);
	if (!Subsystem || !SegmentClass)
	{
		UE_LOG(LogMKIRoads, Warning, TEXT("Road paths: the vanilla Universal path segment is unavailable"));
		return;
	}
	// Nodes this call made, so its own paths join without waiting for the world to know them.
	TArray<AFGVehiclePathNode*> Nodes;
	auto FindNode = [World, &Nodes](const FVector& Location) -> AFGVehiclePathNode*
	{
		for (AFGVehiclePathNode* Node : Nodes) if (IsValid(Node) && FVector::Dist(Node->GetActorLocation(), Location) <= JoinRadiusCm) return Node;
		// Live (#235): the vanilla hologram's node overlap found no node at road ends, so every path made its own and nothing joined;
		// the nodes are searched directly instead.
		AFGVehiclePathNode* Best = nullptr;
		float BestDistance = JoinRadiusCm;
		for (TActorIterator<AFGVehiclePathNode> It(World); It; ++It)
		{
			const float Distance = static_cast<float>(FVector::Dist(It->GetActorLocation(), Location));
			if (!IsValid(*It) || It->IsActorBeingDestroyed() || Distance > BestDistance) continue;
			// A station's docking node only while its station stands (live, #241: a dock's paths re-laid as a fluid station was
			// dismantled joined its dying docking node, and the vehicle network crashed rebuilding without it).
			if (It->GetClass()->GetName().Contains(TEXT("DockingStation")) && !HasStandingStation(World, It->GetActorLocation())) continue;
			Best = *It;
			BestDistance = Distance;
		}
		return Best;
	};
	for (const TArray<FSplinePointData>& Path : Paths)
	{
		if (Path.Num() < 2) continue;
		const FTransform Frame(Path[0].Location);
		AFGVehiclePathSegment* Segment = Cast<AFGVehiclePathSegment>(Subsystem->BeginSpawnBuildable(SegmentClass, Frame));
		if (!Segment) continue;
		TArray<FSplinePointData> Local;
		for (const FSplinePointData& Point : Path) Local.Add(FSplinePointData(Frame.InverseTransformPosition(Point.Location), Point.LeaveTangent));
		Segment->SetSplinePointsAndValidatePath(Local);
		Segment->SetNodeConnections(FindNode(Path[0].Location), FindNode(Path.Last().Location));
		UGameplayStatics::FinishSpawningActor(Segment, Frame);
		for (AFGVehiclePathNode* Node : { Segment->GetStartNode(), Segment->GetEndNode() }) if (Node) Nodes.AddUnique(Node);
		OutSegments.Add(Segment);
	}
}

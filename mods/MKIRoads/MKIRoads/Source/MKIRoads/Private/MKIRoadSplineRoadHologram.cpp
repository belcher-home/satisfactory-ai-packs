#include "MKIRoadSplineRoadHologram.h"

#include "MKIRoadJunction.h"
#include "MKIRoadMarkingPattern.h"
#include "MKIRoadsGameWorldModule.h"
#include "MKIRoadSplineRoadDisqualifiers.h"
#include "Buildables/FGBuildableFoundation.h"
#include "FGLightweightBuildableSubsystem.h"
#include "MKIRoadSplineRoad.h"
#include "Components/SplineComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGFactoryColoringTypes.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInstance.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"

namespace
{
	/** Length, steepest grade, and tightest horizontal radius of one cubic Hermite segment, as a USplineComponent builds it. */
	void SampleSegment(const FVector& P0, const FVector& T0, const FVector& P1, const FVector& T1, FMKIRoadSplinePlan& Plan)
	{
		constexpr int32 Samples = 64;
		FVector Previous = P0;
		for (int32 Index = 0; Index <= Samples; ++Index)
		{
			const double A = static_cast<double>(Index) / Samples, A2 = A * A, A3 = A2 * A;
			const FVector Position = P0 * (2 * A3 - 3 * A2 + 1) + T0 * (A3 - 2 * A2 + A) + P1 * (-2 * A3 + 3 * A2) + T1 * (A3 - A2);
			const FVector D1 = P0 * (6 * A2 - 6 * A) + T0 * (3 * A2 - 4 * A + 1) + P1 * (-6 * A2 + 6 * A) + T1 * (3 * A2 - 2 * A);
			const FVector D2 = P0 * (12 * A - 6) + T0 * (6 * A - 4) + P1 * (-12 * A + 6) + T1 * (6 * A - 2);
			if (Index > 0) Plan.LengthCm += FVector::Distance(Previous, Position);
			Previous = Position;
			const double Horizontal = FVector2D(D1.X, D1.Y).Size();
			if (Horizontal < KINDA_SMALL_NUMBER) { Plan.MaxGradePercent = TNumericLimits<double>::Max(); continue; }
			Plan.MaxGradePercent = FMath::Max(Plan.MaxGradePercent, FMath::Abs(D1.Z) / Horizontal * 100.0);
			const double Cross = FMath::Abs(D1.X * D2.Y - D1.Y * D2.X);
			if (Cross > KINDA_SMALL_NUMBER) Plan.MinRadiusCm = FMath::Min(Plan.MinRadiusCm, FMath::Pow(Horizontal, 3.0) / Cross);
		}
	}
}

AMKIRoadSplineRoadHologram::AMKIRoadSplineRoadHologram()
{
	mLanes = AMKIRoadSplineRoad::DefaultLanes;
	// The build gun's lock (H) and nudge (#377).
	mCanLockHologram = true;
	mCanNudgeHologram = true;
	mDefaultNudgeDistance = AMKIRoadSplineRoad::NudgeStepCm;
}

void AMKIRoadSplineRoadHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMKIRoadSplineRoadHologram, mLanes);
}

int32 AMKIRoadSplineRoadHologram::StepLanes(int32 Current, int32 Delta)
{
	// The maintainer (#226, #255): +/- offers 2 and 4 lanes only. From any other count (a road built wider before), the next offered
	// count in that direction, else the nearest end of the list.
	if (Delta > 0) { for (const int32 Lanes : StepLaneCounts) if (Lanes > Current) return Lanes; return StepLaneCounts[UE_ARRAY_COUNT(StepLaneCounts) - 1]; }
	if (Delta < 0) { for (int32 Index = UE_ARRAY_COUNT(StepLaneCounts) - 1; Index >= 0; --Index) if (StepLaneCounts[Index] < Current) return StepLaneCounts[Index]; return StepLaneCounts[0]; }
	return Current;
}

FVector AMKIRoadSplineRoadHologram::ProjectStraightEnd(const FVector& Start, const FVector& Facing, const FVector& Aim, bool bOnGrid)
{
	double Along = FMath::Max(0.0, FVector::DotProduct(FVector(Aim.X - Start.X, Aim.Y - Start.Y, 0.0), Facing));
	if (bOnGrid) Along = FMath::RoundToDouble(Along / FoundationGridCm) * FoundationGridCm;
	const FVector End = Start + Facing * Along;
	return FVector(End.X, End.Y, Aim.Z);
}

float AMKIRoadSplineRoadHologram::AlignYawToFoundation(float Yaw, float FoundationYaw, float Step)
{
	const float Safe = Step > 0.f ? Step : 90.f;
	return FRotator::NormalizeAxis(FoundationYaw + FMath::RoundToFloat(FRotator::NormalizeAxis(Yaw - FoundationYaw) / Safe) * Safe);
}

void AMKIRoadSplineRoadHologram::SetLanes(int32 Lanes)
{
	if (!AMKIRoadSplineRoad::IsSupportedLaneCount(Lanes)) return;
	mLanes = Lanes;
	Replan();
	UpdatePreview();
}

float AMKIRoadSplineRoadHologram::DeckThicknessCm() const
{
	const AMKIRoadSplineRoad* Road = GetDefaultBuildable<AMKIRoadSplineRoad>();
	return Road ? Road->GetDeckThicknessCm() : AMKIRoadSplineRoad::DefaultDeckThicknessCm;
}

void AMKIRoadSplineRoadHologram::BeginPlay()
{
	Super::BeginPlay();
	mPreview = NewObject<UProceduralMeshComponent>(this, TEXT("Preview"));
	mPreview->SetupAttachment(RootComponent);
	mPreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreview->SetCastShadow(false);
	mPreview->RegisterComponent();
	mPreviewMarkings = NewObject<UProceduralMeshComponent>(this, TEXT("PreviewMarkings"));
	mPreviewMarkings->SetupAttachment(RootComponent);
	mPreviewMarkings->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMarkings->SetCastShadow(false);
	mPreviewMarkings->RegisterComponent();
	mStubSpline = NewObject<USplineComponent>(this, TEXT("StubSpline"));
	if (IsLocalHologram()) RegisterLaneInput();
	// A construction hologram may already carry deserialized spline points; never replan over them.
	if (mSplineData.Num() >= 2 && FVector::Dist(mSplineData[0].Location, mSplineData.Last().Location) > 1.f) SyncFromSplineData();
	else { Replan(); UpdatePreview(); }
}

void AMKIRoadSplineRoadHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterLaneInput();
	RemoveLightweightConverter();
	Super::EndPlay(EndPlayReason);
}

void AMKIRoadSplineRoadHologram::UpdateLightweightConverter()
{
	if (mClearanceData.Num() == 0 || !mSplineComponent) { RemoveLightweightConverter(); return; }
	const FBox Bounds = mSplineComponent->Bounds.GetBox();
	const float Needed = Bounds.GetExtent().Size() + mLanes * AMKIRoadSplineRoad::LaneWidthCm + 400.f;
	const float Radius = LightweightConverterStepCm * FMath::CeilToFloat(Needed / LightweightConverterStepCm);
	AActor* Converter = mLightweightConverter.Get();
	if (!Converter || Radius != mLightweightConverterRadius)
	{
		RemoveLightweightConverter();
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(GetWorld());
		if (!Lightweights) return;
		Converter = Lightweights->AddInstanceConverterInstigator(Radius, nullptr, FTransform(Bounds.GetCenter()));
		mLightweightConverter = Converter;
		mLightweightConverterRadius = Radius;
	}
	if (Converter) Converter->SetActorLocation(Bounds.GetCenter());
}

void AMKIRoadSplineRoadHologram::RemoveLightweightConverter()
{
	if (AActor* Converter = mLightweightConverter.Get())
	{
		if (AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(GetWorld())) Lightweights->RemoveInstanceConverterInstigator(Converter);
	}
	mLightweightConverter.Reset();
	mLightweightConverterRadius = 0.f;
}

bool AMKIRoadSplineRoadHologram::FoundationEdgeEnd(const AFGBuildableFoundation& Foundation, const FHitResult& HitResult, const AActor* Ignore, FVector& OutLocation, FVector& OutOutward)
{
	if (Foundation.mHeight <= 0.f) return false;
	const FTransform Frame(FRotator(0.f, Foundation.GetActorRotation().Yaw, 0.f), Foundation.GetActorLocation());
	const FVector Local = Frame.InverseTransformPosition(HitResult.Location);
	const float HalfX = Foundation.mDepth * 0.5f, HalfY = Foundation.mWidth * 0.5f, TopZ = Foundation.mHeight * 0.5f;
	auto OnGrid = [](float Value, float Half) { return FMath::Clamp(FMath::RoundToFloat((Value + Half) / FoundationGridCm) * FoundationGridCm - Half, -Half, Half); };
	// Its side face, or the top within 2 m of the edge (#207: the maintainer could not start a road at a
	// foundation's edge): on that edge, on the 1 m grid along it, flush with the top, heading straight out from the edge.
	const FVector Normal = Frame.InverseTransformVectorNoScale(HitResult.ImpactNormal.GetSafeNormal());
	const bool bSide = FMath::Abs(Normal.Z) < 0.5f && Local.Z <= TopZ + 25.f && Local.Z >= -TopZ - 25.f;
	const bool bTop = FMath::Abs(Local.Z - TopZ) <= 25.f;
	if (!bSide && !bTop) return false;
	float EdgeX = 0.f, EdgeY = 0.f;
	if (bSide) { if (FMath::Abs(Normal.X) >= FMath::Abs(Normal.Y)) EdgeX = Normal.X >= 0.f ? 1.f : -1.f; else EdgeY = Normal.Y >= 0.f ? 1.f : -1.f; }
	else
	{
		const float ToX = HalfX - FMath::Abs(Local.X), ToY = HalfY - FMath::Abs(Local.Y);
		const float BandX = FoundationEdgeBandCm, BandY = FoundationEdgeBandCm;
		if (ToX <= BandX && (ToX <= ToY || ToY > BandY)) EdgeX = Local.X >= 0.f ? 1.f : -1.f;
		else if (ToY <= BandY) EdgeY = Local.Y >= 0.f ? 1.f : -1.f;
	}
	// A top edge counts only where nothing continues past it at deck height: the seam between two foundations of a
	// pad is not an edge (a side face aimed at always is).
	if (bTop && (EdgeX != 0.f || EdgeY != 0.f) && Foundation.GetWorld())
	{
		const FVector Past = Frame.TransformPosition(FVector(EdgeX * (HalfX + 50.f), EdgeY * (HalfY + 50.f), 0.f) + FVector(EdgeY != 0.f ? Local.X : 0.f, EdgeX != 0.f ? Local.Y : 0.f, TopZ));
		FHitResult Beyond;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(MKIRoadFoundationEdge), false, Ignore);
		if (Foundation.GetWorld()->LineTraceSingleByChannel(Beyond, Past + FVector(0.f, 0.f, 50.f), Past - FVector(0.f, 0.f, 50.f), ECC_Visibility, Query)) EdgeX = EdgeY = 0.f;
	}
	if (EdgeX == 0.f && EdgeY == 0.f) return false;
	const FVector OnEdge = EdgeX != 0.f ? FVector(EdgeX * HalfX, OnGrid(Local.Y, HalfY), TopZ) : FVector(OnGrid(Local.X, HalfX), EdgeY * HalfY, TopZ);
	OutLocation = Frame.TransformPosition(OnEdge);
	OutOutward = Frame.TransformVectorNoScale(FVector(EdgeX, EdgeY, 0.f));
	return true;
}

FMKIRoadSplineEnd AMKIRoadSplineRoadHologram::ResolveEnd(const FHitResult& HitResult, float Thickness) const
{
	FMKIRoadSplineEnd Result;
	// A free point rests the deck on the aimed surface: the spline is the deck top.
	Result.Location = HitResult.Location + FVector(0.f, 0.f, Thickness);
	// The hit actor through its instance handle: aimed at a lightweight foundation the build gun hands over the
	// AbstractInstanceManager, and FetchActor resolves the instance's foundation actor, as the vehicle hologram does.
	// Live, GetActor() saw only the manager, so an end aimed at a foundation edge never snapped.
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor);
	const USplineComponent* Spline = Road ? Road->GetSplineComponent() : nullptr;
	// A junction (#227): aimed within 2 m of a side with an arm (the snap rule every piece shares), the road
	// starts or ends there, on the middle of the arm's end, heading straight out of it (locked and level, as a road end).
	// A piece or one of its squares (the maintainer, 2026-10-18: pieces are built of foundation squares).
	if (const AMKIRoadJunction* Junction = AMKIRoadJunction::FromActor(HitActor))
	{
		FVector OnSide, Outward;
		bool bArm = false;
		if (Junction->NearSide(HitResult.Location, OnSide, Outward, bArm) && bArm && !Outward.IsNearlyZero())
		{
			Result.bSnapped = true;
			Result.bHeadingLocked = true;
			Result.bFixedHeading = true;
			Result.Location = OnSide;
			Result.Outward = Outward;
			Result.OutwardGrade = 0.f;
			Result.Junction = const_cast<AMKIRoadJunction*>(Junction);
			Result.JunctionLanes = Junction->GetLanes();
			return Result;
		}
	}
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2)
	{
		// A foundation (the maintainer's rule, as vanilla rail): on its top the point snaps to the foundation's 1 m grid, counted
		// from its edges, and the deck rests on the top there. The heading stays free and rotates in the normal steps.
		const AFGBuildableFoundation* Foundation = Cast<AFGBuildableFoundation>(HitActor);
		if (!Foundation || Foundation->mHeight <= 0.f) return Result;
		const FTransform Frame(FRotator(0.f, Foundation->GetActorRotation().Yaw, 0.f), Foundation->GetActorLocation());
		const FVector Local = Frame.InverseTransformPosition(HitResult.Location);
		const float HalfX = Foundation->mDepth * 0.5f, HalfY = Foundation->mWidth * 0.5f, TopZ = Foundation->mHeight * 0.5f;
		auto OnGrid = [](float Value, float Half) { return FMath::Clamp(FMath::RoundToFloat((Value + Half) / FoundationGridCm) * FoundationGridCm - Half, -Half, Half); };
		// At an edge (#207): on that edge, flush with the top, heading straight out (as a road end, locked and level).
		FVector OnEdge, EdgeOutward;
		if (FoundationEdgeEnd(*Foundation, HitResult, this, OnEdge, EdgeOutward))
		{
			Result.bSnapped = true;
			Result.bHeadingLocked = true;
			Result.bFixedHeading = true;
			Result.bOnFoundation = true;
			Result.FoundationYaw = Foundation->GetActorRotation().Yaw;
			Result.Location = OnEdge;
			Result.Outward = EdgeOutward;
			Result.OutwardGrade = 0.f;
			Result.bOnEdge = true;
			Result.EdgePoint = OnEdge;
			Result.EdgeOutward = EdgeOutward;
			return Result;
		}
		Result.bOnFoundation = true;
		Result.FoundationYaw = Foundation->GetActorRotation().Yaw;
		Result.Location = Frame.TransformPosition(FVector(OnGrid(Local.X, HalfX), OnGrid(Local.Y, HalfY), TopZ)) + FVector(0.f, 0.f, Thickness);
		return Result;
	}
	// The road end aimed within 2 m of (the snap rule every piece shares).
	FVector End, Outward;
	if (AMKIRoadSplineRoad::NearEnd(*Road, HitResult.Location, End, Outward))
	{
		const float Run = FVector2D(Outward.X, Outward.Y).Size();
		if (Run >= KINDA_SMALL_NUMBER)
		{
			Result.bSnapped = true;
			Result.bHeadingLocked = true;
			Result.bFixedHeading = true;
			Result.Location = End;
			Result.Outward = FVector(Outward.X, Outward.Y, 0.f) / Run;
			Result.OutwardGrade = Outward.Z / Run;
			Result.Road = Road;
		}
	}
	return Result;
}

FMKIRoadSplinePlan AMKIRoadSplineRoadHologram::PlanRoad(const FMKIRoadSplineEnd& Start, const FMKIRoadSplineEnd& End, const FVector& Origin, bool bStraight, int32 Lanes)
{
	FMKIRoadSplinePlan Plan;
	const FVector P0 = Start.Location - Origin, P1 = End.Location - Origin;
	const FVector Chord(P1.X - P0.X, P1.Y - P0.Y, 0.f);
	const double ChordLength = Chord.Size(), Rise = P1.Z - P0.Z;
	if (ChordLength < 1.0)
	{
		const FVector Tangent = FVector::ForwardVector * FMath::Max(1.0, ChordLength);
		Plan.Points = { FSplinePointData(P0, Tangent), FSplinePointData(P1, Tangent) };
		Plan.LengthCm = FVector::Distance(P0, P1);
		return Plan;
	}
	const FVector C = Chord / ChordLength;
	const bool bStartFixed = Start.bFixedHeading && !bStraight, bEndFixed = End.bFixedHeading && !bStraight;
	const FVector D0 = bStartFixed ? Start.Outward : C;
	// From a fixed start the road is a circular arc tangent to it, so it arrives mirrored about the chord. A fixed end the
	// arc already arrives along (a road end facing back beside the start: a half circle, #206) takes the arc too.
	const FVector ArcArrival = (2.0 * FVector::DotProduct(D0, C) * C - D0).GetSafeNormal();
	const bool bArcMeetsEnd = bStartFixed && bEndFixed && FVector::DotProduct(ArcArrival, -End.Outward) >= FMath::Cos(FMath::DegreesToRadians(2.0));
	// An end the arc cannot reach is the plain chord, flagged too sharp. Live, an arc toward a point just behind a snapped
	// start turned almost 360 degrees, its radius (chord / 2 sin(theta / 2)) diverged, and the preview tried to allocate
	// 70 GB of cross-sections, crashing the game; so an arc turns at most MaxArcTurnDeg. Until #206 the limit was an end
	// beside the start (about 174 degrees), so a half circle could not be laid: it flipped to the chord at the last step.
	const double MinArcDot = FMath::Cos(FMath::DegreesToRadians(MaxArcTurnDeg * 0.5));
	const bool bHermite = bEndFixed && !bArcMeetsEnd;
	if ((bStartFixed && FVector::DotProduct(D0, C) <= (bHermite ? 0.05 : MinArcDot)) || (bHermite && FVector::DotProduct(-End.Outward, C) <= 0.05))
	{
		Plan.bTurnTooSharp = true;
		Plan.Points = { FSplinePointData(P0, C * ChordLength + FVector(0, 0, Rise)), FSplinePointData(P1, C * ChordLength + FVector(0, 0, Rise)) };
		Plan.LengthCm = FVector::Distance(P0, P1);
		return Plan;
	}
	const bool bArc = bStartFixed && !bHermite;
	const FVector D1 = bArc ? ArcArrival : (bEndFixed ? -End.Outward : C);

	// Horizontal path: an arc of Theta at Radius, or a single Hermite span.
	const double Theta = bArc ? 2.0 * FMath::Acos(FMath::Clamp(FVector::DotProduct(D0, C), -1.0, 1.0)) : 0.0;
	const bool bCurved = bArc && Theta > 1e-3;
	const double Radius = bCurved ? ChordLength / (2.0 * FMath::Sin(Theta * 0.5)) : 0.0;
	const double PathLength = bCurved ? Radius * Theta : ChordLength;
	// Grades along travel: a snapped end keeps its own grade, a free end the average, eased between them (Hermite in z).
	const double Average = Rise / PathLength;
	const double G0 = bStartFixed && Start.bSnapped ? Start.OutwardGrade : Average, G1 = bEndFixed && End.bSnapped ? -End.OutwardGrade : Average;
	auto HeightAt = [&](double S) { const double T = S / PathLength, T2 = T * T, T3 = T2 * T; return (T3 - 2 * T2 + T) * G0 * PathLength + (-2 * T3 + 3 * T2) * Rise + (T3 - T2) * G1 * PathLength; };
	auto GradeAt = [&](double S) { const double T = S / PathLength, T2 = T * T; return ((3 * T2 - 4 * T + 1) * G0 * PathLength + (-6 * T2 + 6 * T) * Rise + (3 * T2 - 2 * T) * G1 * PathLength) / PathLength; };

	if (!bCurved)
	{
		Plan.Points = {
			FSplinePointData(P0, FVector(D0.X * ChordLength, D0.Y * ChordLength, G0 * ChordLength)),
			FSplinePointData(P1, FVector(D1.X * ChordLength, D1.Y * ChordLength, G1 * ChordLength)) };
	}
	else
	{
		// At most 45 degrees per segment, so each Hermite span stays within a hair of the true arc.
		const int32 Segments = FMath::Max(1, FMath::CeilToInt32(Theta / (PI / 4.0)));
		const double Step = Theta / Segments, Magnitude = 4.0 * FMath::Tan(Step / 4.0) * Radius;
		const double Turn = (D0.X * C.Y - D0.Y * C.X) >= 0.0 ? 1.0 : -1.0;
		const double Heading0 = FMath::Atan2(D0.Y, D0.X);
		auto Perpendicular = [Turn](double Heading) { return FVector(-FMath::Sin(Heading) * Turn, FMath::Cos(Heading) * Turn, 0.0); };
		const FVector Center = FVector(P0.X, P0.Y, 0.0) + Perpendicular(Heading0) * Radius;
		for (int32 Index = 0; Index <= Segments; ++Index)
		{
			const double Heading = Heading0 + Turn * Step * Index, S = PathLength * Index / Segments;
			FVector Location = Center - Perpendicular(Heading) * Radius;
			Location.Z = P0.Z + HeightAt(S);
			if (Index == Segments) Location = P1;
			const FVector Tangent(FMath::Cos(Heading) * Magnitude, FMath::Sin(Heading) * Magnitude, GradeAt(S) * PathLength / Segments);
			Plan.Points.Add(FSplinePointData(Location, Tangent));
		}
	}
	for (int32 Index = 0; Index + 1 < Plan.Points.Num(); ++Index)
	{
		SampleSegment(Plan.Points[Index].Location, Plan.Points[Index].LeaveTangent, Plan.Points[Index + 1].Location, Plan.Points[Index + 1].ArriveTangent, Plan);
	}
	return Plan;
}

void AMKIRoadSplineRoadHologram::SetHologramLocationAndRotation(const FHitResult& HitResult)
{
	const FMKIRoadSplineEnd Aim = ResolveEnd(HitResult, DeckThicknessCm());
	if (mBuildStep == ESplineHologramBuildStep::SHBS_FindStart)
	{
		mStart = Aim;
		mEnd = Aim;
		// A road leaving a junction's arm takes the junction's width (#227), and one leaving a road's end that road's (#238;
		// The maintainer: each road piece snaps to the others and takes the right lane count). Once per piece aimed at, so Plus and
		// Minus still change it there.
		AActor* Source = Aim.Junction.IsValid() ? Aim.Junction.Get() : (Aim.bSnapped ? static_cast<AActor*>(Aim.Road.Get()) : nullptr);
		const int32 SourceLanes = Aim.Junction.IsValid() ? Aim.JunctionLanes : (Aim.Road.IsValid() ? Aim.Road->GetLanes() : 0);
		if (Source != mLanesTakenFrom.Get())
		{
			mLanesTakenFrom = Source;
			if (Source && SourceLanes != mLanes && AMKIRoadSplineRoad::IsSupportedLaneCount(SourceLanes)) mLanes = SourceLanes;
		}
		// The road's actor sits at its start, unrotated; its spline points are relative to it.
		SetActorLocationAndRotation(Aim.Location, FRotator::ZeroRotator);
	}
	else
	{
		mEnd = Aim;
		// Never snap the end back onto the end it starts from: aim there is a free point on the deck.
		if (Aim.bSnapped && Aim.Road == mStart.Road && FVector::Dist(Aim.Location, mStart.Location) < 1.f)
		{
			mEnd = FMKIRoadSplineEnd();
			mEnd.Location = HitResult.Location + FVector(0.f, 0.f, DeckThicknessCm());
		}
	}
	ApplyRotations();
	// A start turned along a foundation's edge sits beside it (ApplyRotations), so the actor follows it.
	if (mBuildStep == ESplineHologramBuildStep::SHBS_FindStart && mStart.bOnEdge) SetActorLocationAndRotation(mStart.Location, FRotator::ZeroRotator);
	if (mStraightMode && mBuildStep == ESplineHologramBuildStep::SHBS_FindStart) SnapStartToWorldGrid();
	Replan();
	UpdatePreview();
	mNudgeBase = mBuildStep == ESplineHologramBuildStep::SHBS_FindStart ? mStart : mEnd;
}

void AMKIRoadSplineRoadHologram::SnapStartToWorldGrid()
{
	// Only a free start: a road end, an arm, or a foundation's top or edge keeps its own snap.
	if (mStart.bSnapped || mStart.bHeadingLocked || mStart.bOnFoundation || mStart.Junction.IsValid()) return;
	mStart.Location = AMKIRoadSplineRoad::WorldGridLocation(mStart.Location);
	const float Radians = FMath::DegreesToRadians(AMKIRoadSplineRoad::WorldGridYaw(mStartRotated ? mStartYaw : ViewYaw()));
	mStart.bFixedHeading = true;
	mStart.Outward = FVector(FMath::Cos(Radians), FMath::Sin(Radians), 0.f);
	mEnd = mStart;
	SetActorLocationAndRotation(mStart.Location, FRotator::ZeroRotator);
}

FTransform AMKIRoadSplineRoadHologram::GetNudgeSpaceTransform() const
{
	return FTransform(FRotator(0.f, CurrentHeadingYaw(mBuildStep == ESplineHologramBuildStep::SHBS_FindStart), 0.f), mNudgeBase.Location);
}

void AMKIRoadSplineRoadHologram::SetHologramNudgeLocation()
{
	const bool bStartEnd = mBuildStep == ESplineHologramBuildStep::SHBS_FindStart;
	FMKIRoadSplineEnd Nudged = mNudgeBase;
	const FVector Offset = GetNudgeSpaceTransform().TransformVectorNoScale(GetHologramNudgeOffset());
	if (!Offset.IsNearlyZero())
	{
		// Nudged off a road end or arm, the point no longer joins it; a heading it had stays fixed.
		Nudged.Location += Offset;
		Nudged.bSnapped = false;
		Nudged.bHeadingLocked = false;
		Nudged.Road = nullptr;
		Nudged.Junction = nullptr;
	}
	if (bStartEnd)
	{
		mStart = Nudged;
		mEnd = Nudged;
		SetActorLocationAndRotation(mStart.Location, FRotator::ZeroRotator);
	}
	else
	{
		mEnd = Nudged;
	}
	ApplyRotations();
	Replan();
	UpdatePreview();
}

TSubclassOf<UFGConstructDisqualifier> AMKIRoadSplineRoadHologram::GetConstructDisqualifierFromClearanceOverlap(const EClearanceOverlapResult& OverlapResult, AActor* OtherActor) const
{
	return AMKIRoadSplineRoad::PieceClearanceDisqualifier(OverlapResult, OtherActor, Super::GetConstructDisqualifierFromClearanceOverlap(OverlapResult, OtherActor));
}

float AMKIRoadSplineRoadHologram::ViewYaw() const
{
	const APawn* Builder = GetConstructionInstigator();
	const AController* Controller = Builder ? Builder->GetController() : nullptr;
	return Controller ? Controller->GetControlRotation().Yaw : 0.f;
}

float AMKIRoadSplineRoadHologram::CurrentHeadingYaw(bool bStartEnd) const
{
	const FMKIRoadSplineEnd& End = bStartEnd ? mStart : mEnd;
	if (End.bFixedHeading)
	{
		const FVector Leaving = bStartEnd ? End.Outward : -End.Outward;
		return FMath::RadiansToDegrees(FMath::Atan2(Leaving.Y, Leaving.X));
	}
	// A free start on a foundation faces the nearest 45 degree step from the foundation's axes (#253).
	if (bStartEnd) return End.bOnFoundation ? AlignYawToFoundation(ViewYaw(), End.FoundationYaw, static_cast<float>(FoundationRotationStepDeg)) : ViewYaw();
	const FVector Arrival = mSplineData.Num() > 0 ? mSplineData.Last().ArriveTangent : FVector::ForwardVector;
	return FMath::RadiansToDegrees(FMath::Atan2(Arrival.Y, Arrival.X));
}

float AMKIRoadSplineRoadHologram::GetPreviewHeadingYaw() const
{
	return CurrentHeadingYaw(mBuildStep == ESplineHologramBuildStep::SHBS_FindStart);
}

void AMKIRoadSplineRoadHologram::ApplyRotations()
{
	// Road ends lock the heading; anywhere else the player's rotation, if any, fixes it.
	auto Heading = [](float Yaw) { const float Radians = FMath::DegreesToRadians(Yaw); return FVector(FMath::Cos(Radians), FMath::Sin(Radians), 0.f); };
	// On a foundation a rotated heading stays on the rotation steps counted from the foundation's axes.
	auto Aligned = [](const FMKIRoadSplineEnd& End, float Yaw) { return End.bOnFoundation ? AlignYawToFoundation(Yaw, End.FoundationYaw, static_cast<float>(FoundationRotationStepDeg)) : Yaw; };
	if (!mStart.bHeadingLocked && mStartRotated) { mStart.bFixedHeading = true; mStart.Outward = Heading(Aligned(mStart, mStartYaw)); }
	if (!mEnd.bHeadingLocked && mEndRotated) { mEnd.bFixedHeading = true; mEnd.Outward = -Heading(Aligned(mEnd, mEndYaw)); }
	// At a foundation's edge the scroll turns the end in quarter turns from straight out (the maintainer, 2026-10-18). Turned
	// along the edge, the deck lies beside the foundation: its centreline half a deck out from the edge, flush with the top.
	const float HalfWidth = mLanes * AMKIRoadSplineRoad::LaneWidthCm * 0.5f;
	auto TurnAtEdge = [HalfWidth](FMKIRoadSplineEnd& End, int32 Turns)
	{
		if (!End.bOnEdge) return;
		const int32 Quarter = ((Turns % 4) + 4) % 4;
		End.Outward = End.EdgeOutward.RotateAngleAxis(90.f * Quarter, FVector::UpVector).GetSafeNormal2D();
		End.Location = End.EdgePoint + (Quarter % 2 == 1 ? End.EdgeOutward * HalfWidth : FVector::ZeroVector);
	};
	TurnAtEdge(mStart, mStartEdgeTurns);
	TurnAtEdge(mEnd, mEndEdgeTurns);
}

void AMKIRoadSplineRoadHologram::ScrollRotate(int32 Delta, int32 Step)
{
	const bool bStartEnd = mBuildStep == ESplineHologramBuildStep::SHBS_FindStart;
	const FMKIRoadSplineEnd& End = bStartEnd ? mStart : mEnd;
	if (End.bOnEdge)
	{
		// At a foundation's edge: a quarter turn per notch, so the road can leave straight out or run along the edge.
		(bStartEnd ? mStartEdgeTurns : mEndEdgeTurns) += Delta > 0 ? 1 : (Delta < 0 ? -1 : 0);
		ApplyRotations();
		if (bStartEnd) SetActorLocationAndRotation(mStart.Location, FRotator::ZeroRotator);
		Replan();
		UpdatePreview();
		return;
	}
	if (End.bHeadingLocked) return;
	// #253: on a foundation 45 degree steps; anywhere else the game's own rotation step, free.
	const float Turn = static_cast<float>(Delta * (End.bOnFoundation ? FoundationRotationStepDeg : (Step != 0 ? Step : RotationStepDeg)));
	const bool bRotated = bStartEnd ? mStartRotated : mEndRotated;
	const float Current = bRotated ? (bStartEnd ? mStartYaw : mEndYaw) : CurrentHeadingYaw(bStartEnd);
	float& Yaw = bStartEnd ? mStartYaw : mEndYaw;
	Yaw = FRotator::NormalizeAxis(Current + Turn);
	(bStartEnd ? mStartRotated : mEndRotated) = true;
	ApplyRotations();
	Replan();
	UpdatePreview();
}
FMKIRoadSplinePlan AMKIRoadSplineRoadHologram::FactsOf(const TArray<FSplinePointData>& Points)
{
	FMKIRoadSplinePlan Facts;
	Facts.Points = Points;
	for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
	{
		SampleSegment(Points[Index].Location, Points[Index].LeaveTangent, Points[Index + 1].Location, Points[Index + 1].ArriveTangent, Facts);
	}
	return Facts;
}

void AMKIRoadSplineRoadHologram::SyncFromSplineData()
{
	mPlan = FactsOf(mSplineData);
	mPlan.bTurnTooSharp = mTurnTooSharp;
	if (mSplineComponent)
	{
		mSplineComponent->ClearSplinePoints(false);
		for (int32 Index = 0; Index < mSplineData.Num(); ++Index)
		{
			mSplineComponent->AddSplinePoint(mSplineData[Index].Location, ESplineCoordinateSpace::Local, false);
			mSplineComponent->SetTangentsAtSplinePoint(Index, mSplineData[Index].ArriveTangent, mSplineData[Index].LeaveTangent, ESplineCoordinateSpace::Local, false);
		}
		mSplineComponent->UpdateSpline();
	}
	UpdateClearanceData();
	UpdatePreview();
}

void AMKIRoadSplineRoadHologram::PostHologramPlacement(const FHitResult& HitResult, bool bCallForChildren)
{
	Super::PostHologramPlacement(HitResult, bCallForChildren);
	// The road snaps by its own spline points, never through the snapped building, which the construct message carries:
	// aimed at a lightweight foundation it is that instance's temporary actor, and live the second click then built
	// nothing (the end snapped to a foundation stayed a hologram).
	mSnappedBuilding = nullptr;
}

void AMKIRoadSplineRoadHologram::SerializeConstructMessage(FArchive& Ar, FNetConstructionID Id)
{
	mSnappedBuilding = nullptr;
	Super::SerializeConstructMessage(Ar, Id);
	// The construction hologram rebuilds its facts from these, never from the placing hologram's private plan.
	Ar << mLanes;
	Ar << mStraightMode;
	Ar << mTurnTooSharp;
	int32 Count = mSplineData.Num();
	Ar << Count;
	if (Ar.IsLoading()) mSplineData.SetNum(FMath::Clamp(Count, 0, 64));
	for (FSplinePointData& Point : mSplineData) { Ar << Point.Location; Ar << Point.ArriveTangent; Ar << Point.LeaveTangent; }
}

void AMKIRoadSplineRoadHologram::PostConstructMessageDeserialization()
{
	Super::PostConstructMessageDeserialization();
	SyncFromSplineData();
}

void AMKIRoadSplineRoadHologram::SetupPendingConstructionHologram(AFGHologram* FromHologram)
{
	Super::SetupPendingConstructionHologram(FromHologram);
	if (const AMKIRoadSplineRoadHologram* From = Cast<AMKIRoadSplineRoadHologram>(FromHologram))
	{
		mLanes = From->mLanes;
		mStraightMode = From->mStraightMode;
		mTurnTooSharp = From->mTurnTooSharp;
		mBuildStep = From->mBuildStep;
		mStart = From->mStart;
		mEnd = From->mEnd;
		mSplineData = From->mSplineData;
		SetActorLocationAndRotation(From->GetActorLocation(), FRotator::ZeroRotator);
		SyncFromSplineData();
	}
}

void AMKIRoadSplineRoadHologram::Replan()
{
	if (mBuildStep == ESplineHologramBuildStep::SHBS_FindStart) mPlan = PlanRoad(mStart, mStart, GetActorLocation(), true, mLanes);
	else if (mStraightMode)
	{
		// Straight mode, as vanilla rail's: straight ahead along the start's heading (a snapped or rotated start's, else
		// the one it showed when the first click fixed it), as far as the aim reaches along it; the aim's sideways offset
		// is ignored, and its height kept.
		FMKIRoadSplineEnd Start = mStart;
		if (!Start.bFixedHeading)
		{
			const float Radians = FMath::DegreesToRadians(mStartFacingYaw);
			Start.bFixedHeading = true;
			Start.Outward = FVector(FMath::Cos(Radians), FMath::Sin(Radians), 0.f);
		}
		FMKIRoadSplineEnd End;
		End.Location = ProjectStraightEnd(Start.Location, Start.Outward, mEnd.Location, mEnd.bOnFoundation);
		End.bFixedHeading = true;
		End.Outward = -Start.Outward;
		mPlan = PlanRoad(Start, End, GetActorLocation(), false, mLanes);
	}
	else mPlan = PlanRoad(mStart, mEnd, GetActorLocation(), false, mLanes);
	mSplineData = mPlan.Points;
	mTurnTooSharp = mPlan.bTurnTooSharp;
	if (!mSplineComponent) return;
	mSplineComponent->ClearSplinePoints(false);
	for (int32 Index = 0; Index < mSplineData.Num(); ++Index)
	{
		mSplineComponent->AddSplinePoint(mSplineData[Index].Location, ESplineCoordinateSpace::Local, false);
		mSplineComponent->SetTangentsAtSplinePoint(Index, mSplineData[Index].ArriveTangent, mSplineData[Index].LeaveTangent, ESplineCoordinateSpace::Local, false);
	}
	mSplineComponent->UpdateSpline();
	UpdateClearanceData();
}

void AMKIRoadSplineRoadHologram::UpdateClearanceData()
{
	mClearanceData.Reset();
	if (mBuildStep != ESplineHologramBuildStep::SHBS_FindStart && mSplineComponent)
	{
		AMKIRoadSplineRoad::MakeClearanceData(mSplineComponent, mLanes * AMKIRoadSplineRoad::LaneWidthCm, DeckThicknessCm(), mClearanceData);
	}
	// Creates the detector on first use (it registers with the build gun state) and sizes it to the boxes.
	AMKIRoadSplineRoad::HardenForPlacement(mClearanceData);
	if (mClearanceData.Num() > 0) SetupClearanceDetector();
	UpdateLightweightConverter();
}

void AMKIRoadSplineRoadHologram::GetIgnoredClearanceActors(TSet<AActor*>& IgnoredActors) const
{
	Super::GetIgnoredClearanceActors(IgnoredActors);
	// The roads the ends join meet this one end to end.
	if (AMKIRoadSplineRoad* Road = mStart.Road.Get()) IgnoredActors.Add(Road);
	if (AMKIRoadSplineRoad* Road = mEnd.Road.Get()) IgnoredActors.Add(Road);
	// And the junctions whose arms they join (#227).
	for (const TWeakObjectPtr<AActor>& Joined : { mStart.Junction, mEnd.Junction })
	{
		if (const AMKIRoadJunction* Junction = AMKIRoadJunction::FromActor(Joined.Get())) Junction->AddSelfAndTiles(IgnoredActors);
	}
}

void AMKIRoadSplineRoadHologram::UpdatePreview()
{
	if (!mPreview || !mSplineComponent) return;
	const float Width = mLanes * AMKIRoadSplineRoad::LaneWidthCm;
	TArray<FMKIRoadDeckSlice> Slices;
	const float Length = mSplineComponent->GetSplineLength();
	if (mBuildStep == ESplineHologramBuildStep::SHBS_FindStart || Length < 1.f)
	{
		// Before the start is set: an 8 m stub of the real width and thickness along the start heading, so a rotation shows.
		const float Radians = FMath::DegreesToRadians(CurrentHeadingYaw(true));
		const FVector Forward(FMath::Cos(Radians), FMath::Sin(Radians), 0.f);
		for (const float Distance : { 0.f, StartStubLengthCm * 0.5f, StartStubLengthCm })
		{
			FMKIRoadDeckSlice Slice;
			Slice.Distance = Distance;
			Slice.Center = Forward * Distance;
			Slice.Forward = Forward;
			Slice.Right = FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal2D();
			Slice.Up = FVector::UpVector;
			Slices.Add(Slice);
		}
	}
	else
	{
		// Never more cross-sections than a road twice the longest allowed (a guard against a degenerate spline).
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32(Length / PreviewStepCm), 1, FMath::CeilToInt32(2.f * MaxLengthCm / PreviewStepCm));
		for (int32 Index = 0; Index <= Steps; ++Index)
		{
			const float Distance = Length * Index / Steps;
			FMKIRoadDeckSlice Slice;
			Slice.Distance = Distance;
			Slice.Center = mSplineComponent->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
			Slice.Forward = mSplineComponent->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::Local);
			Slice.Right = FVector::CrossProduct(FVector::UpVector, Slice.Forward).GetSafeNormal2D();
			if (Slice.Right.IsNearlyZero()) Slice.Right = Slices.Num() > 0 ? Slices.Last().Right : FVector::RightVector;
			Slice.Up = FVector::CrossProduct(Slice.Forward, Slice.Right).GetSafeNormal();
			Slices.Add(Slice);
		}
	}
	mPreviewGeometry = AMKIRoadSplineRoad::SweepDeck(Slices, Width, DeckThicknessCm());
	// The North American Road markings, as the built road gets by default (#234, #241): along the road, or the start stub.
	if (const UMKIRoadMarkingPattern* Pattern = MKIRoadDefaultPatternObject(); Pattern && mPreviewMarkings)
	{
		const USplineComponent* Along = mSplineComponent;
		if ((mBuildStep == ESplineHologramBuildStep::SHBS_FindStart || Length < 1.f) && mStubSpline && Slices.Num() >= 2)
		{
			const FVector Tangent = Slices.Last().Center - Slices[0].Center;
			mStubSpline->ClearSplinePoints(false);
			mStubSpline->AddSplinePoint(Slices[0].Center, ESplineCoordinateSpace::Local, false);
			mStubSpline->AddSplinePoint(Slices.Last().Center, ESplineCoordinateSpace::Local, false);
			for (const int32 Point : { 0, 1 }) mStubSpline->SetTangentAtSplinePoint(Point, Tangent, ESplineCoordinateSpace::Local, false);
			mStubSpline->UpdateSpline();
			Along = mStubSpline;
		}
		FMKIRoadDeckGeometry Paint[2];
		AMKIRoadSplineRoad::MarkingGeometry(*Along, mLanes, *Pattern, Paint);
		MKIRoadApplyPaint(mPreviewMarkings, *Pattern, Paint);
	}
	// Planar UVs for the hologram material: the per-tile vanilla UVs read as a patchwork under it (the maintainer).
	mPreviewPlanarUVs.Reset(mPreviewGeometry.Vertices.Num());
	for (const FVector& Vertex : mPreviewGeometry.Vertices) mPreviewPlanarUVs.Add(FVector2D(Vertex.X / AMKIRoadSplineRoad::UvTileCm, (Vertex.Y + Vertex.Z) / AMKIRoadSplineRoad::UvTileCm));
	ApplyPreviewMesh(mPreviewShowsDeck);
	// The state the build gun last set. GetHologramMaterialState() recomputes it from the disqualifiers, which are empty
	// while the hologram moves (validation adds them back afterwards), and the build gun reapplies a state only when it
	// changes: live, an invalid road showed its disqualifier but stayed blue.
	SetMaterialState(mPlacementMaterialState);
}

int32 AMKIRoadSplineRoadHologram::GetPreviewStencilValue() const
{
	return mPreview ? mPreview->CustomDepthStencilValue : -1;
}

void AMKIRoadSplineRoadHologram::ApplyPreviewMesh(bool bDeckUVs)
{
	if (!mPreview || mPreviewGeometry.Vertices.Num() == 0) return;
	const TArray<FVector2D>& UVs = bDeckUVs ? mPreviewGeometry.UVs : mPreviewPlanarUVs;
	// With tangents, as the built deck has: live, the preview without them shaded the hologram material in patches.
	TArray<FProcMeshTangent> Tangents;
	Tangents.Reserve(mPreviewGeometry.Tangents.Num());
	for (const FVector& Tangent : mPreviewGeometry.Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
	mPreview->CreateMeshSection_LinearColor(0, mPreviewGeometry.Vertices, mPreviewGeometry.Triangles, mPreviewGeometry.Normals, UVs, mPreviewGeometry.UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
}

bool AMKIRoadSplineRoadHologram::IsPreviewShowingDeck() const
{
	return mPreviewShowsDeck;
}

void AMKIRoadSplineRoadHologram::SetMaterialState(EHologramMaterialState State)
{
	Super::SetMaterialState(State);
	if (!mPreview) return;
	// The preview is asphalt in every state, the deck's own material in the default swatch (the maintainer: make the hologram look
	// like asphalt; live, the hologram material over the swept deck read as a patchwork, even with planar UVs and tangents,
	// where a vanilla foundation hologram is smooth). The custom-depth stencil below still shows the state (ok, warning,
	// error) as the vanilla holograms' colored outline.
	UMaterialInterface* Material = nullptr;
	{
		AMKIRoadSplineRoad* Road = const_cast<AMKIRoadSplineRoad*>(GetDefaultBuildable<AMKIRoadSplineRoad>());
		Material = Road ? Road->GetDeckMaterial() : nullptr;
		if (Material && !mPreviewShowsDeck)
		{
			FResolvedFactoryCustomizationData Resolved;
			Resolved.ApplySwatch(Road->GetDefaultSwatchCustomizationOverride(this));
			Resolved.ApplyToMeshComponent(mPreview);
		}
	}
	// The preview's UVs follow its material: the vanilla per-tile layout under the asphalt, planar under the hologram.
	if ((Material != nullptr) != mPreviewShowsDeck) ApplyPreviewMesh(Material != nullptr);
	mPreviewShowsDeck = Material != nullptr;
	if (!Material) Material = State == EHologramMaterialState::HMS_ERROR ? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	if (Material) mPreview->SetMaterial(0, Material);
	mPreview->SetRenderCustomDepth(true);
	mPreview->SetCustomDepthStencilValue(GetStencilForHologramMaterialState(State));
	// The markings keep their paints in every state (the hologram material would hide them).
	if (const UMKIRoadMarkingPattern* Pattern = MKIRoadDefaultPatternObject(); Pattern && mPreviewMarkings)
	{
		if (UMaterialInterface* Yellow = Pattern->YellowMaterial.LoadSynchronous()) mPreviewMarkings->SetMaterial(0, Yellow);
		if (UMaterialInterface* White = Pattern->WhiteMaterial.LoadSynchronous()) mPreviewMarkings->SetMaterial(1, White);
	}
}

bool AMKIRoadSplineRoadHologram::DoMultiStepPlacement(bool bIsInputFromARelease)
{
	if (bIsInputFromARelease) return false;
	if (mBuildStep == ESplineHologramBuildStep::SHBS_FindStart)
	{
		// The first click fixes the start, and the heading it showed; the end follows the aim until the second click builds.
		mStartFacingYaw = CurrentHeadingYaw(true);
		// As vanilla rail: a free start keeps that heading too, so the road curves from it toward the end (#186). Free,
		// it ran straight to wherever the end was, and only a snapped start curved.
		if (!mStart.bFixedHeading)
		{
			const float Radians = FMath::DegreesToRadians(mStartFacingYaw);
			mStart.bFixedHeading = true;
			mStart.Outward = FVector(FMath::Cos(Radians), FMath::Sin(Radians), 0.f);
		}
		mBuildStep = ESplineHologramBuildStep::SHBS_PlacePoleOrSnapEnding;
		Replan();
		UpdatePreview();
		return false;
	}
	return true;
}

void AMKIRoadSplineRoadHologram::ResetBuildSteps()
{
	Super::ResetBuildSteps();
	mBuildStep = ESplineHologramBuildStep::SHBS_FindStart;
	mEnd = mStart;
	mStartRotated = mEndRotated = false;
	mStartEdgeTurns = mEndEdgeTurns = 0;
	Replan();
	UpdatePreview();
}

int32 AMKIRoadSplineRoadHologram::GetBaseCostMultiplier() const
{
	return AMKIRoadSplineRoad::FoundationEquivalents(FactsOf(mSplineData).LengthCm, mLanes);
}

void AMKIRoadSplineRoadHologram::CheckValidPlacement()
{
	Super::CheckValidPlacement();
	if (mBuildStep == ESplineHologramBuildStep::SHBS_FindStart) return;
	const double Width = mLanes * AMKIRoadSplineRoad::LaneWidthCm;
	// From the saved spline points, which the construct message carries, so a construction hologram checks the same road.
	const FMKIRoadSplinePlan Facts = FactsOf(mSplineData);
	if (Facts.LengthCm < MinLengthCm) AddConstructDisqualifier(UMKIRoadCDTooShort::StaticClass());
	if (Facts.LengthCm > MaxLengthCm) AddConstructDisqualifier(UMKIRoadCDTooLong::StaticClass());
	if (Facts.MaxGradePercent > MaxGradePercent + 0.01) AddConstructDisqualifier(UMKIRoadCDTooSteep::StaticClass());
	if (mTurnTooSharp || Facts.MinRadiusCm < Width - 0.5) AddConstructDisqualifier(UMKIRoadCDTurnTooSharp::StaticClass());
}

void AMKIRoadSplineRoadHologram::RunPlacementChecks()
{
	ResetConstructDisqualifiers();
	CheckValidPlacement();
}

void AMKIRoadSplineRoadHologram::ConfigureActor(AFGBuildable* InBuildable) const
{
	Super::ConfigureActor(InBuildable);
	AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(InBuildable);
	if (!Road) return;
	if (TArray<FSplinePointData>* Points = Road->GetMutableSplinePointData()) *Points = mSplineData;
	Road->SetLanes(mLanes);
	// A new road is painted with the North American Road pattern unless the build gun carries one (#234).
	if (!Road->GetCustomizationData_Native().PatternDesc) Road->GetCustomizationData_Native().PatternDesc = MKIRoadDefaultPattern();
}

void AMKIRoadSplineRoadHologram::RegisterLaneInput()
{
	APlayerController* Controller = Cast<APlayerController>(GetConstructionInstigator() ? GetConstructionInstigator()->GetController() : nullptr);
	UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()) : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogMKIRoads, Warning, TEXT("%s: no local Enhanced Input subsystem; Plus/Minus lane selection is unavailable"), *GetName());
		return;
	}
	mPlusAction = NewObject<UInputAction>(this, TEXT("MKIRoadSplinePlus"));
	mMinusAction = NewObject<UInputAction>(this, TEXT("MKIRoadSplineMinus"));
	mLaneMappingContext = NewObject<UInputMappingContext>(this, TEXT("MKIRoadSplineLaneContext"));
	mLaneMappingContext->MapKey(mPlusAction, EKeys::Add);
	mLaneMappingContext->MapKey(mPlusAction, EKeys::Equals);
	mLaneMappingContext->MapKey(mMinusAction, EKeys::Subtract);
	mLaneMappingContext->MapKey(mMinusAction, EKeys::Hyphen);
	EnableInput(Controller);
	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(InputComponent);
	if (!Input)
	{
		UE_LOG(LogMKIRoads, Warning, TEXT("%s: the default input component is not an Enhanced Input component; Plus/Minus lane selection is unavailable"), *GetName());
		DisableInput(Controller);
		return;
	}
	Input->BindAction(mPlusAction, ETriggerEvent::Started, this, &AMKIRoadSplineRoadHologram::OnPlus);
	Input->BindAction(mMinusAction, ETriggerEvent::Started, this, &AMKIRoadSplineRoadHologram::OnMinus);
	Subsystem->AddMappingContext(mLaneMappingContext, LaneInputPriority);
	mLaneInputController = Controller;
}

void AMKIRoadSplineRoadHologram::UnregisterLaneInput()
{
	APlayerController* Controller = mLaneInputController.Get();
	mLaneInputController.Reset();
	if (!Controller) return;
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()))
	{
		Subsystem->RemoveMappingContext(mLaneMappingContext);
	}
	DisableInput(Controller);
}

void AMKIRoadSplineRoadHologram::OnPlus(const FInputActionValue& Value)
{
	SetLanes(StepLanes(mLanes, 1));
}

void AMKIRoadSplineRoadHologram::OnMinus(const FInputActionValue& Value)
{
	SetLanes(StepLanes(mLanes, -1));
}

void AMKIRoadSplineRoadHologram::SetSnapToGuideLines(bool bIsEnabled)
{
	Super::SetSnapToGuideLines(bIsEnabled);
	if (bIsEnabled != mStraightMode) SetStraightMode(bIsEnabled);
}

void AMKIRoadSplineRoadHologram::SetStraightMode(bool bStraight)
{
	mStraightMode = bStraight;
	Replan();
	UpdatePreview();
}

#include "MKIRoadSupports.h"

#include "MKIRoadSplineRoad.h"
#include "MKIRoadJunction.h"
#include "MKIRoadSupportsHologram.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FGFactoryColoringTypes.h"

AMKIRoadSupports::AMKIRoadSupports()
{
	mHologramClass = AMKIRoadSupportsHologram::StaticClass();
	mCanContainLightweightInstances = false;
	mManagedByLightweightBuildableSubsystem = false;
}

UStaticMesh* AMKIRoadSupports::GetBaseMesh() const { return mBaseMesh.LoadSynchronous(); }
UStaticMesh* AMKIRoadSupports::GetMiddleMesh() const { return mMiddleMesh.LoadSynchronous(); }

int32 AMKIRoadSupports::CountPieces(const TArray<FMKIRoadSupportColumn>& Columns)
{
	int32 Count = 0;
	for (const FMKIRoadSupportColumn& Column : Columns) Count += 1 + Column.Middles.Num();
	return Count;
}

FTransform AMKIRoadSupports::MeshTransform(const UStaticMesh* Mesh, const FTransform& PieceCenter)
{
	const FVector Center = Mesh ? Mesh->GetBoundingBox().GetCenter() : FVector::ZeroVector;
	// A scaled (half-height) piece: its bounds center scales with it.
	return FTransform(PieceCenter.GetRotation(), PieceCenter.GetLocation() - PieceCenter.GetRotation().RotateVector(PieceCenter.GetScale3D() * Center), PieceCenter.GetScale3D());
}

bool AMKIRoadSupports::FindTop(const AMKIRoadSplineRoad& Road, const FVector& Aim, FVector& OutTop, FVector& OutForward)
{
	const USplineComponent* Spline = Road.GetSplineComponent();
	if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
	const float Length = Spline->GetSplineLength();
	// Where the aim is along the road and across it, snapped to StepCm (across from the centerline, #216), the column's
	// top inside the deck.
	const float Key = Spline->FindInputKeyClosestToWorldLocation(Aim);
	const float Along = FMath::Clamp(FMath::RoundToFloat(Spline->GetDistanceAlongSplineAtSplineInputKey(Key) / StepCm) * StepCm, 0.f, Length);
	const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World);
	OutForward = Spline->GetDirectionAtDistanceAlongSpline(Along, ESplineCoordinateSpace::World).GetSafeNormal2D();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, OutForward);
	const float Limit = FMath::Max(0.f, Road.GetDeckWidthCm() * 0.5f - MiddleHalfCm);
	const float Lateral = FMath::Clamp(FMath::RoundToFloat(FVector::DotProduct(Aim - Center, Right) / StepCm) * StepCm, -Limit, Limit);
	OutTop = Center + Right * Lateral - FVector(0.f, 0.f, Road.GetDeckThicknessCm());
	return true;
}

bool AMKIRoadSupports::FindTop(const AFGBuildableFoundation& Foundation, const FVector& Aim, FVector& OutTop, FVector& OutForward)
{
	if (Foundation.mHeight <= 0.f || FMath::Abs(Foundation.mElevation) > KINDA_SMALL_NUMBER) return false;
	const FTransform Frame(FRotator(0.f, Foundation.GetActorRotation().Yaw, 0.f), Foundation.GetActorLocation());
	const FVector Local = Frame.InverseTransformPosition(Aim);
	const double LimitX = FMath::Max(0.0, Foundation.mDepth * 0.5 - MiddleHalfCm), LimitY = FMath::Max(0.0, Foundation.mWidth * 0.5 - MiddleHalfCm);
	const double X = FMath::Clamp(FMath::RoundToDouble(Local.X / StepCm) * StepCm, -LimitX, LimitX);
	const double Y = FMath::Clamp(FMath::RoundToDouble(Local.Y / StepCm) * StepCm, -LimitY, LimitY);
	// A foundation's actor sits at its middle; the column's top is its underside.
	OutTop = Frame.TransformPosition(FVector(X, Y, -Foundation.mHeight * 0.5f));
	OutForward = Frame.GetUnitAxis(EAxis::X);
	return true;
}

bool AMKIRoadSupports::FindTop(const AMKIRoadJunction& Junction, const FVector& Aim, FVector& OutTop, FVector& OutForward)
{
	const FTransform Frame(FRotator(0.f, Junction.GetActorRotation().Yaw, 0.f), Junction.GetActorLocation());
	const FMKIRoadJunctionLayout Shape = AMKIRoadJunction::Layout(Junction.GetArmCount(), Junction.GetLanes());
	TArray<FBox2D> Cells;
	for (const TArray<FVector2D>& Cell : Shape.Cells) Cells.Add(FBox2D(Cell).ExpandBy(1.0));
	// The pillar middle's footprint must lie under the deck: each of its corners in some cell (the cells are rectangles).
	auto Covered = [&Cells](const FVector2D& Point)
	{
		for (const FVector2D& Corner : { FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0) })
		{
			const FVector2D At = Point + Corner * MiddleHalfCm;
			bool bIn = false;
			for (const FBox2D& Cell : Cells) if (Cell.IsInside(At)) { bIn = true; break; }
			if (!bIn) return false;
		}
		return true;
	};
	const FVector Local = Frame.InverseTransformPosition(Aim);
	const FVector2D Snapped(FMath::RoundToDouble(Local.X / StepCm) * StepCm, FMath::RoundToDouble(Local.Y / StepCm) * StepCm);
	// The aimed step if it fits, else the nearest that does within two steps.
	bool bFound = false;
	FVector2D Best = Snapped;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 DX = -2; DX <= 2; ++DX) for (int32 DY = -2; DY <= 2; ++DY)
	{
		const FVector2D Candidate = Snapped + FVector2D(DX, DY) * StepCm;
		const double Distance = FVector2D::DistSquared(Candidate, FVector2D(Local.X, Local.Y));
		if (Distance < BestDistance && Covered(Candidate)) { Best = Candidate; BestDistance = Distance; bFound = true; }
	}
	if (!bFound) return false;
	OutTop = Frame.TransformPosition(FVector(Best.X, Best.Y, -Junction.GetDeckThicknessCm()));
	OutForward = Frame.GetUnitAxis(EAxis::X);
	return true;
}

float AMKIRoadSupports::HeightForDepth(float DepthCm)
{
	const float Height = FMath::CeilToFloat(DepthCm / HeightStepCm - 0.01f) * HeightStepCm;
	return FMath::Clamp(Height, MinHeightCm, MaxPieces * PieceHeightCm);
}

FMKIRoadSupportColumn AMKIRoadSupports::MakeColumn(const FVector& Top, const FVector& Forward, float HeightCm)
{
	// The base on the bottom, middles up to the deck's underside; the top one scaled to what is left (half height on the
	// 2 m steps; any height once nudged), a sliver under MinPieceCm going to the one below.
	const float Height = FMath::Clamp(HeightCm, MinHeightCm, MaxPieces * PieceHeightCm);
	const float Bottom = Top.Z - Height;
	const FQuat Rotation = FRotationMatrix::MakeFromXZ(Forward, FVector::UpVector).ToQuat();
	const FVector Plan(Top.X, Top.Y, 0.f);
	FMKIRoadSupportColumn Out;
	Out.Base = FTransform(Rotation, Plan + FVector(0.f, 0.f, Bottom + PieceHeightCm * 0.5f));
	const float Above = Height - PieceHeightCm;
	int32 Full = FMath::FloorToInt32(Above / PieceHeightCm + 0.001f);
	float Rest = Above - Full * PieceHeightCm;
	if (Rest < MinPieceCm && Full > 0 && Rest > 0.5f) { --Full; Rest += PieceHeightCm; }
	float Z = Bottom + PieceHeightCm;
	for (int32 Index = 0; Index < Full; ++Index, Z += PieceHeightCm) Out.Middles.Add(FTransform(Rotation, Plan + FVector(0.f, 0.f, Z + PieceHeightCm * 0.5f)));
	if (Rest > 0.5f) Out.Middles.Add(FTransform(Rotation, Plan + FVector(0.f, 0.f, Z + Rest * 0.5f), FVector(1.f, 1.f, Rest / PieceHeightCm)));
	return Out;
}

void AMKIRoadSupports::Configure(AMKIRoadSplineRoad* Road, AMKIRoadJunction* Junction, const TArray<FMKIRoadSupportColumn>& WorldColumns)
{
	mRoad = Road;
	mJunction = Road ? nullptr : Junction;
	mColumns = WorldColumns;
}

void AMKIRoadSupports::BeginPlay()
{
	BuildPieces();
	Super::BeginPlay();
}

void AMKIRoadSupports::BuildPieces()
{
	for (UStaticMeshComponent* Piece : mPieceComponents) if (Piece) Piece->DestroyComponent();
	mPieceComponents.Reset();
	UStaticMesh* BaseMesh = GetBaseMesh();
	UStaticMesh* MiddleMesh = GetMiddleMesh();
	const FTransform Frame = GetActorTransform();
	auto Add = [this, &Frame](UStaticMesh* Mesh, const FTransform& World)
	{
		if (!Mesh) return;
		UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(this, UStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("SupportPiece_%d"), mPieceComponents.Num()));
		Piece->SetupAttachment(RootComponent);
		Piece->SetMobility(EComponentMobility::Static);
		Piece->SetStaticMesh(Mesh);
		Piece->SetRelativeTransform(MeshTransform(Mesh, World).GetRelativeTransform(Frame));
		Piece->SetCollisionProfileName(TEXT("BuildingMesh"));
		Piece->SetCanEverAffectNavigation(true);
		Piece->RegisterComponent();
		mPieceComponents.Add(Piece);
	};
	for (const FMKIRoadSupportColumn& Column : mColumns)
	{
		Add(BaseMesh, Column.Base);
		for (const FTransform& Middle : Column.Middles) Add(MiddleMesh, Middle);
	}
}

void AMKIRoadSupports::ApplyCustomizationData_Native(const FFactoryCustomizationData& CustomizationData)
{
	Super::ApplyCustomizationData_Native(CustomizationData);
	const FResolvedFactoryCustomizationData Resolved(CustomizationData);
	for (UStaticMeshComponent* Piece : mPieceComponents) if (Piece) Resolved.ApplyToMeshComponent(Piece);
}

int32 AMKIRoadSupports::GetDismantleRefundReturnsMultiplier() const
{
	return FMath::Max(1, CountPieces(mColumns));
}

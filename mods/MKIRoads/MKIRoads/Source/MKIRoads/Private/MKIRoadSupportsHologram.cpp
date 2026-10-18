#include "MKIRoadSupportsHologram.h"

#include "MKIRoadSplineRoad.h"
#include "MKIRoadJunction.h"
#include "MKIRoadSplineRoadDisqualifiers.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstance.h"
#include "FGConstructDisqualifier.h"

AMKIRoadSupportsHologram::AMKIRoadSupportsHologram()
{
	// The build gun's lock (H) and nudge (#377).
	mCanLockHologram = true;
	mCanNudgeHologram = true;
	mDefaultNudgeDistance = AMKIRoadSplineRoad::NudgeStepCm;
}

bool AMKIRoadSupportsHologram::IsValidHitResult(const FHitResult& HitResult) const
{
	// Setting the base, any aim counts: the column's line, not the hit, decides.
	if (mPlacingBase) return true;
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	return Cast<AMKIRoadSplineRoad>(HitActor) != nullptr || AMKIRoadJunction::FromActor(HitActor) != nullptr || Super::IsValidHitResult(HitResult);
}

const AActor* AMKIRoadSupportsHologram::Deck() const
{
	if (mRoad) return mRoad;
	if (mJunction) return mJunction;
	return mFoundation.Get();
}

float AMKIRoadSupportsHologram::BaseZFromView(const FVector& Top, const FVector& TraceStart, const FVector& TraceEnd, const FHitResult& Hit)
{
	// Aimed at something near the column (the ground under it), the base goes there.
	if (Hit.bBlockingHit && FVector::Dist2D(Hit.ImpactPoint, Top) <= AMKIRoadSupports::HalfFootprintCm * 1.5f) return Hit.ImpactPoint.Z;
	// Otherwise the point of the column's vertical line nearest the view ray, as a vanilla pillar zoop.
	const FVector Direction = TraceEnd - TraceStart;
	const FVector2D Flat(Direction.X, Direction.Y);
	const double FlatSquared = Flat.SizeSquared();
	if (FlatSquared < 1.0) return Hit.bBlockingHit ? Hit.ImpactPoint.Z : Top.Z;
	const double T = FMath::Max(0.0, FVector2D::DotProduct(FVector2D(Top.X - TraceStart.X, Top.Y - TraceStart.Y), Flat) / FlatSquared);
	return TraceStart.Z + T * Direction.Z;
}

void AMKIRoadSupportsHologram::SetHologramLocationAndRotation(const FHitResult& HitResult)
{
	mColumns.Reset();
	if (mPlacingBase)
	{
		// The top stays; the base follows the aim, in 2 m steps, at least one piece. It no longer snaps to the ground (the
		// maintainer, 2026-10-18: on uneven ground the snapped base still floated on one side), so the player decides how
		// deep it goes, and nudging up and down (Page Up/Down) fine-tunes it.
		const float Depth = mTop.Z - BaseZFromView(mTop, HitResult.TraceStart, HitResult.TraceEnd, HitResult);
		const float Height = AMKIRoadSupports::HeightForDepth(Depth);
		// Where the column stands is the player's call (#215: the maintainer removed the on-road check).
		mFit = HasDeck() ? EMKIRoadSupportFit::Fits : EMKIRoadSupportFit::NoGround;
		mColumns.Add(AMKIRoadSupports::MakeColumn(mTop, mForward, Height));
		mNudgeTop = mTop;
		mNudgeForward = mForward;
		mNudgeHeight = Height;
		UpdatePreview();
		return;
	}
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor);
	AMKIRoadJunction* Junction = Road ? nullptr : AMKIRoadJunction::FromActor(HitActor);
	// Or a level vanilla foundation (the maintainer, 2026-10-18: supports under foundations too).
	AFGBuildableFoundation* Foundation = (Road || Junction) ? nullptr : Cast<AFGBuildableFoundation>(HitActor);
	mRoad = Road;
	mJunction = Junction;
	mFoundation = Foundation;
	mUnderFoundation = Foundation && AMKIRoadSupports::FindTop(*Foundation, HitResult.Location, mTop, mForward);
	mFit = EMKIRoadSupportFit::NoGround;
	// Under a road, or under an intersection, turnaround, or depot (#320, the maintainer: supports only worked under roads).
	if ((Road && AMKIRoadSupports::FindTop(*Road, HitResult.Location, mTop, mForward)) || (Junction && AMKIRoadSupports::FindTop(*Junction, HitResult.Location, mTop, mForward))
		|| mUnderFoundation)
	{
		// The first step previews the shortest column (#219: the maintainer, it started at 8 m); the second sets its length.
		mFit = EMKIRoadSupportFit::Fits;
		mColumns.Add(AMKIRoadSupports::MakeColumn(mTop, mForward, AMKIRoadSupports::MinHeightCm));
	}
	SetActorLocationAndRotation(Deck() ? Deck()->GetActorLocation() : FVector(HitResult.Location), FRotator::ZeroRotator);
	mNudgeTop = mTop;
	mNudgeForward = mForward;
	mNudgeHeight = AMKIRoadSupports::MinHeightCm;
	UpdatePreview();
}

FTransform AMKIRoadSupportsHologram::GetNudgeSpaceTransform() const
{
	return FTransform(FRotationMatrix::MakeFromXZ(mNudgeForward, FVector::UpVector).ToQuat(), mNudgeTop);
}

void AMKIRoadSupportsHologram::SetHologramNudgeLocation()
{
	if (!HasDeck()) return;
	const FVector Offset = GetNudgeSpaceTransform().TransformVectorNoScale(GetHologramNudgeOffset());
	mColumns.Reset();
	if (mPlacingBase)
	{
		// Setting the base: up and down (Page Up/Down) raise and lower it by the nudge distance, never above the shortest column.
		mTop = mNudgeTop;
		mColumns.Add(AMKIRoadSupports::MakeColumn(mTop, mForward, FMath::Max(AMKIRoadSupports::MinHeightCm, mNudgeHeight - Offset.Z)));
	}
	else
	{
		// Placing the top: along and across the road, staying under its deck height.
		mTop = mNudgeTop + FVector(Offset.X, Offset.Y, 0.f);
		mForward = mNudgeForward;
		mColumns.Add(AMKIRoadSupports::MakeColumn(mTop, mForward, AMKIRoadSupports::MinHeightCm));
	}
	mFit = EMKIRoadSupportFit::Fits;
	UpdatePreview();
}

void AMKIRoadSupportsHologram::UpdateClearance()
{
	mClearanceData.Reset();
	const FVector Origin = GetActorLocation();
	const float Inset = AMKIRoadSplineRoad::ClearanceInsetCm, Half = AMKIRoadSupports::PieceHeightCm * 0.5f;
	for (const FMKIRoadSupportColumn& Column : mColumns)
	{
		// The base, then the stacked middles up to the deck's underside, each inset so the ground, a foundation it stands on,
		// and the deck above only touch it.
		FFGClearanceData Base;
		Base.Type = EClearanceType::CT_Default;
		Base.ClearanceBox = FBox(FVector(-AMKIRoadSupports::HalfFootprintCm + Inset, -AMKIRoadSupports::HalfFootprintCm + Inset, -Half + Inset), FVector(AMKIRoadSupports::HalfFootprintCm - Inset, AMKIRoadSupports::HalfFootprintCm - Inset, Half));
		Base.RelativeTransform = FTransform(Column.Base.GetRotation(), Column.Base.GetLocation() - Origin);
		mClearanceData.Add(Base);
		if (Column.Middles.Num() == 0) continue;
		const float Bottom = Column.Base.GetLocation().Z + Half;
		const FTransform& Last = Column.Middles.Last();
		const float Top = Last.GetLocation().Z + Half * Last.GetScale3D().Z - Inset;
		if (Top - Bottom < 1.f) continue;
		FFGClearanceData Middle;
		Middle.Type = EClearanceType::CT_Default;
		Middle.ClearanceBox = FBox(FVector(-AMKIRoadSupports::MiddleHalfCm + Inset, -AMKIRoadSupports::MiddleHalfCm + Inset, 0.f), FVector(AMKIRoadSupports::MiddleHalfCm - Inset, AMKIRoadSupports::MiddleHalfCm - Inset, Top - Bottom));
		Middle.RelativeTransform = FTransform(Column.Base.GetRotation(), FVector(Column.Base.GetLocation().X, Column.Base.GetLocation().Y, Bottom) - Origin);
		mClearanceData.Add(Middle);
	}
	if (mClearanceData.Num() > 0) SetupClearanceDetector();
}

void AMKIRoadSupportsHologram::GetIgnoredClearanceActors(TSet<AActor*>& IgnoredActors) const
{
	Super::GetIgnoredClearanceActors(IgnoredActors);
	// The deck it holds up rests on its top.
	if (mRoad) IgnoredActors.Add(mRoad);
	if (mJunction) mJunction->AddSelfAndTiles(IgnoredActors);
	if (AFGBuildableFoundation* Foundation = mFoundation.Get()) IgnoredActors.Add(Foundation);
}

TSubclassOf<UFGConstructDisqualifier> AMKIRoadSupportsHologram::GetConstructDisqualifierFromClearanceOverlap(const EClearanceOverlapResult& OverlapResult, AActor* OtherActor) const
{
	return AMKIRoadSplineRoad::PieceClearanceDisqualifier(OverlapResult, OtherActor, Super::GetConstructDisqualifierFromClearanceOverlap(OverlapResult, OtherActor));
}

bool AMKIRoadSupportsHologram::DoMultiStepPlacement(bool bIsInputFromARelease)
{
	if (bIsInputFromARelease) return false;
	if (!mPlacingBase)
	{
		// The first click fixes the top; nothing is built yet.
		if (!HasDeck() || mFit != EMKIRoadSupportFit::Fits || mColumns.Num() == 0) return false;
		mPlacingBase = true;
		return false;
	}
	// The second click builds; the next support starts again from its top.
	mPlacingBase = false;
	return true;
}

bool AMKIRoadSupportsHologram::IsChanged() const
{
	// Setting the base, the right mouse button steps back to the top (#218) instead of opening the build menu.
	return mPlacingBase || Super::IsChanged();
}

bool AMKIRoadSupportsHologram::HandleReset()
{
	if (!mPlacingBase) return Super::HandleReset();
	mPlacingBase = false;
	return true;
}

void AMKIRoadSupportsHologram::UpdatePreview()
{
	const AMKIRoadSupports* Supports = GetDefaultBuildable<AMKIRoadSupports>();
	UStaticMesh* BaseMesh = Supports ? Supports->GetBaseMesh() : nullptr;
	UStaticMesh* MiddleMesh = Supports ? Supports->GetMiddleMesh() : nullptr;
	TArray<TPair<UStaticMesh*, FTransform>> Pieces;
	for (const FMKIRoadSupportColumn& Column : mColumns)
	{
		Pieces.Add({ BaseMesh, Column.Base });
		for (const FTransform& Middle : Column.Middles) Pieces.Add({ MiddleMesh, Middle });
	}
	while (mPreviewPieces.Num() < Pieces.Num())
	{
		UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(this, UStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("PreviewSupport_%d"), mPreviewPieces.Num()));
		Piece->SetupAttachment(RootComponent);
		Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Piece->SetCastShadow(false);
		Piece->RegisterComponent();
		mPreviewPieces.Add(Piece);
	}
	const FTransform Frame = GetActorTransform();
	for (int32 Index = 0; Index < mPreviewPieces.Num(); ++Index)
	{
		UStaticMeshComponent* Piece = mPreviewPieces[Index];
		if (!Piece) continue;
		const bool bUsed = Pieces.IsValidIndex(Index) && Pieces[Index].Key;
		Piece->SetVisibility(bUsed);
		if (!bUsed) continue;
		if (Piece->GetStaticMesh() != Pieces[Index].Key) Piece->SetStaticMesh(Pieces[Index].Key);
		Piece->SetRelativeTransform(AMKIRoadSupports::MeshTransform(Pieces[Index].Key, Pieces[Index].Value).GetRelativeTransform(Frame));
	}
	UpdateClearance();
	SetMaterialState(mPlacementMaterialState);
}

void AMKIRoadSupportsHologram::SetMaterialState(EHologramMaterialState State)
{
	Super::SetMaterialState(State);
	// The pillar's own concrete (#218: the maintainer, the hologram material read as patchwork over the stacked and scaled
	// pieces); the invalid material only when it cannot be built. The outline still shows the state.
	UMaterialInterface* Material = State == EHologramMaterialState::HMS_ERROR ? mInvalidPlacementMaterial.Get() : nullptr;
	for (UStaticMeshComponent* Piece : mPreviewPieces)
	{
		if (!Piece) continue;
		for (int32 Slot = 0; Slot < Piece->GetNumMaterials(); ++Slot) Piece->SetMaterial(Slot, Material);
		Piece->SetRenderCustomDepth(true);
		Piece->SetCustomDepthStencilValue(GetStencilForHologramMaterialState(State));
	}
}

void AMKIRoadSupportsHologram::CheckValidPlacement()
{
	Super::CheckValidPlacement();
	// The player decides where a support goes (#215, #216): only an aim off a road is refused.
	if (!HasDeck() || mColumns.Num() == 0) AddConstructDisqualifier(UMKIRoadCDNeedsRoad::StaticClass());
}

int32 AMKIRoadSupportsHologram::GetBaseCostMultiplier() const
{
	return FMath::Max(1, AMKIRoadSupports::CountPieces(mColumns));
}

void AMKIRoadSupportsHologram::SerializeConstructMessage(FArchive& Ar, FNetConstructionID Id)
{
	Super::SerializeConstructMessage(Ar, Id);
	FString RoadPath = mRoad ? mRoad->GetPathName() : FString();
	Ar << RoadPath;
	FString JunctionPath = mJunction ? mJunction->GetPathName() : FString();
	Ar << JunctionPath;
	Ar << mUnderFoundation;
	int32 Count = mColumns.Num();
	Ar << Count;
	if (Ar.IsLoading())
	{
		mRoad = RoadPath.IsEmpty() ? nullptr : FindObject<AMKIRoadSplineRoad>(nullptr, *RoadPath);
		mJunction = JunctionPath.IsEmpty() ? nullptr : FindObject<AMKIRoadJunction>(nullptr, *JunctionPath);
		mColumns.SetNum(FMath::Clamp(Count, 0, 2048));
	}
	for (FMKIRoadSupportColumn& Column : mColumns)
	{
		Ar << Column.Base;
		int32 Middles = Column.Middles.Num();
		Ar << Middles;
		if (Ar.IsLoading()) Column.Middles.SetNum(FMath::Clamp(Middles, 0, 64));
		for (FTransform& Middle : Column.Middles) Ar << Middle;
	}
}

void AMKIRoadSupportsHologram::SetupPendingConstructionHologram(AFGHologram* FromHologram)
{
	Super::SetupPendingConstructionHologram(FromHologram);
	if (const AMKIRoadSupportsHologram* From = Cast<AMKIRoadSupportsHologram>(FromHologram))
	{
		mRoad = From->mRoad;
		mJunction = From->mJunction;
		mFoundation = From->mFoundation;
		mUnderFoundation = From->mUnderFoundation;
		mColumns = From->mColumns;
		SetActorLocationAndRotation(From->GetActorLocation(), FRotator::ZeroRotator);
		UpdatePreview();
	}
}

void AMKIRoadSupportsHologram::ConfigureActor(AFGBuildable* InBuildable) const
{
	Super::ConfigureActor(InBuildable);
	if (AMKIRoadSupports* Supports = Cast<AMKIRoadSupports>(InBuildable)) Supports->Configure(mRoad, mJunction, mColumns);
}

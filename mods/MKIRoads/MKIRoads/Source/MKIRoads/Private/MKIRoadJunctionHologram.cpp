#include "MKIRoadJunctionHologram.h"
#include "FGConstructDisqualifier.h"

#include "MKIRoadJunction.h"
#include "MKIRoadMarkingPattern.h"
#include "MKIRoadsGameWorldModule.h"
#include "MKIRoadSplineRoadHologram.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Components/SplineComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "FGFactoryColoringTypes.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"

AMKIRoadJunctionHologram::AMKIRoadJunctionHologram()
{
	mLanes = AMKIRoadSplineRoad::DefaultLanes;
	// The build gun's lock (H) and nudge (#377).
	mCanLockHologram = true;
	mCanNudgeHologram = true;
	mDefaultNudgeDistance = AMKIRoadSplineRoad::NudgeStepCm;
}

void AMKIRoadJunctionHologram::UpdateClearance()
{
	mClearanceData.Reset();
	AMKIRoadJunction::MakeClearanceData(AMKIRoadJunction::Layout(ArmCount(), mLanes), DeckThicknessCm(), mClearanceData);
	AMKIRoadSplineRoad::HardenForPlacement(mClearanceData);
	if (mClearanceData.Num() > 0) SetupClearanceDetector();
}

void AMKIRoadJunctionHologram::SetSnapToGuideLines(bool bIsEnabled)
{
	Super::SetSnapToGuideLines(bIsEnabled);
	mWorldGrid = bIsEnabled;
}

void AMKIRoadJunctionHologram::GetIgnoredClearanceActors(TSet<AActor*>& IgnoredActors) const
{
	Super::GetIgnoredClearanceActors(IgnoredActors);
	if (AActor* Joined = mJoinedTo.Get())
	{
		IgnoredActors.Add(Joined);
		// A piece's squares lie under its deck, so they are what it joins too.
		if (const AMKIRoadJunction* Piece = AMKIRoadJunction::FromActor(Joined)) Piece->AddSelfAndTiles(IgnoredActors);
	}
}

TSubclassOf<UFGConstructDisqualifier> AMKIRoadJunctionHologram::GetConstructDisqualifierFromClearanceOverlap(const EClearanceOverlapResult& OverlapResult, AActor* OtherActor) const
{
	return AMKIRoadSplineRoad::PieceClearanceDisqualifier(OverlapResult, OtherActor, Super::GetConstructDisqualifierFromClearanceOverlap(OverlapResult, OtherActor));
}

void AMKIRoadJunctionHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMKIRoadJunctionHologram, mLanes);
}

int32 AMKIRoadJunctionHologram::ArmCount() const
{
	const AMKIRoadJunction* Junction = GetDefaultBuildable<AMKIRoadJunction>();
	return Junction ? Junction->GetArmCount() : 4;
}

float AMKIRoadJunctionHologram::DeckThicknessCm() const
{
	const AMKIRoadJunction* Junction = GetDefaultBuildable<AMKIRoadJunction>();
	return Junction ? Junction->GetDeckThicknessCm() : 100.f;
}

FTransform AMKIRoadJunctionHologram::FaceSideToEnd(int32 Arms, int32 Lanes, int32 QuarterTurn, const FVector& EndLocation, const FVector& EndOutward)
{
	const FMKIRoadJunctionLayout Shape = AMKIRoadJunction::Layout(Arms, Lanes);
	FBox2D Bounds(ForceInit);
	for (const TArray<FVector2D>& Cell : Shape.Cells) for (const FVector2D& Corner : Cell) Bounds += Corner;
	if (!Bounds.bIsValid) return FTransform(EndLocation);
	static const FVector2D Sides[4] = { FVector2D(1.0, 0.0), FVector2D(0.0, 1.0), FVector2D(-1.0, 0.0), FVector2D(0.0, -1.0) };
	const FVector2D Side = Sides[((QuarterTurn % 4) + 4) % 4];
	// The side faces back along what it meets: its outward direction is the end's inward one.
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(-EndOutward.Y, -EndOutward.X)) - FMath::RadiansToDegrees(FMath::Atan2(static_cast<float>(Side.Y), static_cast<float>(Side.X)));
	const FRotator Rotation(0.f, FRotator::NormalizeAxis(Yaw), 0.f);
	// The point on that side that goes on the end: an arm's end there, else the middle of the side.
	const FVector2D Middle = Bounds.GetCenter();
	FVector2D Contact(Side.X > 0.5 ? Bounds.Max.X : (Side.X < -0.5 ? Bounds.Min.X : Middle.X), Side.Y > 0.5 ? Bounds.Max.Y : (Side.Y < -0.5 ? Bounds.Min.Y : Middle.Y));
	for (const FMKIRoadJunctionArm& Arm : Shape.Arms)
	{
		if (FVector2D::DotProduct(Arm.Outward.GetSafeNormal(), Side) > 0.99) { Contact = Arm.Center; break; }
	}
	return FTransform(Rotation, EndLocation - Rotation.RotateVector(FVector(Contact.X, Contact.Y, 0.f)));
}

void AMKIRoadJunctionHologram::TakeLanesFrom(const AActor* Source, int32 Lanes)
{
	// Once per road or piece aimed at (#238), so Plus and Minus still change the width while joined there.
	if (Source == mLanesTakenFrom.Get()) return;
	mLanesTakenFrom = Source;
	SetLanes(Lanes);
}

void AMKIRoadJunctionHologram::SetLanes(int32 Lanes)
{
	// A depot on both sides takes 2 or 4 lanes (#240); Minus stops at 2.
	if (!AMKIRoadJunction::IsSupportedLaneCount(Lanes) || Lanes == mLanes) return;
	mLanes = Lanes;
	UpdatePreview();
}

void AMKIRoadJunctionHologram::BeginPlay()
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
	if (IsLocalHologram()) RegisterLaneInput();
	UpdatePreview();
}

void AMKIRoadJunctionHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterLaneInput();
	Super::EndPlay(EndPlayReason);
}

void AMKIRoadJunctionHologram::SetHologramLocationAndRotation(const FHitResult& HitResult)
{
	AActor* HitActor = HitResult.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = HitResult.GetActor();
	mJoinedTo = nullptr;
	// A road end aimed within 2 m of it (the snap rule every piece shares): the turned side
	// against it, and the width follows the road's.
	if (const AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(HitActor))
	{
		FVector End, Outward;
		if (AMKIRoadSplineRoad::NearEnd(*Road, HitResult.Location, End, Outward) && !Outward.GetSafeNormal2D().IsNearlyZero())
		{
			TakeLanesFrom(Road, Road->GetLanes());
			SetActorTransform(FaceSideToEnd(ArmCount(), mLanes, mQuarterTurns, End, Outward.GetSafeNormal2D()));
			mJoinedTo = const_cast<AMKIRoadSplineRoad*>(Road);
			return;
		}
	}
	// Another intersection, turnaround, or depot (#238; the maintainer: a turnaround did not snap to an intersection), aimed
	// within 2 m of a side: the turned side against that side (on its arm when it has one, whose width it takes).
	if (const AMKIRoadJunction* Other = AMKIRoadJunction::FromActor(HitActor))
	{
		FVector OnSide, Outward;
		bool bArm = false;
		if (Other->NearSide(HitResult.Location, OnSide, Outward, bArm))
		{
			if (bArm) TakeLanesFrom(Other, Other->GetLanes());
			mJoinedTo = const_cast<AMKIRoadJunction*>(Other);
			SetActorTransform(FaceSideToEnd(ArmCount(), mLanes, mQuarterTurns, OnSide, Outward));
			return;
		}
	}
	mLanesTakenFrom = nullptr;
	// A foundation's top: the center on its half-tile grid, along its axes, turned in 45 degree steps (#253).
	if (const AFGBuildableFoundation* Foundation = Cast<AFGBuildableFoundation>(HitActor))
	{
		// Not a road or junction deck: aimed at one away from a road end, it rests on the aimed point (below).
		if (Foundation->mHeight > 0.f && !AMKIRoadJunction::FromActor(HitActor) && !Cast<AMKIRoadSplineRoad>(HitActor))
		{
			// At an edge, as a road end snaps there (#319, the maintainer): the turned side against the edge (an arm's end, or a
			// blank side flush along it), level with the top, the piece reaching out from the foundation.
			FVector OnEdge, EdgeOutward;
			if (AMKIRoadSplineRoadHologram::FoundationEdgeEnd(*Foundation, HitResult, this, OnEdge, EdgeOutward))
			{
				SetActorTransform(FaceSideToEnd(ArmCount(), mLanes, mQuarterTurns, OnEdge, EdgeOutward));
				return;
			}
			const FTransform Frame(FRotator(0.f, Foundation->GetActorRotation().Yaw, 0.f), Foundation->GetActorLocation());
			const FVector Local = Frame.InverseTransformPosition(HitResult.Location);
			const float TopZ = Foundation->mHeight * 0.5f;
			if (FMath::Abs(Local.Z - TopZ) <= 25.f)
			{
				const float HalfX = Foundation->mDepth * 0.5f, HalfY = Foundation->mWidth * 0.5f;
				auto OnGrid = [](float Value, float Half) { return FMath::RoundToFloat((Value + Half) / FoundationGridCm) * FoundationGridCm - Half; };
				// The deck rests on the foundation: its top a deck thickness above the foundation's (the maintainer: placed with its top
				// on the foundation's, the deck sat inside the foundation and could not be built there).
				const FVector Snapped = Frame.TransformPosition(FVector(OnGrid(Local.X, HalfX), OnGrid(Local.Y, HalfY), TopZ)) + FVector(0.f, 0.f, DeckThicknessCm());
				SetActorLocationAndRotation(Snapped, FRotator(0.f, Foundation->GetActorRotation().Yaw + 45.f * mRotateNotches, 0.f));
				return;
			}
		}
	}
	// Anywhere else: on the aimed point, along the view's heading, turned freely by the scroll rotation (#253).
	const APawn* Builder = GetConstructionInstigator();
	const AController* Controller = Builder ? Builder->GetController() : nullptr;
	const float View = Controller ? Controller->GetControlRotation().Yaw : 0.f;
	// The deck rests on the aimed surface, as a road's free end does: its top a deck thickness above it.
	const FVector Free = HitResult.Location + FVector(0.f, 0.f, DeckThicknessCm());
	if (mWorldGrid)
	{
		// Ctrl (#379): on the world grid, along a world axis, turned in quarter turns, as a foundation locks to it.
		SetActorLocationAndRotation(AMKIRoadSplineRoad::WorldGridLocation(Free), FRotator(0.f, AMKIRoadSplineRoad::WorldGridYaw(View) + 90.f * mQuarterTurns, 0.f));
		return;
	}
	SetActorLocationAndRotation(Free, FRotator(0.f, FRotator::NormalizeAxis(View + static_cast<float>(mRotateStepDeg * mRotateNotches)), 0.f));
}

void AMKIRoadJunctionHologram::ScrollRotate(int32 Delta, int32 Step)
{
	const int32 Notch = Delta > 0 ? 1 : (Delta < 0 ? -1 : 0);
	mQuarterTurns = ((mQuarterTurns + Notch) % 4 + 4) % 4;
	mRotateNotches += Delta;
	if (Step > 0) mRotateStepDeg = Step;
}

void AMKIRoadJunctionHologram::PostHologramPlacement(const FHitResult& HitResult, bool bCallForChildren)
{
	Super::PostHologramPlacement(HitResult, bCallForChildren);
	// As the road's hologram: it places itself, never through the snapped building the construct message carries.
	mSnappedBuilding = nullptr;
}

int32 AMKIRoadJunctionHologram::GetBaseCostMultiplier() const
{
	return AMKIRoadJunction::FoundationEquivalents(ArmCount(), mLanes);
}

void AMKIRoadJunctionHologram::UpdatePreview()
{
	if (!mPreview) return;
	const FMKIRoadDeckGeometry Geometry = AMKIRoadJunction::BuildGeometry(AMKIRoadJunction::Layout(ArmCount(), mLanes), DeckThicknessCm());
	TArray<FProcMeshTangent> Tangents;
	for (const FVector& Tangent : Geometry.Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
	mPreview->CreateMeshSection_LinearColor(0, Geometry.Vertices, Geometry.Triangles, Geometry.Normals, Geometry.UVs, Geometry.UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
	mPreviewLanes = mLanes;
	mPreviewShowsDeck = false;
	UpdateClearance();
	// The North American Road markings, as the built piece gets by default (#234, #241).
	if (const UMKIRoadMarkingPattern* Pattern = MKIRoadDefaultPatternObject(); Pattern && mPreviewMarkings)
	{
		FMKIRoadDeckGeometry Paint[2];
		AMKIRoadJunction::MarkingGeometry(ArmCount(), mLanes, Pattern, Paint);
		MKIRoadApplyPaint(mPreviewMarkings, *Pattern, Paint);
	}
	SetMaterialState(mPlacementMaterialState);
}

void AMKIRoadJunctionHologram::SetMaterialState(EHologramMaterialState State)
{
	Super::SetMaterialState(State);
	if (!mPreview) return;
	// As the road's preview: asphalt in the default swatch in every state, the state shown by the coloured outline.
	AMKIRoadJunction* Junction = const_cast<AMKIRoadJunction*>(GetDefaultBuildable<AMKIRoadJunction>());
	UMaterialInterface* Material = Junction ? Junction->GetDeckMaterial() : nullptr;
	if (Material && !mPreviewShowsDeck)
	{
		FResolvedFactoryCustomizationData Resolved;
		Resolved.ApplySwatch(Junction->GetDefaultSwatchCustomizationOverride(this));
		Resolved.ApplyToMeshComponent(mPreview);
		mPreviewShowsDeck = true;
	}
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

void AMKIRoadJunctionHologram::SerializeConstructMessage(FArchive& Ar, FNetConstructionID Id)
{
	mSnappedBuilding = nullptr;
	Super::SerializeConstructMessage(Ar, Id);
	Ar << mLanes;
	if (Ar.IsLoading() && !AMKIRoadJunction::IsSupportedLaneCount(mLanes)) mLanes = AMKIRoadSplineRoad::DefaultLanes;
}

void AMKIRoadJunctionHologram::SetupPendingConstructionHologram(AFGHologram* FromHologram)
{
	Super::SetupPendingConstructionHologram(FromHologram);
	if (const AMKIRoadJunctionHologram* From = Cast<AMKIRoadJunctionHologram>(FromHologram))
	{
		mLanes = From->mLanes;
		SetActorTransform(From->GetActorTransform());
		UpdatePreview();
	}
}

void AMKIRoadJunctionHologram::ConfigureActor(AFGBuildable* InBuildable) const
{
	Super::ConfigureActor(InBuildable);
	AMKIRoadJunction* Junction = Cast<AMKIRoadJunction>(InBuildable);
	if (!Junction) return;
	Junction->SetLanes(mLanes);
	// As a new road, painted with the North American Road pattern unless the build gun carries one (#234).
	if (!Junction->GetCustomizationData_Native().PatternDesc) Junction->GetCustomizationData_Native().PatternDesc = MKIRoadDefaultPattern();
}

void AMKIRoadJunctionHologram::RegisterLaneInput()
{
	// As the road's hologram: a transient mapping context above the build gun's binds Plus and Minus.
	APlayerController* Controller = Cast<APlayerController>(GetConstructionInstigator() ? GetConstructionInstigator()->GetController() : nullptr);
	UEnhancedInputLocalPlayerSubsystem* Subsystem = Controller ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer()) : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogMKIRoads, Warning, TEXT("%s: no local Enhanced Input subsystem; Plus/Minus lane selection is unavailable"), *GetName());
		return;
	}
	mPlusAction = NewObject<UInputAction>(this, TEXT("MKIRoadJunctionPlus"));
	mMinusAction = NewObject<UInputAction>(this, TEXT("MKIRoadJunctionMinus"));
	mLaneMappingContext = NewObject<UInputMappingContext>(this, TEXT("MKIRoadJunctionLaneContext"));
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
	Input->BindAction(mPlusAction, ETriggerEvent::Started, this, &AMKIRoadJunctionHologram::OnPlus);
	Input->BindAction(mMinusAction, ETriggerEvent::Started, this, &AMKIRoadJunctionHologram::OnMinus);
	Subsystem->AddMappingContext(mLaneMappingContext, AMKIRoadSplineRoadHologram::LaneInputPriority);
	mLaneInputController = Controller;
}

void AMKIRoadJunctionHologram::UnregisterLaneInput()
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

void AMKIRoadJunctionHologram::OnPlus(const FInputActionValue& Value)
{
	SetLanes(AMKIRoadSplineRoadHologram::StepLanes(mLanes, 1));
}

void AMKIRoadJunctionHologram::OnMinus(const FInputActionValue& Value)
{
	SetLanes(AMKIRoadSplineRoadHologram::StepLanes(mLanes, -1));
}

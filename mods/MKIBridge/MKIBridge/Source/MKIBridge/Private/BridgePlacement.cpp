#include "BridgeInternal.h"
#include "Components/SplineComponent.h"
#include "UObject/EnumProperty.h"
#include "FGSplineBuildableInterface.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Equipment/FGBuildGunPaint.h"
#include "FGCustomizationRecipe.h"
#include "FGVehicle.h"

// Placement and dismantle routes: structures, factory buildings, extractors, conveyor supports, foundations, and deletion.
// Split from the single MKIBridge.cpp without behavior changes (#44).

namespace
{
    // Temporary spline road placement (step 1 of the spline road investigation). The class lives in MKI Roads, which the
    // bridge never links against, so it is resolved by its native path and fails closed when an older MKI Roads lacks it.
    const TCHAR* SplineRoadClassPath = TEXT("/Script/MKIRoads.MKIRoadSplineRoad");
    const TCHAR* SplineRoadRecipePath = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/AsphaltSet/Recipe_Foundation_Asphalt_8x4.Recipe_Foundation_Asphalt_8x4_C");
    constexpr double SplineRoadLaneWidthCm = 800.0;
    constexpr double SplineRoadFoundationAreaCm2 = 800.0 * 800.0;
    constexpr double SplineRoadMinSegmentCm = 400.0;
    constexpr double SplineRoadMaxSegmentCm = 10000.0;
    // 25 degrees, as vanilla railway track (and the road hologram) allow: tan(25 deg) = 46.63 %.
    constexpr double SplineRoadMaxGradePercent = 46.630766;
    constexpr int32 SplineRoadMaxPoints = 16;
    constexpr int32 SplineRoadSamplesPerSegment = 128;

    UClass* SplineRoadClass()
    {
        UClass* Class = FindObject<UClass>(FTopLevelAssetPath(SplineRoadClassPath));
        return Class && Class->IsChildOf(AFGBuildable::StaticClass()) ? Class : nullptr;
    }

    bool IsSplineRoad(const AActor* Actor)
    {
        UClass* Class = SplineRoadClass();
        return Actor && Class && Actor->IsA(Class);
    }

    /** Equivalent asphalt 8x4 foundations of a deck, ceil(length x width / 64 m2): what placement charges and dismantle refunds. */
    int32 SplineRoadFoundationEquivalents(double LengthCm, int32 Lanes)
    {
        return FMath::Max(1, FMath::CeilToInt32(FMath::Max(0.0, LengthCm) * Lanes * SplineRoadLaneWidthCm / SplineRoadFoundationAreaCm2 - 1e-6));
    }

    /** The refund multiple of a built spline road, read from its spline and saved width (the native getter is protected). */
    int32 SplineRoadRefundMultiplier(AActor* Road)
    {
        const IFGSplineBuildableInterface* Spline = Cast<IFGSplineBuildableInterface>(Road);
        const USplineComponent* Component = Spline ? Spline->GetSplineComponent() : nullptr;
        const FIntProperty* LanesProperty = Road ? FindFProperty<FIntProperty>(Road->GetClass(), TEXT("mLanes")) : nullptr;
        if (!Component || !LanesProperty) return 1;
        return SplineRoadFoundationEquivalents(Component->GetSplineLength(), LanesProperty->GetPropertyValue_InContainer(Road));
    }

    bool ReadSplineVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Value = nullptr;
        if (!Object->TryGetObjectField(Field, Value) || !Value || !Value->IsValid()) return false;
        double X = 0.0, Y = 0.0, Z = 0.0;
        if (!(*Value)->TryGetNumberField(TEXT("x"), X) || !(*Value)->TryGetNumberField(TEXT("y"), Y) || !(*Value)->TryGetNumberField(TEXT("z"), Z)) return false;
        Out = FVector(X, Y, Z);
        return !Out.ContainsNaN();
    }

    TSharedRef<FJsonObject> SplineVectorJson(const FVector& Value)
    {
        const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("x"), Value.X);
        Object->SetNumberField(TEXT("y"), Value.Y);
        Object->SetNumberField(TEXT("z"), Value.Z);
        return Object;
    }

    /** Geometry of one spline segment, sampled from the same cubic Hermite curve a USplineComponent builds from the points. */
    struct FSplineRoadSegmentFacts
    {
        double LengthCm = 0.0;
        double MaxGradePercent = 0.0;
        double MinHorizontalRadiusCm = TNumericLimits<double>::Max();
        bool bVertical = false;
    };

    FSplineRoadSegmentFacts SampleSplineRoadSegment(const FVector& P0, const FVector& T0, const FVector& P1, const FVector& T1)
    {
        FSplineRoadSegmentFacts Facts;
        FVector Previous = P0;
        for (int32 Index = 0; Index <= SplineRoadSamplesPerSegment; ++Index)
        {
            const double A = static_cast<double>(Index) / SplineRoadSamplesPerSegment, A2 = A * A, A3 = A2 * A;
            const FVector Position = P0 * (2 * A3 - 3 * A2 + 1) + T0 * (A3 - 2 * A2 + A) + P1 * (-2 * A3 + 3 * A2) + T1 * (A3 - A2);
            const FVector D1 = P0 * (6 * A2 - 6 * A) + T0 * (3 * A2 - 4 * A + 1) + P1 * (-6 * A2 + 6 * A) + T1 * (3 * A2 - 2 * A);
            const FVector D2 = P0 * (12 * A - 6) + T0 * (6 * A - 4) + P1 * (-12 * A + 6) + T1 * (6 * A - 2);
            if (Index > 0) Facts.LengthCm += FVector::Distance(Previous, Position);
            Previous = Position;
            const double Horizontal = FVector2D(D1.X, D1.Y).Size();
            if (Horizontal < KINDA_SMALL_NUMBER) { Facts.bVertical = true; continue; }
            Facts.MaxGradePercent = FMath::Max(Facts.MaxGradePercent, FMath::Abs(D1.Z) / Horizontal * 100.0);
            const double Cross = FMath::Abs(D1.X * D2.Y - D1.Y * D2.X);
            if (Cross > KINDA_SMALL_NUMBER) Facts.MinHorizontalRadiusCm = FMath::Min(Facts.MinHorizontalRadiusCm, FMath::Pow(Horizontal, 3.0) / Cross);
        }
        return Facts;
    }
}

bool FMKIBridgeModule::HandlePlaceStructure(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    // DispatchRoute has already checked the peer and the token; this re-check keeps the dispatcher safe if it is
    // ever routed directly. Authentication always precedes body parsing.
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString Type;
    if (!Json->TryGetStringField(TEXT("type"), Type))
    {
        OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("A placement type is required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    if (Type.Equals(TEXT("foundation"), ESearchCase::IgnoreCase)) return HandlePlaceFoundation(Request, OnComplete);
    if (Type.Equals(TEXT("foundation_grid"), ESearchCase::IgnoreCase)) return HandlePlaceFoundationGrid(Request, OnComplete);
    if (Type.Equals(TEXT("miner"), ESearchCase::IgnoreCase)) return HandlePlaceMiner(Request, OnComplete);
    if (Type.Equals(TEXT("resource_well"), ESearchCase::IgnoreCase)) return HandlePlaceResourceWell(Request, OnComplete);
    if (Type.Equals(TEXT("geothermal"), ESearchCase::IgnoreCase)) return HandlePlaceGeothermal(Request, OnComplete);
    if (Type.Equals(TEXT("conveyor_pole"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("conveyor_lift"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("pipe_support"), ESearchCase::IgnoreCase)) return HandlePlaceConveyorSupport(Request, OnComplete);
    return HandlePlaceFactoryBuilding(Request, OnComplete);
}

/** How far above a conveyor pole's or lift's requested base the support trace starts (#268). */
static constexpr double ConveyorSupportTraceAboveCm = 300.0;

/**
 * The first buildable whose solid geometry overlaps Box (#329). GetCollidingBuildablesInBoundingBox reports every
 * buildable whose whole-actor bounds touch the box, and large actors (a truck depot's deck and vehicle paths 15 m away)
 * filled an empty spot. Each candidate is judged by its own collision-enabled components' bounds instead; one with no
 * component to judge by (a lightweight instance) still counts, so the check never passes on missing data.
 */
/** How far a belt's or pipe's spline is kept from a foundation's volume: half a belt's width and its height. */
static constexpr double SplineClearanceCm = 100.0;
static AFGBuildable* FirstSolidOverlap(const TArray<AFGBuildable*>& Candidates, const FBox& Box)
{
    for (AFGBuildable* Candidate : Candidates)
    {
        // A power line passes through what is built round it (live: a wire refused a foundation beneath it).
        if (!Candidate || Candidate->IsA<AFGBuildableWire>()) continue;
        // A belt or pipe is a thin spline whose mesh bounds span its whole curve
        // (one curved belt's box covered 35 m by 24 m): it is judged by points along its spline, a belt's half-width round each.
        const FString ClassName = Candidate->GetClass()->GetName();
        const IFGSplineBuildableInterface* SplineBuildable = Cast<IFGSplineBuildableInterface>(Candidate);
        const USplineComponent* Spline = SplineBuildable && (ClassName.Contains(TEXT("ConveyorBelt")) || ClassName.Contains(TEXT("Pipeline"))) ? SplineBuildable->GetSplineComponent() : nullptr;
        if (Spline)
        {
            const FBox Reach = Box.ExpandBy(FVector(SplineClearanceCm, SplineClearanceCm, SplineClearanceCm));
            const float Length = Spline->GetSplineLength();
            for (float Along = 0.f; Along <= Length + 1.f; Along += 50.f)
                if (Reach.IsInside(Spline->GetLocationAtDistanceAlongSpline(FMath::Min(Along, Length), ESplineCoordinateSpace::World))) return Candidate;
            continue;
        }
        TInlineComponentArray<UPrimitiveComponent*> Primitives(Candidate);
        bool bJudged = false;
        for (const UPrimitiveComponent* Primitive : Primitives)
        {
            if (!Primitive || !Primitive->IsRegistered() || Primitive->GetCollisionEnabled() == ECollisionEnabled::NoCollision) continue;
            bJudged = true;
            if (Primitive->Bounds.GetBox().Intersect(Box)) return Candidate;
        }
        if (!bJudged) return Candidate;
    }
    return nullptr;
}

bool FMKIBridgeModule::HandlePlaceConveyorSupport(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString Text; if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR C((const ANSICHAR*)Request.Body.GetData(), Request.Body.Num()); Text = FString(C.Length(), C.Get()); }
    TSharedPtr<FJsonObject> Json; auto Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be JSON."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString Type, RequestId; bool Confirm = false; Json->TryGetStringField(TEXT("type"), Type); Json->TryGetStringField(TEXT("requestId"), RequestId); Json->TryGetBoolField(TEXT("confirm"), Confirm);
    if (Confirm && RequestId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("Confirmed placement requires requestId."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr; AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr; APlayerController* PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    if (!World || !Player || !PC) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    auto ReadVector = [](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr; double X = 0, Y = 0, Z = 0;
        return Parent->TryGetObjectField(Name, Object) && Object && (*Object)->TryGetNumberField(TEXT("x"), X) && (*Object)->TryGetNumberField(TEXT("y"), Y) && (*Object)->TryGetNumberField(TEXT("z"), Z) && (Out = FVector(X, Y, Z), true);
    };
    auto VectorJson = [](const FVector& V) { auto O = MakeShared<FJsonObject>(); O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z); return O; };
    const bool IsLift = Type.Equals(TEXT("conveyor_lift"), ESearchCase::IgnoreCase);
    FVector Base, Top, BottomFacing, TopFacing; double HeightCm = 300, Yaw = 0; FString BottomRole = TEXT("input"), TopRole = TEXT("output");
    if (IsLift)
    {
        const TSharedPtr<FJsonObject>* BottomObject = nullptr; const TSharedPtr<FJsonObject>* TopObject = nullptr;
        if (!Json->TryGetObjectField(TEXT("bottom"), BottomObject) || !Json->TryGetObjectField(TEXT("top"), TopObject) || !BottomObject || !TopObject ||
            !ReadVector(*BottomObject, TEXT("position"), Base) || !ReadVector(*BottomObject, TEXT("facing"), BottomFacing) ||
            !ReadVector(*TopObject, TEXT("position"), Top) || !ReadVector(*TopObject, TEXT("facing"), TopFacing))
        { OnComplete(ErrorResponse(TEXT("invalid_lift_endpoints"), TEXT("Lift placement requires bottom/top position and facing vectors."), EHttpServerResponseCodes::BadRequest)); return true; }
        (*BottomObject)->TryGetStringField(TEXT("role"), BottomRole); (*TopObject)->TryGetStringField(TEXT("role"), TopRole);
        const double LiftHeightCm = FMath::Abs(Base.Z - Top.Z);
        if (!FMath::IsNearlyEqual(LiftHeightCm, SnapToIncrement(LiftHeightCm, 100.0), 0.01) || BottomRole.Equals(TopRole, ESearchCase::IgnoreCase) || (!BottomRole.Equals(TEXT("input"), ESearchCase::IgnoreCase) && !BottomRole.Equals(TEXT("output"), ESearchCase::IgnoreCase)) ||
            (!TopRole.Equals(TEXT("input"), ESearchCase::IgnoreCase) && !TopRole.Equals(TEXT("output"), ESearchCase::IgnoreCase)) || FMath::Abs(Base.Z - Top.Z) < 100.0)
        { OnComplete(ErrorResponse(TEXT("invalid_lift_flow"), TEXT("Lift endpoints require opposite input/output roles and a vertical separation snapped to 100 cm steps."), EHttpServerResponseCodes::BadRequest)); return true; }
        // The lift's bottom and top rotate independently in game, so a lift may turn the belt. The bottom takes any
        // horizontal facing; the top turns relative to the bottom in the hologram's 90-degree steps (verified live:
        // a 30-degree request snapped to 0). So each facing must be horizontal and the turn a multiple of 90 degrees.
        auto IsHorizontalFacing = [](const FVector& Facing) { const FVector Unit = Facing.GetSafeNormal(); return !Unit.IsNearlyZero() && FMath::Abs(Unit.Z) <= 0.01; };
        if (!IsHorizontalFacing(BottomFacing) || !IsHorizontalFacing(TopFacing)) { OnComplete(ErrorResponse(TEXT("invalid_lift_facing"), TEXT("Each lift endpoint facing must be a non-zero horizontal vector."), EHttpServerResponseCodes::BadRequest)); return true; }
        BottomFacing = BottomFacing.GetSafeNormal2D(); TopFacing = TopFacing.GetSafeNormal2D();
        const double TurnDegrees = FRotator::NormalizeAxis(TopFacing.Rotation().Yaw - BottomFacing.Rotation().Yaw);
        if (FMath::Abs(TurnDegrees - 90.0 * FMath::RoundToDouble(TurnDegrees / 90.0)) > 1.0) { OnComplete(ErrorResponse(TEXT("invalid_lift_facing"), TEXT("The lift top turns relative to the bottom in 90-degree steps; the angle between the bottom and top facings must be a multiple of 90 degrees."), EHttpServerResponseCodes::BadRequest)); return true; }
    }
    else
    {
        if (!ReadVector(Json, TEXT("position"), Base) || !Json->TryGetNumberField(TEXT("heightCm"), HeightCm)) { OnComplete(ErrorResponse(TEXT("invalid_pole"), TEXT("Pole placement requires position and heightCm."), EHttpServerResponseCodes::BadRequest)); return true; }
        Json->TryGetNumberField(TEXT("yaw"), Yaw);
        // A pipe support stands its pipe at 175, 275, 375 or 475 cm (measured live, #308); a conveyor pole in 1 m steps.
        HeightCm = Type.Equals(TEXT("pipe_support"), ESearchCase::IgnoreCase) ? FMath::Clamp(PipeSupportMinHeightCm + SnapToIncrement(HeightCm - PipeSupportMinHeightCm, 100.0), PipeSupportMinHeightCm, PipeSupportMaxHeightCm) : FMath::Clamp(SnapToIncrement(HeightCm, 100.0), 100.0, 700.0);
    }
    if (FVector::Distance(Base, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm() || (IsLift && FVector::Distance(Top, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm())) { OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Placement exceeds the configured safety radius."), EHttpServerResponseCodes::Forbidden)); return true; }
    const FVector SupportPoint = IsLift ? Base - FVector(0, 0, 100.0) : Base;
    // The surface under the requested base, traced from just above it (#268): traced from 50 m up, a pole or lift asked
    // for on the ground under a road deck found the deck instead and stood on it.
    FHitResult SurfaceHit; FCollisionQueryParams SurfaceQuery; SurfaceQuery.AddIgnoredActor(Player);
    if (!World->LineTraceSingleByChannel(SurfaceHit, SupportPoint + FVector(0, 0, ConveyorSupportTraceAboveCm), SupportPoint - FVector(0, 0, 10000.0), ECC_WorldStatic, SurfaceQuery))
    { OnComplete(ErrorResponse(TEXT("surface_not_found"), TEXT("No supporting surface was found for the conveyor structure."), EHttpServerResponseCodes::Conflict)); return true; }
    auto Body = MakeShared<FJsonObject>(); Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetStringField(TEXT("type"), Type); Body->SetObjectField(TEXT("base"), VectorJson(Base));
    if (IsLift) { Body->SetObjectField(TEXT("top"), VectorJson(Top)); Body->SetStringField(TEXT("flowDirection"), BottomRole.Equals(TEXT("input"), ESearchCase::IgnoreCase) ? TEXT("bottom_to_top") : TEXT("top_to_bottom")); }
    else Body->SetNumberField(TEXT("heightCm"), HeightCm);
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }
    // A pipe support (#308) is placed and height-checked like a conveyor pole: both are pole holograms with a snap point on top.
    const bool IsPipeSupport = Type.Equals(TEXT("pipe_support"), ESearchCase::IgnoreCase);
    const TCHAR* RecipePath = IsLift ? TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_ConveyorLiftMk1.Recipe_ConveyorLiftMk1_C") : IsPipeSupport ? TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_PipeSupport.Recipe_PipeSupport_C") : TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_ConveyorPole.Recipe_ConveyorPole_C");
    UClass* RecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, RecipePath); if (!RecipeClass) { OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("Conveyor recipe is unavailable."), EHttpServerResponseCodes::ServerError)); return true; }
    FString Missing; if (!ConsumeRecipeCost(World, RecipeClass, 1, Missing)) { OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), Missing, EHttpServerResponseCodes::Conflict)); return true; }
    AFGHologram* Spawned = AFGHologram::SpawnHologramFromRecipe(RecipeClass, PC, Base, Player); AFGBuildableHologram* Hologram = Cast<AFGBuildableHologram>(Spawned);
    if (!Hologram) { RefundRecipeCost(World, RecipeClass, 1); OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"), TEXT("Native conveyor hologram could not be spawned."), EHttpServerResponseCodes::ServerError)); return true; }
    Hologram->WaitForGamestateApplyCustomizationData();
    FTimerHandle Timer;
    World->GetTimerManager().SetTimer(Timer, [this, WeakWorld = TWeakObjectPtr<UWorld>(World), WeakPC = TWeakObjectPtr<APlayerController>(PC), WeakRecipe = TWeakObjectPtr<UClass>(RecipeClass), WeakHologram = TWeakObjectPtr<AFGBuildableHologram>(Hologram), Ticket = ActiveTicket, IsLift, IsPipeSupport, Base, Top, SupportPoint, SurfaceHit, BottomFacing, TopFacing, BottomRole, HeightCm, Yaw, RequestId, Body, OnComplete, VectorJson]() mutable
    {
        UWorld* World = WeakWorld.Get(); APlayerController* PC = WeakPC.Get(); UClass* RecipeClass = WeakRecipe.Get(); AFGBuildableHologram* Hologram = WeakHologram.Get();
        if (!World || !PC || !RecipeClass || !Hologram || (Ticket && Ticket->bAbandoned))
        {
            if (Hologram) Hologram->Destroy();
            if (World && RecipeClass) RefundRecipeCost(World, RecipeClass, 1);
            OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"), TEXT("The world, player, recipe, or hologram disappeared before construction; nothing was built."), EHttpServerResponseCodes::Conflict));
            return;
        }
        const FRotator Saved = PC->GetControlRotation(); const double PlacementYaw = IsLift ? BottomFacing.Rotation().Yaw : Yaw; PC->SetControlRotation(FRotator(Saved.Pitch, PlacementYaw, Saved.Roll)); Hologram->SetScrollRotateValue(FMath::RoundToInt(PlacementYaw));
        if (IsLift && BottomRole.Equals(TEXT("output"), ESearchCase::IgnoreCase))
        {
            TArray<TSubclassOf<UFGBuildGunModeDescriptor>> Modes; Hologram->GetSupportedBuildModes(Modes);
            for (TSubclassOf<UFGBuildGunModeDescriptor> Mode : Modes) if (Mode && Mode->IsChildOf(UFGHologramBuildModeDescriptor::StaticClass()) && Mode->GetName().Contains(TEXT("Reverse"), ESearchCase::IgnoreCase)) { Hologram->SetBuildModeOverride(TSubclassOf<UFGHologramBuildModeDescriptor>(Mode.Get())); break; }
        }
        FHitResult FirstHit = SurfaceHit; FirstHit.Location = SupportPoint; FirstHit.ImpactPoint = SupportPoint; FirstHit.ImpactNormal = FVector::UpVector; FirstHit.Normal = FVector::UpVector;
        Hologram->UpdateHologramPlacement(FirstHit); bool Finished = Hologram->DoMultiStepPlacement(false);
        if (IsLift)
        {
            // Once the first endpoint is locked, the top's rotation is the hologram's scroll-rotation value, read as
            // the top facing's yaw relative to the bottom facing (verified live: bottom yaw + scroll = top yaw).
            const int32 TurnSteps = FMath::RoundToInt(FRotator::NormalizeAxis(TopFacing.Rotation().Yaw - BottomFacing.Rotation().Yaw) / 90.0);
            Hologram->SetScrollRotateValue(((TurnSteps % 4) + 4) % 4 * 90);
            // The second step reads the top height from the aim ray, as the build gun does. The first step's trace is
            // vertical, straight down the lift axis, which left every lift at one height (verified live), so aim a
            // horizontal ray through the requested top point instead.
            FHitResult SecondHit = FirstHit; SecondHit.Location = Top; SecondHit.ImpactPoint = Top;
            SecondHit.TraceStart = Top + BottomFacing * 1000.0; SecondHit.TraceEnd = Top - BottomFacing * 1000.0;
            SecondHit.ImpactNormal = BottomFacing; SecondHit.Normal = BottomFacing;
            Hologram->UpdateHologramPlacement(SecondHit); Finished = Hologram->DoMultiStepPlacement(false);
        }
        // The pipeline pole hologram builds 25 cm shorter than the height it is given (measured live, #308).
        else if (AFGPoleHologram* Pole = Cast<AFGPoleHologram>(Hologram)) { Pole->SetPoleHeight(IsPipeSupport ? HeightCm + PipeSupportHeightOffsetCm : HeightCm); Finished = Pole->DoMultiStepPlacement(false); }
        PC->SetControlRotation(Saved); Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));
        TArray<TSubclassOf<UFGConstructDisqualifier>> Ds; Hologram->GetConstructDisqualifiers(Ds);
        if (Finished && Ds.Num() == 1 && Ds[0] && Ds[0]->IsChildOf(UFGCDInitializing::StaticClass())) { Hologram->ResetConstructDisqualifiers(); Hologram->SetPlacementMaterialState(EHologramMaterialState::HMS_OK); Ds.Reset(); }
        if (!Finished || !Hologram->CanConstruct()) { Hologram->Destroy(); RefundRecipeCost(World, RecipeClass, 1); OnComplete(ErrorResponse(TEXT("hologram_placement_failed"), TEXT("Native conveyor support hologram rejected placement; cost refunded."), EHttpServerResponseCodes::Conflict)); return; }
        TArray<AActor*> Children; AFGBuildable* Built = Cast<AFGBuildable>(Hologram->Construct(Children, FNetConstructionID())); if (IsValid(Hologram)) Hologram->Destroy();
        if (!Built) { RefundRecipeCost(World, RecipeClass, 1); OnComplete(ErrorResponse(TEXT("hologram_construct_failed"), TEXT("Native hologram did not construct a buildable."), EHttpServerResponseCodes::ServerError)); return; }
        if (AFGBuildableConveyorBase* BuiltLift = IsLift ? Cast<AFGBuildableConveyorBase>(Built) : nullptr)
        {
            bool FacingVerified = false; double BuiltHeightCm = -1.0; FString BuiltSummary = TEXT("connections unavailable");
            // Read back the outward connector normals and the height the game actually built.
            UFGFactoryConnectionComponent* C0 = BuiltLift->GetConnection0(); UFGFactoryConnectionComponent* C1 = BuiltLift->GetConnection1();
            if (C0 && C1)
            {
                UFGFactoryConnectionComponent* BottomEnd = C0->GetConnectorLocation(false).Z <= C1->GetConnectorLocation(false).Z ? C0 : C1; UFGFactoryConnectionComponent* TopEnd = BottomEnd == C0 ? C1 : C0;
                const FVector BuiltBottom = BottomEnd->GetConnectorNormal().GetSafeNormal2D(), BuiltTop = TopEnd->GetConnectorNormal().GetSafeNormal2D();
                BuiltHeightCm = TopEnd->GetConnectorLocation(false).Z - BottomEnd->GetConnectorLocation(false).Z;
                Body->SetObjectField(TEXT("builtBottomFacing"), VectorJson(BuiltBottom)); Body->SetObjectField(TEXT("builtTopFacing"), VectorJson(BuiltTop)); Body->SetNumberField(TEXT("builtHeightCm"), BuiltHeightCm);
                FacingVerified = FVector::DotProduct(BuiltBottom, BottomFacing) > 0.996 && FVector::DotProduct(BuiltTop, TopFacing) > 0.996;
                BuiltSummary = FString::Printf(TEXT("built bottom facing (%.2f, %.2f), top facing (%.2f, %.2f), height %.0f cm"), BuiltBottom.X, BuiltBottom.Y, BuiltTop.X, BuiltTop.Y, BuiltHeightCm);
            }
            const bool HeightVerified = FMath::Abs(BuiltHeightCm - FMath::Abs(Top.Z - Base.Z)) <= 1.0;
            // The flow direction too (#302): a lift whose end snaps onto a nearby port takes that port's direction, so a
            // lift asked to carry items up was built carrying them down into a splitter's input and reported as built.
            const AFGBuildableConveyorLift* BuiltLiftActor = Cast<AFGBuildableConveyorLift>(BuiltLift);
            const bool WantsUpwards = BottomRole.Equals(TEXT("input"), ESearchCase::IgnoreCase);
            const bool FlowVerified = BuiltLiftActor && BuiltLiftActor->IsFlowUpwards() == WantsUpwards;
            if (FacingVerified && HeightVerified && !FlowVerified)
            {
                TArray<TSharedPtr<FJsonValue>> Deltas; Deltas.Add(ReceiptDelta(TEXT("flowUpwards"), WantsUpwards ? 1.0 : 0.0, WantsUpwards ? 0.0 : 1.0, 0.0));
                const TSharedRef<FJsonObject> Rejected = BuildReceiptJson(Built, Deltas);
                for (AActor* Child : Children) if (IsValid(Child)) Child->Destroy();
                Built->Destroy(); RefundRecipeCost(World, RecipeClass, 1);
                const TSharedRef<FJsonObject> Error = ErrorBody(TEXT("built_differs_from_request"), FString::Printf(TEXT("The conveyor lift was built carrying items %s, not %s as requested (an end snapped onto a nearby port); it was rolled back and the cost refunded."), WantsUpwards ? TEXT("down") : TEXT("up"), WantsUpwards ? TEXT("up") : TEXT("down")), false);
                Error->SetObjectField(TEXT("receipt"), Rejected); Error->SetBoolField(TEXT("rolledBack"), true);
                OnComplete(JsonResponse(Error, EHttpServerResponseCodes::Conflict));
                return;
            }
            if (!FacingVerified || !HeightVerified)
            {
                for (AActor* Child : Children) if (IsValid(Child)) Child->Destroy();
                Built->Destroy(); RefundRecipeCost(World, RecipeClass, 1);
                OnComplete(ErrorResponse(FacingVerified ? TEXT("lift_height_mismatch") : TEXT("lift_facing_mismatch"), FString::Printf(TEXT("The constructed lift differs from the request (%s; requested height %.0f cm); it was rolled back and the cost refunded."), *BuiltSummary, FMath::Abs(Top.Z - Base.Z)), EHttpServerResponseCodes::Conflict));
                return;
            }
            Body->SetBoolField(TEXT("facingVerified"), true); Body->SetBoolField(TEXT("heightVerified"), true); Body->SetBoolField(TEXT("flowVerified"), true);
        }
        else if (const AFGBuildablePole* BuiltPole = Cast<AFGBuildablePole>(Built))
        {
            // The pole hologram picks one of its mesh variations, which can be shorter than asked for (#302: a 4 m request
            // built at 3 m). Read the built height back and roll back a pole that is not the requested height.
            if (FMath::Abs(BuiltPole->mHeight - HeightCm) > 1.0)
            {
                const double BuiltHeightCm = BuiltPole->mHeight;
                TArray<TSharedPtr<FJsonValue>> Deltas; Deltas.Add(ReceiptDelta(TEXT("heightCm"), HeightCm, BuiltHeightCm, 1.0));
                const TSharedRef<FJsonObject> Rejected = BuildReceiptJson(Built, Deltas);
                for (AActor* Child : Children) if (IsValid(Child)) Child->Destroy();
                Built->Destroy(); RefundRecipeCost(World, RecipeClass, 1);
                const TSharedRef<FJsonObject> Error = ErrorBody(TEXT("built_differs_from_request"), FString::Printf(TEXT("The %s was built %.0f cm tall, not the requested %.0f cm; it was rolled back and the cost refunded."), IsPipeSupport ? TEXT("pipe support") : TEXT("conveyor pole"), BuiltHeightCm, HeightCm), false);
                Error->SetNumberField(TEXT("requestedHeightCm"), HeightCm); Error->SetNumberField(TEXT("builtHeightCm"), BuiltHeightCm); Error->SetObjectField(TEXT("receipt"), Rejected); Error->SetBoolField(TEXT("rolledBack"), true);
                OnComplete(JsonResponse(Error, EHttpServerResponseCodes::Conflict));
                return;
            }
        }
        Body->SetObjectField(TEXT("receipt"), BuildReceiptJson(Built, {}));
        PlacementReceipts.Add(RequestId, Built); Body->SetBoolField(TEXT("placed"), true); Body->SetBoolField(TEXT("hologramValidated"), true); Body->SetStringField(TEXT("buildableId"), Built->GetName()); Body->SetStringField(TEXT("requestId"), RequestId); OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Created));
    }, 0.05f, false);
    return true;
}

bool FMKIBridgeModule::HandlePlaceFactoryBuilding(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString T; if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR C((const ANSICHAR*)Request.Body.GetData(), Request.Body.Num()); T=FString(C.Length(),C.Get()); }
    TSharedPtr<FJsonObject> J; auto R=TJsonReaderFactory<>::Create(T); if(!FJsonSerializer::Deserialize(R,J)||!J){OnComplete(ErrorResponse(TEXT("invalid_json"),TEXT("Request body must be JSON."),EHttpServerResponseCodes::BadRequest));return true;}
    FString Type,RequestId,AnchorId,AnchorPort,AnchorFacing,RequestedClassPath,RequestedRecipePath; double F=0,Q=0,U=0,YawOffset=0,AbsoluteX=0,AbsoluteY=0,AbsoluteZ=0,AbsoluteYaw=0,RequestedSupportOffset=0; bool Confirm=false,SnapToWorldGrid=true; J->TryGetStringField(TEXT("type"),Type);J->TryGetStringField(TEXT("requestId"),RequestId);J->TryGetStringField(TEXT("anchorId"),AnchorId);J->TryGetStringField(TEXT("anchorPort"),AnchorPort);J->TryGetStringField(TEXT("anchorFacing"),AnchorFacing);J->TryGetStringField(TEXT("classPath"),RequestedClassPath);J->TryGetStringField(TEXT("recipePath"),RequestedRecipePath);J->TryGetNumberField(TEXT("forwardCm"),F);J->TryGetNumberField(TEXT("rightCm"),Q);J->TryGetNumberField(TEXT("upCm"),U);J->TryGetNumberField(TEXT("yawOffsetDegrees"),YawOffset);const bool HasRequestedSupportOffset=J->TryGetNumberField(TEXT("supportOffsetCm"),RequestedSupportOffset);J->TryGetBoolField(TEXT("confirm"),Confirm);J->TryGetBoolField(TEXT("snapToWorldGrid"),SnapToWorldGrid);
    // validateHologram: a dry run that drives the native hologram through placement and reports its construct
    // disqualifiers (batch preview, #59) without charging, building, or requiring host authority.
    bool ValidateHologram=false;J->TryGetBoolField(TEXT("validateHologram"),ValidateHologram);
    bool NativeSnap=true;J->TryGetBoolField(TEXT("nativeSnap"),NativeSnap);const bool HasAbsoluteX=J->TryGetNumberField(TEXT("x"),AbsoluteX);const bool HasAbsoluteY=J->TryGetNumberField(TEXT("y"),AbsoluteY);const bool HasAbsoluteZ=J->TryGetNumberField(TEXT("z"),AbsoluteZ);const bool HasAbsoluteYaw=J->TryGetNumberField(TEXT("yaw"),AbsoluteYaw);const bool HasAbsolute=HasAbsoluteX&&HasAbsoluteY;
    FVector RequestedSurfaceNormal=FVector::ZeroVector;const TSharedPtr<FJsonObject>*SurfaceNormalObject=nullptr;double SurfaceNormalX=0,SurfaceNormalY=0,SurfaceNormalZ=0;const bool HasSurfaceNormal=J->TryGetObjectField(TEXT("surfaceNormal"),SurfaceNormalObject)&&SurfaceNormalObject&&(*SurfaceNormalObject)->TryGetNumberField(TEXT("x"),SurfaceNormalX)&&(*SurfaceNormalObject)->TryGetNumberField(TEXT("y"),SurfaceNormalY)&&(*SurfaceNormalObject)->TryGetNumberField(TEXT("z"),SurfaceNormalZ);if(HasSurfaceNormal)RequestedSurfaceNormal=FVector(SurfaceNormalX,SurfaceNormalY,SurfaceNormalZ).GetSafeNormal();
    if((HasAbsoluteX!=HasAbsoluteY)||(HasAbsoluteZ&&!HasAbsolute)){OnComplete(ErrorResponse(TEXT("invalid_absolute_position"),TEXT("Absolute placement requires both x and y; z is optional."),EHttpServerResponseCodes::BadRequest));return true;}
    if(HasSurfaceNormal&&(!HasAbsolute||!HasAbsoluteZ||RequestedSurfaceNormal.IsNearlyZero())){OnComplete(ErrorResponse(TEXT("invalid_surface_normal"),TEXT("surfaceNormal requires absolute x, y, z and a non-zero finite vector."),EHttpServerResponseCodes::BadRequest));return true;}
    FString ClassPath,RecipePath;
    if(Type.Equals(TEXT("generic"),ESearchCase::IgnoreCase)){
        RequestedClassPath.TrimStartAndEndInline();RequestedRecipePath.TrimStartAndEndInline();
        FString PathCode,PathMessage;
        if(!IsPlaceableAssetPath(RequestedClassPath,PathCode,PathMessage)||!IsPlaceableAssetPath(RequestedRecipePath,PathCode,PathMessage)){OnComplete(ErrorResponse(PathCode,PathMessage,PathCode==TEXT("companion_unavailable")?EHttpServerResponseCodes::Conflict:EHttpServerResponseCodes::BadRequest));return true;}
        ClassPath=RequestedClassPath;RecipePath=RequestedRecipePath;
    }
    else if(Type.Equals(TEXT("smelter"),ESearchCase::IgnoreCase)){ClassPath=TEXT("/Game/FactoryGame/Buildable/Factory/SmelterMk1/Build_SmelterMk1.Build_SmelterMk1_C");RecipePath=TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_SmelterBasicMk1.Recipe_SmelterBasicMk1_C");} // Recipe_SmelterMk1 is the Foundry's build recipe (#262)
    else if(Type.Equals(TEXT("constructor"),ESearchCase::IgnoreCase)){ClassPath=ConstructorClassPath;RecipePath=ConstructorRecipePath;}
    else if(Type.Equals(TEXT("storage"),ESearchCase::IgnoreCase)){ClassPath=StorageContainerMk1ClassPath;RecipePath=StorageContainerMk1RecipePath;}
    else if(Type.Equals(TEXT("power_pole"),ESearchCase::IgnoreCase)){ClassPath=TEXT("/Game/FactoryGame/Buildable/Factory/PowerPoleMk1/Build_PowerPoleMk1.Build_PowerPoleMk1_C");RecipePath=TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_PowerPoleMk1.Recipe_PowerPoleMk1_C");}
    else if(Type.Equals(TEXT("biomass_generator"),ESearchCase::IgnoreCase)){ClassPath=BiomassGeneratorClassPath;RecipePath=BiomassGeneratorRecipePath;}
    else if(Type.Equals(TEXT("splitter"),ESearchCase::IgnoreCase)){ClassPath=TEXT("/Game/FactoryGame/Buildable/Factory/CA_Splitter/Build_ConveyorAttachmentSplitter.Build_ConveyorAttachmentSplitter_C");RecipePath=TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_ConveyorAttachmentSplitter.Recipe_ConveyorAttachmentSplitter_C");}
    else if(Type.Equals(TEXT("merger"),ESearchCase::IgnoreCase)){ClassPath=TEXT("/Game/FactoryGame/Buildable/Factory/CA_Merger/Build_ConveyorAttachmentMerger.Build_ConveyorAttachmentMerger_C");RecipePath=TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_ConveyorAttachmentMerger.Recipe_ConveyorAttachmentMerger_C");}
    else {OnComplete(ErrorResponse(TEXT("unsupported_type"),TEXT("type must be generic or a supported convenience alias."),EHttpServerResponseCodes::BadRequest));return true;}
    if(!FMath::IsFinite(F)||!FMath::IsFinite(Q)||!FMath::IsFinite(U)||!FMath::IsFinite(YawOffset)||(HasAbsolute&&(!FMath::IsFinite(AbsoluteX)||!FMath::IsFinite(AbsoluteY)))||(HasAbsoluteZ&&!FMath::IsFinite(AbsoluteZ))||(HasAbsoluteYaw&&!FMath::IsFinite(AbsoluteYaw))||(HasSurfaceNormal&&(!FMath::IsFinite(SurfaceNormalX)||!FMath::IsFinite(SurfaceNormalY)||!FMath::IsFinite(SurfaceNormalZ)))||(HasRequestedSupportOffset&&(!FMath::IsFinite(RequestedSupportOffset)||RequestedSupportOffset < -1000||RequestedSupportOffset > 5000))){OnComplete(ErrorResponse(TEXT("invalid_coordinates"),TEXT("Placement coordinates, yaw, surface normal, and support offset must be finite and within guarded bounds."),EHttpServerResponseCodes::BadRequest));return true;} if(Confirm&&RequestId.IsEmpty()){OnComplete(ErrorResponse(TEXT("request_id_required"),TEXT("Confirmed placement requires requestId."),EHttpServerResponseCodes::BadRequest));return true;}
    UWorld* W=GEngine?GEngine->GetCurrentPlayWorld():nullptr; APawn* P=W?UGameplayStatics::GetPlayerPawn(W,0):nullptr; APlayerController* PC=W?UGameplayStatics::GetPlayerController(W,0):nullptr; AFGBuildableSubsystem* S=W?AFGBuildableSubsystem::Get(W):nullptr; if(!W||!P||!PC||!S){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World is not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}
    AFGBuildable* Anchor=nullptr;
    USceneComponent* AnchorComponent=nullptr;
    const TSharedRef<FLightweightConverterLease> LightweightConverter=MakeShared<FLightweightConverterLease>();
    LightweightConverter->World=W;
    if(!AnchorId.IsEmpty()){
        Anchor=FindBuildableById(W,AnchorId);
        if(!Anchor&&AnchorId.StartsWith(TEXT("lw|"),ESearchCase::CaseSensitive))Anchor=ResolveLightweightAnchor(W,AnchorId,LightweightConverter->TemporaryAnchor);
        if(!Anchor){OnComplete(ErrorResponse(TEXT("anchor_not_found"),TEXT("The exact anchor buildable was not found."),EHttpServerResponseCodes::NotFound));return true;}
        if(!AnchorPort.IsEmpty()){
            TInlineComponentArray<USceneComponent*> Components(Anchor);
            for(USceneComponent* Component:Components)if(Component&&Component->GetName().Equals(AnchorPort,ESearchCase::IgnoreCase)){AnchorComponent=Component;break;}
            if(!AnchorComponent){OnComplete(ErrorResponse(TEXT("anchor_port_not_found"),TEXT("The exact anchor component was not found."),EHttpServerResponseCodes::NotFound));return true;}
        }
    }
    if(!AnchorPort.IsEmpty()&&!Anchor){OnComplete(ErrorResponse(TEXT("invalid_anchor"),TEXT("anchorPort requires anchorId."),EHttpServerResponseCodes::BadRequest));return true;}
    if(Type.Equals(TEXT("generic"),ESearchCase::IgnoreCase)&&RequestedClassPath.Contains(TEXT("Build_ConveyorMonitor"),ESearchCase::IgnoreCase)&&!Anchor){OnComplete(ErrorResponse(TEXT("anchor_required"),TEXT("This spline attachment requires an exact anchorId for the conveyor segment it will monitor."),EHttpServerResponseCodes::Conflict));return true;}
    FVector BaseLocation=AnchorComponent?AnchorComponent->GetComponentLocation():(Anchor?Anchor->GetActorLocation():P->GetActorLocation());
    FRotator Rot(0,AnchorComponent?AnchorComponent->GetComponentRotation().Yaw:(Anchor?Anchor->GetActorRotation().Yaw:PC->GetControlRotation().Yaw),0);
    if(Anchor&&(AnchorFacing.Equals(TEXT("input"),ESearchCase::IgnoreCase)||AnchorFacing.Equals(TEXT("output"),ESearchCase::IgnoreCase))){
        const EFactoryConnectionDirection RequestedDirection=AnchorFacing.Equals(TEXT("input"),ESearchCase::IgnoreCase)
            ? EFactoryConnectionDirection::FCD_INPUT : EFactoryConnectionDirection::FCD_OUTPUT;
        UFGFactoryConnectionComponent* ReferenceConnection=nullptr;
        TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Anchor);
        for(UFGFactoryConnectionComponent* Connection:Connections)if(Connection&&Connection->GetDirection()==RequestedDirection){ReferenceConnection=Connection;break;}
        if(!ReferenceConnection){OnComplete(ErrorResponse(TEXT("anchor_connection_not_found"),FString::Printf(TEXT("The anchor has no factory %s connection."),*AnchorFacing),EHttpServerResponseCodes::Conflict));return true;}
        const FVector ReferenceNormal=ReferenceConnection->GetConnectorNormal().GetSafeNormal2D();
        BaseLocation=ReferenceConnection->GetConnectorLocation(false);
        Rot=ReferenceNormal.Rotation();
    }
    FVector Pos=HasAbsolute?FVector(AbsoluteX,AbsoluteY,HasAbsoluteZ?AbsoluteZ:BaseLocation.Z):BaseLocation+Rot.Vector()*F+FRotationMatrix(Rot).GetUnitAxis(EAxis::Y)*Q;
    if(HasAbsolute){Rot=FRotator(0,HasAbsoluteYaw?AbsoluteYaw:0,0);U=HasAbsoluteZ?AbsoluteZ:U;}
    FHitResult SurfaceHit;
    FCollisionQueryParams SurfaceQuery;
    SurfaceQuery.AddIgnoredActor(P);
    const double TraceReferenceZ=HasAbsoluteZ?AbsoluteZ:Pos.Z;
    const FVector TraceStart=HasSurfaceNormal?Pos+RequestedSurfaceNormal*1000.0:FVector(Pos.X,Pos.Y,FMath::Max(P->GetActorLocation().Z,TraceReferenceZ)+5000.0);
    const FVector TraceEnd=HasSurfaceNormal?Pos-RequestedSurfaceNormal*1000.0:FVector(Pos.X,Pos.Y,FMath::Min(P->GetActorLocation().Z,TraceReferenceZ)-10000.0);
    bool FoundSurface=false;
    if(Anchor&&HasSurfaceNormal){
        UPrimitiveComponent* ClosestPrimitive=nullptr;double ClosestDistance=TNumericLimits<double>::Max();TInlineComponentArray<UPrimitiveComponent*> Primitives(Anchor);
        for(UPrimitiveComponent* Primitive:Primitives)if(Primitive){const double Distance=FVector::DistSquared(Primitive->GetComponentLocation(),Pos);if(Distance<ClosestDistance){ClosestDistance=Distance;ClosestPrimitive=Primitive;}}
        SurfaceHit=FHitResult(Anchor,ClosestPrimitive,Pos,RequestedSurfaceNormal);SurfaceHit.bBlockingHit=true;SurfaceHit.TraceStart=TraceStart;SurfaceHit.TraceEnd=TraceEnd;SurfaceHit.Location=Pos;SurfaceHit.ImpactPoint=Pos;FoundSurface=true;
    }
    if(!FoundSurface)FoundSurface=W->LineTraceSingleByChannel(SurfaceHit,TraceStart,TraceEnd,ECC_WorldStatic,SurfaceQuery);
    // Wall and ceiling buildables commonly expose their hologram snap surface
    // on the visibility channel without blocking WorldStatic. Preserve the
    // terrain/foundation trace as the primary path, then use the engine's
    // visibility surface for explicit directional attachment traces.
    if(!FoundSurface&&HasSurfaceNormal){FoundSurface=W->LineTraceSingleByChannel(SurfaceHit,TraceStart,TraceEnd,ECC_Visibility,SurfaceQuery);}
    // Some snap-only architectural actors do not block either trace channel,
    // but still have queryable collision geometry. Object tracing is limited
    // to explicit surface-normal requests so ordinary terrain placement keeps
    // its original, conservative semantics.
    if(!FoundSurface&&HasSurfaceNormal){FoundSurface=W->LineTraceSingleByObjectType(SurfaceHit,TraceStart,TraceEnd,FCollisionObjectQueryParams::AllObjects,SurfaceQuery);}
    // A vertical ray that grazed a seam between adjacent structures is retried
    // just inside each neighbour (see SupportProbeNudgeCm).
    FVector SupportProbeOffset=FVector::ZeroVector;
    if(!FoundSurface&&!HasSurfaceNormal){
        for(const FVector& Nudge:{FVector(SupportProbeNudgeCm,0,0),FVector(-SupportProbeNudgeCm,0,0),FVector(0,SupportProbeNudgeCm,0),FVector(0,-SupportProbeNudgeCm,0)}){
            if(W->LineTraceSingleByChannel(SurfaceHit,TraceStart+Nudge,TraceEnd+Nudge,ECC_WorldStatic,SurfaceQuery)){FoundSurface=true;SupportProbeOffset=Nudge;break;}
        }
    }
    if(!FoundSurface){
        OnComplete(ErrorResponse(TEXT("surface_not_found"),TEXT("No solid terrain surface was found below the requested position."),EHttpServerResponseCodes::Conflict));return true;
    }
    bool SnappedToFoundationGrid=false;
    if(SnapToWorldGrid&&Cast<AFGBuildableFoundation>(SurfaceHit.GetActor())){
        const FVector SnappedXY(SnapToIncrement(Pos.X,BuildableFoundationGridSizeCm),SnapToIncrement(Pos.Y,BuildableFoundationGridSizeCm),Pos.Z);
        FHitResult SnappedSurfaceHit;
        const FVector SnappedTraceStart(SnappedXY.X,SnappedXY.Y,FMath::Max(P->GetActorLocation().Z,TraceReferenceZ)+5000.0);
        const FVector SnappedTraceEnd(SnappedXY.X,SnappedXY.Y,FMath::Min(P->GetActorLocation().Z,TraceReferenceZ)-10000.0);
        if(W->LineTraceSingleByChannel(SnappedSurfaceHit,SnappedTraceStart,SnappedTraceEnd,ECC_WorldStatic,SurfaceQuery)&&Cast<AFGBuildableFoundation>(SnappedSurfaceHit.GetActor())){
            Pos.X=SnappedXY.X;Pos.Y=SnappedXY.Y;SurfaceHit=SnappedSurfaceHit;
            Rot.Yaw=SnapToIncrement(Rot.Yaw+YawOffset,WorldGridYawStepDegrees)-YawOffset;
            SnappedToFoundationGrid=true;
        }
    }
    const double SupportOffsetCm=HasRequestedSupportOffset?RequestedSupportOffset:GetSupportSurfaceToActorOriginOffsetCm(Type);
    const double SupportSurfaceZ=HasAbsoluteZ?AbsoluteZ:SurfaceHit.ImpactPoint.Z+U;
    Pos.Z=SupportSurfaceZ+SupportOffsetCm;
    if(FVector::Distance(Pos,P->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("Placement exceeds the configured safety radius."),EHttpServerResponseCodes::Forbidden));return true;}
    auto B=MakeShared<FJsonObject>();B->SetBoolField(TEXT("ok"),true);B->SetBoolField(TEXT("dryRun"),!Confirm);B->SetStringField(TEXT("type"),Type);B->SetBoolField(TEXT("snappedToFoundationGrid"),SnappedToFoundationGrid);B->SetBoolField(TEXT("nativeSnap"),NativeSnap);B->SetNumberField(TEXT("supportSurfaceZ"),SupportSurfaceZ);B->SetNumberField(TEXT("supportSurfaceToActorOriginOffsetCm"),SupportOffsetCm);auto Resolved=MakeShared<FJsonObject>();Resolved->SetNumberField(TEXT("x"),Pos.X);Resolved->SetNumberField(TEXT("y"),Pos.Y);Resolved->SetNumberField(TEXT("z"),Pos.Z);B->SetObjectField(TEXT("resolvedPosition"),Resolved);B->SetNumberField(TEXT("resolvedYaw"),Rot.Yaw+YawOffset);
    {
        const TSharedRef<FJsonObject> Probe=MakeShared<FJsonObject>();
        Probe->SetStringField(TEXT("method"),SupportProbeOffset.IsZero()?TEXT("center"):TEXT("seam_nudge"));
        const TSharedRef<FJsonObject> Offset=MakeShared<FJsonObject>();Offset->SetNumberField(TEXT("x"),SupportProbeOffset.X);Offset->SetNumberField(TEXT("y"),SupportProbeOffset.Y);Probe->SetObjectField(TEXT("offsetCm"),Offset);
        Probe->SetNumberField(TEXT("hitZ"),SurfaceHit.ImpactPoint.Z);
        Probe->SetObjectField(TEXT("support"),DescribeSupportHit(W,SurfaceHit));
        B->SetObjectField(TEXT("supportProbe"),Probe);
    }
    if(!Confirm&&!ValidateHologram){OnComplete(JsonResponse(B));return true;} if(Confirm&&W->GetNetMode()==NM_Client){OnComplete(ErrorResponse(TEXT("server_authority_required"),TEXT("Construction is only available to the host or in single-player."),EHttpServerResponseCodes::Forbidden));return true;}
    const bool Charged=Confirm;
    UClass* BC=StaticLoadClass(AFGBuildable::StaticClass(),nullptr,*ClassPath);UClass* RC=StaticLoadClass(UFGRecipe::StaticClass(),nullptr,*RecipePath);if(!BC||!RC){OnComplete(ErrorResponse(TEXT("asset_unavailable"),TEXT("Buildable class or recipe asset unavailable."),EHttpServerResponseCodes::ServerError));return true;}FString Missing;if(Charged&&!ConsumeRecipeCost(W,RC,1,Missing)){OnComplete(ErrorResponse(TEXT("insufficient_build_materials"),Missing,EHttpServerResponseCodes::Conflict));return true;}
    FRotator ObjectRot=Rot;ObjectRot.Yaw+=YawOffset;
    const bool IsFoundationPassthrough=ClassPath.Contains(TEXT("/FoundationPassthrough/"),ESearchCase::IgnoreCase);
    AFGCharacterPlayer* Player=Cast<AFGCharacterPlayer>(P);
    AFGBuildableHologram* Hologram=Cast<AFGBuildableHologram>(AFGHologram::SpawnHologramFromRecipe(RC,PC,SurfaceHit.ImpactPoint,Player));
    if(!Hologram){LightweightConverter->Release();if(Charged)RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"),TEXT("The native buildable hologram could not be spawned; build cost was refunded."),EHttpServerResponseCodes::ServerError));return true;}
    Hologram->WaitForGamestateApplyCustomizationData();
    FTimerHandle InitializationTimer;
    W->GetTimerManager().SetTimer(InitializationTimer,[this,WeakW=TWeakObjectPtr<UWorld>(W),WeakPC=TWeakObjectPtr<APlayerController>(PC),WeakS=TWeakObjectPtr<AFGBuildableSubsystem>(S),WeakBC=TWeakObjectPtr<UClass>(BC),WeakRC=TWeakObjectPtr<UClass>(RC),WeakHologram=TWeakObjectPtr<AFGBuildableHologram>(Hologram),WeakAnchor=TWeakObjectPtr<AFGBuildable>(Anchor),HadAnchor=Anchor!=nullptr,Ticket=ActiveTicket,Charged,SurfaceHit,Pos,SupportSurfaceZ,ObjectRot,HasSurfaceNormal,IsFoundationPassthrough,NativeSnap,ClassPath,RequestId,B,OnComplete,LightweightConverter]() mutable
    {
        auto ReleaseLightweightConverter=[&LightweightConverter](){LightweightConverter->Release();};
        // Every engine object is re-resolved: the world may have been torn down or the anchor dismantled since the request.
        UWorld* W=WeakW.Get();APlayerController* PC=WeakPC.Get();AFGBuildableSubsystem* S=WeakS.Get();UClass* BC=WeakBC.Get();UClass* RC=WeakRC.Get();AFGBuildableHologram* Hologram=WeakHologram.Get();AFGBuildable* Anchor=WeakAnchor.Get();
        if(!W||!PC||!S||!BC||!RC||!Hologram||(HadAnchor&&!Anchor)||(Ticket&&Ticket->bAbandoned)){if(Hologram)Hologram->Destroy();ReleaseLightweightConverter();if(Charged&&W&&RC)RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"),TEXT("The world, player, anchor, or hologram disappeared before construction; nothing was built and build cost was refunded when possible."),EHttpServerResponseCodes::Conflict));return;}
        const FRotator SavedControlRotation=PC->GetControlRotation();
        // Preserve the supporting actor/component from the trace, but present the
        // fully resolved grid/offset position to the native hologram. Passing the
        // raw terrain impact point here silently discarded explicit foundation Z.
        FHitResult PlacementHit=SurfaceHit;
        const FVector SupportPoint(Pos.X,Pos.Y,SupportSurfaceZ);
        PlacementHit.Location=SupportPoint;
        PlacementHit.ImpactPoint=SupportPoint;
        // Aim-sensitive holograms (standalone signs and elevators) validate the
        // hit against the live build-gun view. Aim at the resolved support point
        // while updating/validating, then impose the requested object yaw on the
        // hologram transform so view direction and design orientation stay
        // independent.
        const APawn* ViewPawn=PC->GetPawn();
        const FVector ViewOrigin=ViewPawn?ViewPawn->GetPawnViewLocation():PC->GetFocalLocation();
        PC->SetControlRotation((SupportPoint-ViewOrigin).Rotation());
        // Explicit anchors represent an intentional native snap target. The
        // headless update path does not always perform the build-gun hover
        // transition that initializes spline attachments (pipe pumps/valves,
        // railroad signals, and monitors), so ask the hologram to consume the
        // exact actor/component hit before its normal placement update.
        // nativeSnap=false makes the caller's explicit transform authoritative. The
        // native update still runs, but without a hit actor it cannot attachment-,
        // floor-, or grid-snap the hologram, and the resolved transform is imposed
        // before native validation, so clearance and placement checks judge exactly
        // the requested transform.
        if(!NativeSnap){PlacementHit.HitObjectHandle=FActorInstanceHandle();PlacementHit.Component=nullptr;}
        auto ImposeExplicitTransform=[&](){if(!NativeSnap)Hologram->SetActorLocationAndRotation(Pos,ObjectRot);};
        if(NativeSnap&&PlacementHit.GetActor())Hologram->TrySnapToActor(PlacementHit);
        Hologram->UpdateHologramPlacement(PlacementHit);
        if(!HasSurfaceNormal)Hologram->SetActorRotation(ObjectRot);
        ImposeExplicitTransform();
        if(AFGBuildable* NativeSnapTarget=Hologram->GetSnappedBuilding())B->SetStringField(TEXT("nativeSnappedToId"),NativeSnapTarget->GetName());
        Hologram->ValidatePlacementAndCost(GetPlayerInventory(W));
        bool Finished=Hologram->DoMultiStepPlacement(false);
        // Several otherwise ordinary buildables use the native multi-step
        // interaction solely to accept a default height/angle (poles, supports,
        // sign poles, adjustable jump pads, and standalone signs). Advance
        // those steps with the same resolved hit. Family-specific callers can
        // later provide explicit step hits, but generic placement must still be
        // capable of confirming the native defaults.
        for(int32 Step=1;!Finished&&Step<8;++Step){
            FHitResult StepHit=PlacementHit;
            StepHit.Location=SupportPoint+FVector::UpVector*(200.0*Step);
            StepHit.ImpactPoint=StepHit.Location;
            PC->SetControlRotation((StepHit.Location-ViewOrigin).Rotation());
            Hologram->UpdateHologramPlacement(StepHit);
            if(!HasSurfaceNormal)Hologram->SetActorRotation(ObjectRot);
            ImposeExplicitTransform();
            Hologram->ValidatePlacementAndCost(GetPlayerInventory(W));
            Finished=Hologram->DoMultiStepPlacement(false);
            if(!Finished)Finished=Hologram->DoMultiStepPlacement(true);
        }
        PC->SetControlRotation(SavedControlRotation);
        Hologram->ValidatePlacementAndCost(GetPlayerInventory(W));
        TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
        Hologram->GetConstructDisqualifiers(Disqualifiers);
        bool OnlyNonGeometricInteractionDisqualifiers=Disqualifiers.Num()>0;
        for(TSubclassOf<UFGConstructDisqualifier>D:Disqualifiers){
            if(!D||(!D->IsChildOf(UFGCDInitializing::StaticClass())&&!D->IsChildOf(UFGCDInvalidAimLocation::StaticClass()))){OnlyNonGeometricInteractionDisqualifiers=false;break;}
        }
        // Explicit-coordinate callers have no physical mouse cursor. Once the
        // native multi-step hologram has finished, initialization and cursor-aim
        // state are not geometry failures; collision, support, reach, and snap
        // disqualifiers remain authoritative.
        if(Finished&&OnlyNonGeometricInteractionDisqualifiers){Hologram->ResetConstructDisqualifiers();Hologram->SetPlacementMaterialState(EHologramMaterialState::HMS_OK);Disqualifiers.Reset();}
        if(!Charged){
            // Hologram preview: report the native verdict and disqualifiers, then discard the hologram. Nothing was
            // charged and nothing is built.
            const bool HologramValid=Finished&&Hologram->CanConstruct();
            TArray<TSharedPtr<FJsonValue>> DisqualifierNames;for(TSubclassOf<UFGConstructDisqualifier>D:Disqualifiers)DisqualifierNames.Add(MakeShared<FJsonValueString>(D?D->GetName():TEXT("null")));
            B->SetBoolField(TEXT("hologramValidated"),true);B->SetBoolField(TEXT("hologramValid"),HologramValid);B->SetBoolField(TEXT("multiStepFinished"),Finished);B->SetArrayField(TEXT("disqualifiers"),DisqualifierNames);
            Hologram->Destroy();ReleaseLightweightConverter();OnComplete(JsonResponse(B));return;
        }
        if(!Finished||!Hologram->CanConstruct()){
            FString Names;for(TSubclassOf<UFGConstructDisqualifier>D:Disqualifiers){if(!Names.IsEmpty())Names+=TEXT(",");Names+=D?D->GetName():TEXT("null");}
            Hologram->Destroy();ReleaseLightweightConverter();RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("hologram_placement_failed"),FString::Printf(TEXT("The native buildable hologram rejected placement (finished=%s, disqualifiers=%d [%s]); build cost was refunded."),Finished?TEXT("true"):TEXT("false"),Disqualifiers.Num(),*Names),EHttpServerResponseCodes::Conflict));return;
        }
        TArray<AActor*> Children;AFGBuildable* A=nullptr;FTransform VerifiedFinalTransform;bool FinalizedLightweightAnchoredFoundation=false;
        // The game may convert the new actor to a lightweight instance during construction (#113, verified live), so
        // take the class's instances first: only those count as pre-existing when the placement's origin is recorded.
        const TSet<int32> LightweightBefore=ValidLightweightIndices(W,BC);
        // Construct() cannot finalize a foundation whose snapped parent is a
        // temporary lightweight actor. Native snapping and validation have
        // already resolved the exact transform, so finalize that transform
        // through the authoritative subsystem. Release the converter first: it
        // was needed only to materialize the snap target, and leaving it active
        // would immediately convert the newly spawned road at the same location
        // and return the response actor to the pool sentinel.
        const bool IsLightweightAnchoredFoundation=!IsFoundationPassthrough&&LightweightConverter->TemporaryAnchor&&BC->IsChildOf(AFGBuildableFoundation::StaticClass());
        if(IsLightweightAnchoredFoundation){
            // The native hologram has validated the exact lightweight parent,
            // but its transform is poisoned with PoolTransform because that
            // parent is temporary. Finalize the explicit transform that was
            // validated against the parent instead of copying the sentinel.
            VerifiedFinalTransform=FTransform(ObjectRot,Pos);
            ReleaseLightweightConverter();
            A=S->BeginSpawnBuildable(BC,VerifiedFinalTransform);
            if(A){A->SetBuiltWithRecipe(RC);UGameplayStatics::FinishSpawningActor(A,VerifiedFinalTransform);FinalizedLightweightAnchoredFoundation=true;B->SetBoolField(TEXT("lightweightAnchorFinalized"),true);}
        }else if(IsFoundationPassthrough){
            // Lightweight foundations are represented to the hologram by a
            // temporary conventional actor. Construct() can hand that actor to
            // the network construction path and produce the engine sentinel
            // transform. Preserve the native hologram's validation and exact
            // configuration, but finalize the already-configured buildable
            // through the authoritative subsystem before releasing the
            // temporary foundation converter.
            const AFGBuildableFoundation* FoundationAnchor=Cast<AFGBuildableFoundation>(Anchor);
            const FVector FinalLocation(Pos.X,Pos.Y,SupportSurfaceZ-(FoundationAnchor?FoundationAnchor->mHeight*0.5:0.0));
            const FTransform FinalTransform(ObjectRot,FinalLocation);
            A=S->BeginSpawnBuildable(BC,FinalTransform);
            if(A){
                A->SetBuiltWithRecipe(RC);
                if(AFGBuildablePassthroughBase* Passthrough=Cast<AFGBuildablePassthroughBase>(A))if(AFGBuildableFoundation* Foundation=Cast<AFGBuildableFoundation>(Anchor))Passthrough->SetSnappedBuildingThickness_BeforeBeginPlay(Foundation->mHeight);
                UGameplayStatics::FinishSpawningActor(A,FinalTransform);
            }
        }else{
            A=Cast<AFGBuildable>(Hologram->Construct(Children,FNetConstructionID()));
        }
        if(!A){if(IsValid(Hologram))Hologram->Destroy();ReleaseLightweightConverter();RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("hologram_construct_failed"),TEXT("The validated hologram did not construct a buildable; build cost was refunded."),EHttpServerResponseCodes::ServerError));return;}
        // Some parent-dependent holograms can return a transient placeholder at
        // the engine sentinel depth even though CanConstruct() was true. Never
        // expose that as success or leave inaccessible underground garbage.
        if(!FinalizedLightweightAnchoredFoundation&&(A->GetActorLocation().ContainsNaN()||A->GetActorLocation().Z < -50000.0)){A->Destroy();if(IsValid(Hologram))Hologram->Destroy();ReleaseLightweightConverter();RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("invalid_constructed_transform"),TEXT("The native hologram produced an invalid sentinel transform; construction was rolled back."),EHttpServerResponseCodes::Conflict));return;}
        if(IsValid(Hologram))Hologram->Destroy();
        ReleaseLightweightConverter();
        NotePlacementOrigin(RequestId,A,&LightweightBefore);PlacementReceipts.Add(RequestId,A);B->SetBoolField(TEXT("placed"),true);B->SetBoolField(TEXT("costConsumed"),true);B->SetBoolField(TEXT("verified"),true);B->SetBoolField(TEXT("hologramValidated"),true);B->SetStringField(TEXT("buildableId"),A->GetName());B->SetStringField(TEXT("requestId"),RequestId);B->SetStringField(TEXT("className"),A->GetClass()->GetName());
        TArray<TSharedPtr<FJsonValue>> ChildBuildableIds;for(AActor* Child:Children)if(AFGBuildable* ChildBuildable=Cast<AFGBuildable>(Child))ChildBuildableIds.Add(MakeShared<FJsonValueString>(ChildBuildable->GetName()));B->SetArrayField(TEXT("childBuildableIds"),ChildBuildableIds);
        auto Actual=MakeShared<FJsonObject>();const FVector ActualLocation=FinalizedLightweightAnchoredFoundation?VerifiedFinalTransform.GetLocation():A->GetActorLocation();Actual->SetNumberField(TEXT("x"),ActualLocation.X);Actual->SetNumberField(TEXT("y"),ActualLocation.Y);Actual->SetNumberField(TEXT("z"),ActualLocation.Z);B->SetObjectField(TEXT("actualPosition"),Actual);B->SetNumberField(TEXT("actualYaw"),FinalizedLightweightAnchoredFoundation?VerifiedFinalTransform.Rotator().Yaw:A->GetActorRotation().Yaw);
        // What was built (#302). Snapping is the requested behaviour for generic placement, so how far it moved the
        // building is reported, not rolled back.
        if(!FinalizedLightweightAnchoredFoundation&&IsValid(A)){const TSharedRef<FJsonObject> Receipt=BuildReceiptJson(A,{});Receipt->SetNumberField(TEXT("snapMovedCm"),FVector::Dist(ActualLocation,Pos));B->SetObjectField(TEXT("receipt"),Receipt);}
        OnComplete(JsonResponse(B,EHttpServerResponseCodes::Created));
    },0.05f,false);
    return true;
}

bool FMKIBridgeModule::HandlePlaceMiner(const FHttpServerRequest& Request,const FHttpResultCallback& OnComplete)
{
    if(!IsAuthorized(Request)){OnComplete(ErrorResponse(TEXT("unauthorized"),TEXT("A valid bearer token is required."),EHttpServerResponseCodes::Denied));return true;} FString T;if(!Request.Body.IsEmpty()){FUTF8ToTCHAR C((const ANSICHAR*)Request.Body.GetData(),Request.Body.Num());T=FString(C.Length(),C.Get());}TSharedPtr<FJsonObject>J;auto R=TJsonReaderFactory<>::Create(T);if(!FJsonSerializer::Deserialize(R,J)||!J){OnComplete(ErrorResponse(TEXT("invalid_json"),TEXT("Request body must be JSON."),EHttpServerResponseCodes::BadRequest));return true;}FString NodeId,RequestId,RequestedClassPath,RequestedRecipePath;bool Confirm=false;J->TryGetStringField(TEXT("nodeId"),NodeId);J->TryGetStringField(TEXT("requestId"),RequestId);J->TryGetStringField(TEXT("classPath"),RequestedClassPath);J->TryGetStringField(TEXT("recipePath"),RequestedRecipePath);J->TryGetBoolField(TEXT("confirm"),Confirm);if(NodeId.IsEmpty()){OnComplete(ErrorResponse(TEXT("invalid_request"),TEXT("nodeId is required."),EHttpServerResponseCodes::BadRequest));return true;}if(Confirm&&RequestId.IsEmpty()){OnComplete(ErrorResponse(TEXT("request_id_required"),TEXT("Confirmed placement requires requestId."),EHttpServerResponseCodes::BadRequest));return true;}if((!RequestedClassPath.IsEmpty()&&!RequestedClassPath.StartsWith(TEXT("/Game/FactoryGame/")))||(!RequestedRecipePath.IsEmpty()&&!RequestedRecipePath.StartsWith(TEXT("/Game/FactoryGame/")))){OnComplete(ErrorResponse(TEXT("invalid_asset_path"),TEXT("Extractor assets must come from the observed FactoryGame catalog."),EHttpServerResponseCodes::BadRequest));return true;}
    UWorld*W=GEngine?GEngine->GetCurrentPlayWorld():nullptr;APawn*P=W?UGameplayStatics::GetPlayerPawn(W,0):nullptr;AFGBuildableSubsystem*S=W?AFGBuildableSubsystem::Get(W):nullptr;AFGResourceNode*Node=nullptr;if(!W||!P||!S){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World is not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}for(TActorIterator<AFGResourceNode>I(W);I;++I)if(I->GetName().Equals(NodeId,ESearchCase::CaseSensitive)){Node=*I;break;}if(!Node){OnComplete(ErrorResponse(TEXT("node_not_found"),TEXT("Exact resource node not found."),EHttpServerResponseCodes::NotFound));return true;}if(FVector::Distance(Node->GetActorLocation(),P->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("The resource node is outside the configured safety radius."),EHttpServerResponseCodes::Forbidden));return true;}if(!Node->CanPlaceResourceExtractor()){OnComplete(ErrorResponse(TEXT("node_unavailable"),TEXT("Node is occupied or invalid."),EHttpServerResponseCodes::Conflict));return true;}auto B=MakeShared<FJsonObject>();B->SetBoolField(TEXT("ok"),true);B->SetBoolField(TEXT("dryRun"),!Confirm);B->SetStringField(TEXT("nodeId"),NodeId);B->SetStringField(TEXT("resource"),Node->GetResourceClass()?Node->GetResourceClass()->GetName():TEXT(""));if(!Confirm){OnComplete(JsonResponse(B));return true;}
    const FString ExtractorClassPath=RequestedClassPath.IsEmpty()?TEXT("/Game/FactoryGame/Buildable/Factory/MinerMK1/Build_MinerMk1.Build_MinerMk1_C"):RequestedClassPath;const FString ExtractorRecipePath=RequestedRecipePath.IsEmpty()?TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_MinerMk1.Recipe_MinerMk1_C"):RequestedRecipePath;UClass*MC=StaticLoadClass(AFGBuildable::StaticClass(),nullptr,*ExtractorClassPath);UClass*RC=StaticLoadClass(UFGRecipe::StaticClass(),nullptr,*ExtractorRecipePath);if(!MC||!RC||!MC->IsChildOf(AFGBuildableResourceExtractorBase::StaticClass())){OnComplete(ErrorResponse(TEXT("asset_unavailable"),TEXT("Extractor asset or recipe unavailable or incompatible with resource-node placement."),EHttpServerResponseCodes::ServerError));return true;}FString Missing;if(!ConsumeRecipeCost(W,RC,1,Missing)){OnComplete(ErrorResponse(TEXT("insufficient_build_materials"),Missing,EHttpServerResponseCodes::Conflict));return true;}FTransform X(Node->GetActorRotation(),Node->GetActorLocation());AFGBuildable* Spawned=S->BeginSpawnBuildable(MC,X);if(!Spawned){RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("spawn_failed"),TEXT("Extractor spawn failed; build cost was refunded."),EHttpServerResponseCodes::ServerError));return true;}Spawned->SetBuiltWithRecipe(RC);if(AFGBuildableResourceExtractorBase*M=Cast<AFGBuildableResourceExtractorBase>(Spawned)){M->SetResourceNode(Node);}UGameplayStatics::FinishSpawningActor(Spawned,X);bool Bound=false;if(AFGBuildableResourceExtractorBase*M=Cast<AFGBuildableResourceExtractorBase>(Spawned))Bound=M->GetExtractableResource().GetObject()==Node;if(!Bound){Spawned->Destroy();RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("node_binding_failed"),TEXT("Extractor binding failed; build cost was refunded."),EHttpServerResponseCodes::Conflict));return true;}PlacementReceipts.Add(RequestId,Spawned);B->SetBoolField(TEXT("placed"),true);B->SetBoolField(TEXT("costConsumed"),true);B->SetStringField(TEXT("buildableId"),Spawned->GetName());B->SetStringField(TEXT("className"),Spawned->GetClass()->GetName());B->SetBoolField(TEXT("verified"),true);OnComplete(JsonResponse(B,EHttpServerResponseCodes::Created));return true;
}

bool FMKIBridgeModule::HandlePlaceResourceWell(const FHttpServerRequest& Request,const FHttpResultCallback& OnComplete)
{
    if(!IsAuthorized(Request)){OnComplete(ErrorResponse(TEXT("unauthorized"),TEXT("A valid bearer token is required."),EHttpServerResponseCodes::Denied));return true;}
    FString Text;if(!Request.Body.IsEmpty()){FUTF8ToTCHAR Converted((const ANSICHAR*)Request.Body.GetData(),Request.Body.Num());Text=FString(Converted.Length(),Converted.Get());}
    TSharedPtr<FJsonObject> Json;auto Reader=TJsonReaderFactory<>::Create(Text);if(!FJsonSerializer::Deserialize(Reader,Json)||!Json){OnComplete(ErrorResponse(TEXT("invalid_json"),TEXT("Request body must be JSON."),EHttpServerResponseCodes::BadRequest));return true;}
    FString NodeId,RequestId,ClassPath,RecipePath;bool Confirm=false;Json->TryGetStringField(TEXT("nodeId"),NodeId);Json->TryGetStringField(TEXT("requestId"),RequestId);Json->TryGetStringField(TEXT("classPath"),ClassPath);Json->TryGetStringField(TEXT("recipePath"),RecipePath);Json->TryGetBoolField(TEXT("confirm"),Confirm);
    if(NodeId.IsEmpty()||ClassPath.IsEmpty()||RecipePath.IsEmpty()||(Confirm&&RequestId.IsEmpty())){OnComplete(ErrorResponse(TEXT("invalid_request"),TEXT("nodeId, classPath, recipePath, and confirmed requestId are required."),EHttpServerResponseCodes::BadRequest));return true;}
    if(!ClassPath.StartsWith(TEXT("/Game/FactoryGame/"))||!RecipePath.StartsWith(TEXT("/Game/FactoryGame/"))){OnComplete(ErrorResponse(TEXT("invalid_asset_path"),TEXT("Resource-well assets must come from the observed FactoryGame catalog."),EHttpServerResponseCodes::BadRequest));return true;}
    UWorld* World=GEngine?GEngine->GetCurrentPlayWorld():nullptr;APawn* Player=World?UGameplayStatics::GetPlayerPawn(World,0):nullptr;AFGBuildableSubsystem* Subsystem=World?AFGBuildableSubsystem::Get(World):nullptr;
    AFGResourceNodeFrackingCore* Core=nullptr;AFGResourceNodeFrackingSatellite* Satellite=nullptr;
    if(!World||!Player||!Subsystem){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World is not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}
    for(TActorIterator<AFGResourceNodeFrackingCore> It(World);It;++It)if(It->GetName().Equals(NodeId,ESearchCase::CaseSensitive)){Core=*It;break;}if(!Core)for(TActorIterator<AFGResourceNodeFrackingSatellite> It(World);It;++It)if(It->GetName().Equals(NodeId,ESearchCase::CaseSensitive)){Satellite=*It;break;}
    if(!Core&&!Satellite){OnComplete(ErrorResponse(TEXT("node_not_found"),TEXT("Exact fracking core or satellite not found."),EHttpServerResponseCodes::NotFound));return true;}
    const FVector NodeLocation=Core?Core->GetActorLocation():Satellite->GetActorLocation();if(FVector::Distance(NodeLocation,Player->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("Resource-well node is outside the configured safety radius."),EHttpServerResponseCodes::Forbidden));return true;}
    if((Core&&Core->GetActivator().IsValid())||(Satellite&&(!Satellite->CanPlaceResourceExtractor()||Satellite->GetExtractor().IsValid()))){OnComplete(ErrorResponse(TEXT("node_unavailable"),TEXT("Resource-well node is occupied or not active."),EHttpServerResponseCodes::Conflict));return true;}
    UClass* BuildClass=StaticLoadClass(AFGBuildable::StaticClass(),nullptr,*ClassPath);UClass* RecipeClass=StaticLoadClass(UFGRecipe::StaticClass(),nullptr,*RecipePath);
    const bool Compatible=BuildClass&&RecipeClass&&((Core&&BuildClass->IsChildOf(AFGBuildableFrackingActivator::StaticClass()))||(Satellite&&BuildClass->IsChildOf(AFGBuildableFrackingExtractor::StaticClass())));
    if(!Compatible){OnComplete(ErrorResponse(TEXT("asset_incompatible"),TEXT("The requested asset does not match the selected resource-well node kind."),EHttpServerResponseCodes::BadRequest));return true;}
    auto Body=MakeShared<FJsonObject>();Body->SetBoolField(TEXT("ok"),true);Body->SetBoolField(TEXT("dryRun"),!Confirm);Body->SetStringField(TEXT("nodeId"),NodeId);Body->SetStringField(TEXT("nodeKind"),Core?TEXT("fracking_core"):TEXT("fracking_satellite"));Body->SetStringField(TEXT("classPath"),ClassPath);Body->SetStringField(TEXT("recipePath"),RecipePath);
    if(!Confirm){OnComplete(JsonResponse(Body));return true;}
    FString Missing;if(!ConsumeRecipeCost(World,RecipeClass,1,Missing)){OnComplete(ErrorResponse(TEXT("insufficient_build_materials"),Missing,EHttpServerResponseCodes::Conflict));return true;}
    const FTransform Transform((Core?Core->GetActorRotation():Satellite->GetActorRotation()),NodeLocation);AFGBuildable* Spawned=Subsystem->BeginSpawnBuildable(BuildClass,Transform);if(!Spawned){RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("spawn_failed"),TEXT("Resource-well building spawn failed; build cost was refunded."),EHttpServerResponseCodes::ServerError));return true;}
    Spawned->SetBuiltWithRecipe(RecipeClass);
    if(Core){TScriptInterface<IFGExtractableResourceInterface> Resource;Resource.SetObject(Core);Resource.SetInterface(Cast<IFGExtractableResourceInterface>(Core));CastChecked<AFGBuildableFrackingActivator>(Spawned)->SetExtractableResource(Resource);}
    else{CastChecked<AFGBuildableFrackingExtractor>(Spawned)->SetResourceNode(Satellite);}
    UGameplayStatics::FinishSpawningActor(Spawned,Transform);
    const bool Bound=Core?(Core->GetActivator().Get()==Spawned):(Satellite->GetExtractor().Get()==Spawned);
    if(!Bound){Spawned->Destroy();RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("node_binding_failed"),TEXT("Resource-well binding failed and was rolled back; build cost was refunded."),EHttpServerResponseCodes::Conflict));return true;}
    PlacementReceipts.Add(RequestId,Spawned);Body->SetBoolField(TEXT("placed"),true);Body->SetBoolField(TEXT("verified"),true);Body->SetBoolField(TEXT("costConsumed"),true);Body->SetStringField(TEXT("buildableId"),Spawned->GetName());Body->SetStringField(TEXT("className"),Spawned->GetClass()->GetName());OnComplete(JsonResponse(Body,EHttpServerResponseCodes::Created));return true;
}

bool FMKIBridgeModule::HandlePlaceGeothermal(const FHttpServerRequest& Request,const FHttpResultCallback& OnComplete)
{
    if(!IsAuthorized(Request)){OnComplete(ErrorResponse(TEXT("unauthorized"),TEXT("A valid bearer token is required."),EHttpServerResponseCodes::Denied));return true;}
    FString Text;if(!Request.Body.IsEmpty()){FUTF8ToTCHAR Converted((const ANSICHAR*)Request.Body.GetData(),Request.Body.Num());Text=FString(Converted.Length(),Converted.Get());}
    TSharedPtr<FJsonObject> Json;auto Reader=TJsonReaderFactory<>::Create(Text);if(!FJsonSerializer::Deserialize(Reader,Json)||!Json){OnComplete(ErrorResponse(TEXT("invalid_json"),TEXT("Request body must be JSON."),EHttpServerResponseCodes::BadRequest));return true;}
    FString NodeId,RequestId,ClassPath,RecipePath;bool Confirm=false;Json->TryGetStringField(TEXT("nodeId"),NodeId);Json->TryGetStringField(TEXT("requestId"),RequestId);Json->TryGetStringField(TEXT("classPath"),ClassPath);Json->TryGetStringField(TEXT("recipePath"),RecipePath);Json->TryGetBoolField(TEXT("confirm"),Confirm);
    if(NodeId.IsEmpty()||ClassPath.IsEmpty()||RecipePath.IsEmpty()||(Confirm&&RequestId.IsEmpty())){OnComplete(ErrorResponse(TEXT("invalid_request"),TEXT("nodeId, classPath, recipePath, and confirmed requestId are required."),EHttpServerResponseCodes::BadRequest));return true;}
    if(!ClassPath.StartsWith(TEXT("/Game/FactoryGame/"))||!RecipePath.StartsWith(TEXT("/Game/FactoryGame/"))){OnComplete(ErrorResponse(TEXT("invalid_asset_path"),TEXT("Geothermal assets must come from the observed FactoryGame catalog."),EHttpServerResponseCodes::BadRequest));return true;}
    UWorld* World=GEngine?GEngine->GetCurrentPlayWorld():nullptr;AFGCharacterPlayer* Player=World?Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World,0)):nullptr;APlayerController* Controller=World?UGameplayStatics::GetPlayerController(World,0):nullptr;AFGResourceNodeGeyser* Node=nullptr;
    if(!World||!Player||!Controller){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World is not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}
    for(TActorIterator<AFGResourceNodeGeyser> It(World);It;++It)if(It->GetName().Equals(NodeId,ESearchCase::CaseSensitive)){Node=*It;break;}
    if(!Node){OnComplete(ErrorResponse(TEXT("node_not_found"),TEXT("Exact geyser node not found."),EHttpServerResponseCodes::NotFound));return true;}
    if(FVector::Distance(Node->GetActorLocation(),Player->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("Geyser is outside the configured safety radius."),EHttpServerResponseCodes::Forbidden));return true;}
    if(!Node->CanPlaceResourceExtractor()){OnComplete(ErrorResponse(TEXT("node_unavailable"),TEXT("Geyser is occupied."),EHttpServerResponseCodes::Conflict));return true;}
    UClass* BuildClass=StaticLoadClass(AFGBuildable::StaticClass(),nullptr,*ClassPath);UClass* RecipeClass=StaticLoadClass(UFGRecipe::StaticClass(),nullptr,*RecipePath);if(!BuildClass||!RecipeClass||!BuildClass->IsChildOf(AFGBuildableGeneratorGeoThermal::StaticClass())){OnComplete(ErrorResponse(TEXT("asset_incompatible"),TEXT("The requested asset is not a geothermal generator."),EHttpServerResponseCodes::BadRequest));return true;}
    auto Body=MakeShared<FJsonObject>();Body->SetBoolField(TEXT("ok"),true);Body->SetBoolField(TEXT("dryRun"),!Confirm);Body->SetStringField(TEXT("nodeId"),NodeId);Body->SetStringField(TEXT("classPath"),ClassPath);Body->SetStringField(TEXT("recipePath"),RecipePath);if(!Confirm){OnComplete(JsonResponse(Body));return true;}
    FString Missing;if(!ConsumeRecipeCost(World,RecipeClass,1,Missing)){OnComplete(ErrorResponse(TEXT("insufficient_build_materials"),Missing,EHttpServerResponseCodes::Conflict));return true;}
    AFGGeoThermalGeneratorHologram* Hologram=Cast<AFGGeoThermalGeneratorHologram>(AFGHologram::SpawnHologramFromRecipe(RecipeClass,Controller,Node->GetActorLocation(),Player));if(!Hologram){RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"),TEXT("The geothermal hologram could not be spawned; build cost was refunded."),EHttpServerResponseCodes::ServerError));return true;}
    Hologram->WaitForGamestateApplyCustomizationData();FTimerHandle Timer;World->GetTimerManager().SetTimer(Timer,[this,WeakWorld=TWeakObjectPtr<UWorld>(World),WeakNode=TWeakObjectPtr<AFGResourceNodeGeyser>(Node),WeakRecipe=TWeakObjectPtr<UClass>(RecipeClass),WeakHologram=TWeakObjectPtr<AFGGeoThermalGeneratorHologram>(Hologram),Ticket=ActiveTicket,RequestId,Body,OnComplete]() mutable {
        UWorld* World=WeakWorld.Get();AFGResourceNodeGeyser* Node=WeakNode.Get();UClass* RecipeClass=WeakRecipe.Get();AFGGeoThermalGeneratorHologram* Hologram=WeakHologram.Get();
        if(!World||!Node||!RecipeClass||!Hologram||(Ticket&&Ticket->bAbandoned)){if(Hologram)Hologram->Destroy();if(World&&RecipeClass)RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"),TEXT("The world, geyser, or hologram disappeared before construction; nothing was built."),EHttpServerResponseCodes::Conflict));return;}
        UPrimitiveComponent* Primitive=nullptr;TInlineComponentArray<UPrimitiveComponent*> Primitives(Node);if(Primitives.Num()>0)Primitive=Primitives[0];const FVector Location=Node->GetActorLocation();FHitResult Hit(Node,Primitive,Location,FVector::UpVector);Hit.bBlockingHit=true;Hit.TraceStart=Location+FVector::UpVector*1000.0;Hit.TraceEnd=Location-FVector::UpVector*1000.0;Hit.Location=Location;Hit.ImpactPoint=Location;
        Hologram->TrySnapToActor(Hit);Hologram->UpdateHologramPlacement(Hit);Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));const bool Finished=Hologram->DoMultiStepPlacement(false);Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));
        TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;Hologram->GetConstructDisqualifiers(Disqualifiers);bool OnlyInitializing=Finished&&Disqualifiers.Num()>0;for(auto D:Disqualifiers)if(!D||!D->IsChildOf(UFGCDInitializing::StaticClass())){OnlyInitializing=false;break;}if(OnlyInitializing){Hologram->ResetConstructDisqualifiers();Hologram->SetPlacementMaterialState(EHologramMaterialState::HMS_OK);Disqualifiers.Reset();}if(!Finished||!Hologram->CanConstruct()){FString Names;for(auto D:Disqualifiers){if(!Names.IsEmpty())Names+=TEXT(",");Names+=D?D->GetName():TEXT("null");}Hologram->Destroy();RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("hologram_placement_failed"),FString::Printf(TEXT("Native geothermal hologram rejected the geyser (finished=%s, disqualifiers=[%s]); build cost refunded."),Finished?TEXT("true"):TEXT("false"),*Names),EHttpServerResponseCodes::Conflict));return;}
        TArray<AActor*> Children;AFGBuildable* Built=Cast<AFGBuildable>(Hologram->Construct(Children,FNetConstructionID()));if(IsValid(Hologram))Hologram->Destroy();if(!Built||!Built->IsA<AFGBuildableGeneratorGeoThermal>()||Built->GetActorLocation().Z < -50000.0||FVector::Distance(Built->GetActorLocation(),Node->GetActorLocation())>2500.0){if(Built)Built->Destroy();RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("geothermal_verification_failed"),TEXT("Native geothermal construction returned an invalid class or transform and was rolled back."),EHttpServerResponseCodes::Conflict));return;}
        PlacementReceipts.Add(RequestId,Built);Body->SetBoolField(TEXT("placed"),true);Body->SetBoolField(TEXT("verified"),true);Body->SetBoolField(TEXT("hologramValidated"),true);Body->SetStringField(TEXT("buildableId"),Built->GetName());Body->SetStringField(TEXT("className"),Built->GetClass()->GetName());OnComplete(JsonResponse(Body,EHttpServerResponseCodes::Created));
    },0.05f,false);return true;
}

bool FMKIBridgeModule::HandleDeleteBuildable(const FHttpServerRequest& Request,const FHttpResultCallback& OnComplete)
{
    if(!IsAuthorized(Request)){OnComplete(ErrorResponse(TEXT("unauthorized"),TEXT("A valid bearer token is required."),EHttpServerResponseCodes::Denied));return true;}
    TSharedPtr<FJsonObject> Json;if(!ReadJsonBody(Request,Json)){OnComplete(ErrorResponse(TEXT("invalid_json"),TEXT("Request body must be a JSON object."),EHttpServerResponseCodes::BadRequest));return true;}
    FString BuildableId;bool Confirm=false;Json->TryGetStringField(TEXT("buildableId"),BuildableId);Json->TryGetBoolField(TEXT("confirm"),Confirm);
    if(BuildableId.IsEmpty()){OnComplete(ErrorResponse(TEXT("buildable_id_required"),TEXT("An exact buildableId is required."),EHttpServerResponseCodes::BadRequest));return true;}
    UWorld* World=GEngine?GEngine->GetCurrentPlayWorld():nullptr;APawn* Player=World?UGameplayStatics::GetPlayerPawn(World,0):nullptr;AFGBuildableSubsystem* Subsystem=World?AFGBuildableSubsystem::Get(World):nullptr;
    if(!World||!Player||!Subsystem){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World is not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}
    auto Body=MakeShared<FJsonObject>();Body->SetBoolField(TEXT("ok"),true);Body->SetBoolField(TEXT("dryRun"),!Confirm);Body->SetStringField(TEXT("buildableId"),BuildableId);Body->SetBoolField(TEXT("confirmationRequired"),!Confirm);
    // Refunds return the build recipe's ingredients once, the same unit the bridge charges when it builds. Spline
    // length is not modelled. With no-build-cost enabled nothing is added.
    auto Refund=[World,&Body](TSubclassOf<UFGRecipe> Recipe,int32 Multiplier=1){if(Recipe){RefundRecipeCost(World,Recipe,Multiplier);Body->SetStringField(TEXT("refundRecipePath"),Recipe->GetPathName());Body->SetNumberField(TEXT("refundMultiplier"),Multiplier);}Body->SetBoolField(TEXT("refunded"),Recipe!=nullptr);};
    if(BuildableId.StartsWith(TEXT("lw|"),ESearchCase::CaseSensitive))
    {
        // Exact lightweight dismantle applies the same checks as an actor: strict canonical identity (a malformed
        // index never resolves to instance 0), the mutation safety radius, CanDismantle on the instance's temporary
        // actor, host authority (DispatchRoute), and a recipe refund.
        TSubclassOf<AFGBuildable> Class;int32 InstanceIndex=INDEX_NONE;FString ClassPath;
        if(!ParseCanonicalLightweightId(BuildableId,ClassPath,InstanceIndex)){OnComplete(ErrorResponse(TEXT("invalid_buildable_id"),TEXT("Lightweight IDs must be exactly lw|<classPath>|<index>."),EHttpServerResponseCodes::BadRequest));return true;}
        const FRuntimeBuildableInstanceData* Record=FindLightweightRecord(World,BuildableId,Class,InstanceIndex);
        AFGLightweightBuildableSubsystem* Lightweight=AFGLightweightBuildableSubsystem::Get(World);
        if(!Record||!Lightweight||!Class){OnComplete(ErrorResponse(TEXT("buildable_not_found"),TEXT("No live lightweight instance with that exact ID was found."),EHttpServerResponseCodes::NotFound));return true;}
        const FVector Location=Record->Transform.GetLocation();const TSubclassOf<UFGRecipe> Recipe=Record->BuiltWithRecipe;
        if(FVector::Distance(Location,Player->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("The buildable is outside the configured mutation safety radius."),EHttpServerResponseCodes::Forbidden));return true;}
        // #164: ask CanDismantle of the instance's temporary through the subsystem's own bookkeeping, as the build gun's
        // converter and the hologram probe (#148) do: an existing temporary is reused, and one spawned for the check is
        // removed with RemoveTemporaryBuildableForInstanceIndex. A raw temporary spawned and destroyed here left the instance
        // unable to spawn another, so it refused CanDismantle (and an atomic batch leaked its foundations) until a restart.
        const bool CanDismantle=LightweightCanDismantle(Lightweight,Class,InstanceIndex,Record);
        if(!CanDismantle){OnComplete(ErrorResponse(TEXT("dismantle_rejected"),TEXT("The lightweight instance cannot be dismantled."),EHttpServerResponseCodes::Conflict));return true;}
        Body->SetStringField(TEXT("className"),Class->GetName());Body->SetBoolField(TEXT("lightweight"),true);
        if(!Confirm){OnComplete(JsonResponse(Body));return true;}
        if(World->GetNetMode()==NM_Client){OnComplete(ErrorResponse(TEXT("server_authority_required"),TEXT("Deletion is only available to the host or in single-player."),EHttpServerResponseCodes::Forbidden));return true;}
        Lightweight->RemoveByInstanceIndex(Class,InstanceIndex);
        TSubclassOf<AFGBuildable> AfterClass;int32 AfterIndex=INDEX_NONE;
        const bool Removed=FindLightweightRecord(World,BuildableId,AfterClass,AfterIndex)==nullptr;
        if(!Removed){OnComplete(ErrorResponse(TEXT("dismantle_unverified"),TEXT("The lightweight instance is still present after removal."),EHttpServerResponseCodes::Conflict));return true;}
        Refund(Recipe);Body->SetBoolField(TEXT("deleted"),true);Body->SetBoolField(TEXT("verifiedDismantled"),true);
        OnComplete(JsonResponse(Body));return true;
    }
    AFGBuildable* Target=FindBuildableById(World,BuildableId);
    if(!Target)
    {
        // A vehicle (a truck, tractor, explorer, or rail vehicle), by its exact actor name as /v1/vehicles reports it: the
        // same checks as a buildable (safety radius, CanDismantle, host authority), refunded what its dismantle interface
        // returns, as the build gun dismantles it.
        AFGVehicle* Vehicle=nullptr;
        for(TActorIterator<AFGVehicle> It(World);It;++It)if(IsValid(*It)&&It->GetName().Equals(BuildableId,ESearchCase::CaseSensitive)){Vehicle=*It;break;}
        if(!Vehicle){OnComplete(ErrorResponse(TEXT("buildable_not_found"),TEXT("No buildable or vehicle with that exact ID was found."),EHttpServerResponseCodes::NotFound));return true;}
        if(FVector::Distance(Vehicle->GetActorLocation(),Player->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("The vehicle is outside the configured mutation radius."),EHttpServerResponseCodes::Forbidden));return true;}
        if(!IFGDismantleInterface::Execute_CanDismantle(Vehicle)){OnComplete(ErrorResponse(TEXT("dismantle_rejected"),TEXT("The vehicle cannot be dismantled."),EHttpServerResponseCodes::Conflict));return true;}
        Body->SetStringField(TEXT("className"),Vehicle->GetClass()->GetName());Body->SetBoolField(TEXT("vehicle"),true);
        if(!Confirm){OnComplete(JsonResponse(Body));return true;}
        if(World->GetNetMode()==NM_Client){OnComplete(ErrorResponse(TEXT("server_authority_required"),TEXT("Deletion is only available to the host or in single-player."),EHttpServerResponseCodes::Forbidden));return true;}
        UFGInventoryComponent* VehicleInventory=GetPlayerInventory(World);
        TArray<FInventoryStack> Stacks;IFGDismantleInterface::Execute_GetDismantleRefund(Vehicle,Stacks,VehicleInventory&&VehicleInventory->GetNoBuildCost());
        IFGDismantleInterface::Execute_Dismantle(Vehicle);
        const bool Dismantled=!IsValid(Vehicle)||Vehicle->IsActorBeingDestroyed();
        if(Dismantled&&VehicleInventory&&!VehicleInventory->GetNoBuildCost())for(const FInventoryStack& Stack:Stacks)VehicleInventory->AddStack(Stack,true);
        Body->SetBoolField(TEXT("refunded"),Dismantled&&Stacks.Num()>0);Body->SetBoolField(TEXT("deleted"),Dismantled);Body->SetBoolField(TEXT("verifiedDismantled"),Dismantled);
        OnComplete(JsonResponse(Body));return true;
    }
    if(FVector::Distance(BuildableLocation(Target),Player->GetActorLocation())>GetMaximumPlacementDistanceCm()){OnComplete(ErrorResponse(TEXT("outside_safety_radius"),TEXT("The buildable is outside the configured mutation safety radius."),EHttpServerResponseCodes::Forbidden));return true;}
    if(!IFGDismantleInterface::Execute_CanDismantle(Target)){OnComplete(ErrorResponse(TEXT("dismantle_rejected"),TEXT("The target cannot be dismantled."),EHttpServerResponseCodes::Conflict));return true;}
    Body->SetStringField(TEXT("className"),Target->GetClass()->GetName());
    // Child dismantle actors go with the target, as the build gun dismantles them (a spline road's barriers, #181). Every
    // child that can be dismantled on its own is dismantled and refunded here; one that cannot (a truck station's docking
    // node, #197) belongs to its parent, which removes it, and does not block the dismantle.
    TArray<AActor*> Children;IFGDismantleInterface::Execute_GetChildDismantleActors(Target,Children);
    // A station's or platform's own track refuses to be dismantled alone but goes with its platform (live #18: dismantling a
    // Train Station left its track standing).
    Children.RemoveAll([Target](AActor* Child){const AFGBuildableRailroadTrack* Track=Cast<AFGBuildableRailroadTrack>(Child);return !IsValid(Child)||Child==Target||!Child->Implements<UFGDismantleInterface>()||(!IFGDismantleInterface::Execute_CanDismantle(Child)&&!(Track&&Track->GetIsOwnedByPlatform()));});
    TArray<TSharedPtr<FJsonValue>> ChildIds;for(AActor* Child:Children)ChildIds.Add(MakeShared<FJsonValueString>(Child->GetName()));
    Body->SetArrayField(TEXT("childIds"),ChildIds);
    if(!Confirm){OnComplete(JsonResponse(Body));return true;}
    if(World->GetNetMode()==NM_Client){OnComplete(ErrorResponse(TEXT("server_authority_required"),TEXT("Deletion is only available to the host or in single-player."),EHttpServerResponseCodes::Forbidden));return true;}
    // Children refund what the dismantle interface says they return (their recipe times their own multiplier).
    UFGInventoryComponent* Inventory=GetPlayerInventory(World);int32 ChildrenDismantled=0;
    for(AActor* Child:Children)
    {
        TArray<FInventoryStack> Stacks;IFGDismantleInterface::Execute_GetDismantleRefund(Child,Stacks,Inventory&&Inventory->GetNoBuildCost());
        IFGDismantleInterface::Execute_Dismantle(Child);
        const AFGBuildable* ChildBuildable=Cast<AFGBuildable>(Child);
        if(!ChildBuildable||ChildBuildable->GetIsDismantled()||ChildBuildable->IsAboutToBeDismantled()){++ChildrenDismantled;if(Inventory&&!Inventory->GetNoBuildCost())for(const FInventoryStack& Stack:Stacks)Inventory->AddStack(Stack,true);}
    }
    Body->SetNumberField(TEXT("childrenDismantled"),ChildrenDismantled);
    const TSubclassOf<UFGRecipe> Recipe=Target->GetBuiltWithRecipe();
    // A spline road is charged one recipe per equivalent foundation of its deck, so it refunds the same multiple. Other MKI
    // Roads buildables (a barrier: its recipe per piece, #181) refund what their dismantle interface returns.
    // A road piece's square (MKIRoadTile, a native class) carries its piece's recipe for sampling but refunds nothing itself.
    const bool RoadPackPerPiece=!IsSplineRoad(Target)&&(Target->GetClass()->GetPathName().StartsWith(TEXT("/MKIRoads/"))||Target->GetClass()->GetPathName().StartsWith(TEXT("/Script/MKIRoads.")));
    TArray<FInventoryStack> TargetStacks;if(RoadPackPerPiece)IFGDismantleInterface::Execute_GetDismantleRefund(Target,TargetStacks,Inventory&&Inventory->GetNoBuildCost());
    const int32 RefundMultiplier=IsSplineRoad(Target)?SplineRoadRefundMultiplier(Target):1;
    IFGDismantleInterface::Execute_Dismantle(Target);
    const bool Dismantled=Target->GetIsDismantled()||Target->IsAboutToBeDismantled();
    if(Dismantled&&!RoadPackPerPiece)Refund(Recipe,RefundMultiplier);
    if(Dismantled&&RoadPackPerPiece){if(Inventory&&!Inventory->GetNoBuildCost())for(const FInventoryStack& Stack:TargetStacks)Inventory->AddStack(Stack,true);Body->SetBoolField(TEXT("refunded"),Recipe!=nullptr);if(Recipe)Body->SetStringField(TEXT("refundRecipePath"),Recipe->GetPathName());int32 Units=0;for(const FInventoryStack& Stack:TargetStacks)Units+=Stack.NumItems;Body->SetNumberField(TEXT("refundItems"),Units);}
    Body->SetBoolField(TEXT("deleted"),true);Body->SetBoolField(TEXT("verifiedDismantled"),Dismantled&&ChildrenDismantled==Children.Num());OnComplete(JsonResponse(Body));return true;
}

bool FMKIBridgeModule::HandlePlaceFoundation(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }

    FString RequestText;
    if (!Request.Body.IsEmpty())
    {
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
        RequestText = FString(Converted.Length(), Converted.Get());
    }
    TSharedPtr<FJsonObject> Json;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestText);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
    {
        OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    const TSharedPtr<FJsonObject>* PositionObject = nullptr;
    const TSharedPtr<FJsonObject>* RelativeObject = nullptr;
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    double ForwardCm = 0.0;
    double RightCm = 0.0;
    double UpCm = 0.0;
    const bool HasAbsolute = Json->TryGetObjectField(TEXT("position"), PositionObject) && PositionObject && PositionObject->IsValid() &&
        (*PositionObject)->TryGetNumberField(TEXT("x"), X) &&
        (*PositionObject)->TryGetNumberField(TEXT("y"), Y) &&
        (*PositionObject)->TryGetNumberField(TEXT("z"), Z) &&
        FMath::IsFinite(X) && FMath::IsFinite(Y) && FMath::IsFinite(Z);
    const bool HasRelative = Json->TryGetObjectField(TEXT("relativeToPlayer"), RelativeObject) && RelativeObject && RelativeObject->IsValid() &&
        (*RelativeObject)->TryGetNumberField(TEXT("forwardCm"), ForwardCm) &&
        (*RelativeObject)->TryGetNumberField(TEXT("rightCm"), RightCm) &&
        (*RelativeObject)->TryGetNumberField(TEXT("upCm"), UpCm) &&
        FMath::IsFinite(ForwardCm) && FMath::IsFinite(RightCm) && FMath::IsFinite(UpCm);
    if (HasAbsolute == HasRelative)
    {
        OnComplete(ErrorResponse(TEXT("invalid_position"), TEXT("Provide exactly one of position {x,y,z} or relativeToPlayer {forwardCm,rightCm,upCm}."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    bool Confirm = false;
    Json->TryGetBoolField(TEXT("confirm"), Confirm);
    FString RequestId;
    Json->TryGetStringField(TEXT("requestId"), RequestId);
    RequestId.TrimStartAndEndInline();
    if (Confirm && RequestId.IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("Confirmed placements require a unique requestId."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    // A repeated confirmed requestId never reaches this handler: DispatchRoute replays the recorded result.
    double RequestedYaw = 0.0;
    const bool HasRequestedYaw = Json->TryGetNumberField(TEXT("yaw"), RequestedYaw);
    if (HasRequestedYaw && !FMath::IsFinite(RequestedYaw))
    {
        OnComplete(ErrorResponse(TEXT("invalid_rotation"), TEXT("yaw must be a finite number."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* PlayerPawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    APlayerController* PlayerController = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    if (!World || !PlayerPawn || !PlayerController)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player is available."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }

    FVector Position(X, Y, Z);
    double Yaw = HasRequestedYaw ? RequestedYaw : PlayerController->GetControlRotation().Yaw;
    if (HasRelative)
    {
        const FRotator ViewYaw(0.0, PlayerController->GetControlRotation().Yaw, 0.0);
        Position = PlayerPawn->GetActorLocation()
            + ViewYaw.Vector() * ForwardCm
            + FRotationMatrix(ViewYaw).GetUnitAxis(EAxis::Y) * RightCm
            + FVector::UpVector * UpCm;
    }
    Position.X = SnapToIncrement(Position.X, FoundationWorldGridSizeCm);
    Position.Y = SnapToIncrement(Position.Y, FoundationWorldGridSizeCm);
    Position.Z = SnapToIncrement(Position.Z, FoundationVerticalGridSizeCm);
    Yaw = SnapToIncrement(Yaw, WorldGridYawStepDegrees);
    const double DistanceCm = FVector::Distance(Position, PlayerPawn->GetActorLocation());
    if (DistanceCm > GetMaximumPlacementDistanceCm())
    {
        OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Foundation must be within the configured safety radius of the player."), EHttpServerResponseCodes::Forbidden));
        return true;
    }

    AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(World);
    if (!BuildableSubsystem)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("Buildable subsystem is not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }

    const FBox CandidateBox(Position - FVector(390.0, 390.0, 190.0), Position + FVector(390.0, 390.0, 190.0));
    TArray<AFGBuildable*> Collisions;
    BuildableSubsystem->GetCollidingBuildablesInBoundingBox(Collisions, CandidateBox);
    if (const AFGBuildable* Colliding = FirstSolidOverlap(Collisions, CandidateBox))
    {
        OnComplete(ErrorResponse(TEXT("buildable_collision"), FString::Printf(TEXT("The requested foundation volume overlaps %s."), *Colliding->GetName()), EHttpServerResponseCodes::Conflict));
        return true;
    }

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("confirmationRequired"), !Confirm);
    Body->SetNumberField(TEXT("distanceFromPlayerCm"), DistanceCm);
    const TSharedRef<FJsonObject> ResolvedPosition = MakeShared<FJsonObject>();
    ResolvedPosition->SetNumberField(TEXT("x"), Position.X);
    ResolvedPosition->SetNumberField(TEXT("y"), Position.Y);
    ResolvedPosition->SetNumberField(TEXT("z"), Position.Z);
    Body->SetObjectField(TEXT("resolvedPosition"), ResolvedPosition);
    Body->SetNumberField(TEXT("resolvedYaw"), Yaw);
    Body->SetBoolField(TEXT("worldGridSnapped"), true);
    if (!RequestId.IsEmpty())
    {
        Body->SetStringField(TEXT("requestId"), RequestId);
    }

    if (!Confirm)
    {
        Body->SetBoolField(TEXT("placed"), false);
        Body->SetStringField(TEXT("reason"), TEXT("validated_awaiting_confirmation"));
        OnComplete(JsonResponse(Body));
        return true;
    }
    if (World->GetNetMode() == NM_Client)
    {
        OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Construction is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden));
        return true;
    }

    UClass* FoundationClass = StaticLoadClass(AFGBuildable::StaticClass(), nullptr, FoundationClassPath);
    UClass* FoundationRecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, FoundationRecipePath);
    if (!FoundationClass || !FoundationRecipeClass)
    {
        OnComplete(ErrorResponse(TEXT("foundation_asset_unavailable"), TEXT("The standard 8m x 4m foundation asset could not be loaded."), EHttpServerResponseCodes::ServerError));
        return true;
    }

    // A single foundation is charged exactly like a grid cell and refunded if it cannot be spawned.
    FString MissingBuildMaterials;
    if (!ConsumeRecipeCost(World, FoundationRecipeClass, 1, MissingBuildMaterials))
    {
        OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), MissingBuildMaterials, EHttpServerResponseCodes::Conflict));
        return true;
    }
    const FTransform Transform(FRotator(0.0, Yaw, 0.0), Position);
    // The game converts a foundation to a lightweight instance while it finishes spawning (verified live), so take the
    // existing instances first; only those count as pre-existing for the final placement identity (#113).
    const TSet<int32> LightweightBefore = ValidLightweightIndices(World, FoundationClass);
    AFGBuildable* Buildable = BuildableSubsystem->BeginSpawnBuildable(FoundationClass, Transform);
    if (!Buildable)
    {
        RefundRecipeCost(World, FoundationRecipeClass, 1);
        OnComplete(ErrorResponse(TEXT("spawn_failed"), TEXT("The foundation could not be spawned; build cost was refunded."), EHttpServerResponseCodes::ServerError));
        return true;
    }
    Buildable->SetBuiltWithRecipe(FoundationRecipeClass);
    UGameplayStatics::FinishSpawningActor(Buildable, Transform);
    NotePlacementOrigin(RequestId, Buildable, &LightweightBefore);
    PlacementReceipts.Add(RequestId, Buildable);

    Body->SetBoolField(TEXT("placed"), true);
    Body->SetBoolField(TEXT("costConsumed"), true);
    Body->SetStringField(TEXT("buildableId"), Buildable->GetName());
    Body->SetStringField(TEXT("reason"), TEXT("confirmed_and_placed"));
    UE_LOG(LogMKIBridge, Display, TEXT("Placed foundation %s at %s by confirmed API request"), *Buildable->GetName(), *Position.ToCompactString());
    OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Created));
    return true;
}

bool FMKIBridgeModule::HandlePlaceFoundationGrid(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }

    FString RequestText;
    if (!Request.Body.IsEmpty())
    {
        FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
        RequestText = FString(Converted.Length(), Converted.Get());
    }
    TSharedPtr<FJsonObject> Json;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestText);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
    {
        OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    double RowsValue = 0.0;
    double ColumnsValue = 0.0;
    double ForwardCm = 0.0;
    double RightCm = 0.0;
    double UpCm = 0.0;
    if (!Json->TryGetNumberField(TEXT("rows"), RowsValue) || !Json->TryGetNumberField(TEXT("columns"), ColumnsValue) ||
        !Json->TryGetNumberField(TEXT("forwardCm"), ForwardCm) || !Json->TryGetNumberField(TEXT("rightCm"), RightCm) ||
        !Json->TryGetNumberField(TEXT("upCm"), UpCm))
    {
        OnComplete(ErrorResponse(TEXT("invalid_grid"), TEXT("rows, columns, forwardCm, rightCm, and upCm are required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    const int32 Rows = static_cast<int32>(RowsValue);
    const int32 Columns = static_cast<int32>(ColumnsValue);
    if (RowsValue != Rows || ColumnsValue != Columns || Rows < 1 || Rows > 16 || Columns < 1 || Columns > 16)
    {
        OnComplete(ErrorResponse(TEXT("invalid_grid"), TEXT("rows and columns must be whole numbers from 1 through 16."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    bool Confirm = false;
    Json->TryGetBoolField(TEXT("confirm"), Confirm);
    FString RequestId;
    Json->TryGetStringField(TEXT("requestId"), RequestId);
    RequestId.TrimStartAndEndInline();
    if (Confirm && RequestId.IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("Confirmed grids require a unique requestId."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    // A repeated confirmed requestId never reaches this handler: DispatchRoute replays the recorded result.

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* PlayerPawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    APlayerController* PlayerController = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    AFGBuildableSubsystem* BuildableSubsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    if (!World || !PlayerPawn || !PlayerController || !BuildableSubsystem)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("Player or buildable subsystem is not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }

    const double GridYaw = SnapToIncrement(PlayerController->GetControlRotation().Yaw, WorldGridYawStepDegrees);
    const FRotator ViewYaw(0.0, GridYaw, 0.0);
    const FVector Forward = ViewYaw.Vector();
    const FVector Right = FRotationMatrix(ViewYaw).GetUnitAxis(EAxis::Y);
    FVector Origin = PlayerPawn->GetActorLocation() + Forward * ForwardCm + Right * RightCm + FVector::UpVector * UpCm;
    Origin.X = SnapToIncrement(Origin.X, FoundationWorldGridSizeCm);
    Origin.Y = SnapToIncrement(Origin.Y, FoundationWorldGridSizeCm);
    Origin.Z = SnapToIncrement(Origin.Z, FoundationVerticalGridSizeCm);
    TArray<FTransform> Transforms;
    Transforms.Reserve(Rows * Columns);
    for (int32 Row = 0; Row < Rows; ++Row)
    {
        for (int32 Column = 0; Column < Columns; ++Column)
        {
            const FVector Position = Origin + Forward * (Row * 800.0) + Right * (Column * 800.0);
            if (FVector::Distance(Position, PlayerPawn->GetActorLocation()) > GetMaximumPlacementDistanceCm())
            {
                OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Every grid foundation must be within the configured safety radius of the player."), EHttpServerResponseCodes::Forbidden));
                return true;
            }
            TArray<AFGBuildable*> Collisions;
            const FBox CellBox(Position - FVector(390.0, 390.0, 190.0), Position + FVector(390.0, 390.0, 190.0));
            BuildableSubsystem->GetCollidingBuildablesInBoundingBox(Collisions, CellBox);
            if (const AFGBuildable* Colliding = FirstSolidOverlap(Collisions, CellBox))
            {
                OnComplete(ErrorResponse(TEXT("buildable_collision"), FString::Printf(TEXT("Grid cell row %d column %d overlaps %s."), Row + 1, Column + 1, *Colliding->GetName()), EHttpServerResponseCodes::Conflict));
                return true;
            }
            Transforms.Emplace(ViewYaw, Position);
        }
    }

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("confirmationRequired"), !Confirm);
    Body->SetNumberField(TEXT("foundationCount"), Transforms.Num());
    Body->SetNumberField(TEXT("resolvedYaw"), ViewYaw.Yaw);
    Body->SetBoolField(TEXT("worldGridSnapped"), true);
    const TSharedRef<FJsonObject> ResolvedOrigin = MakeShared<FJsonObject>();
    ResolvedOrigin->SetNumberField(TEXT("x"), Origin.X);
    ResolvedOrigin->SetNumberField(TEXT("y"), Origin.Y);
    ResolvedOrigin->SetNumberField(TEXT("z"), Origin.Z);
    Body->SetObjectField(TEXT("resolvedOrigin"), ResolvedOrigin);
    if (!Confirm)
    {
        Body->SetBoolField(TEXT("placed"), false);
        Body->SetStringField(TEXT("reason"), TEXT("grid_validated_awaiting_confirmation"));
        OnComplete(JsonResponse(Body));
        return true;
    }
    if (World->GetNetMode() == NM_Client)
    {
        OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Construction is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden));
        return true;
    }

    UClass* FoundationClass = StaticLoadClass(AFGBuildable::StaticClass(), nullptr, FoundationClassPath);
    UClass* FoundationRecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, FoundationRecipePath);
    if (!FoundationClass || !FoundationRecipeClass)
    {
        OnComplete(ErrorResponse(TEXT("foundation_asset_unavailable"), TEXT("The standard foundation asset could not be loaded."), EHttpServerResponseCodes::ServerError));
        return true;
    }
    FString MissingBuildMaterials;
    if (!ConsumeRecipeCost(World, FoundationRecipeClass, Transforms.Num(), MissingBuildMaterials))
    {
        OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), MissingBuildMaterials, EHttpServerResponseCodes::Conflict));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> BuildableIds;
    TArray<AFGBuildable*> SpawnedBuildables;
    AFGBuildable* FirstBuildable = nullptr;
    for (const FTransform& Transform : Transforms)
    {
        AFGBuildable* Buildable = BuildableSubsystem->BeginSpawnBuildable(FoundationClass, Transform);
        if (!Buildable)
        {
            for (AFGBuildable* Spawned : SpawnedBuildables) if (IsValid(Spawned)) Spawned->Destroy();
            RefundRecipeCost(World, FoundationRecipeClass, Transforms.Num());
            OnComplete(ErrorResponse(TEXT("spawn_failed"), TEXT("A grid foundation could not be spawned; all partial foundations were destroyed and build cost was refunded."), EHttpServerResponseCodes::ServerError));
            return true;
        }
        Buildable->SetBuiltWithRecipe(FoundationRecipeClass);
        UGameplayStatics::FinishSpawningActor(Buildable, Transform);
        SpawnedBuildables.Add(Buildable);
        FirstBuildable = FirstBuildable ? FirstBuildable : Buildable;
        BuildableIds.Add(MakeShared<FJsonValueString>(Buildable->GetName()));
    }
    PlacementReceipts.Add(RequestId, FirstBuildable);
    PlacementReceiptTransforms.Add(RequestId, Transforms);
    PlacementReceiptClasses.Add(RequestId, FoundationClass);
    Body->SetBoolField(TEXT("placed"), true);
    Body->SetBoolField(TEXT("costConsumed"), true);
    Body->SetStringField(TEXT("requestId"), RequestId);
    Body->SetArrayField(TEXT("buildableIds"), BuildableIds);
    Body->SetStringField(TEXT("reason"), TEXT("grid_confirmed_and_placed"));
    OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Created));
    return true;
}


bool FMKIBridgeModule::HandlePlaceSplineRoad(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    // A request that aims at a start and an end drives the build-gun hologram instead of writing explicit points.
    if (Json->HasField(TEXT("start")) || Json->HasField(TEXT("end"))) return HandlePlaceSplineRoadHologram(Request, OnComplete);
    FString RequestId; bool Confirm = false, DryRun = true; double LanesValue = 2.0;
    Json->TryGetStringField(TEXT("requestId"), RequestId);
    Json->TryGetBoolField(TEXT("confirm"), Confirm);
    // Only confirm: true builds, because the dispatcher applies authority and idempotency to exactly that field. dryRun is
    // accepted for symmetry with the plan's wording, but it must agree with confirm.
    if (Json->TryGetBoolField(TEXT("dryRun"), DryRun) && DryRun == Confirm)
    { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("dryRun must be the opposite of confirm; send confirm: true with a requestId to build."), EHttpServerResponseCodes::BadRequest)); return true; }
    Json->TryGetNumberField(TEXT("lanes"), LanesValue);
    const int32 Lanes = FMath::RoundToInt32(LanesValue);
    if (!FMath::IsNearlyEqual(LanesValue, static_cast<double>(Lanes)) || (Lanes != 2 && Lanes != 4)) { OnComplete(ErrorResponse(TEXT("invalid_lanes"), TEXT("lanes must be 2 or 4."), EHttpServerResponseCodes::BadRequest)); return true; }
    if (Confirm && RequestId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("Confirmed placement requires requestId."), EHttpServerResponseCodes::BadRequest)); return true; }

    const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
    if (!Json->TryGetArrayField(TEXT("points"), PointValues) || !PointValues || PointValues->Num() < 2 || PointValues->Num() > SplineRoadMaxPoints)
    { OnComplete(ErrorResponse(TEXT("invalid_points"), FString::Printf(TEXT("points must hold 2-%d entries."), SplineRoadMaxPoints), EHttpServerResponseCodes::BadRequest)); return true; }
    TArray<FSplinePointData> WorldPoints;
    for (const TSharedPtr<FJsonValue>& Value : *PointValues)
    {
        const TSharedPtr<FJsonObject> Point = Value.IsValid() ? Value->AsObject() : nullptr;
        FSplinePointData Data;
        if (!Point.IsValid() || !ReadSplineVector(Point, TEXT("location"), Data.Location) || !ReadSplineVector(Point, TEXT("arriveTangent"), Data.ArriveTangent) || !ReadSplineVector(Point, TEXT("leaveTangent"), Data.LeaveTangent))
        { OnComplete(ErrorResponse(TEXT("invalid_points"), TEXT("Every point needs location, arriveTangent, and leaveTangent as {x, y, z} in world centimeters."), EHttpServerResponseCodes::BadRequest)); return true; }
        WorldPoints.Add(Data);
    }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr; APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr; AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    if (!World || !Player || !Subsystem) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    UClass* RoadClass = SplineRoadClass();
    if (!RoadClass) { OnComplete(ErrorResponse(TEXT("spline_road_unavailable"), FString::Printf(TEXT("The installed MKI Roads has no %s class."), SplineRoadClassPath), EHttpServerResponseCodes::Conflict)); return true; }
    // An MKI Roads Road entry (its Blueprint and recipe, e.g. Build_MKIR_SplineRoad4m) builds exactly what the build
    // menu does: the deck material, thickness, and collision of that entry. Without one, the bare native class is built.
    FString RequestedClassPath, RequestedRecipePath;
    Json->TryGetStringField(TEXT("classPath"), RequestedClassPath);
    Json->TryGetStringField(TEXT("recipePath"), RequestedRecipePath);
    UClass* RequestedRecipe = nullptr;
    if (!RequestedClassPath.IsEmpty() || !RequestedRecipePath.IsEmpty())
    {
        UClass* RequestedClass = RequestedClassPath.StartsWith(TEXT("/MKIRoads/")) ? StaticLoadClass(AFGBuildable::StaticClass(), nullptr, *RequestedClassPath) : nullptr;
        RequestedRecipe = RequestedRecipePath.StartsWith(TEXT("/MKIRoads/")) ? StaticLoadClass(UFGRecipe::StaticClass(), nullptr, *RequestedRecipePath) : nullptr;
        if (!RequestedClass || !RequestedClass->IsChildOf(RoadClass) || !RequestedRecipe)
        { OnComplete(ErrorResponse(TEXT("invalid_asset_path"), TEXT("classPath and recipePath must both name an MKI Roads spline road entry (a /MKIRoads/ Blueprint of MKIRoadSplineRoad and its recipe)."), EHttpServerResponseCodes::BadRequest)); return true; }
        RoadClass = RequestedClass;
    }
    for (const FSplinePointData& Point : WorldPoints)
        if (FVector::Distance(Point.Location, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm()) { OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Every spline point must be within the configured safety radius of the player."), EHttpServerResponseCodes::Forbidden)); return true; }

    // Validate every segment: 4-100 m long, at most a 25 degree (46.63 %) grade, never vertical, and no tighter than the deck width in
    // radius (the inner edge then turns about a point half a deck width inside it, as the retired MKI Roads turns did).
    const double WidthCm = Lanes * SplineRoadLaneWidthCm, MinRadiusCm = WidthCm;
    double TotalLengthCm = 0.0, MaxGrade = 0.0, MinRadius = TNumericLimits<double>::Max();
    TArray<TSharedPtr<FJsonValue>> SegmentFacts;
    for (int32 Index = 0; Index + 1 < WorldPoints.Num(); ++Index)
    {
        const FSplineRoadSegmentFacts Facts = SampleSplineRoadSegment(WorldPoints[Index].Location, WorldPoints[Index].LeaveTangent, WorldPoints[Index + 1].Location, WorldPoints[Index + 1].ArriveTangent);
        const TSharedRef<FJsonObject> Segment = MakeShared<FJsonObject>();
        Segment->SetNumberField(TEXT("index"), Index);
        Segment->SetNumberField(TEXT("lengthCm"), Facts.LengthCm);
        Segment->SetNumberField(TEXT("maxGradePercent"), Facts.MaxGradePercent);
        if (Facts.MinHorizontalRadiusCm < TNumericLimits<double>::Max()) Segment->SetNumberField(TEXT("minRadiusCm"), Facts.MinHorizontalRadiusCm);
        SegmentFacts.Add(MakeShared<FJsonValueObject>(Segment));
        const FString Where = FString::Printf(TEXT("Segment %d"), Index);
        if (Facts.LengthCm < SplineRoadMinSegmentCm || Facts.LengthCm > SplineRoadMaxSegmentCm) { OnComplete(ErrorResponse(TEXT("invalid_spline_length"), FString::Printf(TEXT("%s is %.1f cm long; segments must be 4-100 m."), *Where, Facts.LengthCm), EHttpServerResponseCodes::Conflict)); return true; }
        if (Facts.bVertical) { OnComplete(ErrorResponse(TEXT("invalid_spline_grade"), FString::Printf(TEXT("%s turns vertical."), *Where), EHttpServerResponseCodes::Conflict)); return true; }
        if (Facts.MaxGradePercent > SplineRoadMaxGradePercent + 0.01) { OnComplete(ErrorResponse(TEXT("invalid_spline_grade"), FString::Printf(TEXT("%s reaches a %.2f %% grade; the limit is 46.63 %% (25 degrees)."), *Where, Facts.MaxGradePercent), EHttpServerResponseCodes::Conflict)); return true; }
        if (Facts.MinHorizontalRadiusCm < MinRadiusCm - 0.5) { OnComplete(ErrorResponse(TEXT("invalid_spline_radius"), FString::Printf(TEXT("%s bends to a %.1f cm radius; a %d-lane road needs at least %.0f cm."), *Where, Facts.MinHorizontalRadiusCm, Lanes, MinRadiusCm), EHttpServerResponseCodes::Conflict)); return true; }
        TotalLengthCm += Facts.LengthCm; MaxGrade = FMath::Max(MaxGrade, Facts.MaxGradePercent); MinRadius = FMath::Min(MinRadius, Facts.MinHorizontalRadiusCm);
    }
    const int32 FoundationEquivalents = SplineRoadFoundationEquivalents(TotalLengthCm, Lanes);

    // Report, but do not refuse, existing buildables inside the road's bounds: a road meeting another road end touches it.
    FBox Bounds(ForceInit);
    for (const FSplinePointData& Point : WorldPoints) { Bounds += Point.Location + FVector(WidthCm * 0.5, WidthCm * 0.5, 0.0); Bounds += Point.Location - FVector(WidthCm * 0.5, WidthCm * 0.5, 400.0); }
    TArray<AFGBuildable*> Overlaps; Subsystem->GetCollidingBuildablesInBoundingBox(Overlaps, Bounds);
    TArray<TSharedPtr<FJsonValue>> OverlapIds;
    for (AFGBuildable* Overlap : Overlaps) if (Overlap) OverlapIds.Add(MakeShared<FJsonValueString>(Overlap->GetName()));

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetBoolField(TEXT("prototype"), true);
    Body->SetNumberField(TEXT("lanes"), Lanes); Body->SetNumberField(TEXT("widthCm"), WidthCm);
    Body->SetNumberField(TEXT("lengthCm"), TotalLengthCm); Body->SetNumberField(TEXT("maxGradePercent"), MaxGrade);
    if (MinRadius < TNumericLimits<double>::Max()) Body->SetNumberField(TEXT("minRadiusCm"), MinRadius);
    Body->SetNumberField(TEXT("minimumAllowedRadiusCm"), MinRadiusCm);
    Body->SetArrayField(TEXT("segments"), SegmentFacts);
    Body->SetNumberField(TEXT("foundationEquivalents"), FoundationEquivalents);
    Body->SetStringField(TEXT("recipePath"), SplineRoadRecipePath);
    Body->SetArrayField(TEXT("boundsOverlapIds"), OverlapIds);
    if (!RequestId.IsEmpty()) Body->SetStringField(TEXT("requestId"), RequestId);
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Construction is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }

    // An entry's recipe is one asphalt 8x4 foundation, as the default, so the same multiple is charged.
    UClass* RecipeClass = RequestedRecipe ? RequestedRecipe : StaticLoadClass(UFGRecipe::StaticClass(), nullptr, SplineRoadRecipePath);
    if (!RecipeClass) { OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("The asphalt 8x4 foundation recipe could not be loaded."), EHttpServerResponseCodes::ServerError)); return true; }
    FString Missing;
    if (!ConsumeRecipeCost(World, RecipeClass, FoundationEquivalents, Missing)) { OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), Missing, EHttpServerResponseCodes::Conflict)); return true; }

    // The actor sits at the first point, unrotated; its spline points are the requested points relative to it.
    const FVector Origin = WorldPoints[0].Location;
    const FTransform Transform(FRotator::ZeroRotator, Origin);
    AFGBuildable* Road = Subsystem->BeginSpawnBuildable(RoadClass, Transform);
    IFGSplineBuildableInterface* Spline = Cast<IFGSplineBuildableInterface>(Road);
    TArray<FSplinePointData>* Data = Spline ? Spline->GetMutableSplinePointData() : nullptr;
    FIntProperty* LanesProperty = FindFProperty<FIntProperty>(RoadClass, TEXT("mLanes"));
    if (!Road || !Data || !LanesProperty)
    {
        if (Road) Road->Destroy();
        RefundRecipeCost(World, RecipeClass, FoundationEquivalents);
        OnComplete(ErrorResponse(TEXT("spawn_failed"), TEXT("The spline road could not be spawned; build cost was refunded."), EHttpServerResponseCodes::ServerError)); return true;
    }
    Data->Reset();
    for (const FSplinePointData& Point : WorldPoints) Data->Add(FSplinePointData(Point.Location - Origin, Point.ArriveTangent, Point.LeaveTangent));
    LanesProperty->SetPropertyValue_InContainer(Road, Lanes);
    Road->SetBuiltWithRecipe(RecipeClass);
    UGameplayStatics::FinishSpawningActor(Road, Transform);

    // Verify the built road: its spline must match the validated length and its width the request; otherwise roll back.
    const USplineComponent* BuiltSpline = Spline->GetSplineComponent();
    const double BuiltLength = BuiltSpline ? BuiltSpline->GetSplineLength() : 0.0;
    const int32 BuiltLanes = LanesProperty->GetPropertyValue_InContainer(Road);
    // The swept deck reports its cross-sections; zero means no deck (and no collision) was built.
    int32 DeckSlices = 0;
    if (FIntProperty* SlicesProperty = FindFProperty<FIntProperty>(RoadClass, TEXT("mDeckSliceCount"))) DeckSlices = SlicesProperty->GetPropertyValue_InContainer(Road);
    if (!BuiltSpline || FMath::Abs(BuiltLength - TotalLengthCm) > FMath::Max(2.0, TotalLengthCm * 0.002) || BuiltLanes != Lanes || DeckSlices < 2)
    {
        Road->Destroy();
        RefundRecipeCost(World, RecipeClass, FoundationEquivalents);
        OnComplete(ErrorResponse(TEXT("placement_verification_failed"), FString::Printf(TEXT("The built road did not match the request (length %.1f of %.1f cm, lanes %d of %d, %d deck slices) and was rolled back; build cost was refunded."),
            BuiltLength, TotalLengthCm, BuiltLanes, Lanes, DeckSlices), EHttpServerResponseCodes::Conflict)); return true;
    }
    PlacementReceipts.Add(RequestId, Road);
    Body->SetBoolField(TEXT("placed"), true); Body->SetBoolField(TEXT("costConsumed"), true); Body->SetBoolField(TEXT("verified"), true);
    Body->SetStringField(TEXT("buildableId"), Road->GetName()); Body->SetStringField(TEXT("className"), Road->GetClass()->GetName());
    Body->SetObjectField(TEXT("origin"), SplineVectorJson(Origin));
    Body->SetNumberField(TEXT("builtLengthCm"), BuiltLength); Body->SetNumberField(TEXT("deckSliceCount"), DeckSlices);
    UE_LOG(LogMKIBridge, Display, TEXT("Placed spline road %s (%d lanes, %.1f m) by confirmed API request"), *Road->GetName(), Lanes, BuiltLength / 100.0);
    OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Created));
    return true;
}
/**
 * The spline road's build-gun path, driven headlessly: spawns the hologram of a spline road recipe, aims it at the start
 * (a build-gun-channel trace from start.eye through start.target), clicks, aims at the end, and, when confirmed, clicks
 * again and constructs, exactly as the build gun's two-step placement does. lanes and straight are set on the hologram
 * as Plus/Minus and the straight toggle would. The preview reports the hologram's own plan, cost, and disqualifiers.
 */
bool FMKIBridgeModule::HandlePlaceSplineRoadHologram(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString RecipePath, RequestId; bool Confirm = false, Straight = false; double LanesValue = 2.0;
    Json->TryGetStringField(TEXT("recipePath"), RecipePath); RecipePath.TrimStartAndEndInline();
    Json->TryGetStringField(TEXT("requestId"), RequestId); Json->TryGetBoolField(TEXT("confirm"), Confirm); Json->TryGetBoolField(TEXT("straight"), Straight); Json->TryGetNumberField(TEXT("lanes"), LanesValue);
    const int32 Lanes = FMath::RoundToInt32(LanesValue);
    if (!FMath::IsNearlyEqual(LanesValue, static_cast<double>(Lanes)) || (Lanes != 2 && Lanes != 4)) { OnComplete(ErrorResponse(TEXT("invalid_lanes"), TEXT("lanes must be 2 or 4."), EHttpServerResponseCodes::BadRequest)); return true; }
    if (Confirm && RequestId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("Confirmed placement requires requestId."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString PathCode, PathMessage;
    if (!IsPlaceableAssetPath(RecipePath, PathCode, PathMessage)) { OnComplete(ErrorResponse(PathCode, PathMessage, PathCode == TEXT("companion_unavailable") ? EHttpServerResponseCodes::Conflict : EHttpServerResponseCodes::BadRequest)); return true; }
    struct FAim { FVector Eye; FVector Target; };
    FAim Aims[2];
    for (int32 Index = 0; Index < 2; ++Index)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (!Json->TryGetObjectField(Index == 0 ? TEXT("start") : TEXT("end"), Object) || !Object || !Object->IsValid() || !ReadSplineVector(*Object, TEXT("eye"), Aims[Index].Eye) || !ReadSplineVector(*Object, TEXT("target"), Aims[Index].Target) || Aims[Index].Eye.Equals(Aims[Index].Target, 1.0))
        { OnComplete(ErrorResponse(TEXT("invalid_aim"), TEXT("start and end need eye and target {x, y, z} at least 1 cm apart."), EHttpServerResponseCodes::BadRequest)); return true; }
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APlayerController* Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
    if (!World || !Controller || !Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    for (const FAim& Aim : Aims)
        if (FVector::Distance(Aim.Eye, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm() || FVector::Distance(Aim.Target, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm())
        { OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Every eye and target must be within the configured safety radius of the player."), EHttpServerResponseCodes::Forbidden)); return true; }
    if (Confirm && World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Construction is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    UClass* RecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, *RecipePath);
    UClass* RoadClass = SplineRoadClass();
    if (!RecipeClass || !RoadClass) { OnComplete(ErrorResponse(TEXT("spline_road_unavailable"), TEXT("The spline road recipe or class is unavailable."), EHttpServerResponseCodes::Conflict)); return true; }
    AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(RecipeClass, Controller, Aims[0].Target, Player);
    if (!Hologram || !Hologram->IsA(AFGSplineHologram::StaticClass())) { if (Hologram) Hologram->Destroy(); OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"), TEXT("The recipe did not spawn a spline hologram."), EHttpServerResponseCodes::Conflict)); return true; }
    Hologram->WaitForGamestateApplyCustomizationData();
    FTimerHandle Timer;
    World->GetTimerManager().SetTimer(Timer, [this, WeakWorld = TWeakObjectPtr<UWorld>(World), WeakPlayer = TWeakObjectPtr<AFGCharacterPlayer>(Player), WeakHologram = TWeakObjectPtr<AFGHologram>(Hologram), WeakRecipe = TWeakObjectPtr<UClass>(RecipeClass),
        WeakRoadClass = TWeakObjectPtr<UClass>(RoadClass), Ticket = ActiveTicket, Aims0 = Aims[0], Aims1 = Aims[1], Lanes, Straight, Confirm, RequestId, RecipePath, OnComplete]() mutable
    {
        UWorld* World = WeakWorld.Get(); AFGCharacterPlayer* Player = WeakPlayer.Get(); AFGHologram* Hologram = WeakHologram.Get(); UClass* RecipeClass = WeakRecipe.Get(); UClass* RoadClass = WeakRoadClass.Get();
        if (!World || !Player || !Hologram || !RecipeClass || !RoadClass || (Ticket && Ticket->bAbandoned))
        {
            if (Hologram) Hologram->Destroy();
            OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"), TEXT("The world, player, or hologram disappeared before placement; nothing was built."), EHttpServerResponseCodes::Conflict));
            return;
        }
        auto CallInt = [Hologram](const TCHAR* Name, int32 Value) { if (UFunction* Function = Hologram->FindFunction(FName(Name))) { struct { int32 Value; } Params{ Value }; Hologram->ProcessEvent(Function, &Params); } };
        auto CallBool = [Hologram](const TCHAR* Name, bool Value) { if (UFunction* Function = Hologram->FindFunction(FName(Name))) { struct { bool Value; } Params{ Value }; Hologram->ProcessEvent(Function, &Params); } };
        auto ReadFloat = [Hologram](const TCHAR* Name) { float Result = 0.f; if (UFunction* Function = Hologram->FindFunction(FName(Name))) { struct { float ReturnValue; } Params{ 0.f }; Hologram->ProcessEvent(Function, &Params); Result = Params.ReturnValue; } return Result; };
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MKIBridgeSplineRoadAim), true);
        Query.AddIgnoredActor(Player); Query.AddIgnoredActor(Hologram);
        auto Aim = [World, Hologram, &Query](const FVector& Eye, const FVector& Target, FHitResult& Hit)
        {
            const FVector End = Eye + (Target - Eye).GetSafeNormal() * (FVector::Distance(Eye, Target) + 100.0);
            if (!World->LineTraceSingleByChannel(Hit, Eye, End, BuildGunTraceChannel(), Query)) return false;
            Hologram->SetHologramLocationAndRotation(Hit);
            return true;
        };
        auto Respond = [](TSharedRef<FJsonObject> Body, const FHitResult& Hit, const TCHAR* Field)
        {
            const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetObjectField(TEXT("location"), SplineVectorJson(Hit.Location));
            Out->SetStringField(TEXT("actorId"), Hit.GetActor() ? Hit.GetActor()->GetName() : TEXT(""));
            Out->SetStringField(TEXT("actorClassName"), Hit.GetActor() ? Hit.GetActor()->GetClass()->GetName() : TEXT(""));
            Body->SetObjectField(Field, Out);
        };
        const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
        Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetBoolField(TEXT("viaHologram"), true); Body->SetStringField(TEXT("recipePath"), RecipePath);
        Body->SetStringField(TEXT("hologramClass"), Hologram->GetClass()->GetName());
        CallInt(TEXT("SetLanes"), Lanes);
        CallBool(TEXT("SetStraightMode"), Straight);
        FHitResult StartHit, EndHit;
        if (!Aim(Aims0.Eye, Aims0.Target, StartHit)) { Hologram->Destroy(); OnComplete(ErrorResponse(TEXT("surface_not_found"), TEXT("The start aim hit nothing on the build-gun channel."), EHttpServerResponseCodes::Conflict)); return; }
        Respond(Body, StartHit, TEXT("startHit"));
        const FVector Origin = Hologram->GetActorLocation();
        Hologram->DoMultiStepPlacement(false);
        Hologram->DoMultiStepPlacement(true);
        if (!Aim(Aims1.Eye, Aims1.Target, EndHit)) { Hologram->Destroy(); OnComplete(ErrorResponse(TEXT("surface_not_found"), TEXT("The end aim hit nothing on the build-gun channel."), EHttpServerResponseCodes::Conflict)); return; }
        Respond(Body, EndHit, TEXT("endHit"));
        // Headless, ValidatePlacementAndCost stops at the initializing sentinel (live: only FGCDInitializing and
        // FGCDInvalidAimLocation came back, never the road's own checks), so run the placement checks directly. Those
        // two remain cursor/initialization artifacts of a hologram driven without the build gun, as /v1/place treats
        // them; every geometric disqualifier (length, grade, radius, clearance) stays authoritative.
        Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));
        UFunction* Checks = Hologram->FindFunction(FName(TEXT("RunPlacementChecks")));
        if (!Checks) { Hologram->Destroy(); OnComplete(ErrorResponse(TEXT("spline_road_unavailable"), TEXT("The spline road hologram cannot run its placement checks (older MKI Roads)."), EHttpServerResponseCodes::Conflict)); return; }
        Hologram->ProcessEvent(Checks, nullptr);
        TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers; Hologram->GetConstructDisqualifiers(Disqualifiers);
        Disqualifiers.RemoveAll([](const TSubclassOf<UFGConstructDisqualifier>& Item) { return Item && (Item->IsChildOf(UFGCDInitializing::StaticClass()) || Item->IsChildOf(UFGCDInvalidAimLocation::StaticClass())); });
        Hologram->ResetConstructDisqualifiers();
        for (const TSubclassOf<UFGConstructDisqualifier>& Item : Disqualifiers) Hologram->AddConstructDisqualifier(Item);
        Hologram->SetPlacementMaterialState(Disqualifiers.Num() == 0 ? EHologramMaterialState::HMS_OK : EHologramMaterialState::HMS_ERROR);
        TArray<TSharedPtr<FJsonValue>> Names; for (const TSubclassOf<UFGConstructDisqualifier>& Item : Disqualifiers) Names.Add(MakeShared<FJsonValueString>(Item ? Item->GetName() : TEXT("null")));
        Body->SetArrayField(TEXT("disqualifiers"), Names);
        const double LengthCm = ReadFloat(TEXT("GetPlannedLengthCm"));
        const int32 Equivalents = SplineRoadFoundationEquivalents(LengthCm, Lanes);
        Body->SetObjectField(TEXT("origin"), SplineVectorJson(Origin));
        Body->SetNumberField(TEXT("plannedLengthCm"), LengthCm); Body->SetNumberField(TEXT("lanes"), Lanes); Body->SetBoolField(TEXT("straight"), Straight);
        Body->SetNumberField(TEXT("foundationEquivalents"), Equivalents);
        if (const AFGSplineHologram* Spline = Cast<AFGSplineHologram>(Hologram)) Body->SetNumberField(TEXT("buildStep"), static_cast<int32>(const_cast<AFGSplineHologram*>(Spline)->GetCurrentBuildStep()));
        const bool CanConstruct = Disqualifiers.Num() == 0;
        Body->SetBoolField(TEXT("canConstruct"), CanConstruct);
        if (!Confirm) { Hologram->Destroy(); OnComplete(JsonResponse(Body)); return; }
        if (!CanConstruct || !Hologram->DoMultiStepPlacement(false)) { Hologram->Destroy(); OnComplete(ErrorResponse(TEXT("hologram_placement_failed"), FString::Printf(TEXT("The spline road hologram rejected the placement (%d disqualifiers); nothing was charged."), Disqualifiers.Num()), EHttpServerResponseCodes::Conflict)); return; }
        FString Missing;
        if (!ConsumeRecipeCost(World, RecipeClass, Equivalents, Missing)) { Hologram->Destroy(); OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), Missing, EHttpServerResponseCodes::Conflict)); return; }
        TArray<AActor*> Children;
        AActor* Constructed = Hologram->Construct(Children, FNetConstructionID());
        Hologram->Destroy();
        AFGBuildable* Road = Cast<AFGBuildable>(Constructed);
        int32 DeckSlices = 0;
        if (Road && Road->IsA(RoadClass)) if (FIntProperty* SlicesProperty = FindFProperty<FIntProperty>(Road->GetClass(), TEXT("mDeckSliceCount"))) DeckSlices = SlicesProperty->GetPropertyValue_InContainer(Road);
        if (!Road || !Road->IsA(RoadClass) || DeckSlices < 2)
        {
            if (IsValid(Constructed)) Constructed->Destroy();
            for (AActor* Child : Children) if (IsValid(Child)) Child->Destroy();
            RefundRecipeCost(World, RecipeClass, Equivalents);
            OnComplete(ErrorResponse(TEXT("placement_verification_failed"), TEXT("The hologram did not construct a spline road with a deck; build cost was refunded."), EHttpServerResponseCodes::Conflict)); return;
        }
        PlacementReceipts.Add(RequestId, Road);
        const IFGSplineBuildableInterface* Built = Cast<IFGSplineBuildableInterface>(Road);
        Body->SetBoolField(TEXT("placed"), true); Body->SetBoolField(TEXT("costConsumed"), true); Body->SetBoolField(TEXT("verified"), true);
        Body->SetStringField(TEXT("buildableId"), Road->GetName()); Body->SetStringField(TEXT("className"), Road->GetClass()->GetName());
        Body->SetNumberField(TEXT("builtLengthCm"), Built && Built->GetSplineComponent() ? Built->GetSplineComponent()->GetSplineLength() : 0.0);
        Body->SetNumberField(TEXT("deckSliceCount"), DeckSlices);
        OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Created));
    }, 0.1f, false);
    return true;
}
/**
 * Drives the player's own build gun, as a player at the keyboard would: equips it with a recipe, then runs steps in
 * order, a short beat apart so the build gun ticks between them: aim (the player's position and view), scroll (the
 * rotate input), press and release (primary fire), wait. After every step it reports the build gun's hologram (class,
 * location, spline build step, disqualifiers, canConstruct); at the end, every buildable or vehicle actor that appeared
 * near the player, with its location. Nothing is charged or refunded here: the build gun does that as in play.
 */
bool FMKIBridgeModule::HandlePlaceWithBuildGun(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString RecipePath, RequestId, BlueprintName; bool Confirm = false;
    Json->TryGetStringField(TEXT("recipePath"), RecipePath); RecipePath.TrimStartAndEndInline();
    Json->TryGetStringField(TEXT("requestId"), RequestId); Json->TryGetBoolField(TEXT("confirm"), Confirm);
    // #365 (feature blueprint_place): a saved blueprint of this session, stamped through the build gun with the blueprint
    // recipe every blueprint shares, as choosing it in the build menu does.
    Json->TryGetStringField(TEXT("blueprintName"), BlueprintName); BlueprintName.TrimStartAndEndInline();
    if (!BlueprintName.IsEmpty())
    {
        const UFGBlueprintSettings* BlueprintSettings = UFGBlueprintSettings::Get();
        UClass* BlueprintRecipe = BlueprintSettings ? BlueprintSettings->mBlueprintRecipeClass.LoadSynchronous() : nullptr;
        if (!BlueprintRecipe) { OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("The blueprint recipe is unavailable."), EHttpServerResponseCodes::ServerError)); return true; }
        RecipePath = BlueprintRecipe->GetPathName();
    }
    FString PathCode, PathMessage;
    if (BlueprintName.IsEmpty() && !IsPlaceableAssetPath(RecipePath, PathCode, PathMessage)) { OnComplete(ErrorResponse(PathCode, PathMessage, PathCode == TEXT("companion_unavailable") ? EHttpServerResponseCodes::Conflict : EHttpServerResponseCodes::BadRequest)); return true; }
    struct FStep { FString Op; FVector Position = FVector::ZeroVector; double Yaw = 0.0, Pitch = 0.0, Seconds = 0.0; int32 Delta = 0; bool bEnabled = false; };
    TArray<FStep> Steps;
    const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
    if (!Json->TryGetArrayField(TEXT("steps"), StepValues) || !StepValues || StepValues->Num() == 0 || StepValues->Num() > 64) { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("steps must hold 1-64 {op} objects."), EHttpServerResponseCodes::BadRequest)); return true; }
    for (const TSharedPtr<FJsonValue>& Value : *StepValues)
    {
        const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
        FStep Step; double Delta = 0.0;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("op"), Step.Op)) { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("Every step needs an op."), EHttpServerResponseCodes::BadRequest)); return true; }
        if (Step.Op == TEXT("aim") && (!ReadSplineVector(Object, TEXT("position"), Step.Position) || !Object->TryGetNumberField(TEXT("yaw"), Step.Yaw) || !Object->TryGetNumberField(TEXT("pitch"), Step.Pitch) || !FMath::IsFinite(Step.Yaw) || !FMath::IsFinite(Step.Pitch)))
        { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("aim needs position {x, y, z}, yaw, and pitch."), EHttpServerResponseCodes::BadRequest)); return true; }
        if (Step.Op == TEXT("wait") && (!Object->TryGetNumberField(TEXT("seconds"), Step.Seconds) || Step.Seconds < 0.0 || Step.Seconds > 10.0)) { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("wait needs 0-10 seconds."), EHttpServerResponseCodes::BadRequest)); return true; }
        if (Step.Op == TEXT("scroll")) { Object->TryGetNumberField(TEXT("delta"), Delta); Step.Delta = FMath::Clamp(FMath::RoundToInt32(Delta), -24, 24); }
        if (Step.Op == TEXT("snap") && !Object->TryGetBoolField(TEXT("enabled"), Step.bEnabled)) { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("snap needs enabled (true or false)."), EHttpServerResponseCodes::BadRequest)); return true; }
        // The hologram lock (H) and one nudge press (#377): x forward/back, y right/left, z up/down, each -1, 0, or 1.
        if (Step.Op == TEXT("lock") && !Object->TryGetBoolField(TEXT("enabled"), Step.bEnabled)) { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("lock needs enabled (true or false)."), EHttpServerResponseCodes::BadRequest)); return true; }
        if (Step.Op == TEXT("nudge"))
        {
            double X = 0.0, Y = 0.0, Z = 0.0; Object->TryGetNumberField(TEXT("x"), X); Object->TryGetNumberField(TEXT("y"), Y); Object->TryGetNumberField(TEXT("z"), Z);
            Step.Position = FVector(FMath::Clamp(FMath::RoundToDouble(X), -1.0, 1.0), FMath::Clamp(FMath::RoundToDouble(Y), -1.0, 1.0), FMath::Clamp(FMath::RoundToDouble(Z), -1.0, 1.0));
            if (Step.Position.IsZero()) { OnComplete(ErrorResponse(TEXT("invalid_steps"), TEXT("nudge needs x, y, or z of 1 or -1."), EHttpServerResponseCodes::BadRequest)); return true; }
        }
        if (Step.Op != TEXT("aim") && Step.Op != TEXT("press") && Step.Op != TEXT("release") && Step.Op != TEXT("wait") && Step.Op != TEXT("scroll") && Step.Op != TEXT("snap") && Step.Op != TEXT("lock") && Step.Op != TEXT("nudge") && Step.Op != TEXT("report"))
        { OnComplete(ErrorResponse(TEXT("invalid_steps"), FString::Printf(TEXT("Unknown op %s; use aim, scroll, snap, lock, nudge, press, release, wait, or report."), *Step.Op), EHttpServerResponseCodes::BadRequest)); return true; }
        Steps.Add(Step);
    }
    if (Confirm && RequestId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("A confirmed build-gun run requires requestId."), EHttpServerResponseCodes::BadRequest)); return true; }
    // Actors to read back after the run (their customization, and a spline road's markings): 0-16 actor names.
    TArray<FString> Inspect;
    if (const TArray<TSharedPtr<FJsonValue>>* InspectValues = nullptr; Json->TryGetArrayField(TEXT("inspect"), InspectValues) && InspectValues)
    {
        if (InspectValues->Num() > 16) { OnComplete(ErrorResponse(TEXT("invalid_inspect"), TEXT("inspect holds at most 16 actor names."), EHttpServerResponseCodes::BadRequest)); return true; }
        for (const TSharedPtr<FJsonValue>& Value : *InspectValues) { FString Name; if (!Value.IsValid() || !Value->TryGetString(Name) || Name.IsEmpty()) { OnComplete(ErrorResponse(TEXT("invalid_inspect"), TEXT("inspect holds actor names."), EHttpServerResponseCodes::BadRequest)); return true; } Inspect.Add(Name); }
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APlayerController* Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
    if (!World || !Controller || !Player || !Player->GetBuildGun()) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World, player, or build gun is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    for (const FStep& Step : Steps)
        if (Step.Op == TEXT("aim") && FVector::Distance(Step.Position, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm()) { OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Every aim position must be within the configured safety radius of the player."), EHttpServerResponseCodes::Forbidden)); return true; }
    UClass* RecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, *RecipePath);
    if (!RecipeClass) { OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("Recipe asset unavailable."), EHttpServerResponseCodes::ServerError)); return true; }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetStringField(TEXT("recipePath"), RecipePath); Body->SetNumberField(TEXT("stepCount"), Steps.Num());
    UFGBlueprintDescriptor* Blueprint = nullptr;
    if (!BlueprintName.IsEmpty())
    {
        AFGBlueprintSubsystem* Blueprints = AFGBlueprintSubsystem::Get(World);
        Blueprint = Blueprints ? Blueprints->GetBlueprintDescriptorByNameString(BlueprintName) : nullptr;
        if (!Blueprint)
        {
            TArray<UFGBlueprintDescriptor*> All; if (Blueprints) AFGBlueprintSubsystem::GetBlueprintDescriptors(All, World);
            TArray<FString> Names; for (const UFGBlueprintDescriptor* Item : All) if (Item) Names.Add(Item->GetBlueprintNameAsString());
            OnComplete(ErrorResponse(TEXT("blueprint_not_found"), FString::Printf(TEXT("No blueprint named %s in this session; it has: %s"), *BlueprintName, Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none")), EHttpServerResponseCodes::NotFound)); return true;
        }
        if (!Blueprint->GetRecipeRequirementsAreMet()) { OnComplete(ErrorResponse(TEXT("blueprint_locked"), TEXT("This blueprint holds buildables whose recipes this save has not unlocked."), EHttpServerResponseCodes::Conflict)); return true; }
        const FIntVector Dimensions = Blueprint->GetDimensionsOnInstance();
        const TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
        Info->SetStringField(TEXT("name"), Blueprint->GetBlueprintNameAsString());
        Info->SetNumberField(TEXT("dimensionX"), Dimensions.X); Info->SetNumberField(TEXT("dimensionY"), Dimensions.Y); Info->SetNumberField(TEXT("dimensionZ"), Dimensions.Z);
        Body->SetObjectField(TEXT("blueprint"), Info);
    }
    // A customization recipe (a swatch or pattern) runs the build gun's paint state, as choosing it in the customizer does.
    const bool bCustomize = RecipeClass->IsChildOf(UFGCustomizationRecipe::StaticClass());
    Body->SetStringField(TEXT("state"), bCustomize ? TEXT("paint") : TEXT("build"));
    if (bCustomize)
    {
        TArray<TSubclassOf<UFGRecipe>> Recipes; TArray<TSubclassOf<UFGCustomizationRecipe>> CustomizationRecipes;
        Player->GetBuildGun()->GetAvailableRecipes(Recipes, CustomizationRecipes);
        Body->SetBoolField(TEXT("customizationRecipeAvailable"), CustomizationRecipes.Contains(TSubclassOf<UFGCustomizationRecipe>(RecipeClass)));
    }
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }

    // Actors near the player before the run, so the result can name what the build gun created.
    const TSharedRef<TSet<FString>> Before = MakeShared<TSet<FString>>();
    for (TActorIterator<AActor> It(World); It; ++It)
        if (It->IsA(AFGBuildable::StaticClass()) || It->IsA(AFGVehicle::StaticClass())) Before->Add(It->GetName());
    // Whether the player held the build gun before the run: a run that equipped it puts it away again (#205: left out, it
    // stayed in the player's hands, and the first-person view showed parts of the body).
    const bool bHadBuildGun = Player->IsBuildGunEquipped();
    Player->EquipBuildGunAndGoToMenuState();
    if (bCustomize) Player->GetBuildGun()->GotoPaintState(TSubclassOf<UFGCustomizationRecipe>(RecipeClass));
    else
    {
        if (Blueprint) if (UFGBuildGunStateBuild* BuildState = Cast<UFGBuildGunStateBuild>(Player->GetBuildGun()->GetBuildGunStateFor(EBuildGunState::BGS_BUILD))) BuildState->SetActiveBlueprintDescriptor(Blueprint);
        Player->GetBuildGun()->GotoBuildState(RecipeClass);
    }
    const TSharedRef<TArray<TSharedPtr<FJsonValue>>> Log = MakeShared<TArray<TSharedPtr<FJsonValue>>>();
    const TSharedRef<int32> Next = MakeShared<int32>(0);
    const TSharedRef<TFunction<void()>> Run = MakeShared<TFunction<void()>>();
    *Run = [this, WeakWorld = TWeakObjectPtr<UWorld>(World), WeakController = TWeakObjectPtr<APlayerController>(Controller), WeakPlayer = TWeakObjectPtr<AFGCharacterPlayer>(Player), Ticket = ActiveTicket, bHadBuildGun, Steps, Inspect, Before, Log, Next, Body, RequestId, OnComplete, WeakRun = TWeakPtr<TFunction<void()>>(Run)]()
    {
        UWorld* World = WeakWorld.Get(); APlayerController* Controller = WeakController.Get(); AFGCharacterPlayer* Player = WeakPlayer.Get();
        AFGBuildGun* BuildGun = Player ? Player->GetBuildGun() : nullptr;
        if (!World || !Controller || !Player || !BuildGun || (Ticket && Ticket->bAbandoned))
        {
            if (Player && BuildGun && !bHadBuildGun) { BuildGun->GotoNoneState(); Player->UnequipBuildGun(); }
            OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"), TEXT("The world, player, or build gun disappeared, or the request was abandoned, during the run."), EHttpServerResponseCodes::Conflict));
            return;
        }
        auto Report = [BuildGun](const FString& Op) -> TSharedRef<FJsonObject>
        {
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("op"), Op);
            const UFGBuildGunStateBuild* Build = Cast<UFGBuildGunStateBuild>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD));
            AFGHologram* Hologram = Build ? Build->GetHologram() : nullptr;
            Entry->SetBoolField(TEXT("buildState"), BuildGun->IsInState(EBuildGunState::BGS_BUILD));
            if (BuildGun->IsInState(EBuildGunState::BGS_PAINT))
            {
                // The paint state's own judgement of the aimed buildable, as the customizer shows it.
                if (UFGBuildGunStatePaint* Paint = Cast<UFGBuildGunStatePaint>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_PAINT)))
                {
                    Entry->SetBoolField(TEXT("paintState"), true);
                    Entry->SetBoolField(TEXT("patternHitValid"), Paint->IsPatternHitValid());
                    Entry->SetBoolField(TEXT("swatchHitValid"), Paint->IsSwatchHitValid());
                    if (UClass* Pattern = Paint->GetActivePatternDesc().Get()) Entry->SetStringField(TEXT("activePatternPath"), Pattern->GetPathName());
                }
                return Entry;
            }
            if (!Hologram) { Entry->SetBoolField(TEXT("hologram"), false); return Entry; }
            Entry->SetStringField(TEXT("hologramClass"), Hologram->GetClass()->GetName());
            Entry->SetObjectField(TEXT("location"), SplineVectorJson(Hologram->GetActorLocation()));
            Entry->SetNumberField(TEXT("yaw"), Hologram->GetActorRotation().Yaw);
            Entry->SetBoolField(TEXT("locked"), Hologram->IsHologramLocked());
            Entry->SetObjectField(TEXT("nudgeOffset"), SplineVectorJson(Hologram->GetHologramNudgeOffset()));
            if (AFGSplineHologram* Spline = Cast<AFGSplineHologram>(Hologram)) Entry->SetNumberField(TEXT("splineBuildStep"), static_cast<int32>(Spline->GetCurrentBuildStep()));
            // A hologram that previews a heading of its own (the spline road's) reports it, so rotation is visible here.
            if (UFunction* Heading = Hologram->FindFunction(FName(TEXT("GetPreviewHeadingYaw")))) { struct { float ReturnValue = 0.f; } Params; Hologram->ProcessEvent(Heading, &Params); Entry->SetNumberField(TEXT("previewYaw"), Params.ReturnValue); }
            // A spline hologram's planned points (the vehicle path's and the spline road's mSplineData, by reflection), in
            // the hologram's space: the vehicle path along road lanes (#192) is checked against them.
            if (const FArrayProperty* SplineProperty = FindFProperty<FArrayProperty>(Hologram->GetClass(), TEXT("mSplineData")))
            {
                const FStructProperty* Inner = CastField<FStructProperty>(SplineProperty->Inner);
                if (Inner && Inner->Struct == FSplinePointData::StaticStruct())
                {
                    const TArray<FSplinePointData>& Points = *SplineProperty->ContainerPtrToValuePtr<TArray<FSplinePointData>>(Hologram);
                    TArray<TSharedPtr<FJsonValue>> PointsJson;
                    for (const FSplinePointData& Point : Points) { const FVector World = Hologram->GetActorTransform().TransformPosition(Point.Location); PointsJson.Add(MakeShared<FJsonValueObject>(SplineVectorJson(World))); }
                    Entry->SetArrayField(TEXT("splinePoints"), PointsJson);
                }
            }
            TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers; Hologram->GetConstructDisqualifiers(Disqualifiers);
            TArray<TSharedPtr<FJsonValue>> Names; for (const TSubclassOf<UFGConstructDisqualifier>& Item : Disqualifiers) Names.Add(MakeShared<FJsonValueString>(Item ? Item->GetName() : TEXT("null")));
            Entry->SetArrayField(TEXT("disqualifiers"), Names);
            Entry->SetBoolField(TEXT("canConstruct"), Hologram->CanConstruct());
            // The colour state the build gun set (0 ok, 1 warning, 2 error), read from the replicated property.
            if (const FByteProperty* State = CastField<FByteProperty>(AFGHologram::StaticClass()->FindPropertyByName(TEXT("mPlacementMaterialState")))) Entry->SetNumberField(TEXT("materialState"), State->GetPropertyValue_InContainer(Hologram));
            else if (const FEnumProperty* EnumState = CastField<FEnumProperty>(AFGHologram::StaticClass()->FindPropertyByName(TEXT("mPlacementMaterialState")))) Entry->SetNumberField(TEXT("materialState"), static_cast<double>(EnumState->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(EnumState->ContainerPtrToValuePtr<void>(Hologram))));
            // A hologram that renders a preview of its own (the spline road's) reports the stencil it draws with, and its clearance.
            if (UFunction* Stencil = Hologram->FindFunction(FName(TEXT("GetPreviewStencilValue")))) { struct { int32 ReturnValue = -1; } Params; Hologram->ProcessEvent(Stencil, &Params); Entry->SetNumberField(TEXT("previewStencil"), Params.ReturnValue); }
            if (UFunction* Boxes = Hologram->FindFunction(FName(TEXT("GetClearanceBoxCount")))) { struct { int32 ReturnValue = 0; } Params; Hologram->ProcessEvent(Boxes, &Params); Entry->SetNumberField(TEXT("clearanceBoxes"), Params.ReturnValue); }
            if (UFunction* Deck = Hologram->FindFunction(FName(TEXT("IsPreviewShowingDeck")))) { struct { bool ReturnValue = false; } Params; Hologram->ProcessEvent(Deck, &Params); Entry->SetBoolField(TEXT("previewShowsDeck"), Params.ReturnValue); }
            if (UFunction* Straight = Hologram->FindFunction(FName(TEXT("IsStraightMode")))) { struct { bool ReturnValue = false; } Params; Hologram->ProcessEvent(Straight, &Params); Entry->SetBoolField(TEXT("straight"), Params.ReturnValue); }
            if (UFunction* Length = Hologram->FindFunction(FName(TEXT("GetPlannedLengthCm")))) { struct { float ReturnValue = 0.f; } Params; Hologram->ProcessEvent(Length, &Params); Entry->SetNumberField(TEXT("plannedLengthCm"), Params.ReturnValue); }
            return Entry;
        };
        double Delay = 0.4;
        if (Next.Get() < Steps.Num())
        {
            const FStep& Step = Steps[Next.Get()];
            if (Step.Op == TEXT("aim")) { Player->SetActorLocation(Step.Position, false, nullptr, ETeleportType::TeleportPhysics); Controller->SetControlRotation(FRotator(Step.Pitch, Step.Yaw, 0.0)); Delay = 0.6; }
            else if (Step.Op == TEXT("press")) BuildGun->OnPrimaryFirePressed();
            else if (Step.Op == TEXT("release")) BuildGun->OnPrimaryFireReleased();
            else if (Step.Op == TEXT("scroll")) BuildGun->Server_Scroll(Step.Delta);
            // The build gun's own snap-mode handler, which holding Ctrl drives.
            else if (Step.Op == TEXT("snap")) { if (UFGBuildGunStateBuild* Build = Cast<UFGBuildGunStateBuild>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD))) Build->ChangeGuidelineSnapMode(Step.bEnabled); }
            // The build gun's own lock toggle and nudge press, which H and the nudge keys drive (#377).
            else if (Step.Op == TEXT("lock"))
            {
                UFGBuildGunStateBuild* Build = Cast<UFGBuildGunStateBuild>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD));
                const AFGHologram* Hologram = Build ? Build->GetHologram() : nullptr;
                if (Build && Hologram && Hologram->IsHologramLocked() != Step.bEnabled) Build->ToggleHologramPositionLock();
            }
            else if (Step.Op == TEXT("nudge"))
            {
                UFGBuildGunStateBuild* Build = Cast<UFGBuildGunStateBuild>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD));
                if (UFunction* Nudge = Build ? Build->FindFunction(FName(TEXT("NudgeOnce"))) : nullptr) { struct { FVector NudgeVector; } Params{ Step.Position }; Build->ProcessEvent(Nudge, &Params); }
            }
            else if (Step.Op == TEXT("wait")) Delay = FMath::Max(0.05, Step.Seconds);
            Log->Add(MakeShared<FJsonValueObject>(Report(Step.Op)));
            ++Next.Get();
            // Each pending timer holds the runner (RunHolder); the runner holds itself only weakly, so it ends with the last step.
            if (const TSharedPtr<TFunction<void()>> RunHolder = WeakRun.Pin()) { FTimerHandle Timer; World->GetTimerManager().SetTimer(Timer, [RunHolder, WeakWorld, Ticket]() { if (WeakWorld.IsValid()) (*RunHolder)(); }, static_cast<float>(Delay), false); }
            return;
        }
        Body->SetArrayField(TEXT("steps"), *Log);
        TArray<TSharedPtr<FJsonValue>> Created;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            // Temporary actors of lightweight instances come and go with the aim; they are not constructions.
            if (!(It->IsA(AFGBuildable::StaticClass()) || It->IsA(AFGVehicle::StaticClass())) || Before->Contains(It->GetName())) continue;
            if (const AFGBuildable* Buildable = Cast<AFGBuildable>(*It)) if (IsLightweightSubsystemActor(Buildable)) continue;
            const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("id"), It->GetName()); Item->SetStringField(TEXT("className"), It->GetClass()->GetName());
            Item->SetObjectField(TEXT("location"), SplineVectorJson(It->GetActorLocation()));
            Item->SetBoolField(TEXT("vehicle"), It->IsA(AFGVehicle::StaticClass()));
            Created.Add(MakeShared<FJsonValueObject>(Item));
        }
        Body->SetArrayField(TEXT("created"), Created);
        // The named actors' customization and, for a spline road, its applied markings (the road's own UFUNCTIONs).
        TArray<TSharedPtr<FJsonValue>> Inspected;
        for (const FString& Name : Inspect)
        {
            const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("id"), Name);
            AFGBuildable* Found = nullptr;
            for (TActorIterator<AFGBuildable> It(World); It; ++It) if (It->GetName().Equals(Name, ESearchCase::CaseSensitive)) { Found = *It; break; }
            Item->SetBoolField(TEXT("found"), Found != nullptr);
            if (Found)
            {
                Item->SetObjectField(TEXT("customization"), FactoryCustomizationJson(Found->GetCustomizationData_Native()));
                auto ReadInt = [Found, &Item](const TCHAR* Function, const TCHAR* Field) { if (UFunction* F = Found->FindFunction(FName(Function))) { struct { int32 ReturnValue = 0; } Params; Found->ProcessEvent(F, &Params); Item->SetNumberField(Field, Params.ReturnValue); } };
                ReadInt(TEXT("GetLanes"), TEXT("lanes"));
                ReadInt(TEXT("GetMarkingLineCount"), TEXT("markingLines"));
                ReadInt(TEXT("GetMarkingVertexCount"), TEXT("markingVertices"));
                if (UFunction* F = Found->FindFunction(FName(TEXT("GetDeckPatternId")))) { struct { float ReturnValue = 0.f; } Params; Found->ProcessEvent(F, &Params); Item->SetNumberField(TEXT("deckPatternId"), Params.ReturnValue); }
                ReadInt(TEXT("GetCollisionSlabCount"), TEXT("collisionSlabs"));
                // Collision as the world sees it (#185): the actor switch and each primitive's mode, profile, and physics state.
                Item->SetBoolField(TEXT("actorEnableCollision"), Found->GetActorEnableCollision());
                TArray<TSharedPtr<FJsonValue>> Primitives;
                for (UPrimitiveComponent* Primitive : TInlineComponentArray<UPrimitiveComponent*>(Found))
                {
                    const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("name"), Primitive->GetName()); Entry->SetStringField(TEXT("class"), Primitive->GetClass()->GetName());
                    Entry->SetNumberField(TEXT("collisionEnabled"), static_cast<int32>(Primitive->GetCollisionEnabled()));
                    Entry->SetStringField(TEXT("profile"), Primitive->GetCollisionProfileName().ToString());
                    Entry->SetBoolField(TEXT("registered"), Primitive->IsRegistered()); Entry->SetBoolField(TEXT("physicsState"), Primitive->IsPhysicsStateCreated());
                    if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Primitive)) Entry->SetNumberField(TEXT("instances"), Instanced->GetInstanceCount());
                    Primitives.Add(MakeShared<FJsonValueObject>(Entry));
                }
                Item->SetArrayField(TEXT("primitives"), Primitives);
            }
            Inspected.Add(MakeShared<FJsonValueObject>(Item));
        }
        Body->SetArrayField(TEXT("inspected"), Inspected);
        Body->SetBoolField(TEXT("ran"), true);
        BuildGun->GotoNoneState();
        if (!bHadBuildGun) Player->UnequipBuildGun();
        Body->SetBoolField(TEXT("buildGunPutAway"), !bHadBuildGun && !Player->IsBuildGunEquipped());
        // #204: whether the run found the build gun out, so a batch can check it ends as it was.
        Body->SetBoolField(TEXT("buildGunWasEquipped"), bHadBuildGun);
        OnComplete(JsonResponse(Body));
    };
    FTimerHandle Start;
    World->GetTimerManager().SetTimer(Start, [RunHolder = Run, WeakWorld = TWeakObjectPtr<UWorld>(World), Ticket = ActiveTicket]() { if (WeakWorld.IsValid()) (*RunHolder)(); }, 0.5f, false);
    return true;
}
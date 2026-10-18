#include "BridgeInternal.h"

// Connection routes: structure connections, spline and belt holograms, and power lines.
// Split from the single MKIBridge.cpp without behavior changes (#44).

bool FMKIBridgeModule::HandleConnectStructures(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString Kind;
    if (!Json->TryGetStringField(TEXT("kind"), Kind))
    {
        OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("Connection kind must be belt or power."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    if (Kind.Equals(TEXT("belt"), ESearchCase::IgnoreCase)) return HandleConnectBeltHologram(Request, OnComplete);
    if (Kind.Equals(TEXT("power"), ESearchCase::IgnoreCase)) return HandleConnectPower(Request, OnComplete);
    if (Kind.Equals(TEXT("pipe"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("hypertube"), ESearchCase::IgnoreCase) || Kind.Equals(TEXT("rail"), ESearchCase::IgnoreCase)) return HandleConnectSplineHologram(Request, OnComplete, Kind.ToLower());
    OnComplete(ErrorResponse(TEXT("unsupported_connection"), TEXT("Connection kind must be belt, power, pipe, hypertube, or rail."), EHttpServerResponseCodes::BadRequest));
    return true;
}

bool FMKIBridgeModule::HandleConnectSplineHologram(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, const FString& Kind)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString Text;
    if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); Text = FString(Converted.Length(), Converted.Get()); }
    TSharedPtr<FJsonObject> Json; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be JSON."), EHttpServerResponseCodes::BadRequest)); return true; }

    FString SourceId, TargetId, SourcePort, TargetPort, RequestId, RecipePath, BuildMode = TEXT("straight"); bool Confirm = false;
    Json->TryGetStringField(TEXT("sourceId"), SourceId); Json->TryGetStringField(TEXT("targetId"), TargetId);
    Json->TryGetStringField(TEXT("sourcePort"), SourcePort); Json->TryGetStringField(TEXT("targetPort"), TargetPort);
    Json->TryGetStringField(TEXT("requestId"), RequestId); Json->TryGetStringField(TEXT("recipePath"), RecipePath);
    Json->TryGetStringField(TEXT("buildMode"), BuildMode); Json->TryGetBoolField(TEXT("confirm"), Confirm);
    // A rail may end at a free point instead of another rail end (#18, feature rail_to_point): a plain track end, from
    // which further tracks branch into a switch (a station's own track takes no switch at its end).
    bool bToPoint = false; FVector TargetPoint = FVector::ZeroVector;
    { const TSharedPtr<FJsonObject>* Point = nullptr; if (Kind == TEXT("rail") && TargetId.IsEmpty() && Json->TryGetObjectField(TEXT("targetPoint"), Point) && Point && (*Point)->HasTypedField<EJson::Number>(TEXT("x")) && (*Point)->HasTypedField<EJson::Number>(TEXT("y")) && (*Point)->HasTypedField<EJson::Number>(TEXT("z")))
      { TargetPoint = FVector((*Point)->GetNumberField(TEXT("x")), (*Point)->GetNumberField(TEXT("y")), (*Point)->GetNumberField(TEXT("z"))); bToPoint = !TargetPoint.ContainsNaN(); } }
    if (SourceId.IsEmpty() || (TargetId.IsEmpty() && !bToPoint) || RecipePath.IsEmpty() || !RecipePath.StartsWith(TEXT("/Game/FactoryGame/")) ||
        (Confirm && RequestId.IsEmpty()) || (!BuildMode.Equals(TEXT("straight"), ESearchCase::IgnoreCase) && !BuildMode.Equals(TEXT("curve"), ESearchCase::IgnoreCase)))
    { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("sourceId, targetId, a FactoryGame recipePath, buildMode, and confirmed requestId are required."), EHttpServerResponseCodes::BadRequest)); return true; }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr; AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    if (!World || !Subsystem) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    AFGBuildable* Source = FindBuildableById(World, SourceId); AFGBuildable* Target = bToPoint ? nullptr : FindBuildableById(World, TargetId);
    if (!Source || (!Target && !bToPoint) || Source == Target) { OnComplete(ErrorResponse(TEXT("endpoint_unavailable"), TEXT("Two distinct exact buildables are required."), EHttpServerResponseCodes::Conflict)); return true; }

    UFGPipeConnectionComponentBase* StartPipe = nullptr; UFGPipeConnectionComponentBase* EndPipe = nullptr;
    UFGRailroadTrackConnectionComponent* StartRail = nullptr; UFGRailroadTrackConnectionComponent* EndRail = nullptr;
    auto PortMatches = [](const UActorComponent* Component, const FString& Requested) { return Component && (Requested.IsEmpty() || Component->GetName().Equals(Requested, ESearchCase::IgnoreCase)); };
    if (Kind == TEXT("pipe"))
    {
        TInlineComponentArray<UFGPipeConnectionComponent*> StartConnections(Source); for (UFGPipeConnectionComponent* C : StartConnections) if (C && !C->IsConnected() && PortMatches(C, SourcePort)) { StartPipe = C; break; }
        TInlineComponentArray<UFGPipeConnectionComponent*> EndConnections(Target); for (UFGPipeConnectionComponent* C : EndConnections) if (C && !C->IsConnected() && PortMatches(C, TargetPort)) { EndPipe = C; break; }
        // A source that is a pipe support with a pipe already resting on it (#308): the new pipe starts from that pipe's
        // free end, so a routed run joins pipe to pipe at every support, as belts do (#111).
        // A target support with a pipe already resting on it (#308) joins the new pipe to that pipe's free end, so a pipe
        // spliced into a run (a junction inserted between two supports) connects on both sides.
        auto PipeEndAt = [World](UFGPipeConnectionComponent* Snap, AFGBuildable*& Owner) -> UFGPipeConnectionComponent*
        {
            const FVector At = Snap->GetConnectorLocation(false);
            for (TActorIterator<AFGBuildablePipeline> It(World); It; ++It)
            {
                AFGBuildablePipeline* Pipeline = *It;
                if (!IsValid(Pipeline) || FVector::DistSquared(Pipeline->GetActorLocation(), At) > FMath::Square(Pipeline->GetLength() + 200.0)) continue;
                TInlineComponentArray<UFGPipeConnectionComponent*> Ends(Pipeline);
                for (UFGPipeConnectionComponent* End : Ends) if (End && !End->IsConnected() && FVector::Distance(End->GetConnectorLocation(false), At) <= 50.0) { Owner = Pipeline; return End; }
            }
            return Snap;
        };
        if (StartPipe && StartPipe->GetPipeConnectionType() == EPipeConnectionType::PCT_SNAP_ONLY) StartPipe = PipeEndAt(Cast<UFGPipeConnectionComponent>(StartPipe), Source);
        if (EndPipe && EndPipe->GetPipeConnectionType() == EPipeConnectionType::PCT_SNAP_ONLY) EndPipe = PipeEndAt(Cast<UFGPipeConnectionComponent>(EndPipe), Target);
    }
    else if (Kind == TEXT("hypertube"))
    {
        TInlineComponentArray<UFGPipeConnectionComponentHyper*> StartConnections(Source); for (UFGPipeConnectionComponentHyper* C : StartConnections) if (C && !C->IsConnected() && PortMatches(C, SourcePort)) { StartPipe = C; break; }
        TInlineComponentArray<UFGPipeConnectionComponentHyper*> EndConnections(Target); for (UFGPipeConnectionComponentHyper* C : EndConnections) if (C && !C->IsConnected() && PortMatches(C, TargetPort)) { EndPipe = C; break; }
    }
    else
    {
        TInlineComponentArray<UFGRailroadTrackConnectionComponent*> StartConnections(Source); for (UFGRailroadTrackConnectionComponent* C : StartConnections) if (C && C->GetConnections().Num() < UFGRailroadTrackConnectionComponent::MAX_NUM_SWITCH_POSITIONS && PortMatches(C, SourcePort)) { StartRail = C; break; }
        if (Target) { TInlineComponentArray<UFGRailroadTrackConnectionComponent*> EndConnections(Target); for (UFGRailroadTrackConnectionComponent* C : EndConnections) if (C && C->GetConnections().Num() < UFGRailroadTrackConnectionComponent::MAX_NUM_SWITCH_POSITIONS && PortMatches(C, TargetPort)) { EndRail = C; break; } }
    }
    // The free end stands on whatever is under the point: the hologram is aimed at a real hit there.
    FHitResult PointHit;
    if (bToPoint)
    {
        FCollisionQueryParams Query(SCENE_QUERY_STAT(BridgeRailPoint), false); if (APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, 0)) Query.AddIgnoredActor(Pawn);
        if (!World->LineTraceSingleByChannel(PointHit, TargetPoint + FVector(0, 0, 1000), TargetPoint - FVector(0, 0, 3000), ECC_Visibility, Query))
        { OnComplete(ErrorResponse(TEXT("no_ground_at_target"), TEXT("Nothing to build on was found under targetPoint (is the region loaded?)."), EHttpServerResponseCodes::Conflict)); return true; }
    }
    if ((!StartPipe || !EndPipe) && (!StartRail || (!EndRail && !bToPoint))) { OnComplete(ErrorResponse(TEXT("endpoint_unavailable"), TEXT("Compatible matching free spline endpoints were not found."), EHttpServerResponseCodes::Conflict)); return true; }
    const FVector Start = StartPipe ? StartPipe->GetConnectorLocation(false) : StartRail->GetConnectorLocation();
    const FVector End = EndPipe ? EndPipe->GetConnectorLocation(false) : EndRail ? EndRail->GetConnectorLocation() : PointHit.ImpactPoint;
    const FVector StartNormal = StartPipe ? StartPipe->GetConnectorNormal() : StartRail->GetConnectorNormal();
    const FVector EndNormal = EndPipe ? EndPipe->GetConnectorNormal() : EndRail ? EndRail->GetConnectorNormal() : FVector(0, 0, 1);
    const double DirectDistance = FVector::Distance(Start, End);
    if (DirectDistance < 50.0 || DirectDistance > 10000.0) { OnComplete(ErrorResponse(TEXT("invalid_spline_length"), TEXT("Spline endpoints must be 0.5-100 meters apart."), EHttpServerResponseCodes::Conflict)); return true; }

    auto Body = MakeShared<FJsonObject>(); Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetStringField(TEXT("kind"), Kind);
    Body->SetNumberField(TEXT("directDistanceCm"), DirectDistance); Body->SetStringField(TEXT("buildMode"), BuildMode.ToLower());
    Body->SetStringField(TEXT("sourcePort"), StartPipe ? StartPipe->GetName() : StartRail->GetName()); Body->SetStringField(TEXT("targetPort"), EndPipe ? EndPipe->GetName() : EndRail ? EndRail->GetName() : FString(TEXT("point")));
    if (bToPoint) Body->SetObjectField(TEXT("targetPoint"), BridgeVectorJson(End));
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }

    UClass* RecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, *RecipePath);
    if (!RecipeClass) { OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("The requested spline recipe is unavailable."), EHttpServerResponseCodes::ServerError)); return true; }
    FString Missing; if (!ConsumeRecipeCost(World, RecipeClass, 1, Missing)) { OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), Missing, EHttpServerResponseCodes::Conflict)); return true; }
    AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)); APlayerController* Controller = UGameplayStatics::GetPlayerController(World, 0);
    AFGSplineHologram* Hologram = Cast<AFGSplineHologram>(AFGHologram::SpawnHologramFromRecipe(RecipeClass, Controller, Start, Player));
    if (!Hologram) { RefundRecipeCost(World, RecipeClass, 1); OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"), TEXT("The requested spline hologram could not be spawned; build cost was refunded."), EHttpServerResponseCodes::ServerError)); return true; }
    Hologram->WaitForGamestateApplyCustomizationData();

    FTimerHandle Timer;
    World->GetTimerManager().SetTimer(Timer, [this, WeakWorld = TWeakObjectPtr<UWorld>(World), WeakRecipe = TWeakObjectPtr<UClass>(RecipeClass), WeakHologram = TWeakObjectPtr<AFGSplineHologram>(Hologram), WeakSource = TWeakObjectPtr<AFGBuildable>(Source), WeakTarget = TWeakObjectPtr<AFGBuildable>(Target),
        WeakStartPipe = TWeakObjectPtr<UFGPipeConnectionComponentBase>(StartPipe), WeakEndPipe = TWeakObjectPtr<UFGPipeConnectionComponentBase>(EndPipe), WeakStartRail = TWeakObjectPtr<UFGRailroadTrackConnectionComponent>(StartRail), WeakEndRail = TWeakObjectPtr<UFGRailroadTrackConnectionComponent>(EndRail),
        IsPipeRoute = StartPipe != nullptr, bToPoint, PointHit, Ticket = ActiveTicket, RecipePath, Start, End, StartNormal, EndNormal, BuildMode, Kind, RequestId, Body, OnComplete]() mutable
    {
        UWorld* World = WeakWorld.Get(); UClass* RecipeClass = WeakRecipe.Get(); AFGSplineHologram* Hologram = WeakHologram.Get(); AFGBuildable* Source = WeakSource.Get(); AFGBuildable* Target = WeakTarget.Get();
        UFGPipeConnectionComponentBase* StartPipe = WeakStartPipe.Get(); UFGPipeConnectionComponentBase* EndPipe = WeakEndPipe.Get(); UFGRailroadTrackConnectionComponent* StartRail = WeakStartRail.Get(); UFGRailroadTrackConnectionComponent* EndRail = WeakEndRail.Get();
        const bool EndpointsAlive = IsPipeRoute ? (StartPipe && EndPipe) : (StartRail && (EndRail || bToPoint));
        if (!World || !RecipeClass || !Hologram || !Source || (!Target && !bToPoint) || !EndpointsAlive || (Ticket && Ticket->bAbandoned))
        {
            if (Hologram) Hologram->Destroy();
            if (World && RecipeClass) RefundRecipeCost(World, RecipeClass, 1);
            OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"), TEXT("The world, an endpoint, or the hologram disappeared before construction; nothing was built."), EHttpServerResponseCodes::Conflict));
            return;
        }
        TArray<TSubclassOf<UFGBuildGunModeDescriptor>> Modes; Hologram->GetSupportedBuildModes(Modes);
        if (Modes.Num() > 0)
        {
            const FString Needle = BuildMode.Equals(TEXT("curve"), ESearchCase::IgnoreCase) ? TEXT("Curve") : TEXT("Straight"); bool Selected = false;
            for (TSubclassOf<UFGBuildGunModeDescriptor> Mode : Modes) { UClass* ModeClass = Mode.Get(); if (ModeClass && ModeClass->IsChildOf(UFGHologramBuildModeDescriptor::StaticClass()) && ModeClass->GetName().Contains(Needle, ESearchCase::IgnoreCase)) { Hologram->SetBuildModeOverride(ModeClass); Selected = true; break; } }
            // Pipes expose an explicit Straight descriptor, while hypertubes and
            // railroad tracks use their native/default descriptor for the same
            // two-endpoint route. Treat that default as straight; a requested
            // curve remains strict because silently substituting it would alter
            // the caller's intended geometry.
            if (!Selected && BuildMode.Equals(TEXT("curve"), ESearchCase::IgnoreCase)) { Hologram->Destroy(); RefundRecipeCost(World, RecipeClass, 1); OnComplete(ErrorResponse(TEXT("build_mode_unavailable"), TEXT("The spline hologram did not expose the requested curved build mode."), EHttpServerResponseCodes::Conflict)); return; }
        }
        auto ClosestPrimitive=[](AFGBuildable* Buildable,const FVector& Point){UPrimitiveComponent* Best=nullptr;double BestDistance=TNumericLimits<double>::Max();TInlineComponentArray<UPrimitiveComponent*> Primitives(Buildable);for(UPrimitiveComponent* P:Primitives)if(P){const double D=FVector::DistSquared(P->GetComponentLocation(),Point);if(D<BestDistance){BestDistance=D;Best=P;}}return Best;};
        FHitResult StartHit(Source, ClosestPrimitive(Source, Start), Start, StartNormal); StartHit.bBlockingHit=true;StartHit.TraceStart=Start-StartNormal*100.0;StartHit.TraceEnd=Start+StartNormal*100.0;StartHit.Location=Start;StartHit.ImpactPoint=Start;
        Hologram->UpdateHologramPlacement(StartHit); const bool StartPress=Hologram->DoMultiStepPlacement(false); const bool StartRelease=Hologram->DoMultiStepPlacement(true);
        FHitResult EndHit=PointHit; if(!bToPoint){EndHit=FHitResult(Target, ClosestPrimitive(Target, End), End, EndNormal); EndHit.bBlockingHit=true;EndHit.TraceStart=End-EndNormal*100.0;EndHit.TraceEnd=End+EndNormal*100.0;EndHit.Location=End;EndHit.ImpactPoint=End;}
        Hologram->UpdateHologramPlacement(EndHit); Hologram->ValidatePlacementAndCost(GetPlayerInventory(World)); const bool EndPress=Hologram->DoMultiStepPlacement(false); const bool EndRelease=EndPress?false:Hologram->DoMultiStepPlacement(true); Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));
        TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers; Hologram->GetConstructDisqualifiers(Disqualifiers);
        if (EndPress && Disqualifiers.Num()==1 && Disqualifiers[0] && Disqualifiers[0]->IsChildOf(UFGCDInitializing::StaticClass())) { Hologram->ResetConstructDisqualifiers(); Hologram->SetPlacementMaterialState(EHologramMaterialState::HMS_OK); Disqualifiers.Reset(); }
        if (StartPress || StartRelease || (!EndPress && !EndRelease) || !Hologram->CanConstruct()) { FString Names;for(auto D:Disqualifiers){if(!Names.IsEmpty())Names+=TEXT(",");Names+=D?D->GetName():TEXT("null");}const int32 Step=static_cast<int32>(Hologram->GetCurrentBuildStep());Hologram->Destroy();RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("hologram_placement_failed"),FString::Printf(TEXT("Native %s hologram rejected the route (step=%d, disqualifiers=[%s]); build cost refunded."),*Kind,Step,*Names),EHttpServerResponseCodes::Conflict));return; }
        TArray<AActor*> Children; AFGBuildable* Built=Cast<AFGBuildable>(Hologram->Construct(Children,FNetConstructionID())); if(IsValid(Hologram))Hologram->Destroy();
        if(!Built){RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("hologram_construct_failed"),TEXT("The validated spline hologram did not construct a buildable; build cost refunded."),EHttpServerResponseCodes::ServerError));return;}
        // Pipe endpoints expose a direct connected state. Railroad station
        // endpoints connect to the two connection components owned by the new
        // track, not directly to one another, so both exact requested ports
        // must simply have acquired a connection after construction.
        // A pipe support's snap point never connects (#308): the new pipe's own end must rest on it instead, as a belt's
        // does on a conveyor pole (#111), and stays free for the next pipe, which starts from that end.
        auto PipeEndVerified=[Built](UFGPipeConnectionComponentBase* Port)
        {
            if(!Port)return false;
            if(Port->GetPipeConnectionType()!=EPipeConnectionType::PCT_SNAP_ONLY)return Port->IsConnected();
            TInlineComponentArray<UFGPipeConnectionComponentBase*> Ends(Built);
            for(UFGPipeConnectionComponentBase* End:Ends)if(End&&FVector::Distance(End->GetConnectorLocation(false),Port->GetConnectorLocation(false))<=50.0)return true;
            return false;
        };
        // A rail to a point ends free there: one of its own ends, unconnected, within 3 m of the point.
        auto FreeEndAtPoint=[Built,End](){TInlineComponentArray<UFGRailroadTrackConnectionComponent*> Ends(Built);for(UFGRailroadTrackConnectionComponent* C:Ends)if(C&&C->GetConnections().Num()==0&&FVector::Dist2D(C->GetConnectorLocation(),End)<=300.0)return true;return false;};
        const bool SourceVerified=StartPipe?PipeEndVerified(StartPipe):StartRail->GetConnections().Num()>0, TargetVerified=StartPipe?PipeEndVerified(EndPipe):bToPoint?FreeEndAtPoint():EndRail->GetConnections().Num()>0;
        // A rolled-back pipe takes its flow indicator with it (#333: one was left standing alone).
        if(!SourceVerified||!TargetVerified){for(AActor* Child:Children)if(IsValid(Child))Child->Destroy();if(AFGBuildablePipeline* Pipe=Cast<AFGBuildablePipeline>(Built))if(AActor* Indicator=Pipe->GetFlowIndicator())if(IsValid(Indicator))Indicator->Destroy();Built->Destroy();RefundRecipeCost(World,RecipeClass,1);OnComplete(ErrorResponse(TEXT("connection_verification_failed"),FString::Printf(TEXT("The constructed spline did not attach at its %s and was rolled back."),!SourceVerified&&!TargetVerified?TEXT("source and target"):!SourceVerified?TEXT("source"):TEXT("target")),EHttpServerResponseCodes::Conflict));return;}
        Body->SetObjectField(TEXT("receipt"),BuildReceiptJson(Built,{}));
        PlacementReceipts.Add(RequestId,Built);Body->SetBoolField(TEXT("placed"),true);Body->SetBoolField(TEXT("verified"),true);Body->SetStringField(TEXT("buildableId"),Built->GetName());Body->SetStringField(TEXT("className"),Built->GetClass()->GetName());Body->SetStringField(TEXT("recipePath"),RecipePath);OnComplete(JsonResponse(Body,EHttpServerResponseCodes::Created));
    },0.1f,false);
    return true;
}

bool FMKIBridgeModule::HandleConnectBeltHologram(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }

    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json))
    {
        OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    FString SourceId, TargetId, SourcePort, TargetPort, RequestId, RequestedRecipePath, BuildMode = TEXT("straight");
    bool Confirm = false;
    Json->TryGetStringField(TEXT("sourceId"), SourceId);
    Json->TryGetStringField(TEXT("targetId"), TargetId);
    Json->TryGetStringField(TEXT("sourcePort"), SourcePort);
    Json->TryGetStringField(TEXT("targetPort"), TargetPort);
    Json->TryGetStringField(TEXT("requestId"), RequestId);
    Json->TryGetStringField(TEXT("recipePath"), RequestedRecipePath);
    Json->TryGetStringField(TEXT("buildMode"), BuildMode);
    Json->TryGetBoolField(TEXT("confirm"), Confirm);
    if (SourceId.IsEmpty() || TargetId.IsEmpty() ||
        (!RequestedRecipePath.IsEmpty() && !RequestedRecipePath.StartsWith(TEXT("/Game/FactoryGame/"))) ||
        (!BuildMode.Equals(TEXT("straight"), ESearchCase::IgnoreCase) && !BuildMode.Equals(TEXT("curve"), ESearchCase::IgnoreCase)))
    {
        OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("sourceId, targetId, and buildMode (straight/curve) are required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    if (Confirm && RequestId.IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("A confirmed connection requires a requestId."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    if (!World || !Subsystem)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }
    AFGBuildable* Source = FindBuildableById(World, SourceId);
    AFGBuildable* Target = FindBuildableById(World, TargetId);

    // Endpoint resolution (#111, pole-to-pole half). A buildable with a free port of the right direction uses that
    // port. A conveyor support (pole, wall or ceiling mount) has only a snap-only point: when a belt already rests
    // there with a free end of the right direction, the new belt connects to that end and the route chains through
    // the support; otherwise the new belt's end is placed on the snap point and stays free for the next belt.
    enum class EBeltEndpoint : uint8 { Port, BeltEnd, SupportSnap };
    auto ResolveEndpoint = [World](AFGBuildable* Buildable, EFactoryConnectionDirection Direction, const FString& Port, EBeltEndpoint& OutKind) -> UFGFactoryConnectionComponent*
    {
        OutKind = EBeltEndpoint::Port;
        if (UFGFactoryConnectionComponent* Free = FindFreeFactoryConnection(Buildable, Direction, Port)) return Free;
        UFGFactoryConnectionComponent* Snap = FindConveyorSupportSnap(Buildable, Port);
        if (!Snap) return nullptr;
        if (UFGFactoryConnectionComponent* BeltEnd = FindBeltEndAtSupport(World, Snap, Direction)) { OutKind = EBeltEndpoint::BeltEnd; return BeltEnd; }
        OutKind = EBeltEndpoint::SupportSnap;
        return Snap;
    };
    EBeltEndpoint SourceKind = EBeltEndpoint::Port, TargetKind = EBeltEndpoint::Port;
    UFGFactoryConnectionComponent* Output = Source ? ResolveEndpoint(Source, EFactoryConnectionDirection::FCD_OUTPUT, SourcePort, SourceKind) : nullptr;
    UFGFactoryConnectionComponent* Input = Target ? ResolveEndpoint(Target, EFactoryConnectionDirection::FCD_INPUT, TargetPort, TargetKind) : nullptr;
    // The hologram is driven against the actor that owns the resolved connection: the endpoint buildable, or the
    // belt already resting on the support.
    AFGBuildable* SourceActor = Output ? Cast<AFGBuildable>(Output->GetOwner()) : nullptr;
    AFGBuildable* TargetActor = Input ? Cast<AFGBuildable>(Input->GetOwner()) : nullptr;
    if (!Source || !Target || Source == Target || !Output || !Input || !SourceActor || !TargetActor || Output == Input)
    {
        OnComplete(ErrorResponse(TEXT("endpoint_unavailable"), TEXT("Compatible free output/input endpoints (a free port, or a conveyor support snap point) were not found."), EHttpServerResponseCodes::Conflict));
        return true;
    }

    const FVector Start = Output->GetConnectorLocation(false);
    const FVector End = Input->GetConnectorLocation(false);
    const double DirectDistance = FVector::Distance(Start, End);
    if (DirectDistance < 200.0 || DirectDistance > 5600.1)
    {
        OnComplete(ErrorResponse(TEXT("invalid_belt_length"), TEXT("Conveyor endpoints must be 2-56 meters apart."), EHttpServerResponseCodes::Conflict));
        return true;
    }
    // The game builds a belt through a support snap only against the snap's facing: the belt leaves or arrives
    // travelling opposite to the snap normal, so the support faces back toward the source (verified live). A support
    // facing along the flow makes the hologram reject the route with FGCDConveyorInvalidShape, so refuse it here,
    // before any cost is charged, and say how to fix it.
    const FVector FlowDirection = (End - Start).GetSafeNormal2D();
    auto SupportFacesFlow = [&FlowDirection](EBeltEndpoint Kind, UFGFactoryConnectionComponent* Connection)
    {
        return Kind == EBeltEndpoint::SupportSnap && FVector::DotProduct(Connection->GetConnectorNormal().GetSafeNormal2D(), FlowDirection) >= 0.0;
    };
    if (SupportFacesFlow(SourceKind, Output) || SupportFacesFlow(TargetKind, Input))
    {
        OnComplete(ErrorResponse(TEXT("support_facing_mismatch"), TEXT("A conveyor support must face back toward the belt's source (its snap normal opposite to the flow); rotate the support 180 degrees or swap source and target."), EHttpServerResponseCodes::Conflict));
        return true;
    }
    // A support snap point has no flow direction of its own; the belt leaves the source toward the target and
    // arrives at the target from the source.
    const FVector StartNormal = SourceKind == EBeltEndpoint::SupportSnap ? (End - Start).GetSafeNormal() : Output->GetConnectorNormal();
    const FVector EndNormal = TargetKind == EBeltEndpoint::SupportSnap ? (Start - End).GetSafeNormal() : Input->GetConnectorNormal();
    auto KindName = [](EBeltEndpoint Kind) { return Kind == EBeltEndpoint::Port ? TEXT("port") : Kind == EBeltEndpoint::BeltEnd ? TEXT("belt_end") : TEXT("support_snap"); };

    TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetNumberField(TEXT("directDistanceCm"), DirectDistance);
    Body->SetStringField(TEXT("buildMode"), BuildMode.ToLower());
    Body->SetStringField(TEXT("sourcePort"), Output->GetName());
    Body->SetStringField(TEXT("targetPort"), Input->GetName());
    Body->SetStringField(TEXT("sourceEndpoint"), KindName(SourceKind));
    Body->SetStringField(TEXT("targetEndpoint"), KindName(TargetKind));
    if (SourceKind == EBeltEndpoint::BeltEnd) Body->SetStringField(TEXT("sourceChainedBeltId"), SourceActor->GetName());
    if (TargetKind == EBeltEndpoint::BeltEnd) Body->SetStringField(TEXT("targetChainedBeltId"), TargetActor->GetName());
    if (!Confirm)
    {
        OnComplete(JsonResponse(Body));
        return true;
    }

    const FString ResolvedRecipePath = RequestedRecipePath.IsEmpty() ? FString(ConveyorBeltMk1RecipePath) : RequestedRecipePath;
    UClass* RecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, *ResolvedRecipePath);
    if (!RecipeClass)
    {
        OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("The Conveyor Belt Mk.1 recipe is unavailable."), EHttpServerResponseCodes::ServerError));
        return true;
    }
    FString Missing;
    if (!ConsumeRecipeCost(World, RecipeClass, 1, Missing))
    {
        OnComplete(ErrorResponse(TEXT("insufficient_build_materials"), Missing, EHttpServerResponseCodes::Conflict));
        return true;
    }

    AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0));
    APlayerController* Controller = UGameplayStatics::GetPlayerController(World, 0);
    AFGConveyorBeltHologram* Hologram = Cast<AFGConveyorBeltHologram>(
        AFGHologram::SpawnHologramFromRecipe(RecipeClass, Controller, Start, Player));
    if (!Hologram)
    {
        RefundRecipeCost(World, RecipeClass, 1);
        OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"), TEXT("The conveyor hologram could not be spawned; build cost was refunded."), EHttpServerResponseCodes::ServerError));
        return true;
    }

    // The normal build-gun path completes this game-state/customization gate
    // before accepting input. An HTTP request drives the hologram immediately
    // in the spawn frame, so explicitly run the same public initialization
    // hook before the two placement steps.
    Hologram->WaitForGamestateApplyCustomizationData();

    FTimerHandle HologramInitializationTimer;
    World->GetTimerManager().SetTimer(HologramInitializationTimer, [this, WeakWorld = TWeakObjectPtr<UWorld>(World), WeakRecipe = TWeakObjectPtr<UClass>(RecipeClass), WeakHologram = TWeakObjectPtr<AFGConveyorBeltHologram>(Hologram),
        WeakOutput = TWeakObjectPtr<UFGFactoryConnectionComponent>(Output), WeakInput = TWeakObjectPtr<UFGFactoryConnectionComponent>(Input), WeakSource = TWeakObjectPtr<AFGBuildable>(SourceActor), WeakTarget = TWeakObjectPtr<AFGBuildable>(TargetActor),
        Ticket = ActiveTicket, SourceKind, TargetKind, StartNormal, EndNormal, ResolvedRecipePath, Start, End, BuildMode, RequestId, Body, OnComplete]() mutable
    {
    UWorld* World = WeakWorld.Get(); UClass* RecipeClass = WeakRecipe.Get(); AFGConveyorBeltHologram* Hologram = WeakHologram.Get();
    UFGFactoryConnectionComponent* Output = WeakOutput.Get(); UFGFactoryConnectionComponent* Input = WeakInput.Get(); AFGBuildable* Source = WeakSource.Get(); AFGBuildable* Target = WeakTarget.Get();
    if (!World || !RecipeClass || !Hologram || !Output || !Input || !Source || !Target || (Ticket && Ticket->bAbandoned))
    {
        if (Hologram) Hologram->Destroy();
        if (World && RecipeClass) RefundRecipeCost(World, RecipeClass, 1);
        OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"), TEXT("The world, an endpoint, or the hologram disappeared before construction; nothing was built."), EHttpServerResponseCodes::Conflict));
        return;
    }
    TArray<TSubclassOf<UFGBuildGunModeDescriptor>> SupportedModes;
    Hologram->GetSupportedBuildModes(SupportedModes);
    const FString ModeNeedle = BuildMode.Equals(TEXT("curve"), ESearchCase::IgnoreCase) ? TEXT("Curve") : TEXT("Straight");
    bool ModeSelected = false;
    for (TSubclassOf<UFGBuildGunModeDescriptor> SupportedMode : SupportedModes)
    {
        UClass* ModeClass = SupportedMode.Get();
        if (ModeClass && ModeClass->IsChildOf(UFGHologramBuildModeDescriptor::StaticClass()) && ModeClass->GetName().Contains(ModeNeedle, ESearchCase::IgnoreCase))
        {
            Hologram->SetBuildModeOverride(ModeClass);
            ModeSelected = true;
            break;
        }
    }
    if (!ModeSelected)
    {
        Hologram->Destroy();
        RefundRecipeCost(World, RecipeClass, 1);
        OnComplete(ErrorResponse(TEXT("build_mode_unavailable"), FString::Printf(TEXT("The conveyor hologram did not expose the requested %s build mode."), *BuildMode), EHttpServerResponseCodes::Conflict));
        return;
    }

    auto FindClosestPrimitive = [](AFGBuildable* Buildable, const FVector& Point) -> UPrimitiveComponent*
    {
        UPrimitiveComponent* Closest = nullptr;
        double ClosestDistanceSquared = TNumericLimits<double>::Max();
        TInlineComponentArray<UPrimitiveComponent*> Primitives(Buildable);
        for (UPrimitiveComponent* Primitive : Primitives)
        {
            if (!Primitive) continue;
            const double DistanceSquared = FVector::DistSquared(Primitive->GetComponentLocation(), Point);
            if (DistanceSquared < ClosestDistanceSquared)
            {
                ClosestDistanceSquared = DistanceSquared;
                Closest = Primitive;
            }
        }
        return Closest;
    };

    FHitResult StartHit(Source, FindClosestPrimitive(Source, Start), Start, StartNormal);
    StartHit.bBlockingHit = true;
    StartHit.TraceStart = Start - StartNormal * 100.0;
    StartHit.TraceEnd = Start + StartNormal * 100.0;
    StartHit.Location = Start;
    StartHit.ImpactPoint = Start;
    Hologram->UpdateHologramPlacement(StartHit);
    const bool FinishedAtStartPress = Hologram->DoMultiStepPlacement(false);
    const bool FinishedAtStartRelease = Hologram->DoMultiStepPlacement(true);

    FHitResult EndHit(Target, FindClosestPrimitive(Target, End), End, EndNormal);
    EndHit.bBlockingHit = true;
    EndHit.TraceStart = End - EndNormal * 100.0;
    EndHit.TraceEnd = End + EndNormal * 100.0;
    EndHit.Location = End;
    EndHit.ImpactPoint = End;
    Hologram->UpdateHologramPlacement(EndHit);
    Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));
    const bool FinishedAtEndPress = Hologram->DoMultiStepPlacement(false);
    const bool FinishedAtEndRelease = FinishedAtEndPress ? false : Hologram->DoMultiStepPlacement(true);

    // Finalizing the second endpoint generates the actual spline and support
    // geometry. Revalidate now so clearance/collision disqualifiers from the
    // completed route are observed before construction.
    Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));

    // A hologram spawned outside AFGBuildGunBuildState retains the build-gun
    // ownership sentinel indefinitely. Remove it only when it is the sole
    // remaining disqualifier and both native endpoint placement steps have
    // completed. All geometric, clearance, routing, and cost disqualifiers are
    // preserved and continue to reject construction below.
    TArray<TSubclassOf<UFGConstructDisqualifier>> PreConstructDisqualifiers;
    Hologram->GetConstructDisqualifiers(PreConstructDisqualifiers);
    if (FinishedAtEndPress && PreConstructDisqualifiers.Num() == 1 &&
        PreConstructDisqualifiers[0] && PreConstructDisqualifiers[0]->IsChildOf(UFGCDInitializing::StaticClass()))
    {
        Hologram->ResetConstructDisqualifiers();
        Hologram->SetPlacementMaterialState(EHologramMaterialState::HMS_OK);
    }
    const bool CanConstructAtEnd = Hologram->CanConstruct();

    if (FinishedAtStartPress || FinishedAtStartRelease || (!FinishedAtEndPress && !FinishedAtEndRelease) || !CanConstructAtEnd)
    {
        TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
        Hologram->GetConstructDisqualifiers(Disqualifiers);
        FString DisqualifierNames;
        for (TSubclassOf<UFGConstructDisqualifier> Disqualifier : Disqualifiers)
        {
            if (!DisqualifierNames.IsEmpty()) DisqualifierNames += TEXT(",");
            DisqualifierNames += Disqualifier ? Disqualifier->GetName() : TEXT("null");
        }
        const int32 BuildStep = static_cast<int32>(Hologram->GetCurrentBuildStep());
        Hologram->Destroy();
        RefundRecipeCost(World, RecipeClass, 1);
        OnComplete(ErrorResponse(TEXT("hologram_placement_failed"),
            FString::Printf(TEXT("The native conveyor hologram rejected the route (startPress=%s, startRelease=%s, endPress=%s, endRelease=%s, step=%d, disqualifiers=%d [%s]); build cost was refunded."),
                FinishedAtStartPress ? TEXT("true") : TEXT("false"), FinishedAtStartRelease ? TEXT("true") : TEXT("false"),
                FinishedAtEndPress ? TEXT("true") : TEXT("false"), FinishedAtEndRelease ? TEXT("true") : TEXT("false"),
                BuildStep, Disqualifiers.Num(), *DisqualifierNames),
            EHttpServerResponseCodes::Conflict));
        return;
    }

    TArray<AActor*> Children;
    AActor* Constructed = Hologram->Construct(Children, FNetConstructionID());
    AFGBuildableConveyorBelt* Belt = Cast<AFGBuildableConveyorBelt>(Constructed);
    if (!Belt)
    {
        if (IsValid(Constructed)) Constructed->Destroy();
        for (AActor* Child : Children) if (IsValid(Child)) Child->Destroy();
        if (IsValid(Hologram)) Hologram->Destroy();
        RefundRecipeCost(World, RecipeClass, 1);
        OnComplete(ErrorResponse(TEXT("hologram_construct_failed"), TEXT("The native conveyor hologram did not construct a belt; build cost was refunded."), EHttpServerResponseCodes::ServerError));
        return;
    }

    if (IsValid(Hologram)) Hologram->Destroy();

    // Verify both requested endpoints. A port or an existing belt end must now be connected. A support snap point
    // never connects, so the new belt's own end must rest on it (and stays free for the next belt). Anything else
    // is rolled back with its cost refunded instead of being reported as an unverified success.
    auto EndRestsOn = [](UFGFactoryConnectionComponent* BeltEnd, const UFGFactoryConnectionComponent* Snap)
    {
        return BeltEnd && Snap && FVector::Distance(BeltEnd->GetConnectorLocation(false), Snap->GetConnectorLocation(false)) <= ConveyorSupportSnapToleranceCm;
    };
    const bool SourceVerified = SourceKind == EBeltEndpoint::SupportSnap ? EndRestsOn(Belt->GetConnection0(), Output) : Output->IsConnected();
    const bool TargetVerified = TargetKind == EBeltEndpoint::SupportSnap ? EndRestsOn(Belt->GetConnection1(), Input) : Input->IsConnected();
    if (!SourceVerified || !TargetVerified)
    {
        for (AActor* Child : Children) if (IsValid(Child)) Child->Destroy();
        Belt->Destroy();
        RefundRecipeCost(World, RecipeClass, 1);
        // Name each end that did not attach (#302), so the caller knows which port or belt end to look at.
        auto EndName = [](const TWeakObjectPtr<AFGBuildable>& Owner, const UFGFactoryConnectionComponent* Port) { return FString::Printf(TEXT("%s.%s"), Owner.IsValid() ? *Owner->GetName() : TEXT("?"), Port ? *Port->GetName() : TEXT("?")); };
        TArray<FString> Unattached;
        if (!SourceVerified) Unattached.Add(FString::Printf(TEXT("source %s"), *EndName(WeakSource, Output)));
        if (!TargetVerified) Unattached.Add(FString::Printf(TEXT("target %s"), *EndName(WeakTarget, Input)));
        OnComplete(ErrorResponse(TEXT("connection_verification_failed"), FString::Printf(TEXT("The constructed belt did not attach at its %s (source=%s, target=%s) and was rolled back; build cost was refunded."),
            *FString::Join(Unattached, TEXT(" and ")), SourceVerified ? TEXT("verified") : TEXT("unverified"), TargetVerified ? TEXT("verified") : TEXT("unverified")), EHttpServerResponseCodes::Conflict));
        return;
    }

    PlacementReceipts.Add(RequestId, Belt);
    Belt->ApplyCustomizationData_Native(Belt->GetCustomizationData_Native());
    Body->SetBoolField(TEXT("placed"), true);
    Body->SetBoolField(TEXT("costConsumed"), true);
    Body->SetStringField(TEXT("beltId"), Belt->GetName());
    Body->SetStringField(TEXT("buildableId"), Belt->GetName());
    Body->SetStringField(TEXT("className"), Belt->GetClass()->GetName());
    Body->SetStringField(TEXT("recipePath"), ResolvedRecipePath);
    Body->SetNumberField(TEXT("splineLengthCm"), Belt->GetSplineComponent() ? Belt->GetSplineComponent()->GetSplineLength() : 0.0);
    TArray<TSharedPtr<FJsonValue>> ChildBuildableIds;
    for (AActor* Child : Children) if (AFGBuildable* ChildBuildable = Cast<AFGBuildable>(Child)) ChildBuildableIds.Add(MakeShared<FJsonValueString>(ChildBuildable->GetName()));
    Body->SetArrayField(TEXT("childBuildableIds"), ChildBuildableIds);
    Body->SetBoolField(TEXT("verified"), true);
    Body->SetObjectField(TEXT("receipt"), BuildReceiptJson(Belt, {}));
    OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Created));
    }, 0.1f, false);
    return true;
}

bool FMKIBridgeModule::HandleConnectPower(const FHttpServerRequest& Request,const FHttpResultCallback& OnComplete)
{
    if(!IsAuthorized(Request)){OnComplete(ErrorResponse(TEXT("unauthorized"),TEXT("A valid bearer token is required."),EHttpServerResponseCodes::Denied));return true;}
    TSharedPtr<FJsonObject>J;if(!ReadJsonBody(Request,J)){OnComplete(ErrorResponse(TEXT("invalid_json"),TEXT("Request body must be a JSON object."),EHttpServerResponseCodes::BadRequest));return true;}
    // sourceId/targetId are the shared connection field names; aId/bId remain accepted aliases for one release.
    FString AId,BId,SourcePort,TargetPort,RequestId;bool Confirm=false;
    if(!J->TryGetStringField(TEXT("sourceId"),AId))J->TryGetStringField(TEXT("aId"),AId);if(!J->TryGetStringField(TEXT("targetId"),BId))J->TryGetStringField(TEXT("bId"),BId);
    J->TryGetStringField(TEXT("sourcePort"),SourcePort);J->TryGetStringField(TEXT("targetPort"),TargetPort);J->TryGetStringField(TEXT("requestId"),RequestId);J->TryGetBoolField(TEXT("confirm"),Confirm);
    if(Confirm&&RequestId.IsEmpty()){OnComplete(ErrorResponse(TEXT("request_id_required"),TEXT("A confirmed connection requires a requestId."),EHttpServerResponseCodes::BadRequest));return true;}
    UWorld*W=GEngine?GEngine->GetCurrentPlayWorld():nullptr;AFGBuildableSubsystem*S=W?AFGBuildableSubsystem::Get(W):nullptr;
    if(!W||!S){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World is not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}
    AFGBuildable*A=FindBuildableById(W,AId);AFGBuildable*Z=FindBuildableById(W,BId);
    auto Find=[](AFGBuildable*X,const FString&Port){UFGCircuitConnectionComponent*O=nullptr;if(X){TInlineComponentArray<UFGCircuitConnectionComponent*>C(X);for(auto*P:C)if(P&&!P->IsHidden()&&P->GetNumFreeConnections()>0&&(Port.IsEmpty()||P->GetName().Equals(Port,ESearchCase::IgnoreCase))){O=P;break;}}return O;};
    UFGCircuitConnectionComponent*AC=Find(A,SourcePort);UFGCircuitConnectionComponent*ZC=Find(Z,TargetPort);
    if(!AC||!ZC||A==Z){OnComplete(ErrorResponse(TEXT("power_endpoint_unavailable"),TEXT("Two distinct matching free power endpoints are required."),EHttpServerResponseCodes::Conflict));return true;}
    double L=FVector::Distance(AC->GetComponentLocation(),ZC->GetComponentLocation());if(L>10000){OnComplete(ErrorResponse(TEXT("wire_too_long"),TEXT("Power line exceeds 100 meters."),EHttpServerResponseCodes::Conflict));return true;}
    auto B=MakeShared<FJsonObject>();B->SetBoolField(TEXT("ok"),true);B->SetBoolField(TEXT("dryRun"),!Confirm);B->SetNumberField(TEXT("lengthCm"),L);B->SetStringField(TEXT("sourcePort"),AC->GetName());B->SetStringField(TEXT("targetPort"),ZC->GetName());if(!Confirm){OnComplete(JsonResponse(B));return true;}
    UClass*WC=StaticLoadClass(AFGBuildableWire::StaticClass(),nullptr,PowerLineClassPath);UClass*RC=StaticLoadClass(UFGRecipe::StaticClass(),nullptr,PowerLineRecipePath);if(!WC||!RC){OnComplete(ErrorResponse(TEXT("asset_unavailable"),TEXT("Power-line asset or recipe unavailable."),EHttpServerResponseCodes::ServerError));return true;}
    FString Missing;if(!ConsumeRecipeCost(W,RC,1,Missing)){OnComplete(ErrorResponse(TEXT("insufficient_build_materials"),Missing,EHttpServerResponseCodes::Conflict));return true;}
    auto*Wire=Cast<AFGBuildableWire>(S->BeginSpawnBuildable(WC,FTransform::Identity));if(!Wire){RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("spawn_failed"),TEXT("Power line spawn failed; build cost was refunded."),EHttpServerResponseCodes::ServerError));return true;}
    Wire->SetBuiltWithRecipe(RC);UGameplayStatics::FinishSpawningActor(Wire,FTransform::Identity);if(!Wire->Connect(AC,ZC)){Wire->Destroy();RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("connection_failed"),TEXT("Power connection failed; build cost was refunded."),EHttpServerResponseCodes::Conflict));return true;}
    // A wire that reports connected but left either endpoint unconnected is rolled back rather than reported.
    if(!(AC->IsConnected()&&ZC->IsConnected())){Wire->Destroy();RefundRecipeCost(W,RC,1);OnComplete(ErrorResponse(TEXT("connection_verification_failed"),TEXT("The power line did not connect both endpoints and was rolled back; build cost was refunded."),EHttpServerResponseCodes::Conflict));return true;}
    PlacementReceipts.Add(RequestId,Wire);B->SetBoolField(TEXT("placed"),true);B->SetBoolField(TEXT("costConsumed"),true);B->SetStringField(TEXT("wireId"),Wire->GetName());B->SetStringField(TEXT("buildableId"),Wire->GetName());B->SetBoolField(TEXT("verified"),true);OnComplete(JsonResponse(B,EHttpServerResponseCodes::Created));return true;
}

#include "MKIBridge.h"
#include "BridgeInternal.h"

DEFINE_LOG_CATEGORY(LogMKIBridge);

// Module lifecycle, route table and dispatch, the in-game status indicator, health, and capabilities.
// Split from the single MKIBridge.cpp without behavior changes (#44).

void FMKIBridgeModule::StartupModule()
{
    // Cook, package, and other commandlets load runtime modules but must not
    // open network listeners. The bridge is only useful in an interactive
    // editor or game process.
    if (IsRunningCommandlet())
    {
        return;
    }

    if (!LoadOrCreateAuthenticationToken())
    {
        UE_LOG(LogMKIBridge, Error, TEXT("Unable to load or create authentication token; bridge will not start"));
        return;
    }

    // Bind this port to loopback explicitly (#37). The engine HTTP server reads per-port listener overrides from
    // [HTTPServer.Listeners] ListenerOverrides when it creates the listener, so any override for this port that a
    // configuration file may have widened is replaced before the router is requested. DispatchRoute additionally
    // rejects any request whose peer address is known and not loopback.
    {
        const TCHAR* ListenerSection = TEXT("HTTPServer.Listeners");
        TArray<FString> Overrides;
        GConfig->GetArray(ListenerSection, TEXT("ListenerOverrides"), Overrides, GEngineIni);
        const int32 Removed = Overrides.RemoveAll([this](const FString& Entry) { uint32 EntryPort = 0; return FParse::Value(*Entry, TEXT("Port="), EntryPort) && EntryPort == Port; });
        if (Removed > 0) UE_LOG(LogMKIBridge, Warning, TEXT("Replacing %d configured listener override(s) for port %u with a loopback-only bind"), Removed, Port);
        ConfiguredBindAddress = TEXT("127.0.0.1");
        Overrides.Add(FString::Printf(TEXT("(Port=%u,BindAddress=%s)"), Port, *ConfiguredBindAddress));
        GConfig->SetArray(ListenerSection, TEXT("ListenerOverrides"), Overrides, GEngineIni);
    }

    FHttpServerModule& HttpServer = FHttpServerModule::Get();
    Router = HttpServer.GetHttpRouter(Port);
    if (!Router.IsValid())
    {
        UE_LOG(LogMKIBridge, Error, TEXT("Unable to create HTTP router on port %u"), Port);
        return;
    }

    using M = FMKIBridgeModule;
    const auto Read = [](const TCHAR* Path) { FRouteOptions Options; Options.Path = Path; return Options; };
    const auto Public = [](const TCHAR* Path) { FRouteOptions Options; Options.Path = Path; Options.bAuthenticated = false; return Options; };
    const auto Mutation = [](const TCHAR* Path, bool bRequestIdRequired) { FRouteOptions Options; Options.Path = Path; Options.bMutation = true; Options.bRequestIdRequiredOnConfirm = bRequestIdRequired; return Options; };
    BindBridgeRoute(TEXT("/v1/health"), EHttpServerRequestVerbs::VERB_GET, &M::HandleHealth, Public(TEXT("/v1/health")));
    BindBridgeRoute(TEXT("/v1/capabilities"), EHttpServerRequestVerbs::VERB_GET, &M::HandleCapabilities, Public(TEXT("/v1/capabilities")));
    BindBridgeRoute(TEXT("/v1/buildables"), EHttpServerRequestVerbs::VERB_GET, &M::HandleBuildables, Read(TEXT("/v1/buildables")));
    BindBridgeRoute(TEXT("/v1/resource-nodes"), EHttpServerRequestVerbs::VERB_GET, &M::HandleResourceNodes, Read(TEXT("/v1/resource-nodes")));
    BindBridgeRoute(TEXT("/v1/player"), EHttpServerRequestVerbs::VERB_GET, &M::HandlePlayerState, Read(TEXT("/v1/player")));
    BindBridgeRoute(TEXT("/v1/player/vitals"), EHttpServerRequestVerbs::VERB_GET, &M::HandlePlayerVitals, Read(TEXT("/v1/player/vitals")));
    BindBridgeRoute(TEXT("/v1/world/volumes"), EHttpServerRequestVerbs::VERB_GET, &M::HandleWorldVolumes, Read(TEXT("/v1/world/volumes")));
    BindBridgeRoute(TEXT("/v1/player/respawn"), EHttpServerRequestVerbs::VERB_POST, &M::HandlePlayerRespawn, Mutation(TEXT("/v1/player/respawn"), false));
    BindBridgeRoute(TEXT("/v1/saves"), EHttpServerRequestVerbs::VERB_GET, &M::HandleListSaves, Read(TEXT("/v1/saves")));
    BindBridgeRoute(TEXT("/v1/load-save"), EHttpServerRequestVerbs::VERB_POST, &M::HandleLoadSave, Mutation(TEXT("/v1/load-save"), false));
    {
        // The handler keeps its own requestId receipts and save-in-flight guard: a save can outlive the mutation
        // watchdog, so it is not wrapped as a generic mutation.
        FRouteOptions Save; Save.Path = TEXT("/v1/save-game");
        BindBridgeRoute(TEXT("/v1/save-game"), EHttpServerRequestVerbs::VERB_POST, &M::HandleSaveGame, Save);
    }
    BindBridgeRoute(TEXT("/v1/place"), EHttpServerRequestVerbs::VERB_POST, &M::HandlePlaceStructure, Mutation(TEXT("/v1/place"), true));
    // Temporary: the spline road prototype, placed from explicit points until it has a build-gun hologram.
    BindBridgeRoute(TEXT("/v1/place-spline-road"), EHttpServerRequestVerbs::VERB_POST, &M::HandlePlaceSplineRoad, Mutation(TEXT("/v1/place-spline-road"), true));
    // The player's own build gun, driven step by step: what a player at the keyboard does, for testing holograms live.
    FRouteOptions BuildGun = Mutation(TEXT("/v1/build-gun"), true);
    BuildGun.WatchdogSeconds = 120.0f;
    BindBridgeRoute(TEXT("/v1/build-gun"), EHttpServerRequestVerbs::VERB_POST, &M::HandlePlaceWithBuildGun, BuildGun);
    BindBridgeRoute(TEXT("/v1/set-machine-recipe"), EHttpServerRequestVerbs::VERB_POST, &M::HandleSetMachineRecipe, Mutation(TEXT("/v1/set-machine-recipe"), false));
    BindBridgeRoute(TEXT("/v1/machine-clock"), EHttpServerRequestVerbs::VERB_POST, &M::HandleMachineClock, Mutation(TEXT("/v1/machine-clock"), false));
    BindBridgeRoute(TEXT("/v1/splitter-rules"), EHttpServerRequestVerbs::VERB_POST, &M::HandleSplitterRules, Mutation(TEXT("/v1/splitter-rules"), false));
    BindBridgeRoute(TEXT("/v1/machine-connections"), EHttpServerRequestVerbs::VERB_GET, &M::HandleMachineConnections, Read(TEXT("/v1/machine-connections")));
    BindBridgeRoute(TEXT("/v1/power-connections"), EHttpServerRequestVerbs::VERB_GET, &M::HandlePowerConnections, Read(TEXT("/v1/power-connections")));
    BindBridgeRoute(TEXT("/v1/connect"), EHttpServerRequestVerbs::VERB_POST, &M::HandleConnectStructures, Mutation(TEXT("/v1/connect"), true));
    BindBridgeRoute(TEXT("/v1/dismantle"), EHttpServerRequestVerbs::VERB_POST, &M::HandleDeleteBuildable, Mutation(TEXT("/v1/dismantle"), false));
    BindBridgeRoute(TEXT("/v1/player-transform"), EHttpServerRequestVerbs::VERB_POST, &M::HandleSetPlayerTransform, Mutation(TEXT("/v1/player-transform"), false));
    BindBridgeRoute(TEXT("/v1/placement-status"), EHttpServerRequestVerbs::VERB_GET, &M::HandlePlacementStatus, Read(TEXT("/v1/placement-status")));
    BindBridgeRoute(TEXT("/v1/site-survey"), EHttpServerRequestVerbs::VERB_POST, &M::HandleValidatePlacement, Read(TEXT("/v1/site-survey")));
    BindBridgeRoute(TEXT("/v1/region/loaded"), EHttpServerRequestVerbs::VERB_GET, &M::HandleRegionLoaded, Read(TEXT("/v1/region/loaded")));
    BindBridgeRoute(TEXT("/v1/terrain/column-probe"), EHttpServerRequestVerbs::VERB_POST, &M::HandleColumnProbe, Read(TEXT("/v1/terrain/column-probe")));
    BindBridgeRoute(TEXT("/v1/world/actor"), EHttpServerRequestVerbs::VERB_GET, &M::HandleWorldActor, Read(TEXT("/v1/world/actor")));
    BindBridgeRoute(TEXT("/v1/world/static-meshes"), EHttpServerRequestVerbs::VERB_POST, &M::HandleStaticMeshSearch, Read(TEXT("/v1/world/static-meshes")));
    // MKI Tools' Terrain dismantle mode, for the AI: the removed rocks, and removing or restoring one by name.
    BindBridgeRoute(TEXT("/v1/terrain/removed-rocks"), EHttpServerRequestVerbs::VERB_GET, &M::HandleTerrainRemovedRocks, Read(TEXT("/v1/terrain/removed-rocks")));
    BindBridgeRoute(TEXT("/v1/terrain/remove-rock"), EHttpServerRequestVerbs::VERB_POST, &M::HandleTerrainRemoveRock, Mutation(TEXT("/v1/terrain/remove-rock"), false));
    BindBridgeRoute(TEXT("/v1/terrain/restore-rock"), EHttpServerRequestVerbs::VERB_POST, &M::HandleTerrainRestoreRock, Mutation(TEXT("/v1/terrain/restore-rock"), false));
    BindBridgeRoute(TEXT("/v1/hologram-probe"), EHttpServerRequestVerbs::VERB_POST, &M::HandleHologramProbe, Read(TEXT("/v1/hologram-probe")));
    BindBridgeRoute(TEXT("/v1/conveyors"), EHttpServerRequestVerbs::VERB_GET, &M::HandleConveyors, Read(TEXT("/v1/conveyors")));
    BindBridgeRoute(TEXT("/v1/machines"), EHttpServerRequestVerbs::VERB_GET, &M::HandleMachines, Read(TEXT("/v1/machines")));
    BindBridgeRoute(TEXT("/v1/power-circuits"), EHttpServerRequestVerbs::VERB_GET, &M::HandlePowerCircuits, Read(TEXT("/v1/power-circuits")));
    BindBridgeRoute(TEXT("/v1/inventory/player"), EHttpServerRequestVerbs::VERB_GET, &M::HandlePlayerInventory, Read(TEXT("/v1/inventory/player")));
    BindBridgeRoute(TEXT("/v1/catalog/buildables"), EHttpServerRequestVerbs::VERB_GET, &M::HandleCatalogBuildables, Read(TEXT("/v1/catalog/buildables")));
    BindBridgeRoute(TEXT("/v1/catalog/recipes"), EHttpServerRequestVerbs::VERB_GET, &M::HandleCatalogRecipes, Read(TEXT("/v1/catalog/recipes")));
    BindBridgeRoute(TEXT("/v1/progression"), EHttpServerRequestVerbs::VERB_GET, &M::HandleProgression, Read(TEXT("/v1/progression")));
    {
        FRouteOptions Batch = Mutation(TEXT("/v1/batch"), true);
        // Operations run one per tick and each has its own 25 s watchdog; the batch as a whole gets longer.
        Batch.WatchdogSeconds = 600.0f;
        Batch.AbandonedMessage = TEXT("The batch did not finish (for example the world was unloaded). Operations that completed may remain built; check placement-status for <requestId>:<opId>.");
        BindBridgeRoute(TEXT("/v1/batch"), EHttpServerRequestVerbs::VERB_POST, &M::HandleBatch, Batch);
    }
    BindBridgeRoute(TEXT("/v1/events"), EHttpServerRequestVerbs::VERB_GET, &M::HandleEvents, Read(TEXT("/v1/events")));
    BindBridgeRoute(TEXT("/v1/vehicles"), EHttpServerRequestVerbs::VERB_GET, &M::HandleVehicles, Read(TEXT("/v1/vehicles")));
    BindBridgeRoute(TEXT("/v1/inventories"), EHttpServerRequestVerbs::VERB_GET, &M::HandleInventories, Read(TEXT("/v1/inventories")));
    BindBridgeRoute(TEXT("/v1/vehicle-route"), EHttpServerRequestVerbs::VERB_POST, &M::HandleVehicleRoute, Mutation(TEXT("/v1/vehicle-route"), false));
    BindBridgeRoute(TEXT("/v1/game-rules"), EHttpServerRequestVerbs::VERB_GET, &M::HandleGameRules, Read(TEXT("/v1/game-rules")));
    BindBridgeRoute(TEXT("/v1/game-rules/fuel"), EHttpServerRequestVerbs::VERB_POST, &M::HandleGameRules, Mutation(TEXT("/v1/game-rules/fuel"), false));
    BindBridgeRoute(TEXT("/v1/vehicle-paths/revalidate"), EHttpServerRequestVerbs::VERB_POST, &M::HandleVehiclePathRevalidate, Mutation(TEXT("/v1/vehicle-paths/revalidate"), false));
    BindBridgeRoute(TEXT("/v1/docking-station"), EHttpServerRequestVerbs::VERB_POST, &M::HandleDockingStation, Mutation(TEXT("/v1/docking-station"), false));
    BindBridgeRoute(TEXT("/v1/inventory/transfer"), EHttpServerRequestVerbs::VERB_POST, &M::HandleInventoryTransfer, Mutation(TEXT("/v1/inventory/transfer"), false));

    HttpServer.StartAllListeners();
    StatusIndicatorTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FMKIBridgeModule::TickStatusIndicator),
        1.0f);
    // Event deadlines, world sampling, and receipt side-file writes run on the game thread at this interval.
    EventsTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FMKIBridgeModule::TickEvents),
        0.25f);
    UE_LOG(LogMKIBridge, Display, TEXT("MKI Bridge listening on %s:%u (loopback only; non-loopback peers are rejected)"), *ConfiguredBindAddress, Port);
}

void FMKIBridgeModule::BindBridgeRoute(const TCHAR* Path, EHttpServerRequestVerbs Verb, FRouteMethod Method, FRouteOptions Options)
{
    RouteTable.Add(Options.Path, TPair<FRouteMethod, FRouteOptions>(Method, Options));
    RouteHandles.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda(
        [this, Method, Options](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) { return DispatchRoute(Request, OnComplete, Method, Options); })));
}

/**
 * Every route passes through here, in this order: loopback peer check, authentication, then, for a confirmed
 * mutation, requestId and host-authority checks and the unified idempotency contract. Nothing is parsed or mutated
 * before authentication succeeds.
 */
bool FMKIBridgeModule::DispatchRoute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, FRouteMethod Method, const FRouteOptions& Options)
{
    if (!Request.PeerAddress.IsValid())
    {
        if (!bWarnedUnknownPeer) { bWarnedUnknownPeer = true; UE_LOG(LogMKIBridge, Warning, TEXT("The HTTP server did not report a peer address; relying on the loopback listener bind")); }
    }
    else if (!IsLoopbackAddressText(Request.PeerAddress->ToString(false)))
    {
        UE_LOG(LogMKIBridge, Warning, TEXT("Rejected a request from non-loopback peer %s"), *Request.PeerAddress->ToString(false));
        OnComplete(ErrorResponse(TEXT("forbidden_peer"), TEXT("The bridge accepts requests only from this machine (loopback)."), EHttpServerResponseCodes::Forbidden));
        return true;
    }
    if (Options.bAuthenticated && !IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }
    // Abusive request rates are refused predictably, before any parsing (#8).
    { int32 RetryAfterMs = 0; if (!AdmitRequest(Options.bMutation, RetryAfterMs))
    { const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>(); Body->SetBoolField(TEXT("ok"), false); Body->SetStringField(TEXT("error"), TEXT("rate_limited")); Body->SetStringField(TEXT("message"), TEXT("Too many requests; retry after retryAfterMs.")); Body->SetBoolField(TEXT("retryable"), true); Body->SetNumberField(TEXT("retryAfterMs"), RetryAfterMs);
      OnComplete(JsonResponse(Body, static_cast<EHttpServerResponseCodes>(429))); return true; } }
    // Receipts belong to the loaded world: a load clears them and restores the session's persisted ones (#38).
    EnsureReceiptScope(GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
    if (!Options.bMutation) return (this->*Method)(Request, OnComplete);

    TSharedPtr<FJsonObject> Json; FString Text;
    if (!ReadJsonBody(Request, Json, &Text)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    bool Confirm = false; FString RequestId;
    Json->TryGetBoolField(TEXT("confirm"), Confirm); Json->TryGetStringField(TEXT("requestId"), RequestId);
    if (!Confirm) return (this->*Method)(Request, OnComplete);
    // #7: a confirmed mutation needs its exact scope; previews stay open to observation.
    { const FString Scope = RequiredScope(Options.Path); if (!HasScope(Scope))
    { OnComplete(ErrorResponse(TEXT("scope_required"), FString::Printf(TEXT("This token lacks the \"%s\" scope that %s needs; grant it in Saved/MKIBridge/scopes.txt."), *Scope, *Options.Path), EHttpServerResponseCodes::Forbidden)); return true; } }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (World && World->GetNetMode() == NM_Client)
    { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Confirmed mutations are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    if (RequestId.IsEmpty())
    {
        if (Options.bRequestIdRequiredOnConfirm) { OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("A confirmed request requires a requestId."), EHttpServerResponseCodes::BadRequest)); return true; }
        const FString AuditRoute = Options.Path;
        return (this->*Method)(Request, [this, OnComplete, AuditRoute](TUniquePtr<FHttpServerResponse>&& Response) { AppendAudit(AuditRoute, FString(), Response.IsValid() ? static_cast<int32>(Response->Code) : 500); OnComplete(MoveTemp(Response)); });
    }
    if (RequestId.Len() > 128 || RequestId.TrimStartAndEnd() != RequestId)
    { OnComplete(ErrorResponse(TEXT("invalid_request_id"), TEXT("requestId must be at most 128 characters without surrounding whitespace."), EHttpServerResponseCodes::BadRequest)); return true; }

    // One contract for every confirmed mutation: a requestId already executing answers request_in_flight, a
    // completed one replays its recorded result, and reusing it for a different route or body is a conflict.
    const uint32 Fingerprint = FCrc::StrCrc32(*Text);
    if (const FMutationReceipt* Existing = MutationReceipts.Find(RequestId))
    {
        if (Existing->Path != Options.Path || Existing->Fingerprint != Fingerprint)
        { OnComplete(ErrorResponse(TEXT("request_id_conflict"), TEXT("This requestId was already used for a different request."), EHttpServerResponseCodes::Conflict)); return true; }
        if (Existing->bInFlight)
        { OnComplete(ErrorResponse(TEXT("request_in_flight"), TEXT("This requestId is still being executed; retry to receive its result."), EHttpServerResponseCodes::Conflict)); return true; }
        TSharedPtr<FJsonObject> Replay; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Existing->Body);
        if (!FJsonSerializer::Deserialize(Reader, Replay) || !Replay.IsValid()) Replay = MakeShared<FJsonObject>();
        Replay->SetBoolField(TEXT("replayed"), true);
        Replay->SetBoolField(TEXT("duplicatePrevented"), true);
        if (Existing->bRestored) Replay->SetBoolField(TEXT("restored"), true);
        OnComplete(JsonResponse(Replay.ToSharedRef(), static_cast<EHttpServerResponseCodes>(Existing->Code)));
        return true;
    }
    FMutationReceipt& Receipt = MutationReceipts.Add(RequestId);
    Receipt.Path = Options.Path; Receipt.Fingerprint = Fingerprint; Receipt.bInFlight = true;

    const TSharedRef<FBridgeRequestTicket> Ticket = MakeShared<FBridgeRequestTicket>();
    Ticket->RequestId = RequestId; Ticket->Respond = OnComplete;
    Ticket->Watchdog = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, Ticket, AbandonedOptions = Options](float)
    {
        if (Ticket->bDone) return false;
        // Deferred work never ran (or is stuck). It checks bAbandoned before mutating, so nothing is built later
        // and the requestId is free to retry.
        Ticket->bDone = true; Ticket->bAbandoned = true;
        CompleteMutationReceipt(Ticket->RequestId, static_cast<int32>(EHttpServerResponseCodes::ServiceUnavail), FString());
        Ticket->Respond(ErrorResponse(TEXT("mutation_abandoned"), AbandonedOptions.AbandonedMessage.IsEmpty() ? FString(TEXT("The deferred mutation did not complete (for example the world was unloaded); nothing was built and the requestId may be retried.")) : AbandonedOptions.AbandonedMessage, EHttpServerResponseCodes::ServiceUnavail));
        return false;
    }), Options.WatchdogSeconds > 0.0f ? Options.WatchdogSeconds : MutationWatchdogSeconds);
    const FString AuditRoute = Options.Path;
    const FHttpResultCallback Wrapped = [this, Ticket, AuditRoute](TUniquePtr<FHttpServerResponse>&& Response)
    {
        if (Ticket->bDone) return;
        AppendAudit(AuditRoute, Ticket->RequestId, Response.IsValid() ? static_cast<int32>(Response->Code) : 500);
        Ticket->bDone = true;
        FTSTicker::GetCoreTicker().RemoveTicker(Ticket->Watchdog);
        FString Body;
        if (Response.IsValid() && Response->Body.Num() > 0)
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
            Body = FString(Converted.Length(), Converted.Get());
        }
        CompleteMutationReceipt(Ticket->RequestId, Response.IsValid() ? static_cast<int32>(Response->Code) : 500, Body);
        Ticket->Respond(MoveTemp(Response));
    };
    // Restore rather than clear: a batch dispatches its operations through this wrapper while its own ticket is active.
    const TSharedPtr<FBridgeRequestTicket> Outer = ActiveTicket;
    ActiveTicket = Ticket;
    const bool bHandled = (this->*Method)(Request, Wrapped);
    ActiveTicket = Outer;
    return bHandled;
}

bool FMKIBridgeModule::TickStatusIndicator(float)
{
    if (!GEngine || !GEngine->GameViewport)
    {
        return true;
    }

    if (CVarMKIBridgeShowStatus.GetValueOnGameThread() == 0)
    {
        RemoveStatusIndicator();
        return true;
    }

    if (!StatusIndicatorRoot.IsValid())
    {
        TSharedRef<SOverlay> Overlay = SNew(SOverlay);
        Overlay->AddSlot()
        .HAlign(HAlign_Right)
        .VAlign(VAlign_Top)
        .Padding(FMargin(16.0f, 16.0f, 16.0f, 0.0f))
        [
            SNew(SBorder)
            .Padding(FMargin(10.0f, 5.0f))
            .BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.78f))
            [
                SAssignNew(StatusIndicatorText, STextBlock)
                .ShadowOffset(FVector2D(1.0f, 1.0f))
                .ShadowColorAndOpacity(FLinearColor::Black)
            ]
        ];
        StatusIndicatorRoot = Overlay;
        GEngine->GameViewport->AddViewportWidgetContent(Overlay, 10000);
    }

    UWorld* World = GEngine->GetCurrentPlayWorld();
    APlayerController* Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
    const bool bWorldReady = Player && Controller && Controller->GetPawn() == Player;
    const FString Status = bWorldReady
        ? FString::Printf(TEXT("MKI Bridge Ready  |  localhost:%u"), Port)
        : FString::Printf(TEXT("MKI Bridge Active  |  localhost:%u  |  world loading"), Port);
    const FLinearColor Color = bWorldReady
        ? FLinearColor(0.31f, 0.86f, 0.47f, 1.0f)
        : FLinearColor(1.0f, 0.75f, 0.24f, 1.0f);
    StatusIndicatorText->SetText(FText::FromString(Status));
    StatusIndicatorText->SetColorAndOpacity(FSlateColor(Color));
    return true;
}

void FMKIBridgeModule::RemoveStatusIndicator()
{
    if (StatusIndicatorRoot.IsValid() && GEngine && GEngine->GameViewport)
    {
        GEngine->GameViewport->RemoveViewportWidgetContent(StatusIndicatorRoot.ToSharedRef());
    }
    StatusIndicatorText.Reset();
    StatusIndicatorRoot.Reset();
}

void FMKIBridgeModule::ShutdownModule()
{
    if (StatusIndicatorTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(StatusIndicatorTickerHandle);
        StatusIndicatorTickerHandle.Reset();
    }
    RemoveStatusIndicator();
    if (EventsTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(EventsTickerHandle);
        EventsTickerHandle.Reset();
    }
    // Open long-polls are dropped with the router; their connections close without an answer.
    EventWaiters.Empty();
    FlushReceiptStore();
    if (Router.IsValid())
    {
        for (const FHttpRouteHandle& Handle : RouteHandles)
        {
            Router->UnbindRoute(Handle);
        }
    }
    RouteHandles.Empty();
    Router.Reset();
}

bool FMKIBridgeModule::HandleHealth(const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
{
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("service"), TEXT("satisfactory-mki"));
    Body->SetStringField(TEXT("apiVersion"), TEXT("v1"));
    Body->SetStringField(TEXT("modVersion"), BridgeModVersion());
    Body->SetStringField(TEXT("bindAddress"), ConfiguredBindAddress);
    Body->SetNumberField(TEXT("port"), Port);
    Body->SetBoolField(TEXT("loopbackOnly"), true);
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleCapabilities(const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
{
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("apiVersion"), TEXT("v1"));
    Body->SetStringField(TEXT("modVersion"), BridgeModVersion());
    { const TSet<FString> Scopes = GrantedScopes(); TArray<TSharedPtr<FJsonValue>> Names; for (const FString& Scope : Scopes) Names.Add(MakeShared<FJsonValueString>(Scope));
      Body->SetBoolField(TEXT("writesEnabled"), Scopes.Num() > 0 && !(Scopes.Num() == 1 && Scopes.Contains(TEXT("observe"))));
      Body->SetArrayField(TEXT("scopes"), Names); }
    Body->SetArrayField(TEXT("read"), {
        MakeShared<FJsonValueString>(TEXT("health")),
        MakeShared<FJsonValueString>(TEXT("capabilities")),
        MakeShared<FJsonValueString>(TEXT("buildables")),
        MakeShared<FJsonValueString>(TEXT("resource_nodes")),
        MakeShared<FJsonValueString>(TEXT("player")),
        MakeShared<FJsonValueString>(TEXT("saves")),
        MakeShared<FJsonValueString>(TEXT("machine_connections")),
        MakeShared<FJsonValueString>(TEXT("power_connections")),
        MakeShared<FJsonValueString>(TEXT("placement_status")),
        MakeShared<FJsonValueString>(TEXT("site_survey")),
        MakeShared<FJsonValueString>(TEXT("terrain_column_probe")), MakeShared<FJsonValueString>(TEXT("hologram_probe")), MakeShared<FJsonValueString>(TEXT("vehicles")),
        MakeShared<FJsonValueString>(TEXT("player_vitals")),
        MakeShared<FJsonValueString>(TEXT("world_volumes")),
        MakeShared<FJsonValueString>(TEXT("machines")),
        MakeShared<FJsonValueString>(TEXT("power_circuits")),
        MakeShared<FJsonValueString>(TEXT("player_inventory")),
        MakeShared<FJsonValueString>(TEXT("catalog_buildables")),
        MakeShared<FJsonValueString>(TEXT("catalog_recipes")),
        MakeShared<FJsonValueString>(TEXT("progression")),
        MakeShared<FJsonValueString>(TEXT("events"))
    });
    Body->SetArrayField(TEXT("write"), {
        MakeShared<FJsonValueString>(TEXT("place")),
        MakeShared<FJsonValueString>(TEXT("connect")),
        MakeShared<FJsonValueString>(TEXT("player_transform")),
        MakeShared<FJsonValueString>(TEXT("load_save")),
        MakeShared<FJsonValueString>(TEXT("save_game")),
        MakeShared<FJsonValueString>(TEXT("player_respawn")),
        MakeShared<FJsonValueString>(TEXT("batch")),
        MakeShared<FJsonValueString>(TEXT("set_machine_recipe")),
        MakeShared<FJsonValueString>(TEXT("machine_clock")),
        MakeShared<FJsonValueString>(TEXT("splitter_rules")),
        MakeShared<FJsonValueString>(TEXT("dismantle")),
        MakeShared<FJsonValueString>(TEXT("place_spline_road")),
        MakeShared<FJsonValueString>(TEXT("build_gun"))
    });
    Body->SetArrayField(TEXT("connectionKinds"), {
        MakeShared<FJsonValueString>(TEXT("belt")),
        MakeShared<FJsonValueString>(TEXT("power")),
        MakeShared<FJsonValueString>(TEXT("pipe")),
        MakeShared<FJsonValueString>(TEXT("hypertube")),
        MakeShared<FJsonValueString>(TEXT("rail"))
    });
    Body->SetBoolField(TEXT("exactConnectionPorts"), true);
    Body->SetStringField(TEXT("placementSupportProbe"), TEXT("seam_nudge_v1"));
    Body->SetArrayField(TEXT("planned"), {
        MakeShared<FJsonValueString>(TEXT("batch_plan")),
        MakeShared<FJsonValueString>(TEXT("inventory_transfer"))
    });
    // Companion mods and the features they gate. A feature is available only while every companion it needs is compatible.
    TArray<TSharedPtr<FJsonValue>> Companions;
    const TSharedRef<FJsonObject> Features = MakeShared<FJsonObject>();
    for (const FCompanionDeclaration& Declaration : CompanionDeclarations())
    {
        const FCompanionStatus& Status = CachedCompanionStatus(Declaration);
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Status.Name);
        Item->SetBoolField(TEXT("declared"), Status.bDeclared);
        Item->SetBoolField(TEXT("required"), Status.bRequired);
        Item->SetStringField(TEXT("requiredVersion"), Status.RequiredVersion);
        Item->SetBoolField(TEXT("present"), Status.bPresent);
        if (!Status.Version.IsEmpty()) Item->SetStringField(TEXT("version"), Status.Version);
        Item->SetBoolField(TEXT("compatible"), Status.bCompatible);
        Item->SetStringField(TEXT("mountPoint"), Status.MountPoint);
        TArray<TSharedPtr<FJsonValue>> FeatureNames;
        for (const FString& Feature : Status.Features) FeatureNames.Add(MakeShared<FJsonValueString>(Feature));
        Item->SetArrayField(TEXT("features"), FeatureNames);
        if (!Status.Reason.IsEmpty()) Item->SetStringField(TEXT("reason"), Status.Reason);
        Companions.Add(MakeShared<FJsonValueObject>(Item));
        for (const FString& Feature : Status.Features)
        {
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetBoolField(TEXT("available"), Status.bCompatible);
            Entry->SetArrayField(TEXT("requires"), { MakeShared<FJsonValueString>(Status.Name) });
            if (!Status.bCompatible) Entry->SetStringField(TEXT("reason"), FString::Printf(TEXT("%s: %s"), *Status.Name, *Status.Reason));
            Features->SetObjectField(Feature, Entry);
        }
    }
    // Bridge-native contract features. Callers gate newer behaviour on these flags and fail closed when absent.
    for (const TCHAR* Feature : BridgeNativeFeatures())
    {
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetBoolField(TEXT("available"), true);
        Entry->SetArrayField(TEXT("requires"), TArray<TSharedPtr<FJsonValue>>{});
        Features->SetObjectField(Feature, Entry);
    }
    const TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
    Contract->SetStringField(TEXT("errorShape"), TEXT("ok_error_message_retryable_v1"));
    Contract->SetStringField(TEXT("idempotency"), TEXT("replay_v1"));
    Contract->SetNumberField(TEXT("maximumMutationReceipts"), MaximumMutationReceipts);
    Contract->SetNumberField(TEXT("mutationWatchdogSeconds"), MutationWatchdogSeconds);
    Contract->SetNumberField(TEXT("maximumSurveySamples"), MaximumSurveySamples);
    Contract->SetBoolField(TEXT("loopbackOnly"), true);
    Contract->SetBoolField(TEXT("caseSensitiveIds"), true);
    Contract->SetStringField(TEXT("receiptPersistence"), TEXT("session_side_file_play_time_v1"));
    Contract->SetNumberField(TEXT("maximumEventBacklog"), MaximumEventBacklog);
    Contract->SetNumberField(TEXT("maximumEventWaitMs"), MaximumEventWaitMs);
    Contract->SetNumberField(TEXT("eventSampleIntervalSeconds"), EventSampleIntervalSeconds);
    Body->SetObjectField(TEXT("contract"), Contract);
    Body->SetArrayField(TEXT("companions"), Companions);
    Body->SetObjectField(TEXT("features"), Features);
    OnComplete(JsonResponse(Body));
    return true;
}

IMPLEMENT_MODULE(FMKIBridgeModule, MKIBridge)

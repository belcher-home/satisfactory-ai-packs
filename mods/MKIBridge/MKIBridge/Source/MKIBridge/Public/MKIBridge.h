#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"
#include "HttpServerConstants.h"
#include "IHttpRouter.h"
#include "Modules/ModuleManager.h"

struct FHttpServerRequest;
struct FBridgeRequestTicket;
struct FBatchRun;
class AFGBuildable;
class FJsonObject;
class STextBlock;
class SWidget;
class UWorld;

class FMKIBridgeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    bool TickStatusIndicator(float DeltaTime);
    void RemoveStatusIndicator();
    bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleCapabilities(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleResourceNodes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceResourceWell(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceGeothermal(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlayerState(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    /** Player safety (#109): health, death, and the volumes around the player; a dump of world volumes; respawn. */
    bool HandlePlayerVitals(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleWorldVolumes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlayerRespawn(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleListSaves(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleLoadSave(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleSaveGame(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceStructure(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleConnectStructures(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleSetPlayerTransform(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleValidatePlacement(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleColumnProbe(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleConveyors(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleRegionLoaded(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleHologramProbe(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceFoundation(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceFoundationGrid(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleSetMachineRecipe(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleMachineClock(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleSplitterRules(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleMachineConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePowerConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceFactoryBuilding(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceMiner(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceConveyorSupport(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceSplineRoad(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceSplineRoadHologram(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlaceWithBuildGun(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleConnectBeltHologram(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleConnectSplineHologram(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, const FString& Kind);
    bool HandleConnectPower(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleDeleteBuildable(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlacementStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleBuildablesQuery(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleMachines(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleBatch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    void RunBatchStep(TSharedRef<FBatchRun> Run);
    void FailBatch(TSharedRef<FBatchRun> Run, const FString& FailedOpId);
    /** Drops a completed receipt so its requestId executes again (used after an atomic batch rolls back). */
    void ForgetMutationReceipt(const FString& RequestId);
    bool HandlePowerCircuits(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandlePlayerInventory(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleCatalogBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleCatalogRecipes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleProgression(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    /** Rescans every buildable and advances the listing revision for anything added, changed, or removed. */
    void RefreshBuildableRevisions(UWorld* World);

    // Events (#117): a bounded, epoch-scoped ring of world and bridge events served by a long-poll route.
    bool HandleEvents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleVehicles(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleVehicleRoute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleVehiclePathRevalidate(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleGameRules(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleDockingStation(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleInventoryTransfer(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleInventories(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    /** Appends an event, starting a new epoch first when the world changed, and answers any waiter it satisfies. */
    void EmitBridgeEvent(UWorld* World, const TCHAR* Type, const TSharedRef<FJsonObject>& Data);
    /** Starts a new event epoch when the play world changed; waiters of the old epoch are answered with resync. */
    void EnsureEventScope(UWorld* World);
    /** Diffs buildables, machine states, and circuit fuses against the previous sample and emits the changes. */
    void SampleWorldEvents(UWorld* World);
    /** Answers waiters that have matching events, a stale epoch, or an expired deadline. */
    void ServeEventWaiters();
    bool TickEvents(float DeltaTime);
    /** The events after `Since` matching `Types`, or null when there are none yet and the wait has not expired. */
    TSharedPtr<FJsonObject> CollectEvents(const FString& Epoch, int64 Since, const TArray<FString>& Types, bool bTimedOut) const;
    struct FBridgeEvent
    {
        int64 Seq = 0;
        FString Type;
        FDateTime Time;
        TSharedPtr<FJsonObject> Data;
    };
    struct FEventWaiter
    {
        FString Epoch;
        int64 Since = 0;
        TArray<FString> Types;
        double Deadline = 0.0;
        FHttpResultCallback Respond;
    };
    TArray<FBridgeEvent> Events;
    TArray<TSharedRef<FEventWaiter>> EventWaiters;
    TWeakObjectPtr<UWorld> EventsWorld;
    FString EventsEpoch;
    int64 EventSeq = 0;
    /** Sequence number of the newest event dropped by the bound; `since` below it cannot be answered. */
    int64 OldestDroppedEventSeq = 0;
    double LastEventsPollSeconds = -1.0e9;
    double LastEventsSampleSeconds = -1.0e9;
    bool bEventBaselineTaken = false;
    TMap<FString, FString> MachineStateSamples;
    TMap<int32, bool> CircuitFuseSamples;
    FTSTicker::FDelegateHandle EventsTickerHandle;

    // Receipt persistence (#38): completed receipts are kept per save session and restored on load up to the
    // loaded play time, so a retry after a game restart replays instead of building twice.
    /** Switches the receipt scope when the play world changed: clears memory receipts, then restores the session's. */
    void EnsureReceiptScope(UWorld* World);
    /** Writes the current scope's completed receipts to its side file when they changed. */
    void FlushReceiptStore();
    TWeakObjectPtr<UWorld> ReceiptScopeWorld;
    bool bReceiptScopeInitialized = false;
    /** Side file of the current scope; empty when the session is unnamed (receipts are then memory-only). */
    FString ReceiptStorePath;
    FString ReceiptSessionName;
    bool bReceiptStoreDirty = false;
    /** The scope's side file was resolved (its game state was ready) and restored. */
    bool bReceiptStoreResolved = false;
    double LastReceiptFlushSeconds = 0.0;
    int32 RestoredReceiptCount = 0;
    int32 DiscardedReceiptCount = 0;

    /** What a single conventional placement built, captured when its confirmed request completed. */
    struct FPlacementOrigin
    {
        FString ActorName;
        FTransform Transform;
        TWeakObjectPtr<UClass> Class;
        /** Lightweight instances of the same class already at the transform at completion; never the placement's. */
        TArray<FString> PreexistingIds;
    };
    /** The actor while it exists, else the single new lightweight instance at its transform; OutResolution says which. */
    FString ResolvePlacementOrigin(UWorld* World, const FPlacementOrigin& Origin, FString& OutResolution);
    /** Valid lightweight instance indices of a class, taken before a construction that the game may convert at once. */
    static TSet<int32> ValidLightweightIndices(UWorld* World, UClass* Class);
    /**
     * Record a placement's origin from its actor, which may already be pending kill because the game converted it to a
     * lightweight instance during construction. With IndicesBefore, only instances that existed before construction
     * count as pre-existing; without it, every instance at the transform does.
     */
    void NotePlacementOrigin(const FString& RequestId, AActor* Actor, const TSet<int32>* IndicesBefore);
    struct FBuildableSnapshot
    {
        uint32 Fingerprint = 0;
        int64 Revision = 0;
        FString ClassPath;
        FVector Location = FVector::ZeroVector;
    };
    struct FBuildableTombstone
    {
        FString Id;
        FString ClassPath;
        FVector Location = FVector::ZeroVector;
        int64 Revision = 0;
    };
    bool IsAuthorized(const FHttpServerRequest& Request) const;
    bool LoadOrCreateAuthenticationToken();

    using FRouteMethod = bool (FMKIBridgeModule::*)(const FHttpServerRequest&, const FHttpResultCallback&);
    /** How the shared route wrapper treats a route before its handler runs. */
    struct FRouteOptions
    {
        /** Route path, also the scope of a mutation receipt. */
        FString Path;
        bool bAuthenticated = true;
        /** A confirmed request with a requestId is executed at most once and its result replayed on retry. */
        bool bMutation = false;
        /** A confirmed request without a requestId is rejected before the handler runs. */
        bool bRequestIdRequiredOnConfirm = false;
        /** Watchdog for a confirmed mutation; 0 uses the default. */
        float WatchdogSeconds = 0.0f;
        /** Message of the mutation_abandoned answer; empty uses the default. */
        FString AbandonedMessage;
    };
    void BindBridgeRoute(const TCHAR* Path, EHttpServerRequestVerbs Verb, FRouteMethod Method, FRouteOptions Options);
    bool DispatchRoute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, FRouteMethod Method, const FRouteOptions& Options);
    /** Records a finished mutation: a 2xx result is kept for replay, any other result frees the requestId for retry. */
    void CompleteMutationReceipt(const FString& RequestId, int32 Code, const FString& Body);
    /** Exact, case-sensitive lookup of a conventional buildable through a name index refreshed on a miss. */
    AFGBuildable* FindBuildableById(UWorld* World, const FString& Id);

    /** Idempotency receipt of one confirmed mutation, keyed by requestId. */
    struct FMutationReceipt
    {
        FString Path;
        uint32 Fingerprint = 0;
        bool bInFlight = true;
        int32 Code = 0;
        FString Body;
        /** Session play time when the mutation completed; a load restores only receipts older than the save. */
        double PlayDurationSeconds = 0.0;
        FDateTime CompletedAt;
        /** Restored from the side file after a load; its live actor bookkeeping is gone. */
        bool bRestored = false;
    };
    TMap<FString, FMutationReceipt> MutationReceipts;
    /** Every bound route by path, so a batch can run its operations through the same wrapper. */
    TMap<FString, TPair<FRouteMethod, FRouteOptions>> RouteTable;
    /** Completion order of MutationReceipts, oldest first, for bounded eviction. */
    TArray<FString> MutationReceiptOrder;
    /** Ticket of the mutation whose handler is running synchronously; deferred work captures it. */
    TSharedPtr<FBridgeRequestTicket> ActiveTicket;
    TMap<FString, TWeakObjectPtr<AFGBuildable>> BuildableIndex;
    FString ConfiguredBindAddress;
    bool bWarnedUnknownPeer = false;
    /** Request-rate windows (#8): the start of the current one-second window and the requests counted in it, reads and mutations apart. */
    double RateWindowStart = 0.0; int32 RateWindowReads = 0; int32 RateWindowMutations = 0;
    /** False when the request would exceed its class's per-second limit; RetryAfterMs is when the window turns. */
    bool AdmitRequest(bool bMutation, int32& RetryAfterMs);
    /** Appends one line per confirmed mutation to Saved/MKIBridge/audit.log: time, route, requestId and HTTP status (never the token or the body). */
    void AppendAudit(const FString& Route, const FString& RequestId, int32 Status);
    TSharedPtr<IHttpRouter> Router;
    TArray<FHttpRouteHandle> RouteHandles;
    FString AuthenticationToken;
    /** The scope a confirmed request to Path needs (#7): build, connect, configure, interact, move, dismantle or admin. */
    static FString RequiredScope(const FString& Path);
    /** The scopes Saved/MKIBridge/scopes.txt grants, read on each check so an edit applies at once; "all" grants every scope. */
    TSet<FString> GrantedScopes() const;
    bool HasScope(const FString& Scope) const;
    TMap<FString, TWeakObjectPtr<AActor>> PlacementReceipts;
    TMap<FString, TArray<FTransform>> PlacementReceiptTransforms;
    TMap<FString, TWeakObjectPtr<UClass>> PlacementReceiptClasses;
    /** Completed save-game responses by requestId, so a retried request replays its result instead of saving again. */
    TMap<FString, TSharedPtr<class FJsonObject>> SaveReceipts;
    /** requestId of the save currently being written; empty when no bridge save is in flight. */
    FString SaveInFlightRequestId;
    TMap<FString, FPlacementOrigin> PlacementOrigins;
    TMap<FString, FBuildableSnapshot> BuildableSnapshots;
    TArray<FBuildableTombstone> BuildableTombstones;
    TWeakObjectPtr<UWorld> BuildableSnapshotWorld;
    FString BuildablesEpoch;
    int64 BuildablesRevision = 0;
    /** Revision of the newest tombstone dropped by the bound; `since` below it cannot be answered incrementally. */
    int64 OldestRetainedTombstoneRevision = 0;
    FTSTicker::FDelegateHandle StatusIndicatorTickerHandle;
    TSharedPtr<SWidget> StatusIndicatorRoot;
    TSharedPtr<STextBlock> StatusIndicatorText;
    uint32 Port = 8081;
};

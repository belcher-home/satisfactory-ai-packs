#pragma once

// MKI Bridge internals shared by every operation family: engine includes, limits, asset paths, and helpers.
// Split from the single MKIBridge.cpp without behavior changes (#44).

#include "MKIBridge.h"
#include "Containers/Ticker.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "FGCharacterPlayer.h"
#include "Engine/CollisionProfile.h"
#include "Kismet/GameplayStatics.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableWire.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildableGeneratorGeoThermal.h"
#include "Buildables/FGBuildableGenerator.h"
#include "FGPowerInfoComponent.h"
#include "FGCircuitSubsystem.h"
#include "FGPowerCircuit.h"
#include "FGRecipeManager.h"
#include "FGSchematicManager.h"
#include "FGSchematic.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGItemDescriptor.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildablePassthroughBase.h"
#include "Buildables/FGBuildableFrackingActivator.h"
#include "Buildables/FGBuildableFrackingExtractor.h"
#include "Resources/FGResourceNode.h"
#include "Resources/FGResourceNodeBase.h"
#include "Resources/FGResourceNodeFrackingCore.h"
#include "Resources/FGResourceNodeFrackingSatellite.h"
#include "FGResourceNodeGeyser.h"
#include "EngineUtils.h"
#include "FGWaterVolume.h"
#include "FGDestructibleActor.h"
#include "Resources/FGResourceDeposit.h"
#include "FGFoliageRemovalSubsystem.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "Engine/OverlapResult.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "AbstractInstanceManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "FGFactoryConnectionComponent.h"
#include "FGCircuitConnectionComponent.h"
#include "FGPowerConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGPipeConnectionComponentHyper.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "FGBlueprintSubsystem.h"
#include "FGBlueprintSettings.h"
#include "FGFactoryBlueprintTypes.h"
#include "Resources/FGOverflowDescriptor.h"
#include "Resources/FGWildCardDescriptor.h"
#include "Resources/FGNoneDescriptor.h"
#include "Resources/FGAnyUndefinedDescriptor.h"
#include "FGTrainPlatformConnection.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "FGSaveSystem.h"
#include "FGSaveSession.h"
#include "FGGameState.h"
#include "HAL/FileManager.h"
#include "FGConstructDisqualifier.h"
#include "Hologram/FGHologram.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGConveyorBeltHologram.h"
#include "Hologram/FGConveyorLiftHologram.h"
#include "Hologram/FGConveyorPoleHologram.h"
#include "Hologram/FGPoleHologram.h"
#include "Buildables/FGBuildablePipeline.h"
#include "FGBuildablePipelineFlowIndicator.h"
#include "Hologram/FGSplineHologram.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "Hologram/FGPassthroughHologram.h"
#include "Hologram/FGGeoThermalGeneratorHologram.h"
#include "ItemAmount.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "IPAddress.h"
#include "Misc/Crc.h"
#include "Interfaces/IPluginManager.h"
#include "Util/SemVersion.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "FGPlayerController.h"
#include "FGHealthComponent.h"
#include "FGVolumeMapArea.h"
#include "FGMapArea.h"
#include "FGMapFunctionLibrary.h"
#include "GameFramework/Volume.h"
#include "GameFramework/WorldSettings.h"
#include "Components/BrushComponent.h"
#include "PhysicsEngine/BodySetup.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMKIBridge, Log, All);

namespace MKIBridgeInternal
{
    extern TAutoConsoleVariable<int32> CVarMKIBridgeShowStatus;

    extern TAutoConsoleVariable<float> CVarMaximumPlacementDistanceCm;

    extern TAutoConsoleVariable<float> CVarMaximumObservationDistanceCm;

    double GetMaximumPlacementDistanceCm();

    double GetMaximumObservationDistanceCm();

    constexpr double FoundationWorldGridSizeCm = 800.0;

    constexpr double FoundationVerticalGridSizeCm = 100.0;

    constexpr double BuildableFoundationGridSizeCm = 100.0;

    constexpr double WorldGridYawStepDegrees = 90.0;

    double SnapToIncrement(double Value, double Increment);

    // Actor origins are not uniformly located at the support surface. Keep
    // these measured profiles centralized so callers specify the foundation
    // surface height rather than memorizing per-asset origin conventions.
    double GetSupportSurfaceToActorOriginOffsetCm(const FString& Type);

    inline const TCHAR* FoundationClassPath = TEXT("/Game/FactoryGame/Buildable/Building/Foundation/Build_Foundation_8x4_01.Build_Foundation_8x4_01_C");

    inline const TCHAR* FoundationRecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Foundations/Recipe_Foundation_8x4_01.Recipe_Foundation_8x4_01_C");

    inline const TCHAR* ConstructorClassPath = TEXT("/Game/FactoryGame/Buildable/Factory/ConstructorMk1/Build_ConstructorMk1.Build_ConstructorMk1_C");

    inline const TCHAR* ConstructorRecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_ConstructorMk1.Recipe_ConstructorMk1_C");

    inline const TCHAR* ConveyorBeltMk1ClassPath = TEXT("/Game/FactoryGame/Buildable/Factory/ConveyorBeltMk1/Build_ConveyorBeltMk1.Build_ConveyorBeltMk1_C");

    inline const TCHAR* ConveyorBeltMk1RecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_ConveyorBeltMk1.Recipe_ConveyorBeltMk1_C");

    inline const TCHAR* StorageContainerMk1ClassPath = TEXT("/Game/FactoryGame/Buildable/Factory/StorageContainerMk1/Build_StorageContainerMk1.Build_StorageContainerMk1_C");

    inline const TCHAR* StorageContainerMk1RecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_StorageContainerMk1.Recipe_StorageContainerMk1_C");

    inline const TCHAR* BiomassGeneratorClassPath = TEXT("/Game/FactoryGame/Buildable/Factory/GeneratorBiomass/Build_GeneratorBiomass_Automated.Build_GeneratorBiomass_Automated_C");

    inline const TCHAR* BiomassGeneratorRecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_GeneratorBiomass_Automated.Recipe_GeneratorBiomass_Automated_C");

    inline const TCHAR* PowerLineClassPath = TEXT("/Game/FactoryGame/Buildable/Factory/PowerLine/Build_PowerLine.Build_PowerLine_C");

    inline const TCHAR* PowerLineRecipePath = TEXT("/Game/FactoryGame/Recipes/Buildings/Recipe_PowerLine.Recipe_PowerLine_C");

    inline const TCHAR* BridgePluginName = TEXT("MKIBridge");

    inline const TCHAR* FactoryGameAssetRoot = TEXT("/Game/FactoryGame/");

    /**
     * First-party companion mods released from this repository. Each is declared in the bridge .uplugin as a
     * required (non-optional) plugin dependency with an SML SemVersion range, so the engine and mod managers refuse
     * to load the bridge without a compatible version. The companions do not depend on the bridge. The status is
     * still resolved at runtime so capabilities report the contract and features fail closed if it is ever broken.
     */
    struct FCompanionDeclaration { const TCHAR* Name; TArray<FString> Features; };

    const TArray<FCompanionDeclaration>& CompanionDeclarations();

    struct FCompanionStatus
    {
        FString Name, MountPoint, RequiredVersion, Version, Reason;
        TArray<FString> Features;
        bool bDeclared = false, bRequired = false, bPresent = false, bCompatible = false;
    };

    TSharedPtr<FJsonObject> ReadPluginDescriptorJson(const TSharedPtr<IPlugin>& Plugin);

    /** Resolve a companion from the descriptors on disk: the range the bridge declares and the version that is loaded. */
    FCompanionStatus ResolveCompanion(const FCompanionDeclaration& Declaration);

    /**
     * Companion status is resolved from descriptors on disk once per process: plugins cannot be mounted or replaced
     * while the game runs, so every later placement and capabilities request reuses the same answer.
     */
    const FCompanionStatus& CachedCompanionStatus(const FCompanionDeclaration& Declaration);

    /**
     * Generic placement accepts base-game assets and assets mounted by a compatible companion. Returns false with an
     * error code and message otherwise (`companion_unavailable` when the path belongs to a declared companion).
     */
    bool IsPlaceableAssetPath(const FString& Path, FString& OutCode, FString& OutMessage);

    UFGInventoryComponent* GetPlayerInventory(UWorld* World);

    /**
     * The build gun's trace channel (TC_BuildGun), resolved by its configured name: the SDK's FactoryGame stubs do not
     * export TC_BuildGun, so the editor build cannot link it. The shipped DefaultEngine.ini names ECC_GameTraceChannel5
     * "BuildGun", the fallback. Used by the hologram probe and the spline road's build-gun placement.
     */
    ECollisionChannel BuildGunTraceChannel();

    bool CanAffordRecipe(UWorld* World, TSubclassOf<UFGRecipe> Recipe, int32 Multiplier, FString& Missing);

    bool ConsumeRecipeCost(UWorld* World, TSubclassOf<UFGRecipe> Recipe, int32 Multiplier, FString& Missing);

    void RefundRecipeCost(UWorld* World, TSubclassOf<UFGRecipe> Recipe, int32 Multiplier);

    // Canonical lightweight identity, exactly as v1/buildables formats it:
    // lw|<classPath>|<runtimeIndex> with a decimal int32 index and no sign,
    // padding, whitespace, or extra separators. Anything else is rejected.
    bool ParseCanonicalLightweightId(const FString& Id, FString& OutClassPath, int32& OutIndex);
    /**
     * True for an actor the lightweight-buildable subsystem owns rather than one a player built: a temporary standing in
     * for an instance, or one of its pooled actors parked at FLightweightBuildablePool::PoolTransform (20 per lightweight
     * class). These are not buildables: they are never listed, resolved by ID, or dismantled. Dismantling a pooled actor
     * failed a game assert in AFGBuildable::Dismantle_Implementation and crashed the game (live, 2026-10-18).
     */
    bool IsLightweightSubsystemActor(const AFGBuildable* Buildable);

    // Owns the unsaved temporary actor used to represent an exact `lw|` anchor.
    // The lease is shared with the deferred hologram timer, so the actor survives
    // through native validation and is destroyed exactly once on every outcome.
    struct FLightweightConverterLease
    {
        TWeakObjectPtr<UWorld> World;
        AActor* Instigator = nullptr;
        AActor* TemporaryAnchor = nullptr;
        void Release()
        {
            AActor* HeldTemporary = TemporaryAnchor;
            TemporaryAnchor = nullptr;
            if (IsValid(HeldTemporary)) HeldTemporary->Destroy();
            AActor* Held = Instigator;
            Instigator = nullptr;
            if (Held) if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World.Get())) Lightweight->RemoveInstanceConverterInstigator(Held);
        }
        ~FLightweightConverterLease() { Release(); }
    };

    // Whether a lightweight instance can be dismantled (#164), asked of its temporary buildable through the subsystem's
    // bookkeeping: FindTemporaryByBuildableClassAndIndex reuses a temporary the instance already has, and one spawned
    // here by FindOrSpawnBuildableForRuntimeData is removed again with RemoveTemporaryBuildableForInstanceIndex.
    bool LightweightCanDismantle(AFGLightweightBuildableSubsystem* Lightweight, TSubclassOf<AFGBuildable> Class, int32 Index, const FRuntimeBuildableInstanceData* Record);

    // Represents an exact `lw|<class>|<index>` anchor by the engine's dedicated
    // unsaved temporary buildable. It is not registered with the subsystem's
    // converter pool, so same-frame headless snapping cannot return it to the
    // pool sentinel. The lease destroys only this temporary; the original
    // lightweight instance is never destroyed or invalidated.
    AFGBuildable* ResolveLightweightAnchor(UWorld* World, const FString& AnchorId, AActor*& OutTemporaryAnchor);

    // One abstract instance: its visual mesh component and its handle ID there.
    // The instance manager keeps both current through remove-swap, and
    // ResolveHit/ResolveOverlap return them for a collision hit, so the pair
    // names exactly one live instance for the duration of a game-thread request.
    using FSurveyInstanceKey = TPair<const UObject*, uint32>;

    FSurveyInstanceKey SurveyInstanceKey(const FInstanceHandle& Handle);

    // A vertical placement support ray that runs exactly along the seam between
    // two adjacent structures (a request centred on a foundation edge) can pass
    // between them. Before reporting surface_not_found, v1/place re-traces this
    // far inside each horizontal neighbour. Only the support hit handed to the
    // hologram changes; the requested position does not, and the hologram still
    // validates the placement.
    constexpr double SupportProbeNudgeCm = 5.0;

    // The supporting structure of a placement trace, per instance: a lightweight
    // buildable as lw|<classPath>|<index> (the shared AbstractInstanceManager is
    // never reported as the support), else the hit actor.
    TSharedRef<FJsonObject> DescribeSupportHit(UWorld* World, const FHitResult& Hit);

    // Customizer state is orthogonal to the material-specific buildable class. Materials may replace the
    // class/recipe, while swatches, paint finishes and patterns remain per-instance save/replication data.
    // Expose both the descriptor identities and their resolved primitive-data values so automation can verify
    // the same contract the build gun presents without mutating the object. Actor buildables and lightweight
    // runtime records carry the same FFactoryCustomizationData, so both report this one shape.
    TSharedRef<FJsonObject> FactoryCustomizationJson(const FFactoryCustomizationData& Customization);

    // Every response is written condensed: listings are large and the client never needs indentation.
    FString SerializeCondensed(const TSharedRef<FJsonObject>& Object);

    TUniquePtr<FHttpServerResponse> JsonResponse(const TSharedRef<FJsonObject>& Object, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok);

    /**
     * The one error shape every route answers with: {ok:false, error, message, retryable}. `retryable` is true only
     * when repeating the identical request can succeed without the caller changing anything (the world is still
     * loading, the same requestId is still being executed, or deferred work was abandoned before it mutated).
     */
    bool IsRetryableError(const FString& Error, EHttpServerResponseCodes Code);

    TSharedRef<FJsonObject> ErrorBody(const FString& Error, const FString& Message, bool bRetryable);

    TUniquePtr<FHttpServerResponse> ErrorResponse(const FString& Error, const FString& Message, EHttpServerResponseCodes Code);

    /** Decodes a UTF-8 request body and parses it as a JSON object. */
    bool ReadJsonBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& OutJson, FString* OutText = nullptr);

    /** Loopback in any spelling the socket layer prints: 127.0.0.0/8, ::1, and IPv4-mapped 127.x. */
    bool IsLoopbackAddressText(FString Address);

    /** The bridge's own version, read once from its plugin descriptor so health and capabilities never drift from it. */
    const FString& BridgeModVersion();

    /** Finds a valid lightweight record by its canonical `lw|<classPath>|<index>` identity. */
    const FRuntimeBuildableInstanceData* FindLightweightRecord(UWorld* World, const FString& Id, TSubclassOf<AFGBuildable>& OutClass, int32& OutIndex);

    /** Free factory connection of the requested direction whose name matches (any name when Port is empty). */
    UFGFactoryConnectionComponent* FindFreeFactoryConnection(AActor* Actor, EFactoryConnectionDirection Direction, const FString& Port);

    /**
     * A conveyor support (pole, wall or ceiling mount, stackable pole) exposes only snap-only factory connections: it
     * carries belt ends but owns no item flow. Returns its snap point, or null for any buildable with input/output ports.
     */
    UFGFactoryConnectionComponent* FindConveyorSupportSnap(AActor* Actor, const FString& Port);

    /** Distance within which a belt end counts as resting on a conveyor support's snap point. */
    constexpr double ConveyorSupportSnapToleranceCm = 10.0;
    /** A pipe support's built heights (175 to 475 cm in 1 m steps) and the amount the pipeline pole hologram builds below the height it is given (#308, measured live). */
    constexpr double PipeSupportMinHeightCm = 175.0, PipeSupportMaxHeightCm = 475.0, PipeSupportHeightOffsetCm = 25.0;

    /**
     * The free end of a belt already resting on a support snap point, in the requested direction: for a source
     * support, an incoming belt's free output end; for a target support, an outgoing belt's free input end. Chaining
     * through the support connects the new belt to that end, exactly as the build gun does.
     */
    UFGFactoryConnectionComponent* FindBeltEndAtSupport(UWorld* World, const UFGFactoryConnectionComponent* Snap, EFactoryConnectionDirection Direction);

    /** Deferred mutation work that has not answered within this time is abandoned and never mutates afterwards. */
    constexpr float MutationWatchdogSeconds = 25.0f;

    /** Completed mutation receipts kept for replay; the oldest is evicted first. */
    constexpr int32 MaximumMutationReceipts = 1024;

    /** Upper bound on site-survey support samples; larger envelopes are sampled more coarsely and say so. */
    constexpr int32 MaximumSurveySamples = 4096;

    /** A completed receipt whose recorded answer is longer than this is kept in memory but not written to disk. */
    constexpr int32 MaximumPersistedReceiptBodyChars = 65536;

    /** Seconds between side-file writes while receipts are changing; the file is also written on world change and shutdown. */
    constexpr double ReceiptFlushIntervalSeconds = 2.0;

    /** Events kept for `since` catch-up; the oldest is dropped first and a cursor before it must resync. */
    constexpr int32 MaximumEventBacklog = 4096;

    /** Events returned by one events answer; `more` says a further call returns the rest. */
    constexpr int32 MaximumEventsPerResponse = 256;

    /** Longest long-poll wait, and how many waits may be open at once. */
    constexpr int32 MaximumEventWaitMs = 30000;

    constexpr int32 MaximumEventWaiters = 16;

    /** World sampling (buildable, machine, and fuse diffs) runs only while a client polled events recently. */
    constexpr double EventSampleIntervalSeconds = 5.0;

    constexpr double EventSubscriptionSeconds = 60.0;

    /** A single sample with more buildable changes than this emits one buildables_bulk_change instead of one event each. */
    constexpr int32 MaximumBuildableEventsPerSample = 256;
    /** A spline buildable's listing carries at most this many of its points (#17). */
    constexpr int32 MaximumSplinePointsListed = 64;

    const TArray<const TCHAR*>& BridgeEventTypes();

    /** Bridge-native features reported in capabilities.features; each is always available on this build. */
    const TArray<const TCHAR*>& BridgeNativeFeatures();

    /** How much of a buildable `/v1/buildables` reports (`fields=compact|ports|full`; the unfiltered listing is `full`). */
    enum class EBuildableFields : uint8 { Compact, Ports, Full };

    TSharedRef<FJsonObject> BridgeVectorJson(const FVector& V);

    TSharedRef<FJsonObject> BridgeRotationJson(const FRotator& Rotation);

    /** One conventional (actor) buildable. */
    /**
     * Where a buildable is for listing, filtering, and the mutation safety radius (#172): its actor location, except a
     * power line, whose actor stays at the world origin after it is built and whose geometry is its two connections, so
     * it is located at their midpoint.
     */
    FVector BuildableLocation(const AFGBuildable* Buildable);

    TSharedRef<FJsonObject> ActorBuildableJson(AFGBuildable* Buildable, EBuildableFields Fields);

    /**
     * What a placement or connection actually built (#302): id, class, final position and rotation, built height for
     * poles and lifts, every port with its world position, facing, semantics and attachment, and whether it matches the
     * request. Deltas are the request fields the build differs in beyond tolerance (ReceiptDelta).
     */
    TSharedRef<FJsonObject> BuildReceiptJson(AFGBuildable* Built, const TArray<TSharedPtr<FJsonValue>>& Deltas);

    /** One receipt delta: a requested field, its requested and built values, and the tolerance it exceeded. */
    TSharedPtr<FJsonValue> ReceiptDelta(const TCHAR* Field, double Requested, double Built, double ToleranceCm);

    /**
     * One lightweight record. Records own no components: ports and connections stay empty, because inspecting them
     * would require materializing a temporary actor.
     */
    TSharedRef<FJsonObject> LightweightBuildableJson(UClass* Class, int32 Index, const FRuntimeBuildableInstanceData& Data, EBuildableFields Fields);

    /**
     * Change fingerprint of one buildable for the listing revision: class, transform (0.1 cm / 0.01 degree), and,
     * for actors, the connected state of every factory, pipe, and power connection.
     */
    uint32 BuildableFingerprint(const FString& ClassPath, const FTransform& Transform, uint32 ConnectionBits);

    uint32 ActorConnectionBits(AFGBuildable* Buildable);

    /** Parses "x,y,z" (or 6 numbers for bounds) strictly: every part must be a finite number. */
    bool ParseNumberList(const FString& Text, int32 Expected, TArray<double>& Out);

    constexpr int32 DefaultBuildablesPageLimit = 500;

    constexpr int32 MaximumBuildablesPageLimit = 5000;

    constexpr int32 MaximumBuildableTombstones = 4096;

    constexpr int32 MaximumBuildableIdsPerQuery = 500;

    /**
     * Derived machine state shared by /v1/machines and machine_state_changed events. Order matters: standby is a
     * player choice, and no power hides every other cause.
     */
    const TCHAR* BridgeMachineState(UWorld* World, AFGBuildableFactory* Machine);
}

using namespace MKIBridgeInternal;

/**
 * One confirmed, idempotent mutation in flight. The route wrapper registers it before the handler runs (and so
 * before any deferred hologram timer), the handler's deferred work checks `bAbandoned` before it mutates, and the
 * watchdog answers the caller if the deferred work never runs (for example when the world is torn down).
 */
struct FBridgeRequestTicket
{
    FString RequestId;
    FHttpResultCallback Respond;
    FTSTicker::FDelegateHandle Watchdog;
    bool bDone = false;
    bool bAbandoned = false;
};

#include "BridgeInternal.h"
#include "FGConveyorChainActor.h"

// Read-only observation routes: player, world volumes, buildables and queries, machines, power, inventory, catalog, progression, and resource nodes.
// Split from the single MKIBridge.cpp without behavior changes (#44).

AFGBuildable* FMKIBridgeModule::FindBuildableById(UWorld* World, const FString& Id)
{
    if (!World || Id.IsEmpty() || Id.StartsWith(TEXT("lw|"), ESearchCase::CaseSensitive)) return nullptr;
    // FString map keys compare case-insensitively; the exact-case check below keeps the lookup case-sensitive.
    auto Exact = [World, &Id](const TWeakObjectPtr<AFGBuildable>* Entry) -> AFGBuildable*
    {
        AFGBuildable* Buildable = Entry ? Entry->Get() : nullptr;
        return IsValid(Buildable) && Buildable->GetWorld() == World && Buildable->GetName().Equals(Id, ESearchCase::CaseSensitive) ? Buildable : nullptr;
    };
    if (AFGBuildable* Cached = Exact(BuildableIndex.Find(Id))) return Cached;
    BuildableIndex.Reset();
    if (AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World))
        for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef()) if (IsValid(Buildable) && !IsLightweightSubsystemActor(Buildable)) BuildableIndex.Add(Buildable->GetName(), Buildable);
    // Foundations registered through the lightweight path can be missing from GetAllBuildablesRef (see HandleBuildables).
    for (TActorIterator<AFGBuildableFoundation> It(World); It; ++It) if (IsValid(*It) && !IsLightweightSubsystemActor(*It)) BuildableIndex.Add(It->GetName(), *It);
    return Exact(BuildableIndex.Find(Id));
}

bool FMKIBridgeModule::HandlePlayerState(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
    APlayerController* Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    if (!World || !Player || !Controller || Controller->GetPawn() != Player)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No controllable local player is available."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }
    const FVector Location = Player->GetActorLocation();
    const FRotator ControlRotation = Controller->GetControlRotation();
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("worldReady"), true);
    Body->SetBoolField(TEXT("playerReady"), true);
    Body->SetBoolField(TEXT("flying"), Player->CheatToggleFlyModeIsActive());
    // #204: test batches check the build gun ends as they found it.
    Body->SetBoolField(TEXT("buildGunEquipped"), Player->IsBuildGunEquipped());
    const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), Location.X); Position->SetNumberField(TEXT("y"), Location.Y); Position->SetNumberField(TEXT("z"), Location.Z);
    Body->SetObjectField(TEXT("position"), Position);
    Body->SetNumberField(TEXT("yaw"), ControlRotation.Yaw);
    Body->SetNumberField(TEXT("pitch"), ControlRotation.Pitch);
    OnComplete(JsonResponse(Body));
    return true;
}

namespace
{
    TSharedRef<FJsonObject> VitalsVectorJson(const FVector& V)
    {
        const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
        return O;
    }

    /** The display name of a map-area volume's area, read through reflection (the getter is protected). */
    FString MapAreaDisplayName(const AFGVolumeMapArea* Volume)
    {
        const FClassProperty* Property = CastField<FClassProperty>(AFGVolumeMapArea::StaticClass()->FindPropertyByName(TEXT("mMapArea")));
        UClass* Area = Property ? Cast<UClass>(Property->GetObjectPropertyValue_InContainer(Volume)) : nullptr;
        if (!Area || !Area->IsChildOf(UFGMapArea::StaticClass())) return FString();
        return UFGMapArea::GetAreaDisplayName(Area).ToString();
    }

    /** One volume: identity, class, world bounds, and (optionally) its convex brush hulls as world-space vertices. */
    TSharedRef<FJsonObject> VolumeJson(AVolume* Volume, bool bGeometry)
    {
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Volume->GetName());
        Item->SetStringField(TEXT("className"), Volume->GetClass()->GetName());
        Item->SetStringField(TEXT("classPath"), Volume->GetClass()->GetPathName());
        const FBox Bounds = Volume->GetComponentsBoundingBox(true);
        Item->SetObjectField(TEXT("boundsMin"), VitalsVectorJson(Bounds.Min));
        Item->SetObjectField(TEXT("boundsMax"), VitalsVectorJson(Bounds.Max));
        if (const AFGVolumeMapArea* MapArea = Cast<AFGVolumeMapArea>(Volume)) Item->SetStringField(TEXT("mapArea"), MapAreaDisplayName(MapArea));
        if (!bGeometry) return Item;
        TArray<TSharedPtr<FJsonValue>> Hulls;
        const UBrushComponent* Brush = Volume->GetBrushComponent();
        const UBodySetup* BodySetup = Brush ? Brush->BrushBodySetup.Get() : nullptr;
        if (BodySetup)
        {
            const FTransform Transform = Brush->GetComponentTransform();
            for (const FKConvexElem& Element : BodySetup->AggGeom.ConvexElems)
            {
                TArray<TSharedPtr<FJsonValue>> Vertices;
                for (const FVector& Local : Element.VertexData)
                {
                    const FVector World = Transform.TransformPosition(Element.GetTransform().TransformPosition(Local));
                    TArray<TSharedPtr<FJsonValue>> Vertex = { MakeShared<FJsonValueNumber>(FMath::RoundToDouble(World.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(World.Y)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(World.Z)) };
                    Vertices.Add(MakeShared<FJsonValueArray>(Vertex));
                }
                Hulls.Add(MakeShared<FJsonValueArray>(Vertices));
            }
        }
        Item->SetArrayField(TEXT("hulls"), Hulls);
        return Item;
    }
}

/**
 * GET /v1/player/vitals: answers whenever a local player controller exists, including while the pawn is dead or
 * respawning (when /v1/player answers 503). Reports health, death and respawn state, the position, the volumes that
 * encompass the player (any class), the map area there, and the minimap world bounds and kill Z.
 */
bool FMKIBridgeModule::HandlePlayerVitals(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGPlayerController* Controller = World ? Cast<AFGPlayerController>(UGameplayStatics::GetPlayerController(World, 0)) : nullptr;
    if (!World || !Controller) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player controller is available."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(Controller->GetPawn());
    const UFGHealthComponent* Health = Player ? Player->GetHealthComponent() : nullptr;
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("hasPawn"), Player != nullptr);
    Body->SetBoolField(TEXT("alive"), Player && Player->IsAliveAndWell());
    Body->SetBoolField(TEXT("dead"), Health ? Health->IsDead() : Player == nullptr);
    Body->SetBoolField(TEXT("needRespawn"), Controller->NeedRespawn());
    Body->SetBoolField(TEXT("respawning"), Controller->IsRespawning());
    if (Health) { Body->SetNumberField(TEXT("health"), Health->GetCurrentHealth()); Body->SetNumberField(TEXT("maxHealth"), Health->GetMaxHealth()); }
    FVector2D BoundsMin, BoundsMax; UFGMapFunctionLibrary::GetWorldBounds(World, BoundsMin, BoundsMax);
    const TSharedRef<FJsonObject> WorldBounds = MakeShared<FJsonObject>();
    WorldBounds->SetNumberField(TEXT("xMin"), BoundsMin.X); WorldBounds->SetNumberField(TEXT("yMin"), BoundsMin.Y); WorldBounds->SetNumberField(TEXT("xMax"), BoundsMax.X); WorldBounds->SetNumberField(TEXT("yMax"), BoundsMax.Y);
    Body->SetObjectField(TEXT("worldBounds"), WorldBounds);
    if (const AWorldSettings* Settings = World->GetWorldSettings()) Body->SetNumberField(TEXT("killZ"), Settings->KillZ);
    if (Player)
    {
        const FVector Location = Player->GetActorLocation();
        Body->SetObjectField(TEXT("position"), VitalsVectorJson(Location));
        TArray<TSharedPtr<FJsonValue>> Inside;
        for (TActorIterator<AVolume> It(World); It; ++It)
        {
            AVolume* Volume = *It;
            if (!IsValid(Volume) || !Volume->GetComponentsBoundingBox(true).IsInsideOrOn(Location) || !Volume->EncompassesPoint(Location)) continue;
            Inside.Add(MakeShared<FJsonValueObject>(VolumeJson(Volume, false)));
            if (const AFGVolumeMapArea* MapArea = Cast<AFGVolumeMapArea>(Volume)) Body->SetStringField(TEXT("mapArea"), MapAreaDisplayName(MapArea));
        }
        Body->SetArrayField(TEXT("insideVolumes"), Inside);
    }
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * GET /v1/world/volumes[?classContains=a,b][&geometry=1]: every volume actor in the world (any class), with bounds;
 * `classContains` keeps classes whose name contains any of the comma-separated substrings (case-insensitive), and
 * `geometry=1` adds each volume's convex brush hulls in world space. Read-only.
 */
bool FMKIBridgeModule::HandleWorldVolumes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No world is loaded."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TArray<FString> Filters;
    if (const FString* Text = Request.QueryParams.Find(TEXT("classContains"))) Text->ParseIntoArray(Filters, TEXT(","), true);
    const FString* GeometryText = Request.QueryParams.Find(TEXT("geometry"));
    const bool bGeometry = GeometryText && (*GeometryText == TEXT("1") || GeometryText->Equals(TEXT("true"), ESearchCase::IgnoreCase));
    if (bGeometry && Filters.Num() == 0) { OnComplete(ErrorResponse(TEXT("filter_required"), TEXT("geometry=1 requires classContains."), EHttpServerResponseCodes::BadRequest)); return true; }
    TArray<TSharedPtr<FJsonValue>> Items; TMap<FString, int32> Classes;
    for (TActorIterator<AVolume> It(World); It; ++It)
    {
        AVolume* Volume = *It;
        if (!IsValid(Volume)) continue;
        const FString ClassName = Volume->GetClass()->GetName();
        Classes.FindOrAdd(ClassName)++;
        if (Filters.Num() > 0 && !Filters.ContainsByPredicate([&](const FString& Filter) { return ClassName.Contains(Filter, ESearchCase::IgnoreCase); })) continue;
        Items.Add(MakeShared<FJsonValueObject>(VolumeJson(Volume, bGeometry)));
    }
    const TSharedRef<FJsonObject> ClassCounts = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : Classes) ClassCounts->SetNumberField(Pair.Key, Pair.Value);
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetObjectField(TEXT("classes"), ClassCounts);
    Body->SetNumberField(TEXT("returned"), Items.Num());
    Body->SetArrayField(TEXT("volumes"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }
    // Any listing parameter selects the filtered, paged, revisioned listing (#113). Without one the listing is the
    // unchanged full inventory within the observation radius.
    for (const TCHAR* Parameter : { TEXT("center"), TEXT("radiusCm"), TEXT("bounds"), TEXT("classPath"), TEXT("ids"), TEXT("fields"), TEXT("limit"), TEXT("cursor"), TEXT("since"), TEXT("epoch") })
    {
        if (Request.QueryParams.Contains(Parameter)) return HandleBuildablesQuery(Request, OnComplete);
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Subsystem || !Player)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World buildables are not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }

    TArray<AFGBuildable*> ObservableBuildables = Subsystem->GetAllBuildablesRef();
    // Foundations can be registered through the lightweight-buildable path and
    // omitted from GetAllBuildablesRef(). Include their actor representations so
    // clients can inspect and dismantle them by exact ID.
    for (TActorIterator<AFGBuildableFoundation> It(World); It; ++It)
    {
        if (*It && !IsLightweightSubsystemActor(*It)) ObservableBuildables.AddUnique(*It);
    }
    TArray<TSharedPtr<FJsonValue>> Items;
    for (AFGBuildable* Buildable : ObservableBuildables)
    {
        if (!Buildable || IsLightweightSubsystemActor(Buildable) || FVector::Distance(BuildableLocation(Buildable), Player->GetActorLocation()) > GetMaximumObservationDistanceCm()) continue;
        Items.Add(MakeShared<FJsonValueObject>(ActorBuildableJson(Buildable, EBuildableFields::Full)));
    }
    if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World))
    {
        // Every valid runtime record is reported, whatever its family. Foundations and ramps share the
        // subsystem's per-class instance arrays with walls, barriers, railings, catwalks, walkways, roofs,
        // frames and beams, so no class filter or allowlist applies. Record indices are stable while the
        // record lives (removal leaves an invalid slot), which keeps `lw|<classPath>|<index>` an exact ID
        // for anchoring and dismantling.
        for (const auto& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            UClass* Class = Pair.Key.Get();
            if (!Class) continue;
            for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
            {
                const FRuntimeBuildableInstanceData& Data = Pair.Value[Index];
                if (!Data.IsValid() || FVector::Distance(Data.Transform.GetLocation(), Player->GetActorLocation()) > GetMaximumObservationDistanceCm()) continue;
                Items.Add(MakeShared<FJsonValueObject>(LightweightBuildableJson(Class, Index, Data, EBuildableFields::Full)));
            }
        }
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetArrayField(TEXT("buildables"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

void FMKIBridgeModule::RefreshBuildableRevisions(UWorld* World)
{
    // The first scan of an epoch is a baseline, not a stream of additions, so it emits no events.
    bool bBaselineScan = false;
    if (BuildableSnapshotWorld.Get() != World || BuildablesEpoch.IsEmpty())
    {
        bBaselineScan = true;
        // A new world (or load) starts a new epoch: revisions and tombstones from the previous world never carry over.
        BuildableSnapshots.Reset(); BuildableTombstones.Reset(); BuildablesRevision = 0; OldestRetainedTombstoneRevision = 0;
        BuildablesEpoch = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        BuildableSnapshotWorld = World;
    }
    const int64 Next = BuildablesRevision + 1;
    bool bChanged = false;
    TSet<FString> Seen;
    struct FChange { const TCHAR* Type; FString Id; FString ClassPath; FVector Location; };
    TArray<FChange> Changes;
    auto Observe = [this, Next, &bChanged, &Seen, &Changes](const FString& Id, const FString& ClassPath, const FTransform& Transform, uint32 ConnectionBits)
    {
        Seen.Add(Id);
        const uint32 Fingerprint = BuildableFingerprint(ClassPath, Transform, ConnectionBits);
        FBuildableSnapshot* Existing = BuildableSnapshots.Find(Id);
        if (Existing && Existing->Fingerprint == Fingerprint) { Existing->Location = Transform.GetLocation(); return; }
        Changes.Add({ Existing ? TEXT("buildable_changed") : TEXT("buildable_added"), Id, ClassPath, Transform.GetLocation() });
        FBuildableSnapshot& Snapshot = Existing ? *Existing : BuildableSnapshots.Add(Id);
        Snapshot.Fingerprint = Fingerprint; Snapshot.Revision = Next; Snapshot.ClassPath = ClassPath; Snapshot.Location = Transform.GetLocation();
        bChanged = true;
    };
    TSet<AFGBuildable*> Actors;
    if (AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World)) for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef()) if (IsValid(Buildable) && !IsLightweightSubsystemActor(Buildable)) Actors.Add(Buildable);
    for (TActorIterator<AFGBuildableFoundation> It(World); It; ++It) if (IsValid(*It) && !IsLightweightSubsystemActor(*It)) Actors.Add(*It);
    for (AFGBuildable* Buildable : Actors) Observe(Buildable->GetName(), Buildable->GetClass()->GetPathName(), Buildable->GetActorTransform(), ActorConnectionBits(Buildable));
    if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World))
    {
        for (const auto& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            UClass* Class = Pair.Key.Get();
            if (!Class) continue;
            const FString ClassPath = Class->GetPathName();
            for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
            {
                const FRuntimeBuildableInstanceData& Data = Pair.Value[Index];
                if (Data.IsValid()) Observe(FString::Printf(TEXT("lw|%s|%d"), *ClassPath, Index), ClassPath, Data.Transform, 0);
            }
        }
    }
    for (auto It = BuildableSnapshots.CreateIterator(); It; ++It)
    {
        if (Seen.Contains(It.Key())) continue;
        BuildableTombstones.Add({ It.Key(), It.Value().ClassPath, It.Value().Location, Next });
        Changes.Add({ TEXT("buildable_removed"), It.Key(), It.Value().ClassPath, It.Value().Location });
        It.RemoveCurrent();
        bChanged = true;
    }
    while (BuildableTombstones.Num() > MaximumBuildableTombstones)
    {
        OldestRetainedTombstoneRevision = BuildableTombstones[0].Revision;
        BuildableTombstones.RemoveAt(0);
    }
    if (bChanged) BuildablesRevision = Next;
    // Events (#117) for changes inside the observation radius. A lightweight conversion appears as the actor's
    // removal plus the instance's addition.
    const APawn* Player = UGameplayStatics::GetPlayerPawn(World, 0);
    if (bBaselineScan || !Player || Changes.Num() == 0) return;
    const double ObservationCm = GetMaximumObservationDistanceCm();
    Changes.RemoveAll([Player, ObservationCm](const FChange& Change) { return FVector::Distance(Change.Location, Player->GetActorLocation()) > ObservationCm; });
    if (Changes.Num() > MaximumBuildableEventsPerSample)
    {
        int32 Added = 0, ChangedCount = 0, Removed = 0;
        for (const FChange& Change : Changes) { const FString Type = Change.Type; Type == TEXT("buildable_added") ? ++Added : Type == TEXT("buildable_removed") ? ++Removed : ++ChangedCount; }
        const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
        Event->SetNumberField(TEXT("added"), Added); Event->SetNumberField(TEXT("changed"), ChangedCount); Event->SetNumberField(TEXT("removed"), Removed);
        Event->SetNumberField(TEXT("revision"), static_cast<double>(BuildablesRevision));
        Event->SetStringField(TEXT("buildablesEpoch"), BuildablesEpoch);
        EmitBridgeEvent(World, TEXT("buildables_bulk_change"), Event);
        return;
    }
    for (const FChange& Change : Changes)
    {
        const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
        Event->SetStringField(TEXT("id"), Change.Id);
        Event->SetStringField(TEXT("classPath"), Change.ClassPath);
        Event->SetObjectField(TEXT("location"), BridgeVectorJson(Change.Location));
        Event->SetNumberField(TEXT("revision"), static_cast<double>(BuildablesRevision));
        Event->SetStringField(TEXT("buildablesEpoch"), BuildablesEpoch);
        EmitBridgeEvent(World, Change.Type, Event);
    }
}

TSet<int32> FMKIBridgeModule::ValidLightweightIndices(UWorld* World, UClass* Class)
{
    TSet<int32> Indices;
    if (AFGLightweightBuildableSubsystem* Lightweight = World && Class ? AFGLightweightBuildableSubsystem::Get(World) : nullptr)
        for (const auto& Pair : Lightweight->GetAllLightweightBuildableInstances())
            if (Pair.Key.Get() == Class)
                for (int32 Index = 0; Index < Pair.Value.Num(); ++Index) if (Pair.Value[Index].IsValid()) Indices.Add(Index);
    return Indices;
}

/**
 * Filtered, paged, revisioned buildables listing (#113). Filters: `center=x,y,z` with `radiusCm`, `bounds=minX,minY,
 * minZ,maxX,maxY,maxZ`, `classPath` (comma-separated exact paths), `ids` (comma-separated exact IDs). The observation
 * radius around the player always applies as well. `fields=compact|ports|full` (default full), `limit` (default 500,
 * at most 5000), `cursor` (opaque, from `nextCursor`), and `since` (+ `epoch`) for changes after a revision, with
 * tombstones for removed buildables. Items are ordered by ID.
 */
bool FMKIBridgeModule::HandleBuildablesQuery(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Subsystem || !Player)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World buildables are not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }
    auto Invalid = [&OnComplete](const FString& Message) { OnComplete(ErrorResponse(TEXT("invalid_query"), Message, EHttpServerResponseCodes::BadRequest)); return true; };
    auto Digits = [](const FString& Text) { if (Text.IsEmpty() || Text.Len() > 18) return false; for (const TCHAR Character : Text) if (Character < TEXT('0') || Character > TEXT('9')) return false; return true; };
    const double ObservationCm = GetMaximumObservationDistanceCm();
    const FVector PlayerLocation = Player->GetActorLocation();

    EBuildableFields Fields = EBuildableFields::Full; FString FieldsName = TEXT("full");
    if (const FString* Text = Request.QueryParams.Find(TEXT("fields")))
    {
        if (Text->Equals(TEXT("compact"), ESearchCase::CaseSensitive)) Fields = EBuildableFields::Compact;
        else if (Text->Equals(TEXT("ports"), ESearchCase::CaseSensitive)) Fields = EBuildableFields::Ports;
        else if (!Text->Equals(TEXT("full"), ESearchCase::CaseSensitive)) return Invalid(TEXT("fields must be compact, ports, or full."));
        FieldsName = *Text;
    }
    bool bSphere = false; FVector Center = FVector::ZeroVector; double RadiusCm = 0.0;
    const FString* CenterText = Request.QueryParams.Find(TEXT("center"));
    const FString* RadiusText = Request.QueryParams.Find(TEXT("radiusCm"));
    if (CenterText || RadiusText)
    {
        TArray<double> Values; TArray<double> Radius;
        if (!CenterText || !RadiusText) return Invalid(TEXT("center and radiusCm must be given together."));
        if (!ParseNumberList(*CenterText, 3, Values)) return Invalid(TEXT("center must be x,y,z."));
        if (!ParseNumberList(*RadiusText, 1, Radius) || Radius[0] <= 0.0 || Radius[0] > ObservationCm) return Invalid(TEXT("radiusCm must be positive and at most the observation radius."));
        Center = FVector(Values[0], Values[1], Values[2]); RadiusCm = Radius[0]; bSphere = true;
    }
    bool bBox = false; FBox Box(ForceInit);
    if (const FString* Text = Request.QueryParams.Find(TEXT("bounds")))
    {
        TArray<double> Values;
        if (!ParseNumberList(*Text, 6, Values) || Values[0] > Values[3] || Values[1] > Values[4] || Values[2] > Values[5]) return Invalid(TEXT("bounds must be minX,minY,minZ,maxX,maxY,maxZ with min <= max."));
        Box = FBox(FVector(Values[0], Values[1], Values[2]), FVector(Values[3], Values[4], Values[5])); bBox = true;
    }
    TArray<FString> ClassPaths;
    if (const FString* Text = Request.QueryParams.Find(TEXT("classPath")))
    {
        Text->ParseIntoArray(ClassPaths, TEXT(","), true);
        if (ClassPaths.Num() == 0 || ClassPaths.Num() > 64) return Invalid(TEXT("classPath must name 1 to 64 exact class paths."));
    }
    TArray<FString> Ids;
    if (const FString* Text = Request.QueryParams.Find(TEXT("ids")))
    {
        Text->ParseIntoArray(Ids, TEXT(","), true);
        if (Ids.Num() == 0 || Ids.Num() > MaximumBuildableIdsPerQuery) return Invalid(FString::Printf(TEXT("ids must name 1 to %d exact IDs."), MaximumBuildableIdsPerQuery));
    }
    int32 Limit = DefaultBuildablesPageLimit;
    if (const FString* Text = Request.QueryParams.Find(TEXT("limit")))
    {
        if (!Digits(*Text)) return Invalid(TEXT("limit must be a positive integer."));
        const int64 Value = FCString::Atoi64(**Text);
        if (Value < 1 || Value > MaximumBuildablesPageLimit) return Invalid(FString::Printf(TEXT("limit must be between 1 and %d."), MaximumBuildablesPageLimit));
        Limit = static_cast<int32>(Value);
    }
    int64 Since = -1;
    if (const FString* Text = Request.QueryParams.Find(TEXT("since")))
    {
        if (!Digits(*Text)) return Invalid(TEXT("since must be a revision number."));
        Since = FCString::Atoi64(**Text);
    }

    RefreshBuildableRevisions(World);

    FString AfterId;
    if (const FString* Text = Request.QueryParams.Find(TEXT("cursor")))
    {
        FString CursorEpoch;
        if (!Text->Split(TEXT(":"), &CursorEpoch, &AfterId, ESearchCase::CaseSensitive, ESearchDir::FromStart) || AfterId.IsEmpty()) return Invalid(TEXT("cursor is malformed; pass nextCursor unchanged."));
        if (!CursorEpoch.Equals(BuildablesEpoch, ESearchCase::CaseSensitive))
        {
            OnComplete(ErrorResponse(TEXT("cursor_expired"), TEXT("The listing epoch changed (a different world was loaded); restart the listing without a cursor."), EHttpServerResponseCodes::Conflict));
            return true;
        }
    }
    // `since` is only meaningful within one epoch and while every tombstone after it is retained; otherwise the
    // caller receives the full filtered listing with resync=true and must replace, not merge, its view.
    bool bResync = false;
    if (Since >= 0)
    {
        const FString* Epoch = Request.QueryParams.Find(TEXT("epoch"));
        if (!Epoch || !Epoch->Equals(BuildablesEpoch, ESearchCase::CaseSensitive) || Since > BuildablesRevision || Since < OldestRetainedTombstoneRevision) { bResync = true; Since = -1; }
    }

    // Stale conventional IDs from this bridge's own placement receipts resolve to the buildable they became.
    const TSharedRef<FJsonObject> ResolvedIds = MakeShared<FJsonObject>();
    if (Ids.Num() > 0)
    {
        TArray<FString> Extra;
        for (const FString& Id : Ids)
        {
            if (Id.StartsWith(TEXT("lw|"), ESearchCase::CaseSensitive) || FindBuildableById(World, Id)) continue;
            for (const TPair<FString, FPlacementOrigin>& Origin : PlacementOrigins)
            {
                if (!Origin.Value.ActorName.Equals(Id, ESearchCase::CaseSensitive)) continue;
                FString Resolution; const FString Final = ResolvePlacementOrigin(World, Origin.Value, Resolution);
                if (!Final.IsEmpty() && !Final.Equals(Id, ESearchCase::CaseSensitive)) { ResolvedIds->SetStringField(Id, Final); Extra.Add(Final); }
                break;
            }
        }
        Ids.Append(Extra);
    }

    auto ContainsExact = [](const TArray<FString>& List, const FString& Value) { return List.ContainsByPredicate([&Value](const FString& Other) { return Other.Equals(Value, ESearchCase::CaseSensitive); }); };
    auto Matches = [&](const FString& Id, const FString& ClassPath, const FVector& Location)
    {
        if (FVector::Distance(Location, PlayerLocation) > ObservationCm) return false;
        if (bSphere && FVector::Distance(Location, Center) > RadiusCm) return false;
        if (bBox && !Box.IsInsideOrOn(Location)) return false;
        if (ClassPaths.Num() > 0 && !ContainsExact(ClassPaths, ClassPath)) return false;
        if (Ids.Num() > 0 && !ContainsExact(Ids, Id)) return false;
        return true;
    };
    auto RevisionOf = [this](const FString& Id) { const FBuildableSnapshot* Snapshot = BuildableSnapshots.Find(Id); return Snapshot ? Snapshot->Revision : BuildablesRevision; };

    struct FCandidate { FString Id; AFGBuildable* Actor = nullptr; UClass* Class = nullptr; int32 Index = INDEX_NONE; const FRuntimeBuildableInstanceData* Data = nullptr; int64 Revision = 0; };
    TArray<FCandidate> Candidates;
    TSet<AFGBuildable*> Actors;
    for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef()) if (IsValid(Buildable) && !IsLightweightSubsystemActor(Buildable)) Actors.Add(Buildable);
    for (TActorIterator<AFGBuildableFoundation> It(World); It; ++It) if (IsValid(*It) && !IsLightweightSubsystemActor(*It)) Actors.Add(*It);
    for (AFGBuildable* Buildable : Actors)
    {
        const FString Id = Buildable->GetName();
        if (!Matches(Id, Buildable->GetClass()->GetPathName(), BuildableLocation(Buildable))) continue;
        const int64 Revision = RevisionOf(Id);
        if (Since >= 0 && Revision <= Since) continue;
        FCandidate& Candidate = Candidates.AddDefaulted_GetRef(); Candidate.Id = Id; Candidate.Actor = Buildable; Candidate.Revision = Revision;
    }
    if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World))
    {
        for (const auto& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            UClass* Class = Pair.Key.Get();
            if (!Class) continue;
            const FString ClassPath = Class->GetPathName();
            for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
            {
                const FRuntimeBuildableInstanceData& Data = Pair.Value[Index];
                if (!Data.IsValid()) continue;
                const FString Id = FString::Printf(TEXT("lw|%s|%d"), *ClassPath, Index);
                if (!Matches(Id, ClassPath, Data.Transform.GetLocation())) continue;
                const int64 Revision = RevisionOf(Id);
                if (Since >= 0 && Revision <= Since) continue;
                FCandidate& Candidate = Candidates.AddDefaulted_GetRef(); Candidate.Id = Id; Candidate.Class = Class; Candidate.Index = Index; Candidate.Data = &Data; Candidate.Revision = Revision;
            }
        }
    }
    Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Id.Compare(B.Id, ESearchCase::CaseSensitive) < 0; });

    TArray<TSharedPtr<FJsonValue>> Items;
    FString LastId; bool bMore = false;
    for (const FCandidate& Candidate : Candidates)
    {
        if (!AfterId.IsEmpty() && Candidate.Id.Compare(AfterId, ESearchCase::CaseSensitive) <= 0) continue;
        if (Items.Num() >= Limit) { bMore = true; break; }
        const TSharedRef<FJsonObject> Item = Candidate.Actor ? ActorBuildableJson(Candidate.Actor, Fields) : LightweightBuildableJson(Candidate.Class, Candidate.Index, *Candidate.Data, Fields);
        Item->SetNumberField(TEXT("revision"), static_cast<double>(Candidate.Revision));
        Items.Add(MakeShared<FJsonValueObject>(Item));
        LastId = Candidate.Id;
    }

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetArrayField(TEXT("buildables"), Items);
    Body->SetStringField(TEXT("epoch"), BuildablesEpoch);
    Body->SetNumberField(TEXT("revision"), static_cast<double>(BuildablesRevision));
    Body->SetStringField(TEXT("fields"), FieldsName);
    Body->SetNumberField(TEXT("total"), Candidates.Num());
    Body->SetNumberField(TEXT("returned"), Items.Num());
    Body->SetBoolField(TEXT("truncated"), bMore);
    if (bMore) Body->SetStringField(TEXT("nextCursor"), BuildablesEpoch + TEXT(":") + LastId);
    Body->SetBoolField(TEXT("resync"), bResync);
    if (ResolvedIds->Values.Num() > 0) Body->SetObjectField(TEXT("resolvedIds"), ResolvedIds);
    if (Since >= 0)
    {
        Body->SetNumberField(TEXT("since"), static_cast<double>(Since));
        TArray<TSharedPtr<FJsonValue>> Removed;
        for (const FBuildableTombstone& Tombstone : BuildableTombstones)
        {
            if (Tombstone.Revision <= Since || !Matches(Tombstone.Id, Tombstone.ClassPath, Tombstone.Location)) continue;
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("id"), Tombstone.Id);
            Entry->SetStringField(TEXT("classPath"), Tombstone.ClassPath);
            Entry->SetObjectField(TEXT("location"), BridgeVectorJson(Tombstone.Location));
            Entry->SetNumberField(TEXT("revision"), static_cast<double>(Tombstone.Revision));
            Removed.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Body->SetArrayField(TEXT("tombstones"), Removed);
    }
    // The region this listing is complete for: the requested sphere, shrunk to the part that lies inside the
    // observation radius around the player. A caller must not treat anything outside it as absent.
    const TSharedRef<FJsonObject> Coverage = MakeShared<FJsonObject>();
    Coverage->SetNumberField(TEXT("observationRadiusCm"), ObservationCm);
    Coverage->SetObjectField(TEXT("observer"), BridgeVectorJson(PlayerLocation));
    if (bSphere)
    {
        Coverage->SetObjectField(TEXT("center"), BridgeVectorJson(Center));
        Coverage->SetNumberField(TEXT("radiusCm"), FMath::Max(0.0, FMath::Min(RadiusCm, ObservationCm - FVector::Distance(Center, PlayerLocation))));
    }
    if (bBox)
    {
        bool bComplete = true;
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const FVector Point((Corner & 1) ? Box.Max.X : Box.Min.X, (Corner & 2) ? Box.Max.Y : Box.Min.Y, (Corner & 4) ? Box.Max.Z : Box.Min.Z);
            if (FVector::Distance(Point, PlayerLocation) > ObservationCm) { bComplete = false; break; }
        }
        Coverage->SetBoolField(TEXT("boundsComplete"), bComplete);
    }
    Body->SetObjectField(TEXT("coverage"), Coverage);
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandlePowerConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }
    const FString* BuildableId = Request.QueryParams.Find(TEXT("buildableId"));
    if (!BuildableId || BuildableId->IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("buildable_id_required"), TEXT("buildableId query parameter is required."), EHttpServerResponseCodes::BadRequest)); return true;
    }
    // Nearby candidates are bounded: radiusCm defaults to 100 m (the longest power line) and limit to 200 entries,
    // nearest first, so the answer no longer scales with the whole observation radius.
    double RadiusCm = 10000.0; int32 Limit = 200;
    if (const FString* RadiusText = Request.QueryParams.Find(TEXT("radiusCm")))
    {
        if (!RadiusText->IsNumeric() || !FMath::IsFinite(FCString::Atod(**RadiusText)) || FCString::Atod(**RadiusText) <= 0.0)
        { OnComplete(ErrorResponse(TEXT("invalid_radius"), TEXT("radiusCm must be a positive number."), EHttpServerResponseCodes::BadRequest)); return true; }
        RadiusCm = FMath::Min(FCString::Atod(**RadiusText), GetMaximumObservationDistanceCm());
    }
    if (const FString* LimitText = Request.QueryParams.Find(TEXT("limit")))
    {
        if (!LimitText->IsNumeric() || FCString::Atoi(**LimitText) < 1 || FCString::Atoi(**LimitText) > 2000)
        { OnComplete(ErrorResponse(TEXT("invalid_limit"), TEXT("limit must be a whole number from 1 through 2000."), EHttpServerResponseCodes::BadRequest)); return true; }
        Limit = FCString::Atoi(**LimitText);
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    if (!World || !Subsystem)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true;
    }
    AFGBuildable* Target = FindBuildableById(World, *BuildableId);
    if (!Target)
    {
        OnComplete(ErrorResponse(TEXT("buildable_not_found"), TEXT("No buildable with that exact ID was found."), EHttpServerResponseCodes::NotFound)); return true;
    }
    auto ConnectionJson = [](UFGCircuitConnectionComponent* Connection, AFGBuildable* Owner, double DistanceCm)
    {
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("buildableId"), Owner ? Owner->GetName() : TEXT(""));
        Item->SetStringField(TEXT("component"), Connection->GetName());
        Item->SetBoolField(TEXT("isPowerConnection"), Connection->IsA<UFGPowerConnectionComponent>());
        Item->SetBoolField(TEXT("hidden"), Connection->IsHidden());
        Item->SetBoolField(TEXT("connectedToCircuit"), Connection->IsConnected() != 0);
        Item->SetNumberField(TEXT("circuitId"), Connection->GetCircuitID());
        Item->SetNumberField(TEXT("connections"), Connection->GetNumConnections());
        Item->SetNumberField(TEXT("maxConnections"), Connection->GetMaxNumConnections());
        Item->SetNumberField(TEXT("freeConnections"), Connection->GetNumFreeConnections());
        Item->SetNumberField(TEXT("distanceCm"), DistanceCm);
        const FVector Location = Connection->GetComponentLocation();
        const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Location.X); Position->SetNumberField(TEXT("y"), Location.Y); Position->SetNumberField(TEXT("z"), Location.Z);
        Item->SetObjectField(TEXT("position"), Position);
        return Item;
    };
    TArray<TSharedPtr<FJsonValue>> TargetConnections;
    TInlineComponentArray<UFGCircuitConnectionComponent*> TargetComponents(Target);
    for (UFGCircuitConnectionComponent* Connection : TargetComponents)
    {
        if (Connection) TargetConnections.Add(MakeShared<FJsonValueObject>(ConnectionJson(Connection, Target, 0.0)));
    }
    TArray<TPair<double, TPair<UFGCircuitConnectionComponent*, AFGBuildable*>>> Candidates;
    for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
    {
        if (!Buildable || Buildable == Target) continue;
        const double DistanceCm = FVector::Distance(Target->GetActorLocation(), Buildable->GetActorLocation());
        if (DistanceCm > RadiusCm) continue;
        TInlineComponentArray<UFGCircuitConnectionComponent*> Components(Buildable);
        for (UFGCircuitConnectionComponent* Connection : Components)
        {
            if (Connection && !Connection->IsHidden() && Connection->GetNumFreeConnections() > 0)
            {
                Candidates.Emplace(DistanceCm, TPair<UFGCircuitConnectionComponent*, AFGBuildable*>(Connection, Buildable));
            }
        }
    }
    Candidates.Sort([](const TPair<double, TPair<UFGCircuitConnectionComponent*, AFGBuildable*>>& A, const TPair<double, TPair<UFGCircuitConnectionComponent*, AFGBuildable*>>& B) { return A.Key < B.Key; });
    TArray<TSharedPtr<FJsonValue>> NearbyConnections;
    for (int32 Index = 0; Index < Candidates.Num() && Index < Limit; ++Index)
        NearbyConnections.Add(MakeShared<FJsonValueObject>(ConnectionJson(Candidates[Index].Value.Key, Candidates[Index].Value.Value, Candidates[Index].Key)));
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetStringField(TEXT("buildableId"), *BuildableId);
    Body->SetNumberField(TEXT("radiusCm"), RadiusCm); Body->SetNumberField(TEXT("limit"), Limit);
    Body->SetNumberField(TEXT("totalAvailable"), Candidates.Num()); Body->SetBoolField(TEXT("truncated"), Candidates.Num() > Limit);
    Body->SetArrayField(TEXT("connections"), TargetConnections); Body->SetArrayField(TEXT("nearbyAvailableConnections"), NearbyConnections);
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleMachineConnections(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }
    // buildableId is the shared identifier name; machineId remains an accepted alias.
    const FString* MachineId = Request.QueryParams.Find(TEXT("buildableId"));
    if (!MachineId || MachineId->IsEmpty()) MachineId = Request.QueryParams.Find(TEXT("machineId"));
    if (!MachineId || MachineId->IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("machine_id_required"), TEXT("machineId query parameter is required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }
    AFGBuildable* TargetBuildable = FindBuildableById(World, *MachineId);
    if (!TargetBuildable)
    {
        OnComplete(ErrorResponse(TEXT("buildable_not_found"), TEXT("No buildable with that exact ID was found."), EHttpServerResponseCodes::NotFound));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> ConnectionsJson;
    TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(TargetBuildable);
    UFGFactoryConnectionComponent::SortComponentList(Connections);
    for (int32 Index = 0; Index < Connections.Num(); ++Index)
    {
        UFGFactoryConnectionComponent* Connection = Connections[Index];
        if (!Connection)
        {
            continue;
        }
        const FVector Location = Connection->GetConnectorLocation(false);
        const FVector Normal = Connection->GetConnectorNormal();
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("index"), Index);
        Item->SetStringField(TEXT("component"), Connection->GetName());
        const EFactoryConnectionDirection Direction = Connection->GetDirection();
        Item->SetStringField(TEXT("direction"), Direction == EFactoryConnectionDirection::FCD_INPUT ? TEXT("input") :
            Direction == EFactoryConnectionDirection::FCD_OUTPUT ? TEXT("output") :
            Direction == EFactoryConnectionDirection::FCD_ANY ? TEXT("any") : TEXT("snap_only"));
        Item->SetBoolField(TEXT("connected"), Connection->IsConnected());
        const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Location.X);
        Position->SetNumberField(TEXT("y"), Location.Y);
        Position->SetNumberField(TEXT("z"), Location.Z);
        Item->SetObjectField(TEXT("position"), Position);
        const TSharedRef<FJsonObject> DirectionVector = MakeShared<FJsonObject>();
        DirectionVector->SetNumberField(TEXT("x"), Normal.X);
        DirectionVector->SetNumberField(TEXT("y"), Normal.Y);
        DirectionVector->SetNumberField(TEXT("z"), Normal.Z);
        Item->SetObjectField(TEXT("normal"), DirectionVector);
        if (UFGFactoryConnectionComponent* Connected = Connection->GetConnection())
        {
            Item->SetStringField(TEXT("connectedComponent"), Connected->GetName());
            if (AFGBuildable* Other = Connected->GetOuterBuildable())
            {
                Item->SetStringField(TEXT("connectedBuildableId"), Other->GetName());
            }
        }
        ConnectionsJson.Add(MakeShared<FJsonValueObject>(Item));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("machineId"), *MachineId);
    Body->SetStringField(TEXT("buildableId"), *MachineId);
    Body->SetArrayField(TEXT("connections"), ConnectionsJson);
    OnComplete(JsonResponse(Body));
    return true;
}

namespace
{
    /** Item stacks of an inventory: one entry per occupied slot. */
    TArray<TSharedPtr<FJsonValue>> InventoryJson(UFGInventoryComponent* Inventory)
    {
        TArray<TSharedPtr<FJsonValue>> Stacks;
        if (!Inventory) return Stacks;
        for (int32 Slot = 0; Slot < Inventory->GetSizeLinear(); ++Slot)
        {
            FInventoryStack Stack;
            if (!Inventory->GetStackFromIndex(Slot, Stack) || !Stack.HasItems()) continue;
            const TSubclassOf<UFGItemDescriptor> Item = Stack.Item.GetItemClass();
            if (!Item) continue;
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("slot"), Slot);
            Entry->SetStringField(TEXT("item"), Item->GetName());
            Entry->SetStringField(TEXT("itemPath"), Item->GetPathName());
            Entry->SetNumberField(TEXT("count"), Stack.NumItems);
            Stacks.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Stacks;
    }

    const TCHAR* ResourceFormName(EResourceForm Form)
    {
        switch (Form)
        {
        case EResourceForm::RF_SOLID: return TEXT("solid");
        case EResourceForm::RF_LIQUID: return TEXT("liquid");
        case EResourceForm::RF_GAS: return TEXT("gas");
        default: return TEXT("invalid");
        }
    }

    const TCHAR* ResourcePurityName(EResourcePurity Purity)
    {
        switch (Purity)
        {
        case EResourcePurity::RP_Inpure: return TEXT("impure");
        case EResourcePurity::RP_Pure: return TEXT("pure");
        default: return TEXT("normal");
        }
    }

    /** `{ item, itemPath, amount, form }` for every entry; fluid amounts are in the game's units (1000 = 1 m3). */
    TArray<TSharedPtr<FJsonValue>> ItemAmountsJson(const TArray<FItemAmount>& Amounts)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (const FItemAmount& Amount : Amounts)
        {
            if (!Amount.ItemClass) continue;
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("item"), Amount.ItemClass->GetName());
            Entry->SetStringField(TEXT("itemPath"), Amount.ItemClass->GetPathName());
            Entry->SetNumberField(TEXT("amount"), Amount.Amount);
            Entry->SetStringField(TEXT("form"), ResourceFormName(UFGItemDescriptor::GetForm(Amount.ItemClass)));
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Entries;
    }

    /**
     * The machines a factory connection leads to, following belts and lifts until a non-conveyor owner (a machine,
     * splitter, merger, or storage). Bounded so a malformed loop cannot hang the game thread.
     */
    AActor* FollowConveyorChain(UFGFactoryConnectionComponent* Start)
    {
        UFGFactoryConnectionComponent* Current = Start;
        for (int32 Hop = 0; Hop < 512 && Current && Current->IsConnected(); ++Hop)
        {
            UFGFactoryConnectionComponent* Other = Current->GetConnection();
            AActor* Owner = Other ? Other->GetOwner() : nullptr;
            AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(Owner);
            if (!Conveyor) return Owner;
            Current = Conveyor->GetConnection0() == Other ? Conveyor->GetConnection1() : Conveyor->GetConnection0();
        }
        return nullptr;
    }

    struct FSpatialFilter
    {
        bool bSphere = false; FVector Center = FVector::ZeroVector; double RadiusCm = 0.0;
        bool bBox = false; FBox Box = FBox(ForceInit);
        bool Matches(const FVector& Location) const
        {
            if (bSphere && FVector::Distance(Location, Center) > RadiusCm) return false;
            if (bBox && !Box.IsInsideOrOn(Location)) return false;
            return true;
        }
    };

    /** Parses `center`+`radiusCm` and `bounds`; false with a message on malformed input. */
    bool ParseSpatialFilter(const FHttpServerRequest& Request, FSpatialFilter& Out, FString& OutMessage)
    {
        const FString* CenterText = Request.QueryParams.Find(TEXT("center"));
        const FString* RadiusText = Request.QueryParams.Find(TEXT("radiusCm"));
        if (CenterText || RadiusText)
        {
            TArray<double> Values; TArray<double> Radius;
            if (!CenterText || !RadiusText || !ParseNumberList(*CenterText, 3, Values) || !ParseNumberList(*RadiusText, 1, Radius) || Radius[0] <= 0.0)
            { OutMessage = TEXT("center (x,y,z) and a positive radiusCm must be given together."); return false; }
            Out.bSphere = true; Out.Center = FVector(Values[0], Values[1], Values[2]); Out.RadiusCm = Radius[0];
        }
        if (const FString* Text = Request.QueryParams.Find(TEXT("bounds")))
        {
            TArray<double> Values;
            if (!ParseNumberList(*Text, 6, Values) || Values[0] > Values[3] || Values[1] > Values[4] || Values[2] > Values[5])
            { OutMessage = TEXT("bounds must be minX,minY,minZ,maxX,maxY,maxZ with min <= max."); return false; }
            Out.bBox = true; Out.Box = FBox(FVector(Values[0], Values[1], Values[2]), FVector(Values[3], Values[4], Values[5]));
        }
        return true;
    }

    /** Comma-separated exact values of a query parameter (empty when absent). */
    TArray<FString> QueryList(const FHttpServerRequest& Request, const TCHAR* Name)
    {
        TArray<FString> Values;
        if (const FString* Text = Request.QueryParams.Find(Name)) Text->ParseIntoArray(Values, TEXT(","), true);
        return Values;
    }

    bool ContainsCaseSensitive(const TArray<FString>& List, const FString& Value)
    {
        return List.ContainsByPredicate([&Value](const FString& Other) { return Other.Equals(Value, ESearchCase::CaseSensitive); });
    }

    /** Local transform of a component template relative to its actor class: the template's own relative transform. */
    FTransform TemplateRelativeTransform(const USceneComponent* Template)
    {
        return Template ? Template->GetRelativeTransform() : FTransform::Identity;
    }

    /**
     * Connection and mesh templates of a buildable class: native default subobjects of the class default object plus
     * the Blueprint construction-script templates of the class and its Blueprint parents. Positions are each
     * template's relative transform; templates attached below another scene component are reported with
     * `nested: true` because their parent's offset is not composed.
     */
    void CollectClassTemplates(UClass* Class, TArray<TPair<USceneComponent*, bool>>& Out)
    {
        if (AActor* Default = Class ? Class->GetDefaultObject<AActor>() : nullptr)
        {
            TInlineComponentArray<USceneComponent*> Native(Default);
            for (USceneComponent* Component : Native) if (Component) Out.Add({ Component, Component->GetAttachParent() != nullptr && Component->GetAttachParent() != Default->GetRootComponent() });
        }
        for (UClass* Current = Class; Current; Current = Current->GetSuperClass())
        {
            UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Current);
            if (!Generated || !Generated->SimpleConstructionScript) continue;
            for (USCS_Node* Node : Generated->SimpleConstructionScript->GetAllNodes())
            {
                USceneComponent* Template = Node ? Cast<USceneComponent>(Node->ComponentTemplate) : nullptr;
                if (Template) Out.Add({ Template, Node->ParentComponentOrVariableName != NAME_None });
            }
        }
    }

    TSharedRef<FJsonObject> CatalogBuildableJson(UWorld* World, TSubclassOf<UFGRecipe> Recipe, UClass* BuildableClass, TSubclassOf<UFGItemDescriptor> Descriptor, bool bDetails)
    {
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("classPath"), BuildableClass->GetPathName());
        Entry->SetStringField(TEXT("className"), BuildableClass->GetName());
        Entry->SetStringField(TEXT("recipePath"), Recipe->GetPathName());
        Entry->SetStringField(TEXT("recipeName"), UFGRecipe::GetRecipeName(Recipe).ToString());
        if (Descriptor)
        {
            Entry->SetStringField(TEXT("descriptorPath"), Descriptor->GetPathName());
            Entry->SetStringField(TEXT("displayName"), UFGItemDescriptor::GetItemName(Descriptor).ToString());
        }
        Entry->SetArrayField(TEXT("cost"), ItemAmountsJson(UFGRecipe::GetIngredients(World, Recipe)));
        if (!bDetails) return Entry;
        TArray<TPair<USceneComponent*, bool>> Templates;
        CollectClassTemplates(BuildableClass, Templates);
        TArray<TSharedPtr<FJsonValue>> Ports;
        FBox MeshBounds(ForceInit);
        for (const TPair<USceneComponent*, bool>& Template : Templates)
        {
            const FTransform Relative = TemplateRelativeTransform(Template.Key);
            if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Template.Key))
            {
                if (Mesh->GetStaticMesh() && !Template.Value) MeshBounds += Mesh->GetStaticMesh()->GetBoundingBox().TransformBy(Relative);
                continue;
            }
            FString Kind; FString Direction = TEXT("any");
            if (const UFGFactoryConnectionComponent* Factory = Cast<UFGFactoryConnectionComponent>(Template.Key))
            {
                Kind = TEXT("conveyor");
                const EFactoryConnectionDirection D = Factory->GetDirection();
                Direction = D == EFactoryConnectionDirection::FCD_INPUT ? TEXT("input") : D == EFactoryConnectionDirection::FCD_OUTPUT ? TEXT("output") : D == EFactoryConnectionDirection::FCD_ANY ? TEXT("any") : TEXT("snap_only");
            }
            else if (const UFGPipeConnectionComponentBase* Pipe = Cast<UFGPipeConnectionComponentBase>(Template.Key))
            {
                Kind = Pipe->IsA<UFGPipeConnectionComponentHyper>() ? TEXT("hypertube") : TEXT("fluid");
                const EPipeConnectionType Type = Pipe->GetPipeConnectionType();
                Direction = Type == EPipeConnectionType::PCT_CONSUMER ? TEXT("input") : Type == EPipeConnectionType::PCT_PRODUCER ? TEXT("output") : Type == EPipeConnectionType::PCT_SNAP_ONLY ? TEXT("snap_only") : TEXT("any");
            }
            else if (Template.Key->IsA<UFGPowerConnectionComponent>()) Kind = TEXT("power");
            else continue;
            const TSharedRef<FJsonObject> Port = MakeShared<FJsonObject>();
            Port->SetStringField(TEXT("name"), Template.Key->GetName());
            Port->SetStringField(TEXT("kind"), Kind);
            Port->SetStringField(TEXT("direction"), Direction);
            Port->SetObjectField(TEXT("localPosition"), BridgeVectorJson(Relative.GetLocation()));
            Port->SetObjectField(TEXT("localFacing"), BridgeVectorJson(Relative.GetRotation().GetForwardVector()));
            Port->SetBoolField(TEXT("nested"), Template.Value);
            Ports.Add(MakeShared<FJsonValueObject>(Port));
        }
        Entry->SetArrayField(TEXT("ports"), Ports);
        if (MeshBounds.IsValid)
        {
            const TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
            Bounds->SetObjectField(TEXT("min"), BridgeVectorJson(MeshBounds.Min));
            Bounds->SetObjectField(TEXT("max"), BridgeVectorJson(MeshBounds.Max));
            Bounds->SetStringField(TEXT("source"), TEXT("top_level_static_meshes"));
            Entry->SetObjectField(TEXT("localBounds"), Bounds);
        }
        return Entry;
    }
}

/**
 * Machine state (#114): recipe, derived state, productivity, clock, power, inventories, and the machines each factory
 * connection leads to. Filters: `ids`, `center`+`radiusCm`, `bounds`, `limit` (default 200, at most 2000). Only
 * machines inside the observation radius are reported.
 */
bool FMKIBridgeModule::HandleMachines(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Subsystem || !Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World machines are not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    FSpatialFilter Filter; FString Message;
    if (!ParseSpatialFilter(Request, Filter, Message)) { OnComplete(ErrorResponse(TEXT("invalid_query"), Message, EHttpServerResponseCodes::BadRequest)); return true; }
    const TArray<FString> Ids = QueryList(Request, TEXT("ids"));
    if (Ids.Num() > 500) { OnComplete(ErrorResponse(TEXT("invalid_query"), TEXT("ids may name at most 500 machines."), EHttpServerResponseCodes::BadRequest)); return true; }
    int32 Limit = 200;
    if (const FString* Text = Request.QueryParams.Find(TEXT("limit")))
    {
        TArray<double> Value;
        if (!ParseNumberList(*Text, 1, Value) || Value[0] < 1 || Value[0] > 2000 || Value[0] != FMath::FloorToDouble(Value[0])) { OnComplete(ErrorResponse(TEXT("invalid_query"), TEXT("limit must be an integer between 1 and 2000."), EHttpServerResponseCodes::BadRequest)); return true; }
        Limit = static_cast<int32>(Value[0]);
    }
    const double ObservationCm = GetMaximumObservationDistanceCm();
    TArray<AFGBuildableFactory*> Machines;
    for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
    {
        AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable);
        if (!IsValid(Factory) || FVector::Distance(Factory->GetActorLocation(), Player->GetActorLocation()) > ObservationCm) continue;
        if (Ids.Num() > 0 && !ContainsCaseSensitive(Ids, Factory->GetName())) continue;
        if (!Filter.Matches(Factory->GetActorLocation())) continue;
        Machines.Add(Factory);
    }
    Machines.Sort([](const AFGBuildableFactory& A, const AFGBuildableFactory& B) { return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0; });
    TArray<TSharedPtr<FJsonValue>> Items;
    for (AFGBuildableFactory* Machine : Machines)
    {
        if (Items.Num() >= Limit) break;
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Machine->GetName());
        Item->SetStringField(TEXT("className"), Machine->GetClass()->GetName());
        Item->SetStringField(TEXT("classPath"), Machine->GetClass()->GetPathName());
        Item->SetObjectField(TEXT("location"), BridgeVectorJson(Machine->GetActorLocation()));
        AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine);
        AFGBuildableResourceExtractorBase* Extractor = Cast<AFGBuildableResourceExtractorBase>(Machine);
        AFGBuildableGenerator* Generator = Cast<AFGBuildableGenerator>(Machine);
        Item->SetStringField(TEXT("kind"), Manufacturer ? TEXT("manufacturer") : Extractor ? TEXT("extractor") : Generator ? TEXT("generator") : TEXT("other"));
        const bool bProducing = Machine->IsProducing();
        const bool bStandby = Machine->IsProductionPaused();
        UFGPowerInfoComponent* PowerInfo = Machine->GetPowerInfo();
        const bool bHasPower = PowerInfo ? PowerInfo->HasPower() : false;
        Item->SetBoolField(TEXT("producing"), bProducing);
        Item->SetBoolField(TEXT("standby"), bStandby);
        Item->SetBoolField(TEXT("hasPower"), bHasPower);
        Item->SetNumberField(TEXT("productivity"), Machine->GetProductivity());
        Item->SetNumberField(TEXT("currentPotential"), Machine->GetCurrentPotential());
        Item->SetNumberField(TEXT("pendingPotential"), Machine->GetPendingPotential());
        Item->SetNumberField(TEXT("productionProgress"), Machine->GetProductionProgress());
        if (PowerInfo)
        {
            Item->SetNumberField(TEXT("powerConsumptionMW"), PowerInfo->GetActualConsumption());
            Item->SetNumberField(TEXT("targetConsumptionMW"), PowerInfo->GetTargetConsumption());
        }
        if (Manufacturer)
        {
            const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
            if (Recipe)
            {
                Item->SetStringField(TEXT("recipePath"), Recipe->GetPathName());
                Item->SetStringField(TEXT("recipeName"), UFGRecipe::GetRecipeName(Recipe).ToString());
                Item->SetNumberField(TEXT("recipeDurationSeconds"), UFGRecipe::GetManufacturingDuration(Recipe));
            }
            Item->SetArrayField(TEXT("inputInventory"), InventoryJson(Manufacturer->GetInputInventory()));
            Item->SetArrayField(TEXT("outputInventory"), InventoryJson(Manufacturer->GetOutputInventory()));
        }
        if (Extractor)
        {
            if (UObject* Node = Extractor->GetExtractableResource().GetObject()) Item->SetStringField(TEXT("resourceNodeId"), Node->GetName());
        }
        if (AFGBuildableGeneratorFuel* Fuel = Cast<AFGBuildableGeneratorFuel>(Machine)) Item->SetArrayField(TEXT("fuelInventory"), InventoryJson(Fuel->GetFuelInventory()));
        if (Generator) Item->SetNumberField(TEXT("powerProductionCapacityMW"), Generator->GetPowerProductionCapacity());
        Item->SetStringField(TEXT("state"), BridgeMachineState(World, Machine));
        TArray<TSharedPtr<FJsonValue>> Direct; TArray<TSharedPtr<FJsonValue>> Upstream; TArray<TSharedPtr<FJsonValue>> Downstream;
        TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Machine);
        for (UFGFactoryConnectionComponent* Connection : Connections)
        {
            if (!Connection || !Connection->IsConnected() || !Connection->GetConnection()) continue;
            if (AActor* Neighbour = Connection->GetConnection()->GetOwner()) Direct.Add(MakeShared<FJsonValueString>(Neighbour->GetName()));
            AActor* Terminal = FollowConveyorChain(Connection);
            if (!Terminal || Terminal == Machine) continue;
            (Connection->GetDirection() == EFactoryConnectionDirection::FCD_INPUT ? Upstream : Downstream).Add(MakeShared<FJsonValueString>(Terminal->GetName()));
        }
        TInlineComponentArray<UFGPipeConnectionComponentBase*> Pipes(Machine);
        for (UFGPipeConnectionComponentBase* Pipe : Pipes)
            if (Pipe && Pipe->GetConnection() && Pipe->GetConnection()->GetOwner()) Direct.Add(MakeShared<FJsonValueString>(Pipe->GetConnection()->GetOwner()->GetName()));
        Item->SetArrayField(TEXT("connectedBuildableIds"), Direct);
        Item->SetArrayField(TEXT("upstreamIds"), Upstream);
        Item->SetArrayField(TEXT("downstreamIds"), Downstream);
        TInlineComponentArray<UFGPowerConnectionComponent*> Power(Machine);
        for (UFGPowerConnectionComponent* Connection : Power) if (Connection && Connection->GetCircuitID() != INDEX_NONE) { Item->SetNumberField(TEXT("circuitId"), Connection->GetCircuitID()); break; }
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetNumberField(TEXT("total"), Machines.Num());
    Body->SetBoolField(TEXT("truncated"), Machines.Num() > Items.Num());
    Body->SetArrayField(TEXT("machines"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

/** Power circuit statistics for every circuit that has a powered buildable inside the observation radius (#114). */
bool FMKIBridgeModule::HandlePowerCircuits(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    AFGCircuitSubsystem* Circuits = World ? AFGCircuitSubsystem::Get(World) : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Subsystem || !Circuits || !Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("Power circuits are not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TMap<int32, int32> MemberCounts;
    for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
    {
        if (!IsValid(Buildable) || FVector::Distance(Buildable->GetActorLocation(), Player->GetActorLocation()) > GetMaximumObservationDistanceCm()) continue;
        TInlineComponentArray<UFGPowerConnectionComponent*> Power(Buildable);
        for (UFGPowerConnectionComponent* Connection : Power)
            if (Connection && Connection->GetCircuitID() != INDEX_NONE) { MemberCounts.FindOrAdd(Connection->GetCircuitID())++; break; }
    }
    TArray<int32> CircuitIds; MemberCounts.GetKeys(CircuitIds); CircuitIds.Sort();
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const int32 CircuitId : CircuitIds)
    {
        UFGPowerCircuit* Circuit = Cast<UFGPowerCircuit>(Circuits->FindCircuit(CircuitId));
        if (!Circuit) continue;
        FPowerCircuitStats Stats;
        Circuit->GetStats(Stats);
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("circuitId"), CircuitId);
        Item->SetNumberField(TEXT("observedBuildables"), MemberCounts[CircuitId]);
        Item->SetNumberField(TEXT("productionMW"), Stats.PowerProduced);
        Item->SetNumberField(TEXT("consumptionMW"), Stats.PowerConsumed);
        Item->SetNumberField(TEXT("capacityMW"), Circuit->GetPowerProductionCapacity());
        Item->SetNumberField(TEXT("maximumConsumptionMW"), Circuit->GetMaximumPowerConsumption());
        // With the "no power" advanced game setting the game reports production equal to consumption (verified live).
        Item->SetBoolField(TEXT("noPowerCheat"), Circuit->IsNoPowerCheatOn());
        Item->SetBoolField(TEXT("fuseTriggered"), Circuit->IsFuseTriggered());
        const TSharedRef<FJsonObject> Battery = MakeShared<FJsonObject>();
        Battery->SetNumberField(TEXT("inputMW"), Stats.BatteryPowerInput);
        Battery->SetNumberField(TEXT("storedMWh"), Circuit->GetBatterySumPowerStore());
        Battery->SetNumberField(TEXT("capacityMWh"), Circuit->GetBatterySumPowerStoreCapacity());
        Battery->SetNumberField(TEXT("secondsUntilEmpty"), Circuit->GetTimeToBatteriesEmpty());
        Battery->SetNumberField(TEXT("secondsUntilFull"), Circuit->GetTimeToBatteriesFull());
        Item->SetObjectField(TEXT("battery"), Battery);
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetArrayField(TEXT("circuits"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandlePlayerInventory(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    UFGInventoryComponent* Inventory = GetPlayerInventory(World);
    if (!Inventory) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("The player inventory is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TMap<FString, int32> Totals;
    TArray<TSharedPtr<FJsonValue>> Stacks = InventoryJson(Inventory);
    for (const TSharedPtr<FJsonValue>& Stack : Stacks)
    {
        const TSharedPtr<FJsonObject>& Entry = Stack->AsObject();
        Totals.FindOrAdd(Entry->GetStringField(TEXT("itemPath"))) += static_cast<int32>(Entry->GetNumberField(TEXT("count")));
    }
    TArray<TSharedPtr<FJsonValue>> TotalItems;
    for (const TPair<FString, int32>& Total : Totals)
    {
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("itemPath"), Total.Key);
        Entry->SetNumberField(TEXT("count"), Total.Value);
        TotalItems.Add(MakeShared<FJsonValueObject>(Entry));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetNumberField(TEXT("slots"), Inventory->GetSizeLinear());
    Body->SetBoolField(TEXT("noBuildCost"), Inventory->GetNoBuildCost());
    Body->SetArrayField(TEXT("stacks"), Stacks);
    Body->SetArrayField(TEXT("totals"), TotalItems);
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * Unlocked buildables (#114): every available recipe whose product is a building descriptor. `classPath` filters to
 * exact classes; `details=true` (or a classPath filter) adds the port layout and approximate local mesh bounds.
 */
bool FMKIBridgeModule::HandleCatalogBuildables(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
    if (!Recipes) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("The recipe manager is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    const TArray<FString> ClassPaths = QueryList(Request, TEXT("classPath"));
    const FString* DetailsText = Request.QueryParams.Find(TEXT("details"));
    const bool bDetails = ClassPaths.Num() > 0 || (DetailsText && DetailsText->Equals(TEXT("true"), ESearchCase::CaseSensitive));
    TArray<TSubclassOf<UFGRecipe>> Available;
    Recipes->GetAllAvailableRecipes(Available);
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const TSubclassOf<UFGRecipe>& Recipe : Available)
    {
        if (!Recipe) continue;
        for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
        {
            UClass* ItemClass = Product.ItemClass.Get();
            if (!ItemClass || !ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass())) continue;
            UClass* BuildableClass = UFGBuildingDescriptor::GetBuildableClass(TSubclassOf<UFGBuildingDescriptor>(ItemClass)).Get();
            if (!BuildableClass) continue;
            if (ClassPaths.Num() > 0 && !ContainsCaseSensitive(ClassPaths, BuildableClass->GetPathName())) continue;
            Items.Add(MakeShared<FJsonValueObject>(CatalogBuildableJson(World, Recipe, BuildableClass, Product.ItemClass, bDetails)));
        }
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("details"), bDetails);
    Body->SetNumberField(TEXT("total"), Items.Num());
    Body->SetArrayField(TEXT("buildables"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * Unlocked production recipes (#114): ingredients, products (with form; fluid amounts in the game's units), duration,
 * and producing machines. `all=true` also lists building and customization recipes.
 */
bool FMKIBridgeModule::HandleCatalogRecipes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
    if (!Recipes) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("The recipe manager is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    const FString* AllText = Request.QueryParams.Find(TEXT("all"));
    const bool bAll = AllText && AllText->Equals(TEXT("true"), ESearchCase::CaseSensitive);
    TArray<TSubclassOf<UFGRecipe>> Available;
    Recipes->GetAllAvailableRecipes(Available);
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const TSubclassOf<UFGRecipe>& Recipe : Available)
    {
        if (!Recipe) continue;
        TArray<TSharedPtr<FJsonValue>> ProducedIn;
        for (const TSubclassOf<UObject>& Producer : UFGRecipe::GetProducedIn(Recipe))
        {
            UClass* Class = Producer.Get();
            if (Class && Class->IsChildOf(AFGBuildableManufacturer::StaticClass())) ProducedIn.Add(MakeShared<FJsonValueString>(Class->GetPathName()));
        }
        if (!bAll && ProducedIn.Num() == 0) continue;
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("recipePath"), Recipe->GetPathName());
        Item->SetStringField(TEXT("recipeName"), UFGRecipe::GetRecipeName(Recipe).ToString());
        Item->SetNumberField(TEXT("durationSeconds"), UFGRecipe::GetManufacturingDuration(Recipe));
        Item->SetArrayField(TEXT("ingredients"), ItemAmountsJson(UFGRecipe::GetIngredients(World, Recipe)));
        Item->SetArrayField(TEXT("products"), ItemAmountsJson(UFGRecipe::GetProducts(Recipe)));
        Item->SetArrayField(TEXT("producedIn"), ProducedIn);
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("fluidUnits"), TEXT("amounts of liquid and gas items are in thousandths of a cubic metre"));
    Body->SetNumberField(TEXT("total"), Items.Num());
    Body->SetArrayField(TEXT("recipes"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleProgression(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGSchematicManager* Schematics = World ? AFGSchematicManager::Get(World) : nullptr;
    if (!Schematics) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("The schematic manager is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TArray<TSubclassOf<UFGSchematic>> Purchased;
    Schematics->GetAllPurchasedSchematics(Purchased);
    TArray<TSharedPtr<FJsonValue>> Items;
    int32 HighestTier = 0;
    for (const TSubclassOf<UFGSchematic>& Schematic : Purchased)
    {
        if (!Schematic) continue;
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("schematicPath"), Schematic->GetPathName());
        Item->SetStringField(TEXT("name"), UFGSchematic::GetSchematicDisplayName(Schematic).ToString());
        Item->SetStringField(TEXT("type"), StaticEnum<ESchematicType>()->GetNameStringByValue(static_cast<int64>(UFGSchematic::GetType(Schematic))));
        const int32 Tier = UFGSchematic::GetTechTier(Schematic);
        Item->SetNumberField(TEXT("tier"), Tier);
        if (UFGSchematic::GetType(Schematic) == ESchematicType::EST_Milestone) HighestTier = FMath::Max(HighestTier, Tier);
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetNumberField(TEXT("purchasedCount"), Items.Num());
    Body->SetNumberField(TEXT("highestMilestoneTier"), HighestTier);
    if (const TSubclassOf<UFGSchematic> Active = Schematics->GetActiveSchematic()) Body->SetStringField(TEXT("activeSchematicPath"), Active->GetPathName());
    Body->SetArrayField(TEXT("purchasedSchematics"), Items);
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * Resource nodes (#114): ore nodes, geysers, fracking cores and satellites. Each node reports `id`, `kind`
 * (`ore_node`, `geyser`, `fracking_core`, `fracking_satellite`), `resource` (item descriptor class name; omitted for
 * geysers), `form`, `purity` (omitted for fracking cores, which have none), `occupied`, `extractorId` when an
 * extractor or activator is bound, and `location`; fracking entries keep `satelliteCount`, `state`, and `coreId`.
 * Filters: `center`+`radiusCm`, `bounds`, `kind` and `resource` (comma-separated exact values).
 */
bool FMKIBridgeModule::HandleResourceNodes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if(!IsAuthorized(Request)){OnComplete(ErrorResponse(TEXT("unauthorized"),TEXT("A valid bearer token is required."),EHttpServerResponseCodes::Denied));return true;}
    UWorld* World=GEngine?GEngine->GetCurrentPlayWorld():nullptr;
    if(!World){OnComplete(ErrorResponse(TEXT("world_unavailable"),TEXT("World resource nodes are not ready."),EHttpServerResponseCodes::ServiceUnavail));return true;}
    FSpatialFilter Filter; FString Message;
    if (!ParseSpatialFilter(Request, Filter, Message)) { OnComplete(ErrorResponse(TEXT("invalid_query"), Message, EHttpServerResponseCodes::BadRequest)); return true; }
    const TArray<FString> Kinds = QueryList(Request, TEXT("kind"));
    TArray<FString> KnownKinds; KnownKinds.Add(TEXT("ore_node")); KnownKinds.Add(TEXT("geyser")); KnownKinds.Add(TEXT("fracking_core")); KnownKinds.Add(TEXT("fracking_satellite"));
    for (const FString& Kind : Kinds)
        if (!ContainsCaseSensitive(KnownKinds, Kind))
        { OnComplete(ErrorResponse(TEXT("invalid_query"), TEXT("kind must be ore_node, geyser, fracking_core, or fracking_satellite."), EHttpServerResponseCodes::BadRequest)); return true; }
    const TArray<FString> Resources = QueryList(Request, TEXT("resource"));
    auto VectorJson=[](const FVector& V){auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("x"),V.X);O->SetNumberField(TEXT("y"),V.Y);O->SetNumberField(TEXT("z"),V.Z);return O;};
    // Extractors report the node they mine; nodes do not reliably report their extractor.
    TMap<const UObject*, FString> ExtractorByNode;
    for (TActorIterator<AFGBuildableResourceExtractorBase> It(World); It; ++It)
        if (IsValid(*It)) if (const UObject* Node = It->GetExtractableResource().GetObject()) ExtractorByNode.Add(Node, It->GetName());
    TArray<TSharedPtr<FJsonValue>> Items;
    auto Add = [&](AActor* Node, const TCHAR* Kind, TSubclassOf<UFGResourceDescriptor> Resource, const TCHAR* Purity, bool bOccupied, const FString& ExtractorId) -> TSharedPtr<FJsonObject>
    {
        if (!Filter.Matches(Node->GetActorLocation())) return nullptr;
        if (Kinds.Num() > 0 && !ContainsCaseSensitive(Kinds, Kind)) return nullptr;
        const bool bGeyser = FCString::Strcmp(Kind, TEXT("geyser")) == 0;
        if (Resources.Num() > 0 && (bGeyser || !Resource || !ContainsCaseSensitive(Resources, Resource->GetName()))) return nullptr;
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Node->GetName());
        Item->SetStringField(TEXT("kind"), Kind);
        if (!bGeyser && Resource) Item->SetStringField(TEXT("resource"), Resource->GetName());
        Item->SetStringField(TEXT("form"), bGeyser ? TEXT("heat") : Resource ? ResourceFormName(UFGItemDescriptor::GetForm(Resource)) : TEXT("invalid"));
        if (Purity) Item->SetStringField(TEXT("purity"), Purity);
        Item->SetBoolField(TEXT("occupied"), bOccupied || !ExtractorId.IsEmpty());
        if (!ExtractorId.IsEmpty()) Item->SetStringField(TEXT("extractorId"), ExtractorId);
        Item->SetObjectField(TEXT("location"), VectorJson(Node->GetActorLocation()));
        Items.Add(MakeShared<FJsonValueObject>(Item));
        return Item;
    };
    for(TActorIterator<AFGResourceNodeFrackingCore> It(World);It;++It){
        AFGResourceNodeFrackingCore* Core=*It;if(!Core)continue;
        const FString Activator = Core->GetActivator().IsValid() ? Core->GetActivator()->GetName() : FString();
        if (TSharedPtr<FJsonObject> Item = Add(Core, TEXT("fracking_core"), Core->GetResourceClass(), nullptr, Core->GetActivator().IsValid(), Activator))
            Item->SetNumberField(TEXT("satelliteCount"),Core->Native_GetSatellites().Num());
    }
    for(TActorIterator<AFGResourceNodeFrackingSatellite> It(World);It;++It){
        AFGResourceNodeFrackingSatellite* Satellite=*It;if(!Satellite)continue;
        FString Extractor = Satellite->GetExtractor().IsValid() ? Satellite->GetExtractor()->GetName() : ExtractorByNode.FindRef(Satellite);
        if (TSharedPtr<FJsonObject> Item = Add(Satellite, TEXT("fracking_satellite"), Satellite->GetResourceClass(), ResourcePurityName(Satellite->GetResourcePurity()), Satellite->GetExtractor().IsValid(), Extractor))
        {
            Item->SetNumberField(TEXT("state"),static_cast<uint8>(Satellite->GetState()));
            if(Satellite->GetCore().IsValid())Item->SetStringField(TEXT("coreId"),Satellite->GetCore()->GetName());
        }
    }
    for (TActorIterator<AFGResourceNode> It(World); It; ++It)
    {
        AFGResourceNode* Node = *It;
        if (!Node || Node->IsA<AFGResourceNodeFrackingSatellite>() || Node->IsA<AFGResourceNodeFrackingCore>()) continue;
        const bool bGeyser = Node->IsA<AFGResourceNodeGeyser>();
        Add(Node, bGeyser ? TEXT("geyser") : TEXT("ore_node"), Node->GetResourceClass(), ResourcePurityName(Node->GetResourcePurity()), Node->IsOccupied(), ExtractorByNode.FindRef(Node));
    }
    auto Body=MakeShared<FJsonObject>();Body->SetBoolField(TEXT("ok"),true);Body->SetNumberField(TEXT("total"),Items.Num());Body->SetArrayField(TEXT("nodes"),Items);OnComplete(JsonResponse(Body));return true;
}

/**
 * GET /v1/conveyors?ids=&center=&radiusCm=&limit=: what each belt and lift is carrying (#304). Since 1.0 items travel on
 * conveyor chain actors, so each conveyor reports its own items (by item class, from its chain segment), its chain
 * (`chainId`, `chainItems`, `chainAvailableSpaceCm`, `chainFull` when less than one item spacing is free,
 * `chainOutputWaiting` when an item waits at the chain's exit), and its two ends with what they are attached to. Each item class carries `fuelEnergyMJ` (0 for an item that is not a fuel), so a
 * caller can tell ore backed up at a fuel-only input. Read-only.
 */
bool FMKIBridgeModule::HandleConveyors(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TSet<FString> Ids;
    if (const FString* Text = Request.QueryParams.Find(TEXT("ids"))) { TArray<FString> Parts; Text->ParseIntoArray(Parts, TEXT(","), true); Ids.Append(Parts); }
    bool bHasCenter = false; FVector Center = FVector::ZeroVector; double RadiusCm = 5000.0;
    if (const FString* Text = Request.QueryParams.Find(TEXT("center")))
    {
        TArray<FString> Parts; Text->ParseIntoArray(Parts, TEXT(","), true);
        if (Parts.Num() != 3) { OnComplete(ErrorResponse(TEXT("invalid_query"), TEXT("center must be x,y,z."), EHttpServerResponseCodes::BadRequest)); return true; }
        Center = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2])); bHasCenter = true;
    }
    if (const FString* Text = Request.QueryParams.Find(TEXT("radiusCm"))) RadiusCm = FMath::Clamp(FCString::Atod(**Text), 100.0, 200000.0);
    int32 Limit = 500;
    if (const FString* Text = Request.QueryParams.Find(TEXT("limit"))) Limit = FMath::Clamp(FCString::Atoi(**Text), 1, 2000);
    if (Ids.Num() == 0 && !bHasCenter) { OnComplete(ErrorResponse(TEXT("invalid_query"), TEXT("Pass ids or center (with radiusCm)."), EHttpServerResponseCodes::BadRequest)); return true; }

    TArray<AFGBuildableConveyorBase*> Conveyors;
    for (TActorIterator<AFGBuildableConveyorBase> It(World); It; ++It)
    {
        AFGBuildableConveyorBase* Conveyor = *It;
        if (!IsValid(Conveyor)) continue;
        if (Ids.Num() && !Ids.Contains(Conveyor->GetName())) continue;
        if (bHasCenter && FVector::Dist(Conveyor->GetActorLocation(), Center) > RadiusCm) continue;
        Conveyors.Add(Conveyor);
    }
    Conveyors.Sort([](const AFGBuildableConveyorBase& A, const AFGBuildableConveyorBase& B) { return A.GetName() < B.GetName(); });
    const bool bTruncated = Conveyors.Num() > Limit;
    if (bTruncated) Conveyors.SetNum(Limit);

    TArray<TSharedPtr<FJsonValue>> Items;
    for (AFGBuildableConveyorBase* Conveyor : Conveyors)
    {
        const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("id"), Conveyor->GetName());
        Json->SetStringField(TEXT("className"), Conveyor->GetClass()->GetName());
        Json->SetObjectField(TEXT("position"), BridgeVectorJson(Conveyor->GetActorLocation()));
        Json->SetNumberField(TEXT("lengthCm"), Conveyor->GetLength());
        Json->SetNumberField(TEXT("speedCmPerS"), Conveyor->GetSpeed());
        TMap<UClass*, int32> Counts; int32 Count = 0;
        if (AFGConveyorChainActor* Chain = Conveyor->GetConveyorChainActor())
        {
            TArray<FConveyorBeltItem*> OnBelt; Chain->GetItemsForSegment(Conveyor, OnBelt);
            for (const FConveyorBeltItem* Item : OnBelt) if (Item && Item->Item.GetItemClass()) { ++Counts.FindOrAdd(Item->Item.GetItemClass()); ++Count; }
            Json->SetStringField(TEXT("chainId"), Chain->GetName());
            Json->SetNumberField(TEXT("chainItems"), Chain->GetNumActualItems());
            const float Space = Chain->GetAvailableSpace();
            Json->SetNumberField(TEXT("chainAvailableSpaceCm"), Space);
            // Less room than one item: the game spaces items 120 cm apart (AFGBuildableConveyorBase::ITEM_SPACING, protected).
            Json->SetBoolField(TEXT("chainFull"), Space < 120.0f);
            // An item at the chain's exit that nothing has taken yet: a chain stopped at a port that refuses its items is not
            // full until the whole belt fills, but its lead item waits at the end (peeking takes nothing).
            TArray<FInventoryItem> Waiting;
            Json->SetBoolField(TEXT("chainOutputWaiting"), Chain->Factory_PeekOutput(Waiting));
        }
        Json->SetNumberField(TEXT("itemCount"), Count);
        TArray<TSharedPtr<FJsonValue>> Groups;
        for (const TPair<UClass*, int32>& Group : Counts)
        {
            const TSharedRef<FJsonObject> G = MakeShared<FJsonObject>();
            G->SetStringField(TEXT("item"), Group.Key->GetName()); G->SetNumberField(TEXT("count"), Group.Value);
            G->SetNumberField(TEXT("fuelEnergyMJ"), UFGItemDescriptor::GetEnergyValue(TSubclassOf<UFGItemDescriptor>(Group.Key)));
            Groups.Add(MakeShared<FJsonValueObject>(G));
        }
        Json->SetArrayField(TEXT("items"), Groups);
        auto End = [](UFGFactoryConnectionComponent* Connection)
        {
            const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
            if (!Connection) return E;
            E->SetStringField(TEXT("component"), Connection->GetName());
            E->SetBoolField(TEXT("connected"), Connection->IsConnected());
            if (const UFGFactoryConnectionComponent* Other = Connection->GetConnection())
            {
                if (const AActor* Owner = Other->GetOwner()) E->SetStringField(TEXT("connectedBuildableId"), Owner->GetName());
                E->SetStringField(TEXT("connectedComponent"), Other->GetName());
            }
            return E;
        };
        Json->SetObjectField(TEXT("input"), End(Conveyor->GetConnection0()));
        Json->SetObjectField(TEXT("output"), End(Conveyor->GetConnection1()));
        Items.Add(MakeShared<FJsonValueObject>(Json));
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetArrayField(TEXT("conveyors"), Items);
    Body->SetNumberField(TEXT("total"), Items.Num());
    Body->SetBoolField(TEXT("truncated"), bTruncated);
    OnComplete(JsonResponse(Body));
    return true;
}

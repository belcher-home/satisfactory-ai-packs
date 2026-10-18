#include "BridgeInternal.h"
#include "FGRailroadVehicle.h"
#include "FGGameRulesSubsystem.h"
#include "FGGameState.h"
#include "FGTimeSubsystem.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"
#include "Components/SplineComponent.h"
#include "EngineUtils.h"
#include "WheeledVehicles/FGVehiclePathPreset.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"

// Vehicles, docking stations and inventories (#263): observation, a vehicle's station route and self-driving, a station's
// load or unload mode, and moving items between inventories. These are the game's own vehicle-identifier and
// docking-station calls, the same ones its vehicle and station menus make; the bridge decides nothing about routes.

namespace
{
    TArray<TSharedPtr<FJsonValue>> StacksJson(UFGInventoryComponent* Inventory)
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

    /** Every inventory component an actor owns, by component name (for example FuelInventory, StorageInventory). */
    TSharedRef<FJsonObject> InventoriesJson(AActor* Actor)
    {
        const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Actor) return Out;
        TInlineComponentArray<UFGInventoryComponent*> Components(Actor);
        for (UFGInventoryComponent* Component : Components)
        {
            if (!Component) continue;
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("slots"), Component->GetSizeLinear());
            Entry->SetArrayField(TEXT("stacks"), StacksJson(Component));
            Out->SetObjectField(Component->GetName(), Entry);
        }
        return Out;
    }

    UFGInventoryComponent* FindInventory(AActor* Actor, const FString& Name)
    {
        if (!Actor) return nullptr;
        TInlineComponentArray<UFGInventoryComponent*> Components(Actor);
        if (Name.IsEmpty() && Components.Num() == 1) return Components[0];
        for (UFGInventoryComponent* Component : Components) if (Component && Component->GetName().Equals(Name, ESearchCase::IgnoreCase)) return Component;
        return nullptr;
    }

    AFGWheeledVehicleIdentifier* FindVehicleIdentifier(UWorld* World, const FString& VehicleId)
    {
        AFGVehicleSubsystem* Subsystem = World ? AFGVehicleSubsystem::Get(World) : nullptr;
        if (!Subsystem || VehicleId.IsEmpty()) return nullptr;
        for (AFGWheeledVehicleIdentifier* Identifier : Subsystem->GetAllVehicles())
        {
            if (!IsValid(Identifier)) continue;
            AFGWheeledVehicle* Vehicle = Identifier->GetOwnerVehicle();
            if ((Vehicle && Vehicle->GetName().Equals(VehicleId, ESearchCase::CaseSensitive)) || Identifier->GetName().Equals(VehicleId, ESearchCase::CaseSensitive)) return Identifier;
        }
        return nullptr;
    }

    FString GuidText(const FGuid& Guid) { return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : FString(); }

    const TCHAR* AutopilotErrorName(EVehicleAutopilotErrorStatus Status)
    {
        switch (Status)
        {
        case EVehicleAutopilotErrorStatus::None: return TEXT("none");
        case EVehicleAutopilotErrorStatus::StationUnreachable: return TEXT("station_unreachable");
        case EVehicleAutopilotErrorStatus::NotOnPath: return TEXT("not_on_path");
        case EVehicleAutopilotErrorStatus::TooFewStations: return TEXT("too_few_stations");
        case EVehicleAutopilotErrorStatus::NoFuel: return TEXT("no_fuel");
        case EVehicleAutopilotErrorStatus::Deadlocked: return TEXT("deadlocked");
        default: return TEXT("unknown");
        }
    }

    const TCHAR* StationStatusName(EDockingStationStatus Status)
    {
        switch (Status)
        {
        case EDockingStationStatus::DSS_Operational: return TEXT("operational");
        case EDockingStationStatus::DSS_FuelTypeWarning: return TEXT("fuel_type_warning");
        case EDockingStationStatus::DSS_NoPowerWarning: return TEXT("no_power");
        default: return TEXT("unknown");
        }
    }

    TSharedRef<FJsonObject> VehicleJson(AFGWheeledVehicleIdentifier* Identifier)
    {
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        AFGWheeledVehicle* Vehicle = Identifier->GetOwnerVehicle();
        Item->SetStringField(TEXT("id"), Vehicle ? Vehicle->GetName() : Identifier->GetName());
        Item->SetStringField(TEXT("identifierId"), Identifier->GetName());
        Item->SetStringField(TEXT("name"), Identifier->GetVehicleName().ToString());
        if (const UClass* Class = Identifier->GetVehicleClass().Get()) Item->SetStringField(TEXT("className"), Class->GetName());
        if (Vehicle)
        {
            Item->SetObjectField(TEXT("location"), BridgeVectorJson(Vehicle->GetActorLocation()));
            Item->SetNumberField(TEXT("yaw"), Vehicle->GetActorRotation().Yaw);
            Item->SetNumberField(TEXT("speedCmPerS"), Vehicle->GetVelocity().Size());
            Item->SetObjectField(TEXT("inventories"), InventoriesJson(Vehicle));
            if (AActor* Docked = Vehicle->GetCurrentlyDockingStationActor()) Item->SetStringField(TEXT("dockingStationId"), Docked->GetName());
        }
        Item->SetBoolField(TEXT("autopilotEnabled"), Identifier->IsAutopilotEnabled());
        Item->SetBoolField(TEXT("canEnableAutopilot"), Identifier->CanEnableAutopilot());
        Item->SetStringField(TEXT("autopilotError"), AutopilotErrorName(Identifier->GetAutopilotErrorStatus()));
        Item->SetBoolField(TEXT("docking"), Identifier->IsCurrentlyDocking());
        Item->SetNumberField(TEXT("targetWaypointIndex"), Identifier->GetCurrentTargetWaypointIndex());
        Item->SetBoolField(TEXT("onPath"), Identifier->GetCurrentFromPathNodeGUID().IsValid());
        TArray<TSharedPtr<FJsonValue>> Route;
        for (const FGuid& Guid : Identifier->GetVehicleRoute()) Route.Add(MakeShared<FJsonValueString>(GuidText(Guid)));
        Item->SetArrayField(TEXT("route"), Route);
        return Item;
    }

    TSharedRef<FJsonObject> StationJson(AFGDockingStationIdentifier* Identifier)
    {
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        AFGBuildableDockingStation* Station = Identifier->GetStation();
        Item->SetStringField(TEXT("id"), Station ? Station->GetName() : Identifier->GetName());
        Item->SetStringField(TEXT("name"), Identifier->GetStationName().ToString());
        Item->SetStringField(TEXT("pathNodeGuid"), GuidText(Identifier->GetPathNodeGUID()));
        Item->SetStringField(TEXT("status"), StationStatusName(Identifier->GetStationStatus()));
        if (Station)
        {
            Item->SetStringField(TEXT("className"), Station->GetClass()->GetName());
            Item->SetObjectField(TEXT("location"), BridgeVectorJson(Station->GetActorLocation()));
            Item->SetNumberField(TEXT("yaw"), Station->GetActorRotation().Yaw);
            Item->SetBoolField(TEXT("loadMode"), Station->GetIsInLoadMode());
            Item->SetBoolField(TEXT("loadUnloading"), Station->IsLoadUnloading());
            if (AActor* Docked = Station->GetDockedActor()) Item->SetStringField(TEXT("dockedVehicleId"), Docked->GetName());
            Item->SetObjectField(TEXT("inventories"), InventoriesJson(Station));
        }
        return Item;
    }

    const TCHAR* PathValidationName(EVehiclePathValidationResult Result)
    {
        switch (Result)
        {
        case EVehiclePathValidationResult::Valid: return TEXT("valid");
        case EVehiclePathValidationResult::TurnTooSharp: return TEXT("turn_too_sharp");
        case EVehiclePathValidationResult::NoFloor: return TEXT("no_floor");
        case EVehiclePathValidationResult::TerrainTooSteep: return TEXT("terrain_too_steep");
        case EVehiclePathValidationResult::StaticObstacle: return TEXT("static_obstacle");
        case EVehiclePathValidationResult::InvalidShape: return TEXT("invalid_shape");
        case EVehiclePathValidationResult::SubmergedInWater: return TEXT("submerged_in_water");
        default: return TEXT("unknown");
        }
    }

    /**
     * The game's own validation of every vehicle path segment (#312): the segments some vehicle type cannot drive, with
     * the reason and the point on the path where the check failed (a static obstacle is what a fence on a lane causes).
     */
    TSharedRef<FJsonObject> PathsJson(UWorld* World)
    {
        int32 Segments = 0;
        TArray<TSharedPtr<FJsonValue>> Invalid;
        for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It)
        {
            AFGVehiclePathSegment* Segment = *It;
            if (!IsValid(Segment)) continue;
            ++Segments;
            const USplineComponent* Spline = Segment->GetSplineComponent();
            for (const FVehiclePathSegmentValidationData& Data : Segment->GetPathSegmentValidationData())
            {
                if (Data.ValidationInfo.Result == EVehiclePathValidationResult::Valid) continue;
                const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("id"), Segment->GetName());
                Item->SetStringField(TEXT("preset"), Data.PathPreset ? Data.PathPreset->GetName() : FString());
                Item->SetStringField(TEXT("result"), PathValidationName(Data.ValidationInfo.Result));
                Item->SetNumberField(TEXT("failDistanceCm"), Data.ValidationInfo.SplineValidationFailDistance);
                if (Spline && Spline->GetNumberOfSplinePoints() >= 2)
                {
                    Item->SetObjectField(TEXT("from"), BridgeVectorJson(Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World)));
                    Item->SetObjectField(TEXT("to"), BridgeVectorJson(Spline->GetLocationAtSplinePoint(Spline->GetNumberOfSplinePoints() - 1, ESplineCoordinateSpace::World)));
                    Item->SetObjectField(TEXT("failAt"), BridgeVectorJson(Spline->GetLocationAtDistanceAlongSpline(Data.ValidationInfo.SplineValidationFailDistance, ESplineCoordinateSpace::World)));
                }
                Invalid.Add(MakeShared<FJsonValueObject>(Item));
            }
        }
        const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("segments"), Segments);
        Out->SetArrayField(TEXT("invalid"), Invalid);
        return Out;
    }

    bool ReadBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& Json)
    {
        FString Text;
        if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); Text = FString(Converted.Length(), Converted.Get()); }
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        return FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid();
    }
}

bool FMKIBridgeModule::HandleVehicles(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGVehicleSubsystem* Subsystem = World ? AFGVehicleSubsystem::Get(World) : nullptr;
    if (!Subsystem) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("Vehicle subsystem is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TArray<TSharedPtr<FJsonValue>> Vehicles, Stations;
    for (AFGWheeledVehicleIdentifier* Identifier : Subsystem->GetAllVehicles()) if (IsValid(Identifier)) Vehicles.Add(MakeShared<FJsonValueObject>(VehicleJson(Identifier)));
    for (AFGDockingStationIdentifier* Identifier : Subsystem->GetDockingStations()) if (IsValid(Identifier)) Stations.Add(MakeShared<FJsonValueObject>(StationJson(Identifier)));
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetArrayField(TEXT("vehicles"), Vehicles);
    Body->SetArrayField(TEXT("stations"), Stations);
    // Rail vehicles (locomotives and wagons), which the wheeled-vehicle list leaves out: each by its exact actor name, so
    // /v1/dismantle can remove it (a track with a vehicle on it refuses to be dismantled).
    TArray<TSharedPtr<FJsonValue>> RailVehicles;
    for (TActorIterator<AFGRailroadVehicle> It(World); It; ++It)
    {
        if (!IsValid(*It)) continue;
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), It->GetName());
        Item->SetStringField(TEXT("className"), It->GetClass()->GetName());
        Item->SetObjectField(TEXT("location"), BridgeVectorJson(It->GetActorLocation()));
        RailVehicles.Add(MakeShared<FJsonValueObject>(Item));
    }
    Body->SetArrayField(TEXT("railVehicles"), RailVehicles);
    Body->SetObjectField(TEXT("paths"), PathsJson(World));
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleVehicleRoute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString VehicleId; Json->TryGetStringField(TEXT("vehicleId"), VehicleId);
    bool Confirm = false; Json->TryGetBoolField(TEXT("confirm"), Confirm);
    bool Autopilot = false; const bool HasAutopilot = Json->TryGetBoolField(TEXT("autopilot"), Autopilot);
    const TArray<TSharedPtr<FJsonValue>>* StationValues = nullptr; const bool HasRoute = Json->TryGetArrayField(TEXT("stationIds"), StationValues);
    if (VehicleId.IsEmpty() || (!HasRoute && !HasAutopilot)) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("vehicleId and stationIds and/or autopilot are required."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (World && World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Vehicle routes are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    AFGWheeledVehicleIdentifier* Identifier = FindVehicleIdentifier(World, VehicleId);
    if (!Identifier) { OnComplete(ErrorResponse(TEXT("vehicle_not_found"), TEXT("No vehicle with that exact ID was found."), EHttpServerResponseCodes::NotFound)); return true; }
    TArray<FGuid> Route;
    if (HasRoute)
    {
        for (const TSharedPtr<FJsonValue>& Value : *StationValues)
        {
            FString StationId; if (!Value.IsValid() || !Value->TryGetString(StationId)) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("stationIds must be strings."), EHttpServerResponseCodes::BadRequest)); return true; }
            AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(FindBuildableById(World, StationId));
            AFGDockingStationIdentifier* StationIdentifier = Station ? Station->GetStationIdentifier() : nullptr;
            if (!StationIdentifier || !StationIdentifier->GetPathNodeGUID().IsValid()) { OnComplete(ErrorResponse(TEXT("station_not_found"), FString::Printf(TEXT("%s is not a docking station on a vehicle path."), *StationId), EHttpServerResponseCodes::NotFound)); return true; }
            Route.Add(StationIdentifier->GetPathNodeGUID());
        }
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("confirmationRequired"), !Confirm);
    if (Confirm)
    {
        if (HasRoute) Identifier->SetVehicleRoute(Route);
        if (HasAutopilot)
        {
            if (Autopilot && !Identifier->CanEnableAutopilot()) { Body->SetBoolField(TEXT("ok"), false); Body->SetStringField(TEXT("error"), TEXT("autopilot_unavailable")); Body->SetStringField(TEXT("message"), TEXT("The game refuses self-driving for this vehicle now (CanEnableAutopilot is false).")); Body->SetObjectField(TEXT("vehicle"), VehicleJson(Identifier)); OnComplete(JsonResponse(Body, EHttpServerResponseCodes::Conflict)); return true; }
            Identifier->SetAutopilotEnabled(Autopilot);
        }
    }
    Body->SetObjectField(TEXT("vehicle"), VehicleJson(Identifier));
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleDockingStation(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString StationId; Json->TryGetStringField(TEXT("stationId"), StationId);
    bool LoadMode = false; const bool HasLoadMode = Json->TryGetBoolField(TEXT("loadMode"), LoadMode);
    bool Confirm = false; Json->TryGetBoolField(TEXT("confirm"), Confirm);
    if (StationId.IsEmpty() || !HasLoadMode) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("stationId and loadMode are required."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (World && World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Station settings are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    AFGBuildableDockingStation* Station = Cast<AFGBuildableDockingStation>(FindBuildableById(World, StationId));
    if (!Station) { OnComplete(ErrorResponse(TEXT("station_not_found"), TEXT("No docking station with that exact buildable ID was found."), EHttpServerResponseCodes::NotFound)); return true; }
    if (Confirm) Station->SetIsInLoadMode(LoadMode);
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("loadMode"), Station->GetIsInLoadMode());
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleVehiclePathRevalidate(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    // The game keeps each path segment's validation until something asks again, so a removed obstacle leaves the segment
    // reported blocked (#312: a walkway moved off a lane stayed static_obstacle). This is the game's own batched
    // re-validation, the one it runs when buildings change near a path.
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    const TSharedPtr<FJsonObject>* MinJson = nullptr;
    const TSharedPtr<FJsonObject>* MaxJson = nullptr;
    auto ReadVector = [](const TSharedPtr<FJsonObject>& Object, FVector& Out)
    {
        double X = 0, Y = 0, Z = 0;
        const bool bOk = Object.IsValid() && Object->TryGetNumberField(TEXT("x"), X) && Object->TryGetNumberField(TEXT("y"), Y) && Object->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X, Y, Z);
        return bOk;
    };
    FVector Min, Max;
    if (!Json->TryGetObjectField(TEXT("min"), MinJson) || !Json->TryGetObjectField(TEXT("max"), MaxJson) || !ReadVector(*MinJson, Min) || !ReadVector(*MaxJson, Max) || Min.X >= Max.X || Min.Y >= Max.Y || Min.Z >= Max.Z)
    { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("min and max ({x,y,z}, min below max on every axis) are required."), EHttpServerResponseCodes::BadRequest)); return true; }
    if ((Max - Min).GetMax() > 1000000.0) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("The box may be at most 10 km on a side."), EHttpServerResponseCodes::BadRequest)); return true; }
    bool Confirm = false; Json->TryGetBoolField(TEXT("confirm"), Confirm);
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGVehicleSubsystem* Subsystem = World ? AFGVehicleSubsystem::Get(World) : nullptr;
    if (!Subsystem) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("Vehicle subsystem is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Path validation is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    const FBox Box(Min, Max);
    int32 Segments = 0;
    for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It) if (IsValid(*It) && Box.Intersect(It->GetComponentsBoundingBox())) ++Segments;
    if (Confirm) Subsystem->InvalidateVehiclePathsInVolume(Box);
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetNumberField(TEXT("segments"), Segments);
    Body->SetStringField(TEXT("note"), TEXT("Re-validation is spread over the next frames; read GET /v1/vehicles paths after a moment."));
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleInventoryTransfer(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString FromId, FromInventory, ToId, ToInventory, ItemQuery; double CountValue = 0;
    Json->TryGetStringField(TEXT("fromId"), FromId); Json->TryGetStringField(TEXT("fromInventory"), FromInventory);
    Json->TryGetStringField(TEXT("toId"), ToId); Json->TryGetStringField(TEXT("toInventory"), ToInventory);
    Json->TryGetStringField(TEXT("item"), ItemQuery); Json->TryGetNumberField(TEXT("count"), CountValue);
    bool Confirm = false; Json->TryGetBoolField(TEXT("confirm"), Confirm);
    const int32 Count = static_cast<int32>(CountValue);
    if (FromId.IsEmpty() || ToId.IsEmpty() || ItemQuery.IsEmpty() || Count < 1 || CountValue != Count) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("fromId, toId, item and a whole count of at least 1 are required (\"player\" names the player)."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Inventory changes are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    auto Resolve = [this, World](const FString& Id, const FString& Name) -> UFGInventoryComponent*
    {
        if (Id.Equals(TEXT("player"), ESearchCase::IgnoreCase)) return GetPlayerInventory(World);
        AActor* Actor = FindBuildableById(World, Id);
        if (!Actor) if (AFGWheeledVehicleIdentifier* Identifier = FindVehicleIdentifier(World, Id)) Actor = Identifier->GetOwnerVehicle();
        return FindInventory(Actor, Name);
    };
    UFGInventoryComponent* From = Resolve(FromId, FromInventory); UFGInventoryComponent* To = Resolve(ToId, ToInventory);
    if (!From || !To) { OnComplete(ErrorResponse(TEXT("inventory_not_found"), TEXT("No inventory by that actor ID and component name (see GET /v1/vehicles or /v1/inventories for names)."), EHttpServerResponseCodes::NotFound)); return true; }
    TSubclassOf<UFGItemDescriptor> Item = nullptr;
    for (int32 Slot = 0; Slot < From->GetSizeLinear() && !Item; ++Slot)
    {
        FInventoryStack Stack;
        if (!From->GetStackFromIndex(Slot, Stack) || !Stack.HasItems()) continue;
        const TSubclassOf<UFGItemDescriptor> Candidate = Stack.Item.GetItemClass();
        if (Candidate && (Candidate->GetName().Equals(ItemQuery, ESearchCase::IgnoreCase) || Candidate->GetPathName().Equals(ItemQuery, ESearchCase::IgnoreCase))) Item = Candidate;
    }
    if (!Item || !From->HasItems(Item, Count)) { OnComplete(ErrorResponse(TEXT("insufficient_items"), FString::Printf(TEXT("%s holds fewer than %d of %s."), *FromId, Count, *ItemQuery), EHttpServerResponseCodes::Conflict)); return true; }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetStringField(TEXT("item"), Item->GetName());
    Body->SetNumberField(TEXT("requested"), Count);
    if (Confirm)
    {
        // Moved as one remove and one add; whatever the target will not take goes back where it came from.
        From->Remove(Item, Count);
        const int32 Added = To->AddStack(FInventoryStack(Count, Item), true);
        if (Added < Count) From->AddStack(FInventoryStack(Count - Added, Item), true);
        Body->SetNumberField(TEXT("moved"), Added);
        if (Added < Count) { Body->SetBoolField(TEXT("ok"), Added > 0); Body->SetStringField(TEXT("warning"), TEXT("The target took fewer than requested; the rest was returned.")); }
    }
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleInventories(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    const FString* Id = Request.QueryParams.Find(TEXT("id"));
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!Id || Id->IsEmpty()) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("id is required."), EHttpServerResponseCodes::BadRequest)); return true; }
    AActor* Actor = FindBuildableById(World, *Id);
    if (!Actor) if (AFGWheeledVehicleIdentifier* Identifier = FindVehicleIdentifier(World, *Id)) Actor = Identifier->GetOwnerVehicle();
    if (!Actor) { OnComplete(ErrorResponse(TEXT("buildable_not_found"), TEXT("No buildable or vehicle with that exact ID was found."), EHttpServerResponseCodes::NotFound)); return true; }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("id"), *Id);
    Body->SetObjectField(TEXT("inventories"), InventoriesJson(Actor));
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * GET /v1/game-rules (#309): the session's cheat rules that change what the factory needs: `noPower`, `noBuildCost` and
 * `noFuelCost`. POST /v1/game-rules/fuel `{ noFuelCost, confirm }` sets only the fuel rule, so vehicles can be tested burning
 * fuel; the other rules stay the player's. Answers the rules as they stand after the change.
 */
bool FMKIBridgeModule::HandleGameRules(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGGameState* State = World ? World->GetGameState<AFGGameState>() : nullptr;
    if (!State) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    bool bConfirm = false, bChanged = false;
    if (Request.Verb == EHttpServerRequestVerbs::VERB_POST)
    {
        TSharedPtr<FJsonObject> Json;
        if (!ReadBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
        bool bNoFuelCost = false;
        if (!Json->TryGetBoolField(TEXT("noFuelCost"), bNoFuelCost)) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("noFuelCost (true or false) is required; only the fuel rule can be set."), EHttpServerResponseCodes::BadRequest)); return true; }
        Json->TryGetBoolField(TEXT("confirm"), bConfirm);
        if (bConfirm && State->GetCheatNoFuel() != bNoFuelCost)
        {
            AFGGameRulesSubsystem* Rules = AFGGameRulesSubsystem::Get(World);
            if (!Rules) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("The game rules subsystem is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
            Rules->SetNoFuelCost(bNoFuelCost);
            bChanged = true;
        }
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("dryRun"), Request.Verb == EHttpServerRequestVerbs::VERB_POST && !bConfirm);
    Body->SetBoolField(TEXT("changed"), bChanged);
    Body->SetBoolField(TEXT("noPower"), State->GetCheatNoPower());
    Body->SetBoolField(TEXT("noBuildCost"), State->GetCheatNoCost());
    Body->SetBoolField(TEXT("noFuelCost"), State->GetCheatNoFuel());
    // The in-game clock, so a test can run for an in-game span ("an in-game hour") rather than a guessed real one.
    if (const AFGTimeOfDaySubsystem* Time = AFGTimeOfDaySubsystem::Get(World))
    {
        const TSharedRef<FJsonObject> Clock = MakeShared<FJsonObject>();
        Clock->SetNumberField(TEXT("hours"), Time->GetHours()); Clock->SetNumberField(TEXT("minutes"), Time->GetDayMinutes());
        Clock->SetNumberField(TEXT("dayLengthMinutes"), Time->GetDayLength()); Clock->SetNumberField(TEXT("nightLengthMinutes"), Time->GetNightLength());
        Body->SetObjectField(TEXT("timeOfDay"), Clock);
    }
    OnComplete(JsonResponse(Body));
    return true;
}

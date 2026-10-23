#include "BridgeInternal.h"

// World identity (what an actor or a hit is: class, component, mesh, instance, level or world partition cell, bounds),
// a search for the actors and instances that use a mesh, and the MKI Tools terrain routes (removed rocks), which call
// MKI Tools by reflection so the bridge never requires it.

/**
 * GET /v1/world/actor?name=: what a loaded actor is, by its exact name (as the column probe reports actorId): class,
 * path, level or world partition cell, bounds, and every primitive component with its mesh, mobility and instances.
 * Read-only; the actor must lie within the observation radius of the player.
 */
bool FMKIBridgeModule::HandleWorldActor(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    const FString* Name = Request.QueryParams.Find(TEXT("name"));
    if (!Name || Name->IsEmpty()) { OnComplete(ErrorResponse(TEXT("name_required"), TEXT("name is the exact actor name."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player is available for bounded observation."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    AActor* Actor = BridgeFindLoadedActor(World, *Name);
    if (!Actor) { OnComplete(ErrorResponse(TEXT("actor_not_found"), TEXT("No loaded actor has that exact name."), EHttpServerResponseCodes::NotFound)); return true; }
    const FBox Bounds = Actor->GetComponentsBoundingBox(true);
    const double Distance = Bounds.IsValid ? FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(Player->GetActorLocation())) : FVector::Dist(Actor->GetActorLocation(), Player->GetActorLocation());
    if (Distance > GetMaximumObservationDistanceCm()) { OnComplete(ErrorResponse(TEXT("outside_observation_radius"), TEXT("The actor is outside the observation radius."), EHttpServerResponseCodes::Forbidden)); return true; }
    const TSharedRef<FJsonObject> Body = BridgeActorIdentityJson(Actor, nullptr, INDEX_NONE);
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetNumberField(TEXT("distanceCm"), Distance);
    TArray<TSharedPtr<FJsonValue>> Components;
    TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
    for (const UPrimitiveComponent* Component : Primitives)
    {
        if (Components.Num() >= 64) { Body->SetBoolField(TEXT("componentsTruncated"), true); break; }
        if (Component) Components.Add(MakeShared<FJsonValueObject>(BridgeComponentJson(Component, INDEX_NONE)));
    }
    Body->SetArrayField(TEXT("components"), Components);
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * POST /v1/world/static-meshes { mesh, x, y, z?, radiusCm?, limit? }: the loaded actors and instances that draw a mesh near
 * a point. `mesh` is a full object path or a case-insensitive part of the mesh name. Plain components report their
 * actor; instanced ones (foliage, HLOD, buildable instances) report how many instances lie in range and the first few.
 */
bool FMKIBridgeModule::HandleStaticMeshSearch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString MeshQuery; double X = 0.0, Y = 0.0, Z = 0.0, RadiusCm = 10000.0, LimitValue = 50.0;
    Json->TryGetStringField(TEXT("mesh"), MeshQuery);
    if (MeshQuery.Len() < 3) { OnComplete(ErrorResponse(TEXT("mesh_required"), TEXT("mesh is a mesh path or at least 3 characters of its name."), EHttpServerResponseCodes::BadRequest)); return true; }
    if (!Json->TryGetNumberField(TEXT("x"), X) || !Json->TryGetNumberField(TEXT("y"), Y) || !FMath::IsFinite(X) || !FMath::IsFinite(Y))
    { OnComplete(ErrorResponse(TEXT("invalid_center"), TEXT("x and y are required finite numbers."), EHttpServerResponseCodes::BadRequest)); return true; }
    const bool bHasZ = Json->TryGetNumberField(TEXT("z"), Z) && FMath::IsFinite(Z);
    if (Json->HasField(TEXT("radiusCm")) && (!Json->TryGetNumberField(TEXT("radiusCm"), RadiusCm) || RadiusCm < 100.0 || RadiusCm > 50000.0))
    { OnComplete(ErrorResponse(TEXT("invalid_radius"), TEXT("radiusCm must be between 100 and 50000."), EHttpServerResponseCodes::BadRequest)); return true; }
    Json->TryGetNumberField(TEXT("limit"), LimitValue);
    const int32 Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 200);
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player is available for bounded observation."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    if (FVector2D::Distance(FVector2D(X, Y), FVector2D(Player->GetActorLocation())) > GetMaximumObservationDistanceCm())
    { OnComplete(ErrorResponse(TEXT("outside_observation_radius"), TEXT("The center is outside the observation radius."), EHttpServerResponseCodes::Forbidden)); return true; }

    const bool bExactPath = MeshQuery.StartsWith(TEXT("/"));
    const FVector Center(X, Y, Z);
    auto InRange = [&](const FVector& Location) { return bHasZ ? FVector::Dist(Location, Center) <= RadiusCm : FVector2D::Distance(FVector2D(Location), FVector2D(Center)) <= RadiusCm; };
    auto Matches = [&](const UStaticMesh* Mesh) { return Mesh && (bExactPath ? Mesh->GetPathName().Equals(MeshQuery, ESearchCase::IgnoreCase) : Mesh->GetName().Contains(MeshQuery, ESearchCase::IgnoreCase)); };
    TArray<TSharedPtr<FJsonValue>> Results; int32 Matched = 0, Instances = 0;
    for (const ULevel* Level : World->GetLevels())
    {
        if (!Level) continue;
        for (AActor* Actor : Level->Actors)
        {
            if (!Actor || Actor->IsActorBeingDestroyed()) continue;
            TInlineComponentArray<UStaticMeshComponent*> Meshes(Actor);
            for (const UStaticMeshComponent* Mesh : Meshes)
            {
                if (!Mesh || !Matches(Mesh->GetStaticMesh())) continue;
                if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Mesh))
                {
                    TArray<TSharedPtr<FJsonValue>> Near; int32 Count = 0;
                    for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
                    {
                        FTransform Instance;
                        if (!Instanced->GetInstanceTransform(Index, Instance, true) || !InRange(Instance.GetLocation())) continue;
                        ++Count;
                        if (Near.Num() < 20)
                        {
                            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                            Entry->SetNumberField(TEXT("index"), Index); Entry->SetObjectField(TEXT("location"), BridgeVectorJson(Instance.GetLocation())); Entry->SetObjectField(TEXT("scale"), BridgeVectorJson(Instance.GetScale3D()));
                            Near.Add(MakeShared<FJsonValueObject>(Entry));
                        }
                    }
                    if (Count == 0) continue;
                    ++Matched; Instances += Count;
                    if (Results.Num() >= Limit) continue;
                    const TSharedRef<FJsonObject> Entry = BridgeActorIdentityJson(Actor, Mesh, INDEX_NONE);
                    Entry->SetNumberField(TEXT("instancesInRange"), Count); Entry->SetArrayField(TEXT("instances"), Near);
                    Results.Add(MakeShared<FJsonValueObject>(Entry));
                    continue;
                }
                if (!InRange(Mesh->GetComponentLocation())) continue;
                ++Matched;
                if (Results.Num() >= Limit) continue;
                const TSharedRef<FJsonObject> Entry = BridgeActorIdentityJson(Actor, Mesh, INDEX_NONE);
                Entry->SetObjectField(TEXT("location"), BridgeVectorJson(Mesh->GetComponentLocation()));
                Results.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("mesh"), MeshQuery); Body->SetNumberField(TEXT("radiusCm"), RadiusCm);
    Body->SetNumberField(TEXT("matched"), Matched); Body->SetNumberField(TEXT("instancesInRange"), Instances);
    Body->SetBoolField(TEXT("truncated"), Matched > Results.Num());
    Body->SetArrayField(TEXT("results"), Results);
    OnComplete(JsonResponse(Body));
    return true;
}

namespace
{
    /**
     * Calls one of MKI Tools' bridge functions (AMKITerrainSubsystem::Bridge*) by reflection: every parameter is set by
     * name (ActorName, bConfirm) and the returned JSON text is parsed. False with an error code when MKI Tools is not
     * installed, its subsystem is not in the world yet, or the answer is not JSON.
     */
    bool CallTerrainSubsystem(UWorld* World, const TCHAR* FunctionName, const FString& ActorName, bool bConfirm, TSharedPtr<FJsonObject>& OutAnswer, FString& OutError)
    {
        UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/MKITools.MKITerrainSubsystem"));
        if (!Class) { OutError = TEXT("mki_tools_missing"); return false; }
        AActor* Subsystem = nullptr;
        for (TActorIterator<AActor> It(World, Class); It; ++It) if (IsValid(*It)) { Subsystem = *It; break; }
        UFunction* Function = Subsystem ? Subsystem->FindFunction(FunctionName) : nullptr;
        if (!Function) { OutError = Subsystem ? TEXT("mki_tools_incompatible") : TEXT("terrain_subsystem_unavailable"); return false; }
        uint8* Parameters = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(Function->ParmsSize, 1)));
        FMemory::Memzero(Parameters, Function->ParmsSize);
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It) It->InitializeValue_InContainer(Parameters);
        if (FStrProperty* Name = CastField<FStrProperty>(Function->FindPropertyByName(TEXT("ActorName")))) Name->SetPropertyValue_InContainer(Parameters, ActorName);
        if (FBoolProperty* Confirm = CastField<FBoolProperty>(Function->FindPropertyByName(TEXT("bConfirm")))) Confirm->SetPropertyValue_InContainer(Parameters, bConfirm);
        Subsystem->ProcessEvent(Function, Parameters);
        FString Text;
        if (FStrProperty* Return = CastField<FStrProperty>(Function->GetReturnProperty())) Text = Return->GetPropertyValue_InContainer(Parameters);
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It) It->DestroyValue_InContainer(Parameters);
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, OutAnswer) || !OutAnswer.IsValid()) { OutError = TEXT("mki_tools_invalid_answer"); return false; }
        return true;
    }

    EHttpServerResponseCodes TerrainErrorCode(const FString& Error)
    {
        if (Error == TEXT("rock_not_loaded") || Error == TEXT("not_removed")) return EHttpServerResponseCodes::NotFound;
        if (Error == TEXT("server_authority_required")) return EHttpServerResponseCodes::Forbidden;
        if (Error == TEXT("mki_tools_missing") || Error == TEXT("mki_tools_incompatible")) return EHttpServerResponseCodes::NotSupported;
        if (Error == TEXT("terrain_subsystem_unavailable") || Error == TEXT("mki_tools_invalid_answer")) return EHttpServerResponseCodes::ServiceUnavail;
        return EHttpServerResponseCodes::Conflict;
    }
}

/**
 * The MKI Tools terrain routes (Terrain dismantle mode). GET /v1/terrain/removed-rocks lists the removed rocks;
 * POST /v1/terrain/remove-rock and /v1/terrain/restore-rock { actorName, confirm } remove or restore one by exact actor
 * name (dry run without confirm). The rock must be loaded and within the mutation safety radius to be removed; MKI Tools
 * applies its own rules (only plain level-placed rocks). No refund either way.
 */
bool FMKIBridgeModule::HandleTerrainRocks(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete, const TCHAR* Action)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* Player = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    const bool bList = FCString::Strcmp(Action, TEXT("list")) == 0, bRemove = FCString::Strcmp(Action, TEXT("remove")) == 0;
    FString ActorName; bool bConfirm = false;
    if (!bList)
    {
        TSharedPtr<FJsonObject> Json;
        if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
        Json->TryGetStringField(TEXT("actorName"), ActorName); Json->TryGetBoolField(TEXT("confirm"), bConfirm);
        if (ActorName.IsEmpty()) { OnComplete(ErrorResponse(TEXT("actor_name_required"), TEXT("actorName is the rock's exact actor name."), EHttpServerResponseCodes::BadRequest)); return true; }
        if (bConfirm && World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Only the host can change terrain."), EHttpServerResponseCodes::Forbidden)); return true; }
        if (bRemove)
        {
            const AActor* Rock = BridgeFindLoadedActor(World, ActorName);
            const FBox Bounds = Rock ? Rock->GetComponentsBoundingBox(true) : FBox(ForceInit);
            if (Rock && Bounds.IsValid && FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(Player->GetActorLocation())) > GetMaximumPlacementDistanceCm())
            { OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("The rock is outside the configured mutation safety radius."), EHttpServerResponseCodes::Forbidden)); return true; }
        }
    }
    TSharedPtr<FJsonObject> Answer; FString Error;
    const TCHAR* Function = bList ? TEXT("BridgeListRemovedRocks") : bRemove ? TEXT("BridgeRemoveRock") : TEXT("BridgeRestoreRock");
    if (!CallTerrainSubsystem(World, Function, ActorName, bConfirm, Answer, Error))
    {
        const FString Message = Error == TEXT("mki_tools_missing") ? TEXT("MKI Tools is not installed.") : Error == TEXT("terrain_subsystem_unavailable") ? TEXT("MKI Tools' terrain subsystem is not in this world yet.") : TEXT("MKI Tools did not answer as expected.");
        OnComplete(ErrorResponse(Error, Message, TerrainErrorCode(Error)));
        return true;
    }
    bool bOk = false;
    if (!Answer->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
    {
        FString Code = TEXT("terrain_refused"), Message; Answer->TryGetStringField(TEXT("error"), Code); Answer->TryGetStringField(TEXT("message"), Message);
        OnComplete(ErrorResponse(Code, Message, TerrainErrorCode(Code)));
        return true;
    }
    OnComplete(JsonResponse(Answer.ToSharedRef()));
    return true;
}

bool FMKIBridgeModule::HandleTerrainRemovedRocks(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) { return HandleTerrainRocks(Request, OnComplete, TEXT("list")); }
bool FMKIBridgeModule::HandleTerrainRemoveRock(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) { return HandleTerrainRocks(Request, OnComplete, TEXT("remove")); }
bool FMKIBridgeModule::HandleTerrainRestoreRock(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) { return HandleTerrainRocks(Request, OnComplete, TEXT("restore")); }

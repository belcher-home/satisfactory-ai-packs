#include "BridgeInternal.h"

// Player and game interaction: transform, respawn, saves, and machine recipes.
// Split from the single MKIBridge.cpp without behavior changes (#44).

bool FMKIBridgeModule::HandleSetPlayerTransform(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString Text;
    if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); Text = FString(Converted.Length(), Converted.Get()); }
    TSharedPtr<FJsonObject> Json; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be JSON."), EHttpServerResponseCodes::BadRequest)); return true; }
    double X = 0, Y = 0, Z = 0, Yaw = 0, Pitch = 0; bool Confirm = false, RequestedFlying = false;
    const bool HasFlying = Json->TryGetBoolField(TEXT("flying"), RequestedFlying);
    const bool HasX = Json->TryGetNumberField(TEXT("x"), X); const bool HasY = Json->TryGetNumberField(TEXT("y"), Y); const bool HasZ = Json->TryGetNumberField(TEXT("z"), Z);
    Json->TryGetNumberField(TEXT("yaw"), Yaw); Json->TryGetNumberField(TEXT("pitch"), Pitch); Json->TryGetBoolField(TEXT("confirm"), Confirm);
    if (!HasX || !HasY || !HasZ || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z) || !FMath::IsFinite(Yaw) || !FMath::IsFinite(Pitch))
    { OnComplete(ErrorResponse(TEXT("invalid_transform"), TEXT("Finite x, y, z, yaw, and pitch values are required."), EHttpServerResponseCodes::BadRequest)); return true; }
    if (FMath::Abs(X) > 1000000.0 || FMath::Abs(Y) > 1000000.0 || FMath::Abs(Z) > 1000000.0)
    { OnComplete(ErrorResponse(TEXT("outside_world_bounds"), TEXT("The requested transform is outside guarded world bounds."), EHttpServerResponseCodes::Forbidden)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr; AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
    if (!World || !Player) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player is available."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    const FVector Destination(X, Y, Z); const FRotator Rotation(Pitch, Yaw, 0);
    const bool FlyingBefore = Player->CheatToggleFlyModeIsActive();
    auto Body = MakeShared<FJsonObject>(); Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("flyingBefore"), FlyingBefore);
    if (HasFlying) Body->SetBoolField(TEXT("requestedFlying"), RequestedFlying);
    auto Position = MakeShared<FJsonObject>(); Position->SetNumberField(TEXT("x"), X); Position->SetNumberField(TEXT("y"), Y); Position->SetNumberField(TEXT("z"), Z); Body->SetObjectField(TEXT("resolvedPosition"), Position); Body->SetNumberField(TEXT("resolvedYaw"), Yaw); Body->SetNumberField(TEXT("resolvedPitch"), Pitch);
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Player movement is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
    // Stop the old movement before moving the capsule.  Leaving velocity or a stale falling mode active lets the
    // character-movement correction path pull the local pawn through several intermediate positions after TeleportTo,
    // which looks like repeated teleports and can strand the player in a half-flying state.
    if (Movement)
    {
        Movement->StopMovementImmediately();
        Movement->DisableMovement();
    }
    const bool Teleported = Player->TeleportTo(Destination, Rotation, false, true);
    if (AController* Controller = Player->GetController()) Controller->SetControlRotation(Rotation);
    if (Movement)
    {
        Movement->Velocity = FVector::ZeroVector;
        if (HasFlying)
        {
            Player->SetIsFlyingToggleable(true);
            Movement->bCheatFlying = RequestedFlying;
            Movement->SetMovementMode(RequestedFlying ? MOVE_Flying : MOVE_Walking);
        }
        else
        {
            // Preserve the caller's current flight state, but never leave the component disabled after a move.
            Movement->SetMovementMode(FlyingBefore ? MOVE_Flying : MOVE_Walking);
        }
        Movement->StopMovementImmediately();
    }
    Player->ForceNetUpdate();
    const bool Moved = Teleported && FVector::DistSquared(Player->GetActorLocation(), Destination) < 100.0;
    const bool FlyingAfter = Player->CheatToggleFlyModeIsActive();
    const bool FlightVerified = !HasFlying || FlyingAfter == RequestedFlying;
    Body->SetBoolField(TEXT("flying"), FlyingAfter); Body->SetBoolField(TEXT("flightVerified"), FlightVerified);
    Body->SetBoolField(TEXT("moved"), Moved); Body->SetBoolField(TEXT("verified"), Moved && FlightVerified);
    OnComplete(JsonResponse(Body, Moved ? EHttpServerResponseCodes::Ok : EHttpServerResponseCodes::Conflict)); return true;
}

bool FMKIBridgeModule::HandleListSaves(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr; UFGSaveSystem* SaveSystem = World ? UFGSaveSystem::Get(World) : nullptr;
    if (!SaveSystem || !SaveSystem->IsSaveManagerAvailable()) { OnComplete(ErrorResponse(TEXT("save_manager_unavailable"), TEXT("The local save manager is unavailable."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TArray<FSaveHeader> Saves = SaveSystem->NativeEnumerateSaveGamesSync();
    Saves.Sort([](const FSaveHeader& A, const FSaveHeader& B) { return A.SaveDateTime > B.SaveDateTime; });
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FSaveHeader& Save : Saves)
    {
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("saveName"), Save.SaveName); Item->SetStringField(TEXT("sessionName"), Save.SessionName);
        Item->SetStringField(TEXT("mapName"), Save.MapName); Item->SetStringField(TEXT("savedAt"), Save.SaveDateTime.ToIso8601()); Item->SetNumberField(TEXT("playDurationSeconds"), Save.PlayDurationSeconds);
        Item->SetNumberField(TEXT("buildVersion"), Save.BuildVersion); Item->SetBoolField(TEXT("modded"), Save.IsModdedSave); Item->SetBoolField(TEXT("creative"), Save.IsCreativeModeEnabled);
        Items.Add(MakeShared<FJsonValueObject>(Item));
    }
    auto Body = MakeShared<FJsonObject>(); Body->SetBoolField(TEXT("ok"), true); Body->SetNumberField(TEXT("total"), Items.Num()); Body->SetArrayField(TEXT("saves"), Items); OnComplete(JsonResponse(Body)); return true;
}

bool FMKIBridgeModule::HandleLoadSave(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString Text; if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); Text = FString(Converted.Length(), Converted.Get()); }
    TSharedPtr<FJsonObject> Json; const auto Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be JSON."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString SaveName, SessionName; bool Confirm = false, Advanced = false; Json->TryGetStringField(TEXT("saveName"), SaveName); Json->TryGetStringField(TEXT("sessionName"), SessionName); Json->TryGetBoolField(TEXT("confirm"), Confirm); const bool HasAdvanced = Json->TryGetBoolField(TEXT("advancedGameSettings"), Advanced);
    if ((SaveName.IsEmpty() && SessionName.IsEmpty()) || (!SaveName.IsEmpty() && !SessionName.IsEmpty())) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("Provide exactly one exact saveName or sessionName."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr; UFGSaveSystem* SaveSystem = World ? UFGSaveSystem::Get(World) : nullptr; APlayerController* Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    if (!SaveSystem || !Controller || !SaveSystem->IsSaveManagerAvailable()) { OnComplete(ErrorResponse(TEXT("save_manager_unavailable"), TEXT("The local save manager or menu player is unavailable."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    TArray<FSaveHeader> Matches;
    for (const FSaveHeader& Save : SaveSystem->NativeEnumerateSaveGamesSync()) if ((!SaveName.IsEmpty() && Save.SaveName.Equals(SaveName, ESearchCase::IgnoreCase)) || (!SessionName.IsEmpty() && Save.SessionName.Equals(SessionName, ESearchCase::IgnoreCase))) Matches.Add(Save);
    if (Matches.Num() == 0) { OnComplete(ErrorResponse(TEXT("save_not_found"), TEXT("No exact save or session matched the request."), EHttpServerResponseCodes::NotFound)); return true; }
    Matches.Sort([](const FSaveHeader& A, const FSaveHeader& B) { return A.SaveDateTime > B.SaveDateTime; }); const FSaveHeader Selected = Matches[0];
    auto Body = MakeShared<FJsonObject>(); Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetStringField(TEXT("saveName"), Selected.SaveName); Body->SetStringField(TEXT("sessionName"), Selected.SessionName); Body->SetStringField(TEXT("savedAt"), Selected.SaveDateTime.ToIso8601()); Body->SetBoolField(TEXT("creative"), Selected.IsCreativeModeEnabled);
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }
    FLoadSaveFileParameters Parameters; Parameters.bEnableAdvancedGameSettings = HasAdvanced ? Advanced : Selected.IsCreativeModeEnabled;
    USessionMigrationSequence* Migration = SaveSystem->LoadSaveFile(Selected, Parameters, Controller);
    Body->SetBoolField(TEXT("accepted"), Migration != nullptr); Body->SetBoolField(TEXT("advancedGameSettings"), Parameters.bEnableAdvancedGameSettings);
    OnComplete(JsonResponse(Body, Migration ? EHttpServerResponseCodes::Accepted : EHttpServerResponseCodes::Conflict)); return true;
}

// Writes the running world to a named save through the game's own save session. A preview (confirm:false) reports the
// resolved name and whether it exists. The default name is new and timestamped, so autosaves and the player's saves are
// never replaced; replacing a save in the same session needs its exact name and overwrite:true, and a save belonging
// to another session is never replaced. The response is sent when the game reports the file written and its header
// reads back, or after a timeout.
bool FMKIBridgeModule::HandleSaveGame(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString Text; if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); Text = FString(Converted.Length(), Converted.Get()); }
    TSharedPtr<FJsonObject> Json; const auto Reader = TJsonReaderFactory<>::Create(Text.IsEmpty() ? FString(TEXT("{}")) : Text);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be JSON."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString RequestedName, RequestId; bool Confirm = false, Overwrite = false;
    Json->TryGetStringField(TEXT("saveName"), RequestedName); Json->TryGetStringField(TEXT("requestId"), RequestId); Json->TryGetBoolField(TEXT("confirm"), Confirm); Json->TryGetBoolField(TEXT("overwrite"), Overwrite);
    if (Confirm && RequestId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("A confirmed save requires a requestId."), EHttpServerResponseCodes::BadRequest)); return true; }
    if (Confirm)
    {
        if (const TSharedPtr<FJsonObject>* Receipt = SaveReceipts.Find(RequestId))
        {
            const TSharedRef<FJsonObject> Replay = MakeShared<FJsonObject>(); Replay->Values = (*Receipt)->Values; Replay->SetBoolField(TEXT("replayed"), true);
            OnComplete(JsonResponse(Replay)); return true;
        }
        if (!SaveInFlightRequestId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("save_in_progress"), FString::Printf(TEXT("Save request '%s' is still being written."), *SaveInFlightRequestId), EHttpServerResponseCodes::Conflict)); return true; }
    }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    UFGSaveSystem* SaveSystem = World ? UFGSaveSystem::Get(World) : nullptr; UFGSaveSession* SaveSession = World ? UFGSaveSession::Get(World) : nullptr;
    AFGGameState* GameState = World ? World->GetGameState<AFGGameState>() : nullptr;
    const FString SessionName = GameState ? GameState->GetSessionName() : FString();
    if (!SaveSystem || !SaveSession || !SaveSystem->IsSaveManagerAvailable() || SessionName.IsEmpty() || !World->GetAuthGameMode())
    { OnComplete(ErrorResponse(TEXT("world_not_ready"), TEXT("Saving requires a loaded single-player world with an active save session."), EHttpServerResponseCodes::ServiceUnavail)); return true; }

    const TArray<FSaveHeader> ExistingSaves = SaveSystem->NativeEnumerateSaveGamesSync();
    FString ExistingSession, SaveName = RequestedName;
    if (SaveName.IsEmpty())
    {
        // The timestamp has one-second resolution; a default name that is already taken gets the first free _2.._99
        // suffix, so back-to-back default saves never collide with each other or replace anything.
        const FString Stem = FString::Printf(TEXT("%s_ai_%s"), *UFGSaveSystem::SanitizeSaveName(SessionName), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
        SaveName = Stem;
        for (int32 Suffix = 2; Suffix < 100 && UFGSaveSystem::GetCachedSaveExists(ExistingSaves, SaveName, SessionName, ExistingSession) != ESaveExists::SE_DoesntExist; ++Suffix)
            SaveName = FString::Printf(TEXT("%s_%d"), *Stem, Suffix);
    }
    if (UFGSaveSystem::SanitizeSaveName(SaveName) != SaveName || !UFGSaveSystem::IsValidSaveName(SaveName))
    { OnComplete(ErrorResponse(TEXT("invalid_save_name"), FString::Printf(TEXT("'%s' is not a valid save file name."), *SaveName), EHttpServerResponseCodes::BadRequest)); return true; }
    const ESaveExists Exists = UFGSaveSystem::GetCachedSaveExists(ExistingSaves, SaveName, SessionName, ExistingSession);

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetStringField(TEXT("saveName"), SaveName); Body->SetStringField(TEXT("sessionName"), SessionName);
    Body->SetBoolField(TEXT("exists"), Exists != ESaveExists::SE_DoesntExist); Body->SetBoolField(TEXT("overwrite"), Exists == ESaveExists::SE_ExistsInSameSession);
    if (!RequestId.IsEmpty()) Body->SetStringField(TEXT("requestId"), RequestId);
    if (Exists == ESaveExists::SE_ExistsInOtherSession)
    { OnComplete(ErrorResponse(TEXT("save_exists_in_other_session"), FString::Printf(TEXT("'%s' belongs to session '%s' and is never replaced."), *SaveName, *ExistingSession), EHttpServerResponseCodes::Conflict)); return true; }
    if (Exists == ESaveExists::SE_ExistsInSameSession && !Overwrite)
    { OnComplete(ErrorResponse(TEXT("save_exists"), FString::Printf(TEXT("'%s' already exists; pass overwrite:true to replace it."), *SaveName), EHttpServerResponseCodes::Conflict)); return true; }
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }

    // The game saves at the end of the frame and writes the file in the background; respond exactly once, from
    // whichever comes first: the completion delegate or the timeout.
    struct FPendingSave { FHttpResultCallback Respond; bool bDone = false; FTSTicker::FDelegateHandle Timeout; double StartSeconds = 0; };
    const TSharedRef<FPendingSave> Pending = MakeShared<FPendingSave>(); Pending->Respond = OnComplete; Pending->StartSeconds = FPlatformTime::Seconds();
    SaveInFlightRequestId = RequestId;
    TWeakObjectPtr<UFGSaveSystem> WeakSaveSystem = SaveSystem;
    Pending->Timeout = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, Pending, RequestId](float) {
        if (Pending->bDone) return false;
        Pending->bDone = true; if (SaveInFlightRequestId == RequestId) SaveInFlightRequestId.Reset();
        Pending->Respond(ErrorResponse(TEXT("save_timeout"), TEXT("The game did not report the save complete within 120 seconds; list saves before retrying."), EHttpServerResponseCodes::ServerError));
        return false;
    }), 120.0f);
    SaveSession->SaveGame(SaveName, FOnSaveGameComplete::CreateLambda([this, Pending, Body, RequestId, SaveName, WeakSaveSystem](bool bSuccess, const FText& Error, void*) {
        if (Pending->bDone) return;
        Pending->bDone = true; FTSTicker::GetCoreTicker().RemoveTicker(Pending->Timeout);
        if (SaveInFlightRequestId == RequestId) SaveInFlightRequestId.Reset();
        if (!bSuccess) { Pending->Respond(ErrorResponse(TEXT("save_failed"), Error.IsEmpty() ? TEXT("The game reported the save as failed.") : Error.ToString(), EHttpServerResponseCodes::ServerError)); return; }
        FSaveHeader Header;
        if (!WeakSaveSystem.IsValid() || !WeakSaveSystem->LoadSaveGameHeaderSync(SaveName, Header))
        { Pending->Respond(ErrorResponse(TEXT("save_unverified"), TEXT("The game reported success but the save header could not be read back."), EHttpServerResponseCodes::ServerError)); return; }
        Body->SetBoolField(TEXT("saved"), true); Body->SetStringField(TEXT("savedAt"), Header.SaveDateTime.ToIso8601()); Body->SetStringField(TEXT("sessionName"), Header.SessionName);
        Body->SetNumberField(TEXT("playDurationSeconds"), Header.PlayDurationSeconds); Body->SetNumberField(TEXT("elapsedMs"), FMath::RoundToInt((FPlatformTime::Seconds() - Pending->StartSeconds) * 1000.0));
        SaveReceipts.Add(RequestId, Body);
        Pending->Respond(JsonResponse(Body));
    }), nullptr);
    return true;
}

/** POST /v1/player/respawn { confirm }: starts the game's own respawn when the local player needs one (dead). */
bool FMKIBridgeModule::HandlePlayerRespawn(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString Text;
    if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); Text = FString(Converted.Length(), Converted.Get()); }
    TSharedPtr<FJsonObject> Json; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be JSON."), EHttpServerResponseCodes::BadRequest)); return true; }
    bool Confirm = false; Json->TryGetBoolField(TEXT("confirm"), Confirm);
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGPlayerController* Controller = World ? Cast<AFGPlayerController>(UGameplayStatics::GetPlayerController(World, 0)) : nullptr;
    if (!World || !Controller) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player controller is available."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    const bool bNeed = Controller->NeedRespawn(), bRespawning = Controller->IsRespawning();
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("needRespawn"), bNeed); Body->SetBoolField(TEXT("respawningBefore"), bRespawning);
    if (!bNeed || bRespawning || !Confirm) { Body->SetBoolField(TEXT("started"), false); OnComplete(JsonResponse(Body)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Respawn is only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    Controller->StartRespawn();
    Body->SetBoolField(TEXT("started"), true); Body->SetBoolField(TEXT("respawning"), Controller->IsRespawning());
    OnComplete(JsonResponse(Body));
    return true;
}

bool FMKIBridgeModule::HandleSetMachineRecipe(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
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
    FString MachineId;
    FString RecipeQuery;
    if (!Json->TryGetStringField(TEXT("buildableId"), MachineId)) Json->TryGetStringField(TEXT("machineId"), MachineId);
    if (!Json->TryGetStringField(TEXT("recipe"), RecipeQuery) || MachineId.IsEmpty() || RecipeQuery.IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("machineId and recipe are required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    bool Confirm = false;
    Json->TryGetBoolField(TEXT("confirm"), Confirm);

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    AFGBuildableSubsystem* BuildableSubsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
    if (!World || !BuildableSubsystem)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("Buildable subsystem is not ready."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }
    AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(FindBuildableById(World, MachineId));
    if (!Manufacturer)
    {
        OnComplete(ErrorResponse(TEXT("machine_not_found"), TEXT("No manufacturer with that exact buildable ID was found."), EHttpServerResponseCodes::NotFound));
        return true;
    }

    TArray<TSubclassOf<UFGRecipe>> AvailableRecipes;
    Manufacturer->GetAvailableRecipes(AvailableRecipes);
    TArray<TSubclassOf<UFGRecipe>> Matches;
    for (const TSubclassOf<UFGRecipe>& Recipe : AvailableRecipes)
    {
        if (!Recipe)
        {
            continue;
        }
        const FString DisplayName = UFGRecipe::GetRecipeName(Recipe).ToString();
        if (DisplayName.Equals(RecipeQuery, ESearchCase::IgnoreCase) || Recipe->GetName().Equals(RecipeQuery, ESearchCase::IgnoreCase) ||
            Recipe->GetPathName().Equals(RecipeQuery, ESearchCase::IgnoreCase))
        {
            Matches.Add(Recipe);
        }
    }
    if (Matches.Num() != 1)
    {
        const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
        Body->SetBoolField(TEXT("ok"), false);
        Body->SetStringField(TEXT("error"), TEXT("recipe_not_unique"));
        Body->SetStringField(TEXT("message"), FString::Printf(TEXT("The recipe matched %d available recipes; exactly one is required."), Matches.Num()));
        Body->SetBoolField(TEXT("retryable"), false);
        Body->SetNumberField(TEXT("matchCount"), Matches.Num());
        TArray<TSharedPtr<FJsonValue>> Names;
        for (const TSubclassOf<UFGRecipe>& Recipe : AvailableRecipes)
        {
            if (Recipe)
            {
                Names.Add(MakeShared<FJsonValueString>(UFGRecipe::GetRecipeName(Recipe).ToString()));
            }
        }
        Body->SetArrayField(TEXT("availableRecipes"), Names);
        OnComplete(JsonResponse(Body, EHttpServerResponseCodes::BadRequest));
        return true;
    }

    UFGInventoryComponent* Input = Manufacturer->GetInputInventory();
    UFGInventoryComponent* Output = Manufacturer->GetOutputInventory();
    if ((Input && !Input->IsEmpty()) || (Output && !Output->IsEmpty()))
    {
        OnComplete(ErrorResponse(TEXT("inventory_not_empty"), TEXT("Input and output inventories must be empty before changing recipe."), EHttpServerResponseCodes::Conflict));
        return true;
    }
    const TSubclassOf<UFGRecipe> RequestedRecipe = Matches[0];
    const TSubclassOf<UFGRecipe> CurrentRecipe = Manufacturer->GetCurrentRecipe();
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("machineId"), MachineId);
    Body->SetStringField(TEXT("requestedRecipe"), UFGRecipe::GetRecipeName(RequestedRecipe).ToString());
    Body->SetStringField(TEXT("requestedRecipeClass"), RequestedRecipe->GetPathName());
    Body->SetStringField(TEXT("currentRecipe"), CurrentRecipe ? UFGRecipe::GetRecipeName(CurrentRecipe).ToString() : TEXT(""));
    Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetBoolField(TEXT("confirmationRequired"), !Confirm);
    if (!Confirm)
    {
        Body->SetBoolField(TEXT("applied"), false);
        OnComplete(JsonResponse(Body));
        return true;
    }
    if (World->GetNetMode() == NM_Client)
    {
        OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Recipe changes are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden));
        return true;
    }
    Manufacturer->SetRecipe(RequestedRecipe);
    Body->SetBoolField(TEXT("applied"), true);
    Body->SetStringField(TEXT("verifiedRecipe"), UFGRecipe::GetRecipeName(Manufacturer->GetCurrentRecipe()).ToString());
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * POST /v1/machine-clock (#19): set a factory machine's clock speed. `clockPercent` is the requested pending potential in
 * percent; it must lie within the machine's current range (1-100 %, higher only with power shards in its slots). A dry
 * run reports the range and the current clock; a confirmed request sets the pending potential, which the machine adopts.
 */
bool FMKIBridgeModule::HandleMachineClock(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString MachineId; double ClockPercent = 0.0; bool Confirm = false;
    if (!Json->TryGetStringField(TEXT("buildableId"), MachineId)) Json->TryGetStringField(TEXT("machineId"), MachineId);
    Json->TryGetBoolField(TEXT("confirm"), Confirm);
    if (MachineId.IsEmpty() || !Json->TryGetNumberField(TEXT("clockPercent"), ClockPercent) || !FMath::IsFinite(ClockPercent))
    { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("buildableId and a finite clockPercent are required."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No world is loaded."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    AFGBuildableFactory* Machine = Cast<AFGBuildableFactory>(FindBuildableById(World, MachineId));
    if (!Machine) { OnComplete(ErrorResponse(TEXT("machine_not_found"), TEXT("No factory machine with that exact buildable ID was found."), EHttpServerResponseCodes::NotFound)); return true; }
    if (!Machine->GetCanChangePotential()) { OnComplete(ErrorResponse(TEXT("clock_fixed"), TEXT("This machine's clock speed cannot be changed."), EHttpServerResponseCodes::Conflict)); return true; }
    const double Minimum = Machine->GetCurrentMinPotential() * 100.0, Maximum = Machine->GetCurrentMaxPotential() * 100.0;
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("buildableId"), MachineId);
    Body->SetNumberField(TEXT("minimumPercent"), Minimum); Body->SetNumberField(TEXT("maximumPercent"), Maximum);
    Body->SetNumberField(TEXT("currentPercent"), Machine->GetCurrentPotential() * 100.0);
    if (ClockPercent < Minimum - 0.001 || ClockPercent > Maximum + 0.001)
    {
        Body->SetBoolField(TEXT("ok"), false); Body->SetStringField(TEXT("error"), TEXT("clock_out_of_range"));
        Body->SetStringField(TEXT("message"), FString::Printf(TEXT("clockPercent must lie between %.2f and %.2f for this machine."), Minimum, Maximum)); Body->SetBoolField(TEXT("retryable"), false);
        OnComplete(JsonResponse(Body, EHttpServerResponseCodes::BadRequest)); return true;
    }
    Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), !Confirm); Body->SetNumberField(TEXT("requestedPercent"), ClockPercent);
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Clock changes are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    Machine->SetPendingPotential(static_cast<float>(ClockPercent / 100.0));
    Body->SetBoolField(TEXT("applied"), true); Body->SetNumberField(TEXT("pendingPercent"), Machine->GetPendingPotential() * 100.0);
    OnComplete(JsonResponse(Body));
    return true;
}

/**
 * POST /v1/splitter-rules (#19, feature splitter_rules): a Smart or Programmable Splitter's sort rules, each an item
 * ("overflow", "any", "none", "undefined" or an item descriptor class path) and an output index (0-2). A dry run reports
 * the current rules; confirmed, the given rules replace them. An overflow-to-sink split is [any -> storage, overflow -> sink].
 */
bool FMKIBridgeModule::HandleSplitterRules(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString SplitterId; bool Confirm = false; Json->TryGetStringField(TEXT("buildableId"), SplitterId); Json->TryGetBoolField(TEXT("confirm"), Confirm);
    if (SplitterId.IsEmpty()) { OnComplete(ErrorResponse(TEXT("invalid_request"), TEXT("buildableId is required."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No world is loaded."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    AFGBuildableSplitterSmart* Splitter = Cast<AFGBuildableSplitterSmart>(FindBuildableById(World, SplitterId));
    if (!Splitter) { OnComplete(ErrorResponse(TEXT("splitter_not_found"), TEXT("No Smart or Programmable Splitter with that exact buildable ID was found."), EHttpServerResponseCodes::NotFound)); return true; }
    auto ItemName = [](TSubclassOf<UFGItemDescriptor> Item) -> FString
    {
        if (!Item) return TEXT("none");
        if (Item->IsChildOf(UFGOverflowDescriptor::StaticClass())) return TEXT("overflow");
        if (Item->IsChildOf(UFGWildCardDescriptor::StaticClass())) return TEXT("any");
        if (Item->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass())) return TEXT("undefined");
        if (Item->IsChildOf(UFGNoneDescriptor::StaticClass())) return TEXT("none");
        return Item->GetPathName();
    };
    auto RulesJson = [&ItemName](const TArray<FSplitterSortRule>& Rules) { TArray<TSharedPtr<FJsonValue>> Out; for (const FSplitterSortRule& Rule : Rules) { const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("item"), ItemName(Rule.ItemClass)); Item->SetNumberField(TEXT("output"), Rule.OutputIndex); Out.Add(MakeShared<FJsonValueObject>(Item)); } return Out; };
    TArray<FSplitterSortRule> Requested;
    const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
    if (Json->TryGetArrayField(TEXT("rules"), Rules) && Rules)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Rules)
        {
            const TSharedPtr<FJsonObject>* Rule = nullptr; FString Item; double Output = -1.0;
            if (!Value->TryGetObject(Rule) || !Rule || !(*Rule)->TryGetStringField(TEXT("item"), Item) || !(*Rule)->TryGetNumberField(TEXT("output"), Output) || Output < 0.0 || Output > 2.0 || FMath::Fractional(Output) != 0.0)
            { OnComplete(ErrorResponse(TEXT("invalid_rule"), TEXT("Each rule needs an item and an output of 0, 1 or 2."), EHttpServerResponseCodes::BadRequest)); return true; }
            TSubclassOf<UFGItemDescriptor> Class = nullptr;
            if (Item == TEXT("overflow")) Class = UFGOverflowDescriptor::StaticClass();
            else if (Item == TEXT("any")) Class = UFGWildCardDescriptor::StaticClass();
            else if (Item == TEXT("undefined")) Class = UFGAnyUndefinedDescriptor::StaticClass();
            else if (Item == TEXT("none")) Class = UFGNoneDescriptor::StaticClass();
            else if (Item.StartsWith(TEXT("/Game/FactoryGame/"))) Class = StaticLoadClass(UFGItemDescriptor::StaticClass(), nullptr, *Item);
            if (!Class) { OnComplete(ErrorResponse(TEXT("unknown_item"), FString::Printf(TEXT("%s is not overflow, any, none, undefined or an item descriptor."), *Item), EHttpServerResponseCodes::BadRequest)); return true; }
            Requested.Add(FSplitterSortRule(Class, static_cast<int32>(Output)));
        }
    }
    if (Requested.Num() > Splitter->GetMaxNumSortRules()) { OnComplete(ErrorResponse(TEXT("too_many_rules"), FString::Printf(TEXT("This splitter takes at most %d rules."), Splitter->GetMaxNumSortRules()), EHttpServerResponseCodes::BadRequest)); return true; }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetStringField(TEXT("buildableId"), SplitterId); Body->SetBoolField(TEXT("dryRun"), !Confirm);
    Body->SetNumberField(TEXT("maximumRules"), Splitter->GetMaxNumSortRules());
    Body->SetArrayField(TEXT("currentRules"), RulesJson(Splitter->GetSortRules()));
    if (!Confirm) { OnComplete(JsonResponse(Body)); return true; }
    if (World->GetNetMode() == NM_Client) { OnComplete(ErrorResponse(TEXT("server_authority_required"), TEXT("Splitter rules are only available to the host or in single-player."), EHttpServerResponseCodes::Forbidden)); return true; }
    Splitter->SetSortRules(Requested);
    Body->SetArrayField(TEXT("rules"), RulesJson(Splitter->GetSortRules())); Body->SetBoolField(TEXT("applied"), true);
    OnComplete(JsonResponse(Body));
    return true;
}

#include "BridgeInternal.h"

// Mutation receipts: completion, replay, persistence per save session (#38), placement origins, and placement status.
// Split from the single MKIBridge.cpp without behavior changes (#44).

void FMKIBridgeModule::CompleteMutationReceipt(const FString& RequestId, int32 Code, const FString& Body)
{
    FMutationReceipt* Receipt = MutationReceipts.Find(RequestId);
    if (!Receipt) return;
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    {
        const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
        Event->SetStringField(TEXT("requestId"), RequestId);
        Event->SetStringField(TEXT("path"), Receipt->Path);
        Event->SetNumberField(TEXT("code"), Code);
        Event->SetBoolField(TEXT("ok"), Code >= 200 && Code < 300);
        if (World) EmitBridgeEvent(World, TEXT("receipt_completed"), Event);
    }
    if (Code < 200 || Code >= 300)
    {
        // Every failure path rolls back and refunds, so the same requestId may be executed again.
        MutationReceipts.Remove(RequestId); PlacementOrigins.Remove(RequestId);
        return;
    }
    Receipt->bInFlight = false; Receipt->Code = Code; Receipt->Body = Body;
    // Play time orders the receipt against saves: a later load keeps it only if the save was written after it.
    // Without a game state the receipt is memory-only (-1 is never persisted).
    AFGGameState* GameState = World ? World->GetGameState<AFGGameState>() : nullptr;
    Receipt->PlayDurationSeconds = GameState ? static_cast<double>(GameState->GetTotalPlayDuration()) : -1.0;
    Receipt->CompletedAt = FDateTime::UtcNow();
    bReceiptStoreDirty = true;
    // A single conventional placement records where and what it built, plus any lightweight instance of that class
    // already at that transform, so placement-status and filtered listings can later resolve the receipt to the
    // lightweight instance the actor may be converted into (#113) without mistaking a pre-existing one for it.
    // A placement the game converts during construction is recorded at its placement site instead (NotePlacementOrigin
    // with the indices taken before construction), because its actor is already gone here (verified live).
    if (const TWeakObjectPtr<AActor>* Placed = PlacementReceipts.Find(RequestId))
    {
        AActor* Actor = Placed->Get();
        if (IsValid(Actor) && !PlacementReceiptTransforms.Contains(RequestId) && !PlacementOrigins.Contains(RequestId)) NotePlacementOrigin(RequestId, Actor, nullptr);
    }
    MutationReceiptOrder.Remove(RequestId);
    MutationReceiptOrder.Add(RequestId);
    while (MutationReceiptOrder.Num() > MaximumMutationReceipts)
    {
        const FString Oldest = MutationReceiptOrder[0];
        MutationReceiptOrder.RemoveAt(0);
        MutationReceipts.Remove(Oldest);
        PlacementReceipts.Remove(Oldest); PlacementReceiptTransforms.Remove(Oldest); PlacementReceiptClasses.Remove(Oldest); PlacementOrigins.Remove(Oldest);
    }
}

void FMKIBridgeModule::NotePlacementOrigin(const FString& RequestId, AActor* Actor, const TSet<int32>* IndicesBefore)
{
    // A converted actor is pending kill but still readable in the frame that converted it.
    if (!Actor || RequestId.IsEmpty()) return;
    FPlacementOrigin Origin; Origin.ActorName = Actor->GetName(); Origin.Transform = Actor->GetActorTransform(); Origin.Class = Actor->GetClass();
    if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(Actor->GetWorld()))
    {
        for (const auto& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            if (Pair.Key.Get() != Actor->GetClass()) continue;
            for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
                if (Pair.Value[Index].IsValid() && Pair.Value[Index].Transform.GetLocation().Equals(Origin.Transform.GetLocation(), 1.0) && (!IndicesBefore || IndicesBefore->Contains(Index)))
                    Origin.PreexistingIds.Add(FString::Printf(TEXT("lw|%s|%d"), *Actor->GetClass()->GetPathName(), Index));
        }
    }
    PlacementOrigins.Add(RequestId, MoveTemp(Origin));
}

FString FMKIBridgeModule::ResolvePlacementOrigin(UWorld* World, const FPlacementOrigin& Origin, FString& OutResolution)
{
    UClass* Class = Origin.Class.Get();
    if (!World || !Class) { OutResolution = TEXT("unavailable"); return FString(); }
    if (AFGBuildable* Actor = FindBuildableById(World, Origin.ActorName))
    {
        if (Actor->GetClass() == Class && Actor->GetActorLocation().Equals(Origin.Transform.GetLocation(), 1.0)) { OutResolution = TEXT("actor"); return Actor->GetName(); }
    }
    // The actor is gone. The conventional placement became a lightweight instance only if exactly one instance of the
    // same class now sits at the recorded transform and that instance was not already there when the placement
    // completed (PreexistingIds, captured server-side at completion).
    TArray<FString> Matches;
    if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World))
    {
        for (const auto& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            if (Pair.Key.Get() != Class) continue;
            for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
            {
                const FRuntimeBuildableInstanceData& Data = Pair.Value[Index];
                if (!Data.IsValid() || !Data.Transform.GetLocation().Equals(Origin.Transform.GetLocation(), 1.0)) continue;
                if (FMath::Abs(FRotator::NormalizeAxis(Data.Transform.Rotator().Yaw - Origin.Transform.Rotator().Yaw)) > 0.1) continue;
                const FString Id = FString::Printf(TEXT("lw|%s|%d"), *Class->GetPathName(), Index);
                if (Origin.PreexistingIds.ContainsByPredicate([&Id](const FString& Other) { return Other.Equals(Id, ESearchCase::CaseSensitive); })) continue;
                Matches.Add(Id);
            }
        }
    }
    if (Matches.Num() == 1) { OutResolution = TEXT("lightweight_conversion"); return Matches[0]; }
    OutResolution = Matches.Num() == 0 ? TEXT("gone") : TEXT("ambiguous");
    return FString();
}

bool FMKIBridgeModule::HandlePlacementStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied));
        return true;
    }
    const FString* RequestId = Request.QueryParams.Find(TEXT("requestId"));
    if (!RequestId || RequestId->IsEmpty())
    {
        OnComplete(ErrorResponse(TEXT("request_id_required"), TEXT("requestId query parameter is required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    const FMutationReceipt* Mutation = MutationReceipts.Find(*RequestId);
    if (Mutation && Mutation->bInFlight)
    {
        OnComplete(ErrorResponse(TEXT("request_in_flight"), TEXT("This requestId is still being executed."), EHttpServerResponseCodes::Conflict));
        return true;
    }
    const TWeakObjectPtr<AActor>* Receipt = PlacementReceipts.Find(*RequestId);
    const TArray<FTransform>* ReceiptTransforms = PlacementReceiptTransforms.Find(*RequestId);
    if (!Receipt && !ReceiptTransforms && Mutation && Mutation->bRestored)
    {
        // Restored after a load (#38): the recorded answer is known, but the live actor bookkeeping is not, so no
        // existence or final identity is claimed. Callers observe the world to resolve it.
        const TSharedRef<FJsonObject> Restored = MakeShared<FJsonObject>();
        Restored->SetBoolField(TEXT("ok"), true);
        Restored->SetStringField(TEXT("requestId"), *RequestId);
        Restored->SetBoolField(TEXT("restored"), true);
        Restored->SetStringField(TEXT("path"), Mutation->Path);
        Restored->SetNumberField(TEXT("code"), Mutation->Code);
        Restored->SetStringField(TEXT("completedAt"), Mutation->CompletedAt.ToIso8601());
        TSharedPtr<FJsonObject> Result; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Mutation->Body);
        if (FJsonSerializer::Deserialize(Reader, Result) && Result.IsValid()) Restored->SetObjectField(TEXT("result"), Result);
        OnComplete(JsonResponse(Restored));
        return true;
    }
    if (!Receipt && !ReceiptTransforms)
    {
        OnComplete(ErrorResponse(TEXT("receipt_not_found"), TEXT("No placement receipt exists for this requestId in the current game session."), EHttpServerResponseCodes::NotFound));
        return true;
    }

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("requestId"), *RequestId);
    const bool ReceiptExists = Receipt && Receipt->IsValid();
    Body->SetBoolField(TEXT("exists"), ReceiptExists || (ReceiptTransforms && !ReceiptTransforms->IsEmpty()));
    if (ReceiptTransforms)
    {
        TArray<TSharedPtr<FJsonValue>> BuildableIds;
        UClass* ReceiptClass = PlacementReceiptClasses.FindRef(*RequestId).Get();
        AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(GEngine ? GEngine->GetCurrentPlayWorld() : nullptr);
        if (Lightweight && ReceiptClass)
        {
            for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Pair : Lightweight->GetAllLightweightBuildableInstances())
            {
                if (Pair.Key.Get() != ReceiptClass) continue;
                for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
                {
                    const FRuntimeBuildableInstanceData& Data = Pair.Value[Index];
                    if (!Data.IsValid()) continue;
                    for (const FTransform& Expected : *ReceiptTransforms) if (Data.Transform.GetLocation().Equals(Expected.GetLocation(), 1.0))
                    {
                        BuildableIds.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("lw|%s|%d"), *ReceiptClass->GetPathName(), Index)));
                        break;
                    }
                }
            }
        }
        Body->SetArrayField(TEXT("buildableIds"), BuildableIds);
    }
    // The final identity of a single conventional placement: the actor while it exists, otherwise the one lightweight
    // instance it was proven to become (`placement_final_ids`). An ambiguous or missing instance is reported, never guessed.
    if (const FPlacementOrigin* Origin = PlacementOrigins.Find(*RequestId))
    {
        FString Resolution; const FString Final = ResolvePlacementOrigin(GEngine ? GEngine->GetCurrentPlayWorld() : nullptr, *Origin, Resolution);
        Body->SetStringField(TEXT("receiptBuildableId"), Origin->ActorName);
        Body->SetStringField(TEXT("finalResolution"), Resolution);
        if (!Final.IsEmpty()) Body->SetStringField(TEXT("finalBuildableId"), Final);
        if (!ReceiptExists && Resolution == TEXT("lightweight_conversion"))
        {
            Body->SetBoolField(TEXT("exists"), true);
            Body->SetStringField(TEXT("buildableId"), Final);
            Body->SetObjectField(TEXT("position"), BridgeVectorJson(Origin->Transform.GetLocation()));
        }
    }
    if (ReceiptExists)
    {
        Body->SetStringField(TEXT("buildableId"), Receipt->Get()->GetName());
        const FVector Location = Receipt->Get()->GetActorLocation();
        const TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Location.X);
        Position->SetNumberField(TEXT("y"), Location.Y);
        Position->SetNumberField(TEXT("z"), Location.Z);
        Body->SetObjectField(TEXT("position"), Position);
    }
    // The recorded confirmed response (the same body a retry replays), for callers that need the original receipt.
    if (Mutation)
    {
        TSharedPtr<FJsonObject> Recorded; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Mutation->Body);
        if (FJsonSerializer::Deserialize(Reader, Recorded) && Recorded.IsValid()) Body->SetObjectField(TEXT("response"), Recorded);
    }
    OnComplete(JsonResponse(Body));
    return true;
}

void FMKIBridgeModule::ForgetMutationReceipt(const FString& RequestId)
{
    MutationReceipts.Remove(RequestId); MutationReceiptOrder.Remove(RequestId); bReceiptStoreDirty = true;
    PlacementReceipts.Remove(RequestId); PlacementReceiptTransforms.Remove(RequestId); PlacementReceiptClasses.Remove(RequestId); PlacementOrigins.Remove(RequestId);
}

namespace
{
    /** Side file of a save session: a readable prefix of the name plus a checksum of the exact name. */
    FString ReceiptStoreFile(const FString& SessionName)
    {
        FString Safe;
        for (const TCHAR Character : SessionName.Left(48)) Safe.AppendChar(FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_') ? Character : TEXT('_'));
        return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MKIBridge"), TEXT("receipts"), FString::Printf(TEXT("%s-%08x.json"), *Safe, FCrc::StrCrc32(*SessionName)));
    }
}

/**
 * Receipts are scoped to the loaded world. When the play world changes (any load, including a restart), memory
 * receipts and their live bookkeeping are cleared. Once the new world's game state is ready, the session's side file
 * is read and only receipts completed at a play time strictly before the loaded play time are restored: those were
 * recorded before the loaded save was written, so their effects are in it. Later receipts belong to play that was
 * not saved (or to a branch abandoned by loading an older save) and are discarded from the file.
 */
void FMKIBridgeModule::EnsureReceiptScope(UWorld* World)
{
    const bool bSameWorld = bReceiptScopeInitialized && ReceiptScopeWorld.Get() == World;
    if (bSameWorld && bReceiptStoreResolved) return;
    if (!bSameWorld)
    {
        FlushReceiptStore();
        MutationReceipts.Reset(); MutationReceiptOrder.Reset();
        PlacementReceipts.Reset(); PlacementReceiptTransforms.Reset(); PlacementReceiptClasses.Reset(); PlacementOrigins.Reset();
        ReceiptScopeWorld = World; bReceiptScopeInitialized = true; bReceiptStoreResolved = false;
        ReceiptStorePath.Empty(); ReceiptSessionName.Empty(); RestoredReceiptCount = 0; DiscardedReceiptCount = 0;
    }
    AFGGameState* GameState = World ? World->GetGameState<AFGGameState>() : nullptr;
    if (!GameState) return; // Not a game world yet (menu, or still loading); receipts stay memory-only until it is.
    bReceiptStoreResolved = true;
    ReceiptSessionName = GameState->GetSessionName();
    if (ReceiptSessionName.IsEmpty()) return;
    ReceiptStorePath = ReceiptStoreFile(ReceiptSessionName);
    const double LoadedPlaySeconds = static_cast<double>(GameState->GetTotalPlayDuration());
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *ReceiptStorePath)) return;
    TSharedPtr<FJsonObject> Root; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    FString StoredSession;
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || !Root->TryGetStringField(TEXT("sessionName"), StoredSession)
        || StoredSession != ReceiptSessionName || !Root->TryGetArrayField(TEXT("receipts"), Entries))
    {
        UE_LOG(LogMKIBridge, Warning, TEXT("Ignoring unreadable receipt file %s"), *ReceiptStorePath);
        bReceiptStoreDirty = true; // Rewritten from memory on the next flush.
        return;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Entries)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString RequestId, Path, Body, CompletedAt; double Fingerprint = 0, Code = 0, PlaySeconds = -1;
        if (!Value.IsValid() || !Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("requestId"), RequestId) || !(*Entry)->TryGetStringField(TEXT("path"), Path)
            || !(*Entry)->TryGetStringField(TEXT("body"), Body) || !(*Entry)->TryGetNumberField(TEXT("fingerprint"), Fingerprint) || !(*Entry)->TryGetNumberField(TEXT("code"), Code)
            || !(*Entry)->TryGetNumberField(TEXT("playDurationSeconds"), PlaySeconds) || RequestId.IsEmpty() || RequestId.Len() > 128 || Code < 200 || Code >= 300)
        { ++DiscardedReceiptCount; continue; }
        if (PlaySeconds < 0 || PlaySeconds >= LoadedPlaySeconds) { ++DiscardedReceiptCount; continue; }
        // A requestId completed in this world before the game state was ready wins over the stored one.
        if (MutationReceipts.Contains(RequestId)) continue;
        FMutationReceipt& Receipt = MutationReceipts.Add(RequestId);
        Receipt.Path = Path; Receipt.Fingerprint = static_cast<uint32>(Fingerprint); Receipt.bInFlight = false; Receipt.Code = static_cast<int32>(Code); Receipt.Body = Body;
        Receipt.PlayDurationSeconds = PlaySeconds; Receipt.bRestored = true;
        (*Entry)->TryGetStringField(TEXT("completedAt"), CompletedAt);
        FDateTime::ParseIso8601(*CompletedAt, Receipt.CompletedAt);
        MutationReceiptOrder.Insert(RequestId, RestoredReceiptCount);
        ++RestoredReceiptCount;
    }
    while (MutationReceiptOrder.Num() > MaximumMutationReceipts)
    {
        MutationReceipts.Remove(MutationReceiptOrder[0]);
        MutationReceiptOrder.RemoveAt(0);
    }
    if (DiscardedReceiptCount > 0) bReceiptStoreDirty = true;
    UE_LOG(LogMKIBridge, Display, TEXT("Restored %d mutation receipt(s) for session '%s' at play time %.0f s; discarded %d recorded after the loaded save"),
        RestoredReceiptCount, *ReceiptSessionName, LoadedPlaySeconds, DiscardedReceiptCount);
}

void FMKIBridgeModule::FlushReceiptStore()
{
    LastReceiptFlushSeconds = FPlatformTime::Seconds();
    if (!bReceiptStoreDirty) return;
    bReceiptStoreDirty = false;
    if (ReceiptStorePath.IsEmpty()) return; // Unnamed session or no game state: memory-only.
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FString& RequestId : MutationReceiptOrder)
    {
        const FMutationReceipt* Receipt = MutationReceipts.Find(RequestId);
        if (!Receipt || Receipt->bInFlight || Receipt->PlayDurationSeconds < 0 || Receipt->Body.Len() > MaximumPersistedReceiptBodyChars) continue;
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("requestId"), RequestId);
        Entry->SetStringField(TEXT("path"), Receipt->Path);
        Entry->SetNumberField(TEXT("fingerprint"), Receipt->Fingerprint);
        Entry->SetNumberField(TEXT("code"), Receipt->Code);
        Entry->SetStringField(TEXT("body"), Receipt->Body);
        Entry->SetNumberField(TEXT("playDurationSeconds"), Receipt->PlayDurationSeconds);
        Entry->SetStringField(TEXT("completedAt"), Receipt->CompletedAt.ToIso8601());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("version"), 1);
    Root->SetStringField(TEXT("sessionName"), ReceiptSessionName);
    Root->SetArrayField(TEXT("receipts"), Entries);
    FString Text; const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Root, Writer);
    // Write a temporary file and move it over the old one so a crash mid-write never leaves a truncated store.
    const FString Temporary = ReceiptStorePath + TEXT(".tmp");
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*FPaths::GetPath(ReceiptStorePath));
    if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) || !IFileManager::Get().Move(*ReceiptStorePath, *Temporary, true))
    {
        UE_LOG(LogMKIBridge, Warning, TEXT("Unable to write receipt file %s; receipts remain in memory"), *ReceiptStorePath);
        bReceiptStoreDirty = true;
    }
}

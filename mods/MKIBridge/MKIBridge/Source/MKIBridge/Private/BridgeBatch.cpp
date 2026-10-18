#include "BridgeInternal.h"

// Atomic batches: validation, repeat expansion, stepping, and rollback.
// Split from the single MKIBridge.cpp without behavior changes (#44).

/** One expanded operation of a `/v1/batch` request (#59). */
struct FBatchOperation
{
    FString OpId;
    FString Kind;
    FString Route;
    TSharedPtr<FJsonObject> Request;
};

/** State of one batch while its operations run one after another. */
struct FBatchRun
{
    TArray<FBatchOperation> Operations;
    bool bConfirm = false;
    bool bAtomic = true;
    FString RequestId;
    TMap<FString, TArray<FString>> Headers;
    TSharedPtr<FInternetAddr> PeerAddress;
    FHttpResultCallback Respond;
    TSharedPtr<FBridgeRequestTicket> Ticket;
    bool bPreview = true;
    int32 Next = 0;
    bool bPreviewFailed = false;
    TMap<FString, FString> ResultIds;
    TArray<TSharedPtr<FJsonValue>> PreviewResults;
    TArray<TSharedPtr<FJsonValue>> Results;
    /** Created buildable IDs in creation order, for atomic rollback. */
    TArray<FString> Created;
    TArray<FString> SubRequestIds;
};

/**
 * The absolute placement shape `repeat` can offset in a batch request (#133): "flat" (numeric x and y), "position" (a
 * position object with numeric x, y, and z), or "lift" (bottom.position and top.position); empty when it has none.
 */
static FString BatchRepeatShape(const TSharedPtr<FJsonObject>& Body)
{
    auto HasVector = [](const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        return Parent.IsValid() && Parent->TryGetObjectField(Name, Object) && Object && (*Object).IsValid()
            && (*Object)->HasTypedField<EJson::Number>(TEXT("x")) && (*Object)->HasTypedField<EJson::Number>(TEXT("y")) && (*Object)->HasTypedField<EJson::Number>(TEXT("z"));
    };
    if (!Body.IsValid()) return FString();
    if (Body->HasTypedField<EJson::Number>(TEXT("x")) && Body->HasTypedField<EJson::Number>(TEXT("y"))) return TEXT("flat");
    if (HasVector(Body, TEXT("position"))) return TEXT("position");
    const TSharedPtr<FJsonObject>* Bottom = nullptr; const TSharedPtr<FJsonObject>* Top = nullptr;
    if (Body->TryGetObjectField(TEXT("bottom"), Bottom) && Bottom && Body->TryGetObjectField(TEXT("top"), Top) && Top && HasVector(*Bottom, TEXT("position")) && HasVector(*Top, TEXT("position"))) return TEXT("lift");
    return FString();
}

/** A copy of Parent.Name (an x/y/z object) moved by Delta; Parent gets the copy, so the batch's other copies keep theirs. */
static void OffsetBatchVector(const TSharedRef<FJsonObject>& Parent, const TCHAR* Name, const FVector& Delta)
{
    const TSharedPtr<FJsonObject> Source = Parent->GetObjectField(Name);
    const TSharedRef<FJsonObject> Moved = MakeShared<FJsonObject>();
    Moved->Values = Source->Values;
    Moved->SetNumberField(TEXT("x"), Source->GetNumberField(TEXT("x")) + Delta.X);
    Moved->SetNumberField(TEXT("y"), Source->GetNumberField(TEXT("y")) + Delta.Y);
    Moved->SetNumberField(TEXT("z"), Source->GetNumberField(TEXT("z")) + Delta.Z);
    Parent->SetObjectField(Name, Moved);
}

/** Move one repeated copy of a placement request by Delta in its own shape (BatchRepeatShape); the copy owns its nested objects. */
static void OffsetBatchRepeat(const TSharedRef<FJsonObject>& Request, const FVector& Delta)
{
    const FString Shape = BatchRepeatShape(Request);
    if (Shape == TEXT("flat"))
    {
        Request->SetNumberField(TEXT("x"), Request->GetNumberField(TEXT("x")) + Delta.X);
        Request->SetNumberField(TEXT("y"), Request->GetNumberField(TEXT("y")) + Delta.Y);
        double Z = 0; if (Request->TryGetNumberField(TEXT("z"), Z)) Request->SetNumberField(TEXT("z"), Z + Delta.Z);
    }
    else if (Shape == TEXT("position")) OffsetBatchVector(Request, TEXT("position"), Delta);
    else if (Shape == TEXT("lift"))
    {
        for (const TCHAR* End : { TEXT("bottom"), TEXT("top") })
        {
            const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
            Copy->Values = Request->GetObjectField(End)->Values;
            OffsetBatchVector(Copy, TEXT("position"), Delta);
            Request->SetObjectField(End, Copy);
        }
    }
}

namespace
{
    constexpr int32 MaximumBatchOperations = 256;
    constexpr int32 MaximumBatchRequestIdLength = 80;

    bool IsBatchOpId(const FString& Id)
    {
        if (Id.IsEmpty() || Id.Len() > 40) return false;
        for (const TCHAR Character : Id) if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')) return false;
        return true;
    }

    /** `$<opId>.buildableId` -> opId; empty when the text is not a reference. */
    FString BatchReferenceTarget(const FString& Text)
    {
        if (!Text.StartsWith(TEXT("$"), ESearchCase::CaseSensitive) || !Text.EndsWith(TEXT(".buildableId"), ESearchCase::CaseSensitive)) return FString();
        return Text.Mid(1, Text.Len() - 1 - 12);
    }

    /** Every created ID a mutation response names, primary first. */
    TArray<FString> CreatedIdsOf(const TSharedPtr<FJsonObject>& Body)
    {
        TArray<FString> Ids;
        if (!Body.IsValid()) return Ids;
        FString Primary;
        if (Body->TryGetStringField(TEXT("buildableId"), Primary) && !Primary.IsEmpty()) Ids.Add(Primary);
        for (const TCHAR* Field : { TEXT("buildableIds"), TEXT("childBuildableIds") })
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Body->TryGetArrayField(Field, Values)) for (const TSharedPtr<FJsonValue>& Value : *Values) { FString Id; if (Value.IsValid() && Value->TryGetString(Id) && !Id.IsEmpty()) Ids.AddUnique(Id); }
        }
        return Ids;
    }
}

/**
 * POST /v1/batch (#59): ordered place, connect, dismantle, and set_recipe operations. A preview pass runs every
 * operation as a dry run first (placements drive the native hologram with validateHologram and report its
 * disqualifiers); nothing is built unless every preview passes. Confirmed operations then run one per tick through
 * the shared route wrapper, each with its own requestId `<batch requestId>:<opId>`, so a retried batch replays what
 * already completed instead of building it twice. `"$<opId>.buildableId"` in a request names an earlier operation's
 * created buildable. `repeat` expands one placement into `count` copies (`<opId>#<i>`), each offset by `offsetCm` in
 * the request's own shape (flat x/y/z, position{}, or a lift's bottom/top positions; #133) and turned by `yawStep`. With `atomic` (the default) a failure dismantles everything the batch created and frees
 * every operation's requestId; atomic batches therefore accept only place and connect.
 */
bool FMKIBridgeModule::HandleBatch(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    TSharedPtr<FJsonObject> Json;
    if (!ReadJsonBody(Request, Json)) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    auto Invalid = [&OnComplete](const TCHAR* Code, const FString& Message) { OnComplete(ErrorResponse(Code, Message, EHttpServerResponseCodes::BadRequest)); return true; };
    const TSharedRef<FBatchRun> Run = MakeShared<FBatchRun>();
    Json->TryGetBoolField(TEXT("confirm"), Run->bConfirm);
    Json->TryGetBoolField(TEXT("atomic"), Run->bAtomic);
    Json->TryGetStringField(TEXT("requestId"), Run->RequestId);
    if (Run->bConfirm && (Run->RequestId.IsEmpty() || Run->RequestId.Len() > MaximumBatchRequestIdLength || Run->RequestId.Contains(TEXT(":"))))
        return Invalid(TEXT("invalid_request_id"), FString::Printf(TEXT("A confirmed batch requires a requestId of at most %d characters without ':'."), MaximumBatchRequestIdLength));
    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Json->TryGetArrayField(TEXT("ops"), Ops) || Ops->Num() == 0 || Ops->Num() > 64) return Invalid(TEXT("invalid_batch"), TEXT("ops must list 1 to 64 operations."));

    TSet<FString> Known;
    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Entry)) return Invalid(TEXT("invalid_batch"), TEXT("Each operation must be an object."));
        FString OpId, Kind; (*Entry)->TryGetStringField(TEXT("opId"), OpId); (*Entry)->TryGetStringField(TEXT("op"), Kind);
        if (!IsBatchOpId(OpId) || Known.Contains(OpId)) return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("opId '%s' must be unique, 1-40 characters of letters, digits, '_' or '-'."), *OpId));
        FString Route;
        if (Kind == TEXT("place")) Route = TEXT("/v1/place");
        else if (Kind == TEXT("connect")) Route = TEXT("/v1/connect");
        else if (Kind == TEXT("dismantle")) Route = TEXT("/v1/dismantle");
        else if (Kind == TEXT("set_recipe")) Route = TEXT("/v1/set-machine-recipe");
        else return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("Operation %s: op must be place, connect, dismantle, or set_recipe."), *OpId));
        if (Run->bAtomic && Route != TEXT("/v1/place") && Route != TEXT("/v1/connect"))
            return Invalid(TEXT("atomic_irreversible_op"), FString::Printf(TEXT("Operation %s: an atomic batch can only place and connect, because dismantling and recipe changes cannot be rolled back. Send atomic:false."), *OpId));
        const TSharedPtr<FJsonObject>* Body = nullptr;
        if (!(*Entry)->TryGetObjectField(TEXT("request"), Body)) return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("Operation %s needs a request object."), *OpId));
        for (const TCHAR* Reserved : { TEXT("confirm"), TEXT("requestId"), TEXT("validateHologram") })
            if ((*Body)->HasField(Reserved)) return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("Operation %s: %s is set by the batch."), *OpId, Reserved));

        int32 Count = 1; FVector Offset = FVector::ZeroVector; double YawStep = 0.0;
        const TSharedPtr<FJsonObject>* Repeat = nullptr;
        if ((*Entry)->TryGetObjectField(TEXT("repeat"), Repeat))
        {
            double CountValue = 0;
            if (Route != TEXT("/v1/place") || !(*Repeat)->TryGetNumberField(TEXT("count"), CountValue) || CountValue < 1 || CountValue > 64 || CountValue != FMath::FloorToDouble(CountValue))
                return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("Operation %s: repeat applies to place and needs an integer count of 1 to 64."), *OpId));
            // #133: repeat offsets every absolute placement shape the batch accepts: flat x/y (generic placements), a
            // position{} (foundations, poles, and other position-shaped kinds), or a lift's bottom.position and top.position.
            if (BatchRepeatShape(*Body).IsEmpty())
                return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("Operation %s: repeat needs an absolute placement: numeric x and y, a position{x,y,z}, or a lift's bottom.position and top.position (relativeToPlayer placements cannot repeat)."), *OpId));
            Count = static_cast<int32>(CountValue);
            const TSharedPtr<FJsonObject>* OffsetJson = nullptr;
            if ((*Repeat)->TryGetObjectField(TEXT("offsetCm"), OffsetJson)) { (*OffsetJson)->TryGetNumberField(TEXT("x"), Offset.X); (*OffsetJson)->TryGetNumberField(TEXT("y"), Offset.Y); (*OffsetJson)->TryGetNumberField(TEXT("z"), Offset.Z); }
            (*Repeat)->TryGetNumberField(TEXT("yawStep"), YawStep);
            if (!FMath::IsFinite(Offset.X) || !FMath::IsFinite(Offset.Y) || !FMath::IsFinite(Offset.Z) || !FMath::IsFinite(YawStep)) return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("Operation %s: repeat values must be finite."), *OpId));
        }
        if (Run->Operations.Num() + Count > MaximumBatchOperations) return Invalid(TEXT("invalid_batch"), FString::Printf(TEXT("A batch expands to at most %d operations."), MaximumBatchOperations));
        // References may only name operations that come earlier, so every one is resolved before it is needed.
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Body)->Values)
        {
            FString Text;
            if (!Field.Value.IsValid() || !Field.Value->TryGetString(Text) || !Text.StartsWith(TEXT("$"), ESearchCase::CaseSensitive)) continue;
            const FString Target = BatchReferenceTarget(Text);
            if (Target.IsEmpty() || !Run->Operations.ContainsByPredicate([&Target](const FBatchOperation& Earlier) { return Earlier.OpId.Equals(Target, ESearchCase::CaseSensitive); }))
                return Invalid(TEXT("invalid_reference"), FString::Printf(TEXT("Operation %s: %s must be \"$<earlier opId>.buildableId\"; a repeated placement is named <opId>#<index>."), *OpId, *Text));
        }
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FBatchOperation& Operation = Run->Operations.AddDefaulted_GetRef();
            Operation.OpId = Count == 1 && !Repeat ? OpId : FString::Printf(TEXT("%s#%d"), *OpId, Index);
            Operation.Kind = Kind; Operation.Route = Route;
            Operation.Request = MakeShared<FJsonObject>(); Operation.Request->Values = (*Body)->Values;
            if (Repeat)
            {
                OffsetBatchRepeat(Operation.Request.ToSharedRef(), Offset * Index);
                if (YawStep != 0.0) { double Yaw = 0; (*Body)->TryGetNumberField(TEXT("yaw"), Yaw); Operation.Request->SetNumberField(TEXT("yaw"), FRotator::NormalizeAxis(Yaw + YawStep * Index)); }
            }
        }
        Known.Add(OpId);
    }
    Run->Headers = Request.Headers; Run->PeerAddress = Request.PeerAddress; Run->Respond = OnComplete; Run->Ticket = ActiveTicket;
    RunBatchStep(Run);
    return true;
}

void FMKIBridgeModule::RunBatchStep(TSharedRef<FBatchRun> Run)
{
    if (Run->Ticket.IsValid() && Run->Ticket->bAbandoned)
    {
        UE_LOG(LogMKIBridge, Warning, TEXT("Batch %s was abandoned after %d operation(s); created: %s"), *Run->RequestId, Run->Next, *FString::Join(Run->Created, TEXT(",")));
        return;
    }
    if (Run->Next >= Run->Operations.Num())
    {
        if (Run->bPreview)
        {
            if (Run->bPreviewFailed || !Run->bConfirm)
            {
                const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
                Body->SetBoolField(TEXT("ok"), !Run->bPreviewFailed);
                if (Run->bPreviewFailed) { Body->SetStringField(TEXT("error"), TEXT("batch_preview_failed")); Body->SetStringField(TEXT("message"), TEXT("At least one operation failed its preview; nothing was built.")); Body->SetBoolField(TEXT("retryable"), false); }
                Body->SetBoolField(TEXT("dryRun"), true);
                Body->SetBoolField(TEXT("atomic"), Run->bAtomic);
                Body->SetNumberField(TEXT("operationCount"), Run->Operations.Num());
                Body->SetArrayField(TEXT("preview"), Run->PreviewResults);
                Run->Respond(JsonResponse(Body, Run->bPreviewFailed ? EHttpServerResponseCodes::Conflict : EHttpServerResponseCodes::Ok));
                return;
            }
            Run->bPreview = false; Run->Next = 0;
        }
        else
        {
            const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
            Body->SetBoolField(TEXT("ok"), true);
            Body->SetBoolField(TEXT("dryRun"), false);
            Body->SetBoolField(TEXT("atomic"), Run->bAtomic);
            Body->SetStringField(TEXT("requestId"), Run->RequestId);
            Body->SetArrayField(TEXT("preview"), Run->PreviewResults);
            Body->SetArrayField(TEXT("results"), Run->Results);
            TArray<TSharedPtr<FJsonValue>> Created; for (const FString& Id : Run->Created) Created.Add(MakeShared<FJsonValueString>(Id));
            Body->SetArrayField(TEXT("createdIds"), Created);
            const TSharedRef<FJsonObject> Ids = MakeShared<FJsonObject>(); for (const TPair<FString, FString>& Pair : Run->ResultIds) Ids->SetStringField(Pair.Key, Pair.Value);
            Body->SetObjectField(TEXT("buildableIds"), Ids);
            Run->Respond(JsonResponse(Body, EHttpServerResponseCodes::Created));
            return;
        }
    }

    const FBatchOperation& Operation = Run->Operations[Run->Next];
    const FString SubRequestId = FString::Printf(TEXT("%s:%s"), *Run->RequestId, *Operation.OpId);
    const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("opId"), Operation.OpId);
    Entry->SetStringField(TEXT("op"), Operation.Kind);
    auto Advance = [this, Run]()
    {
        ++Run->Next;
        // One operation per tick: deferred hologram work and the game get a frame between operations.
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, Run](float) { RunBatchStep(Run); return false; }), 0.0f);
    };
    // Resume: an operation a previous attempt of this confirmed batch already completed is not previewed again (its
    // own buildable would now collide with it); its recorded result supplies its ID, and the confirmed pass replays it.
    if (Run->bPreview && Run->bConfirm)
    {
        const FMutationReceipt* Done = MutationReceipts.Find(SubRequestId);
        if (Done && !Done->bInFlight)
        {
            TSharedPtr<FJsonObject> Recorded; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Done->Body);
            FJsonSerializer::Deserialize(Reader, Recorded);
            const TArray<FString> Ids = CreatedIdsOf(Recorded);
            if (Ids.Num() > 0) Run->ResultIds.Add(Operation.OpId, Ids[0]);
            Entry->SetBoolField(TEXT("ok"), true);
            Entry->SetBoolField(TEXT("alreadyCompleted"), true);
            Run->PreviewResults.Add(MakeShared<FJsonValueObject>(Entry));
            Advance();
            return;
        }
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>(); Body->Values = Operation.Request->Values;
    bool bDeferred = false;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Operation.Request->Values)
    {
        FString Text;
        if (!Field.Value.IsValid() || !Field.Value->TryGetString(Text)) continue;
        const FString Target = BatchReferenceTarget(Text);
        if (Target.IsEmpty()) continue;
        if (const FString* Resolved = Run->ResultIds.Find(Target)) Body->SetStringField(Field.Key, *Resolved);
        else bDeferred = true;
    }
    if (Run->bPreview && bDeferred)
    {
        // An operation that names a buildable the batch has not built yet cannot be previewed before mutation; it is
        // previewed again implicitly by its own handler when the confirmed pass reaches it.
        Entry->SetBoolField(TEXT("ok"), true);
        Entry->SetBoolField(TEXT("deferred"), true);
        Entry->SetStringField(TEXT("reason"), TEXT("depends_on_earlier_operation"));
        Run->PreviewResults.Add(MakeShared<FJsonValueObject>(Entry));
        Advance();
        return;
    }
    const TPair<FRouteMethod, FRouteOptions>* Route = RouteTable.Find(Operation.Route);
    if (!Route) { Run->Respond(ErrorResponse(TEXT("batch_route_unavailable"), Operation.Route, EHttpServerResponseCodes::ServerError)); return; }
    Body->SetBoolField(TEXT("confirm"), !Run->bPreview);
    if (!Run->bPreview) { Body->SetStringField(TEXT("requestId"), SubRequestId); Run->SubRequestIds.Add(SubRequestId); }
    if (Run->bPreview && Operation.Route == TEXT("/v1/place")) Body->SetBoolField(TEXT("validateHologram"), true);
    FHttpServerRequest Sub;
    Sub.Verb = EHttpServerRequestVerbs::VERB_POST;
    Sub.Headers = Run->Headers;
    Sub.PeerAddress = Run->PeerAddress;
    const FString Text = SerializeCondensed(Body);
    const FTCHARToUTF8 Utf8(*Text);
    Sub.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    const FHttpResultCallback Callback = [this, Run, Entry](TUniquePtr<FHttpServerResponse>&& Response)
    {
        FString ResponseText;
        if (Response.IsValid() && Response->Body.Num() > 0) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num()); ResponseText = FString(Converted.Length(), Converted.Get()); }
        TSharedPtr<FJsonObject> Parsed; const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseText);
        FJsonSerializer::Deserialize(Reader, Parsed);
        const int32 Code = Response.IsValid() ? static_cast<int32>(Response->Code) : 500;
        bool bOk = Code >= 200 && Code < 300 && Parsed.IsValid();
        bool BodyOk = true, HologramValid = true;
        if (Parsed.IsValid() && Parsed->TryGetBoolField(TEXT("ok"), BodyOk) && !BodyOk) bOk = false;
        if (Parsed.IsValid() && Parsed->TryGetBoolField(TEXT("hologramValid"), HologramValid) && !HologramValid) bOk = false;
        Entry->SetBoolField(TEXT("ok"), bOk);
        Entry->SetNumberField(TEXT("status"), Code);
        if (Parsed.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Disqualifiers = nullptr;
            if (Parsed->TryGetArrayField(TEXT("disqualifiers"), Disqualifiers)) Entry->SetArrayField(TEXT("disqualifiers"), *Disqualifiers);
            Entry->SetObjectField(TEXT("response"), Parsed);
        }
        if (Run->bPreview)
        {
            Run->PreviewResults.Add(MakeShared<FJsonValueObject>(Entry));
            Run->bPreviewFailed |= !bOk;
            ++Run->Next;
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, Run](float) { RunBatchStep(Run); return false; }), 0.0f);
            return;
        }
        Run->Results.Add(MakeShared<FJsonValueObject>(Entry));
        if (bOk)
        {
            const TArray<FString> Ids = CreatedIdsOf(Parsed);
            if (Ids.Num() > 0) Run->ResultIds.Add(Run->Operations[Run->Next].OpId, Ids[0]);
            for (const FString& Id : Ids) Run->Created.AddUnique(Id);
            ++Run->Next;
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, Run](float) { RunBatchStep(Run); return false; }), 0.0f);
            return;
        }
        FailBatch(Run, Run->Operations[Run->Next].OpId);
    };
    DispatchRoute(Sub, Callback, Route->Key, Route->Value);
}

void FMKIBridgeModule::FailBatch(TSharedRef<FBatchRun> Run, const FString& FailedOpId)
{
    TArray<TSharedPtr<FJsonValue>> RolledBack, Leaked; const TSharedRef<FJsonObject> ResolvedRollbackIds = MakeShared<FJsonObject>();
    if (Run->bAtomic)
    {
        // Undo in reverse creation order through the same checked dismantle route, then free every operation's
        // requestId so a retried batch builds again instead of replaying operations that were undone.
        // The game converts foundations and walls to lightweight instances as they are built, so an operation's
        // actor ID may no longer exist (a rollback leaked such a foundation live). Resolve each created ID through
        // its operation's placement origin (#113) first; the origins are still held because the receipts are only
        // forgotten below.
        UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
        TMap<FString, const FPlacementOrigin*> OriginsByActor;
        for (const FString& SubRequestId : Run->SubRequestIds) if (const FPlacementOrigin* Origin = PlacementOrigins.Find(SubRequestId)) OriginsByActor.Add(Origin->ActorName, Origin);
        for (int32 Index = Run->Created.Num() - 1; Index >= 0; --Index)
        {
            const FString& Id = Run->Created[Index];
            FString DismantleId = Id;
            if (const FPlacementOrigin* const* Origin = OriginsByActor.Find(Id))
            {
                FString Resolution; const FString Final = ResolvePlacementOrigin(World, **Origin, Resolution);
                if (Resolution == TEXT("lightweight_conversion") && !Final.IsEmpty()) { DismantleId = Final; ResolvedRollbackIds->SetStringField(Id, Final); }
            }
            const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
            Body->SetStringField(TEXT("buildableId"), DismantleId); Body->SetBoolField(TEXT("confirm"), true);
            FHttpServerRequest Sub; Sub.Verb = EHttpServerRequestVerbs::VERB_POST; Sub.Headers = Run->Headers; Sub.PeerAddress = Run->PeerAddress;
            const FString Text = SerializeCondensed(Body); const FTCHARToUTF8 Utf8(*Text); Sub.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            bool bRemoved = false; FString Detail = TEXT("no response");
            HandleDeleteBuildable(Sub, [&bRemoved, &Detail](TUniquePtr<FHttpServerResponse>&& Response)
            {
                if (!Response.IsValid()) return;
                FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Response->Body.GetData()), Response->Body.Num());
                Detail = FString(Converted.Length(), Converted.Get());
                bRemoved = static_cast<int32>(Response->Code) >= 200 && static_cast<int32>(Response->Code) < 300 && !Detail.Contains(TEXT("\"verifiedDismantled\":false"));
            });
            if (bRemoved) RolledBack.Add(MakeShared<FJsonValueString>(Id));
            else { const TSharedRef<FJsonObject> Leak = MakeShared<FJsonObject>(); Leak->SetStringField(TEXT("id"), DismantleId); Leak->SetStringField(TEXT("detail"), Detail); Leaked.Add(MakeShared<FJsonValueObject>(Leak)); }
        }
        for (const FString& SubRequestId : Run->SubRequestIds) ForgetMutationReceipt(SubRequestId);
    }
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), false);
    Body->SetStringField(TEXT("error"), TEXT("batch_failed"));
    Body->SetStringField(TEXT("message"), Run->bAtomic
        ? FString::Printf(TEXT("Operation %s failed; the batch was rolled back (%d removed, %d leaked)."), *FailedOpId, RolledBack.Num(), Leaked.Num())
        : FString::Printf(TEXT("Operation %s failed; earlier operations remain built. Retrying the same requestId replays them and resumes at %s."), *FailedOpId, *FailedOpId));
    Body->SetBoolField(TEXT("retryable"), false);
    Body->SetStringField(TEXT("failedOpId"), FailedOpId);
    Body->SetBoolField(TEXT("atomic"), Run->bAtomic);
    Body->SetArrayField(TEXT("preview"), Run->PreviewResults);
    Body->SetArrayField(TEXT("results"), Run->Results);
    TArray<TSharedPtr<FJsonValue>> Created; for (const FString& Id : Run->Created) Created.Add(MakeShared<FJsonValueString>(Id));
    Body->SetArrayField(TEXT("createdIds"), Created);
    if (Run->bAtomic) { Body->SetArrayField(TEXT("rolledBack"), RolledBack); Body->SetArrayField(TEXT("leaked"), Leaked); Body->SetObjectField(TEXT("rollbackResolvedIds"), ResolvedRollbackIds); }
    Run->Respond(JsonResponse(Body, EHttpServerResponseCodes::Conflict));
}

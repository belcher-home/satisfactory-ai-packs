#include "BridgeInternal.h"

// The native event stream (#117): scope, emission, sampling, and long-poll waiters.
// Split from the single MKIBridge.cpp without behavior changes (#44).

void FMKIBridgeModule::EnsureEventScope(UWorld* World)
{
    if (EventsWorld.Get() == World && !EventsEpoch.IsEmpty()) return;
    // A new world (or load) starts a new epoch: sequence numbers and samples never carry over, and every open
    // wait of the old epoch is answered with resync.
    Events.Reset(); EventSeq = 0; OldestDroppedEventSeq = 0;
    EventsEpoch = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    EventsWorld = World;
    bEventBaselineTaken = false; MachineStateSamples.Reset(); CircuitFuseSamples.Reset();
    ServeEventWaiters();
}

void FMKIBridgeModule::EmitBridgeEvent(UWorld* World, const TCHAR* Type, const TSharedRef<FJsonObject>& Data)
{
    EnsureEventScope(World);
    FBridgeEvent& Event = Events.AddDefaulted_GetRef();
    Event.Seq = ++EventSeq; Event.Type = Type; Event.Time = FDateTime::UtcNow(); Event.Data = Data;
    if (Events.Num() > MaximumEventBacklog)
    {
        OldestDroppedEventSeq = Events[0].Seq;
        Events.RemoveAt(0);
    }
    ServeEventWaiters();
}

TSharedPtr<FJsonObject> FMKIBridgeModule::CollectEvents(const FString& Epoch, int64 Since, const TArray<FString>& Types, bool bTimedOut) const
{
    // A cursor from another epoch, from the future, or older than the retained backlog cannot be continued: the
    // caller re-observes the world and continues from latestSeq.
    const bool bResync = !Epoch.Equals(EventsEpoch, ESearchCase::CaseSensitive) || Since > EventSeq || Since < OldestDroppedEventSeq;
    TArray<TSharedPtr<FJsonValue>> Items;
    int64 Cursor = Since;
    bool bMore = false;
    if (!bResync)
    {
        for (const FBridgeEvent& Event : Events)
        {
            if (Event.Seq <= Since) continue;
            if (Types.Num() > 0 && !Types.Contains(Event.Type)) { Cursor = Event.Seq; continue; }
            if (Items.Num() >= MaximumEventsPerResponse) { bMore = true; break; }
            const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            Item->SetNumberField(TEXT("seq"), static_cast<double>(Event.Seq));
            Item->SetStringField(TEXT("type"), Event.Type);
            Item->SetStringField(TEXT("time"), Event.Time.ToIso8601());
            Item->SetObjectField(TEXT("data"), Event.Data.IsValid() ? Event.Data : MakeShared<FJsonObject>());
            Items.Add(MakeShared<FJsonValueObject>(Item));
            Cursor = Event.Seq;
        }
    }
    if (!bResync && Items.Num() == 0 && !bTimedOut) return nullptr;
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetStringField(TEXT("epoch"), EventsEpoch);
    Body->SetNumberField(TEXT("latestSeq"), static_cast<double>(EventSeq));
    Body->SetNumberField(TEXT("nextSince"), static_cast<double>(bResync ? EventSeq : Cursor));
    Body->SetBoolField(TEXT("resync"), bResync);
    Body->SetBoolField(TEXT("more"), bMore);
    Body->SetBoolField(TEXT("timedOut"), !bResync && Items.Num() == 0);
    Body->SetArrayField(TEXT("events"), Items);
    return Body;
}

void FMKIBridgeModule::ServeEventWaiters()
{
    const double Now = FPlatformTime::Seconds();
    for (int32 Index = 0; Index < EventWaiters.Num();)
    {
        const TSharedRef<FEventWaiter> Waiter = EventWaiters[Index];
        const TSharedPtr<FJsonObject> Body = CollectEvents(Waiter->Epoch, Waiter->Since, Waiter->Types, Now >= Waiter->Deadline);
        if (!Body.IsValid()) { ++Index; continue; }
        EventWaiters.RemoveAt(Index);
        Waiter->Respond(JsonResponse(Body.ToSharedRef()));
    }
}

/**
 * GET /v1/events?since=<seq>&epoch=<epoch>&timeoutMs=<0..30000>&types=<a,b>. Answers at once when events after
 * `since` exist (or the cursor needs resync); otherwise holds the request until one arrives or the wait expires.
 * Without `since` the cursor starts at the latest event, so a first call never replays the backlog.
 */
bool FMKIBridgeModule::HandleEvents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No world is loaded."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    EnsureEventScope(World);
    LastEventsPollSeconds = FPlatformTime::Seconds();
    auto Invalid = [&OnComplete](const FString& Message) { OnComplete(ErrorResponse(TEXT("invalid_query"), Message, EHttpServerResponseCodes::BadRequest)); return true; };
    auto Integer = [](const FString& Text, int64 Minimum, int64 Maximum, int64& Out)
    {
        if (Text.IsEmpty() || Text.Len() > 18) return false;
        for (const TCHAR Character : Text) if (!FChar::IsDigit(Character)) return false;
        Out = FCString::Atoi64(*Text);
        return Out >= Minimum && Out <= Maximum;
    };
    int64 Since = EventSeq; FString Epoch = EventsEpoch;
    const FString* SinceText = Request.QueryParams.Find(TEXT("since"));
    const FString* EpochText = Request.QueryParams.Find(TEXT("epoch"));
    if (SinceText)
    {
        if (!Integer(*SinceText, 0, MAX_int64 / 2, Since)) return Invalid(TEXT("since must be a non-negative integer."));
        // Sequence numbers restart with every epoch, so a cursor is meaningless without the epoch it came from.
        if (!EpochText || EpochText->IsEmpty()) return Invalid(TEXT("since requires the epoch it was read in."));
        Epoch = *EpochText;
    }
    else if (EpochText) return Invalid(TEXT("epoch is only meaningful with since."));
    int64 TimeoutMs = 0;
    if (const FString* Text = Request.QueryParams.Find(TEXT("timeoutMs")))
        if (!Integer(*Text, 0, MaximumEventWaitMs, TimeoutMs)) return Invalid(FString::Printf(TEXT("timeoutMs must be an integer between 0 and %d."), MaximumEventWaitMs));
    TArray<FString> Types;
    if (const FString* Text = Request.QueryParams.Find(TEXT("types")))
    {
        Text->ParseIntoArray(Types, TEXT(","), true);
        if (Types.Num() == 0 || Types.Num() > BridgeEventTypes().Num()) return Invalid(TEXT("types must list known event types."));
        for (const FString& Type : Types)
        {
            bool bKnown = false;
            for (const TCHAR* Known : BridgeEventTypes()) if (Type.Equals(Known, ESearchCase::CaseSensitive)) bKnown = true;
            if (!bKnown) return Invalid(FString::Printf(TEXT("Unknown event type '%s'."), *Type));
        }
    }
    if (const TSharedPtr<FJsonObject> Body = CollectEvents(Epoch, Since, Types, TimeoutMs == 0))
    {
        OnComplete(JsonResponse(Body.ToSharedRef()));
        return true;
    }
    if (EventWaiters.Num() >= MaximumEventWaiters)
    {
        OnComplete(ErrorResponse(TEXT("too_many_waiters"), FString::Printf(TEXT("At most %d event waits may be open at once; retry shortly."), MaximumEventWaiters), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }
    const TSharedRef<FEventWaiter> Waiter = MakeShared<FEventWaiter>();
    Waiter->Epoch = Epoch; Waiter->Since = Since; Waiter->Types = Types; Waiter->Respond = OnComplete;
    Waiter->Deadline = FPlatformTime::Seconds() + static_cast<double>(TimeoutMs) / 1000.0;
    EventWaiters.Add(Waiter);
    return true;
}

void FMKIBridgeModule::SampleWorldEvents(UWorld* World)
{
    // Buildable additions, changes, and removals come from the same rescan that drives filtered listings (#113).
    RefreshBuildableRevisions(World);
    AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World);
    AFGCircuitSubsystem* Circuits = AFGCircuitSubsystem::Get(World);
    const APawn* Player = UGameplayStatics::GetPlayerPawn(World, 0);
    if (!Subsystem || !Player) return;
    const double ObservationCm = GetMaximumObservationDistanceCm();
    TSet<FString> SeenMachines;
    TSet<int32> SeenCircuits;
    for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
    {
        AFGBuildableFactory* Machine = Cast<AFGBuildableFactory>(Buildable);
        if (!IsValid(Machine) || FVector::Distance(Machine->GetActorLocation(), Player->GetActorLocation()) > ObservationCm) continue;
        const FString Id = Machine->GetName();
        const FString State = BridgeMachineState(World, Machine);
        SeenMachines.Add(Id);
        // A machine seen for the first time is recorded silently; its arrival is a buildable_added event.
        if (FString* Previous = MachineStateSamples.Find(Id))
        {
            if (!Previous->Equals(State, ESearchCase::CaseSensitive))
            {
                const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
                Event->SetStringField(TEXT("id"), Id);
                Event->SetStringField(TEXT("classPath"), Machine->GetClass()->GetPathName());
                Event->SetStringField(TEXT("previousState"), *Previous);
                Event->SetStringField(TEXT("state"), State);
                *Previous = State;
                EmitBridgeEvent(World, TEXT("machine_state_changed"), Event);
            }
        }
        else MachineStateSamples.Add(Id, State);
        TInlineComponentArray<UFGPowerConnectionComponent*> Power(Machine);
        for (UFGPowerConnectionComponent* Connection : Power)
            if (Connection && Connection->GetCircuitID() != INDEX_NONE) { SeenCircuits.Add(Connection->GetCircuitID()); break; }
    }
    for (auto It = MachineStateSamples.CreateIterator(); It; ++It) if (!SeenMachines.Contains(It.Key())) It.RemoveCurrent();
    if (Circuits)
    {
        for (const int32 CircuitId : SeenCircuits)
        {
            // UFGPowerCircuit::IsFuseTriggered is not const in the FactoryGame headers.
            UFGPowerCircuit* Circuit = Cast<UFGPowerCircuit>(Circuits->FindCircuit(CircuitId));
            if (!Circuit) continue;
            const bool bTripped = Circuit->IsFuseTriggered();
            if (const bool* Previous = CircuitFuseSamples.Find(CircuitId))
            {
                if (*Previous != bTripped)
                {
                    const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
                    Event->SetNumberField(TEXT("circuitId"), CircuitId);
                    Event->SetBoolField(TEXT("fuseTriggered"), bTripped);
                    EmitBridgeEvent(World, bTripped ? TEXT("power_circuit_tripped") : TEXT("power_circuit_restored"), Event);
                }
            }
            CircuitFuseSamples.Add(CircuitId, bTripped);
        }
        for (auto It = CircuitFuseSamples.CreateIterator(); It; ++It) if (!SeenCircuits.Contains(It.Key())) It.RemoveCurrent();
    }
    bEventBaselineTaken = true;
}

bool FMKIBridgeModule::TickEvents(float)
{
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    const double Now = FPlatformTime::Seconds();
    if (World)
    {
        EnsureEventScope(World);
        EnsureReceiptScope(World);
        // Sampling rescans the world, so it runs only while a client is listening.
        const bool bListening = EventWaiters.Num() > 0 || Now - LastEventsPollSeconds < EventSubscriptionSeconds;
        if (bListening && Now - LastEventsSampleSeconds >= EventSampleIntervalSeconds)
        {
            LastEventsSampleSeconds = Now;
            SampleWorldEvents(World);
        }
    }
    ServeEventWaiters();
    if (bReceiptStoreDirty && Now - LastReceiptFlushSeconds >= ReceiptFlushIntervalSeconds) FlushReceiptStore();
    return true;
}

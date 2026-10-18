#include "BridgeInternal.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"

// Non-mutating probes: placement validation and site survey, terrain column probe, and hologram probe.
// Split from the single MKIBridge.cpp without behavior changes (#44).

bool FMKIBridgeModule::HandleValidatePlacement(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
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

    const TSharedPtr<FJsonObject>* PositionObject = nullptr;
    if (!Json->TryGetObjectField(TEXT("position"), PositionObject) || !PositionObject || !PositionObject->IsValid())
    {
        OnComplete(ErrorResponse(TEXT("invalid_position"), TEXT("position with numeric x, y, and z fields is required."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    if (!(*PositionObject)->TryGetNumberField(TEXT("x"), X) ||
        !(*PositionObject)->TryGetNumberField(TEXT("y"), Y) ||
        !(*PositionObject)->TryGetNumberField(TEXT("z"), Z) ||
        !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
    {
        OnComplete(ErrorResponse(TEXT("invalid_position"), TEXT("position x, y, and z must be finite numbers."), EHttpServerResponseCodes::BadRequest));
        return true;
    }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* PlayerPawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!PlayerPawn)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player is available for bounded validation."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }

    double WidthCm = 800.0, LengthCm = 800.0, HeightCm = 400.0, Yaw = 0.0, SampleSpacingCm = 200.0, SupportToleranceCm = 10.0;
    Json->TryGetNumberField(TEXT("widthCm"), WidthCm); Json->TryGetNumberField(TEXT("lengthCm"), LengthCm);
    Json->TryGetNumberField(TEXT("heightCm"), HeightCm); Json->TryGetNumberField(TEXT("yaw"), Yaw);
    Json->TryGetNumberField(TEXT("sampleSpacingCm"), SampleSpacingCm); Json->TryGetNumberField(TEXT("supportToleranceCm"), SupportToleranceCm);
    if (!FMath::IsFinite(WidthCm) || !FMath::IsFinite(LengthCm) || !FMath::IsFinite(HeightCm) || !FMath::IsFinite(Yaw) || WidthCm <= 0 || LengthCm <= 0 || HeightCm <= 0 || WidthCm > 50000 || LengthCm > 50000 || HeightCm > 20000)
    {
        OnComplete(ErrorResponse(TEXT("invalid_envelope"), TEXT("Finite positive widthCm, lengthCm, and heightCm within guarded limits are required."), EHttpServerResponseCodes::BadRequest)); return true;
    }
    SampleSpacingCm = FMath::Clamp(SampleSpacingCm, 50.0, 800.0); SupportToleranceCm = FMath::Clamp(SupportToleranceCm, 0.0, 100.0);
    // Optional caller-selected exclusions (at most 64) whose own volume should not
    // count as support, penetration, or collision. A conventional buildable ID
    // excludes that actor. A canonical lightweight identity
    // (lw|<classPath>|<index>) excludes exactly that lightweight instance: the
    // abstract instances its runtime record owns and, if one exists, its
    // temporary actor. The shared AbstractInstanceManager and every other
    // instance it holds remain checked. The bridge applies no policy; it echoes
    // what it resolved and why anything else did not resolve.
    TArray<FString> IgnoreIds; const TArray<TSharedPtr<FJsonValue>>* IgnoreValues = nullptr;
    if (Json->TryGetArrayField(TEXT("ignoreActorIds"), IgnoreValues) && IgnoreValues)
    {
        bool IgnoreListValid = IgnoreValues->Num() <= 64;
        for (const TSharedPtr<FJsonValue>& Value : *IgnoreValues) { FString Id; if (!Value.IsValid() || !Value->TryGetString(Id) || Id.IsEmpty()) IgnoreListValid = false; else IgnoreIds.AddUnique(Id); }
        if (!IgnoreListValid) { OnComplete(ErrorResponse(TEXT("invalid_ignore_list"), TEXT("ignoreActorIds must be at most 64 non-empty buildable ID strings."), EHttpServerResponseCodes::BadRequest)); return true; }
    }
    const FVector RequestedPosition(X, Y, Z);
    const FVector PlayerPosition = PlayerPawn->GetActorLocation();
    const double DistanceCm = FVector::Distance(RequestedPosition, PlayerPosition);
    const bool WithinSafetyRadius = DistanceCm <= GetMaximumPlacementDistanceCm();

    const FRotator Rotation(0, Yaw, 0); const FVector Forward = Rotation.Vector(); const FVector Right = FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y);
    const double BottomZ = Z - HeightCm * 0.5; const double TopZ = Z + HeightCm * 0.5;
    // At most MaximumSurveySamples support samples: a larger grid is sampled more coarsely, and the response reports
    // the requested and effective spacing so the caller can split the survey when it needs the finer grid.
    const double RequestedSampleSpacingCm = SampleSpacingCm;
    int32 XSamples = FMath::Max(2, FMath::CeilToInt(LengthCm / SampleSpacingCm)); int32 YSamples = FMath::Max(2, FMath::CeilToInt(WidthCm / SampleSpacingCm));
    bool SamplesCapped = false;
    while ((XSamples + 1) * (YSamples + 1) > MaximumSurveySamples)
    {
        SampleSpacingCm *= 1.25; SamplesCapped = true;
        XSamples = FMath::Max(2, FMath::CeilToInt(LengthCm / SampleSpacingCm)); YSamples = FMath::Max(2, FMath::CeilToInt(WidthCm / SampleSpacingCm));
    }
    bool SummaryOnly = false; Json->TryGetBoolField(TEXT("summaryOnly"), SummaryOnly);
    double MinimumSurfaceZ = TNumericLimits<double>::Max(), MaximumSurfaceZ = -TNumericLimits<double>::Max(); int32 MissingSupportSamples = 0, PenetratingSamples = 0, IncompleteSamples = 0, IgnoredTraceHits = 0, IgnoredOverlaps = 0;
    TArray<TSharedPtr<FJsonValue>> Samples;
    FCollisionQueryParams Query; Query.AddIgnoredActor(PlayerPawn);
    TArray<TSharedPtr<FJsonValue>> IgnoredActorIds, UnresolvedIgnoreActorIds, UnresolvedIgnores, IgnoredInstances;
    TSet<FSurveyInstanceKey> ExcludedInstances;
    AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World);
    if (IgnoreIds.Num() > 0)
    {
        TSet<FString> Remaining;
        for (const FString& Id : IgnoreIds) if (!Id.StartsWith(TEXT("lw|"), ESearchCase::CaseSensitive)) Remaining.Add(Id);
        if (Remaining.Num() > 0) if (AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World)) for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
            if (Buildable && Remaining.Remove(Buildable->GetName()) > 0) { Query.AddIgnoredActor(Buildable); IgnoredActorIds.Add(MakeShared<FJsonValueString>(Buildable->GetName())); }
        auto Unresolved = [&UnresolvedIgnoreActorIds, &UnresolvedIgnores](const FString& Id, const TCHAR* Reason)
        {
            UnresolvedIgnoreActorIds.Add(MakeShared<FJsonValueString>(Id));
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("id"), Id); Entry->SetStringField(TEXT("reason"), Reason);
            UnresolvedIgnores.Add(MakeShared<FJsonValueObject>(Entry));
        };
        for (const FString& Id : IgnoreIds)
        {
            if (!Id.StartsWith(TEXT("lw|"), ESearchCase::CaseSensitive)) { if (Remaining.Contains(Id)) Unresolved(Id, TEXT("buildable_not_found")); continue; }
            FString ClassPath; int32 Index = INDEX_NONE;
            if (!ParseCanonicalLightweightId(Id, ClassPath, Index)) { Unresolved(Id, TEXT("lightweight_id_malformed")); continue; }
            if (!Lightweight) { Unresolved(Id, TEXT("lightweight_subsystem_unavailable")); continue; }
            const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>* Match = nullptr;
            for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Pair : Lightweight->GetAllLightweightBuildableInstances())
                if (Pair.Key.Get() && Pair.Key.Get()->GetPathName().Equals(ClassPath, ESearchCase::CaseSensitive)) { Match = &Pair; break; }
            if (!Match) { Unresolved(Id, TEXT("lightweight_class_not_found")); continue; }
            if (!Match->Value.IsValidIndex(Index)) { Unresolved(Id, TEXT("lightweight_index_out_of_range")); continue; }
            const FRuntimeBuildableInstanceData& Data = Match->Value[Index];
            if (!Data.IsValid()) { Unresolved(Id, TEXT("lightweight_instance_removed")); continue; }
            TArray<FSurveyInstanceKey> Keys;
            for (const FInstanceOwnerHandlePtr& Handle : Data.Handles) if (Handle.IsValid() && Handle->IsInstanced() && Handle->GetInstanceComponent()) Keys.AddUnique(SurveyInstanceKey(*Handle));
            if (Keys.Num() == 0) { Unresolved(Id, TEXT("lightweight_instance_unresolvable")); continue; }
            for (const FSurveyInstanceKey& Key : Keys) ExcludedInstances.Add(Key);
            AFGBuildable* Temporary = Lightweight->FindTemporaryByBuildableClassAndIndex(Match->Key, Index);
            if (Temporary) Query.AddIgnoredActor(Temporary);
            IgnoredActorIds.Add(MakeShared<FJsonValueString>(Id));
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("id"), Id); Entry->SetStringField(TEXT("classPath"), ClassPath); Entry->SetStringField(TEXT("className"), Match->Key.Get()->GetName());
            Entry->SetNumberField(TEXT("lightweightIndex"), Index); Entry->SetNumberField(TEXT("instanceCount"), Keys.Num());
            const FVector Location = Data.Transform.GetLocation(); const FRotator InstanceRotation = Data.Transform.Rotator();
            const TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>(); LocationJson->SetNumberField(TEXT("x"), Location.X); LocationJson->SetNumberField(TEXT("y"), Location.Y); LocationJson->SetNumberField(TEXT("z"), Location.Z); Entry->SetObjectField(TEXT("location"), LocationJson);
            const TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>(); RotationJson->SetNumberField(TEXT("yaw"), InstanceRotation.Yaw); RotationJson->SetNumberField(TEXT("pitch"), InstanceRotation.Pitch); RotationJson->SetNumberField(TEXT("roll"), InstanceRotation.Roll); Entry->SetObjectField(TEXT("rotation"), RotationJson);
            if (Temporary) Entry->SetStringField(TEXT("temporaryActorId"), Temporary->GetName());
            IgnoredInstances.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    // Stable identity of a trace or overlap hit. Abstract-instanced geometry is
    // reported per instance: a lightweight buildable as lw|<classPath>|<index>,
    // any other instance by the actor that owns it. An instanced hit the manager
    // cannot resolve keeps the manager's actor name and is marked unresolved; it
    // never masks an overlap and is never treated as excluded.
    struct FSurveyIdentity { FString Id; FString ClassName; FString ActorId; FString ActorClassName; FString ClassPath; int32 LightweightIndex = INDEX_NONE; bool Instanced = false; bool Resolved = true; bool Excluded = false; };
    TMap<FSurveyInstanceKey, TPair<UClass*, int32>> LightweightByInstance; bool LightweightByInstanceBuilt = false;
    auto Identify = [&](AActor* Actor, AAbstractInstanceManager* Manager, bool HandleResolved, const FInstanceHandle& Handle)
    {
        FSurveyIdentity Identity;
        if (!Actor) { Identity.Resolved = false; return Identity; }
        Identity.ActorId = Actor->GetName(); Identity.ActorClassName = Actor->GetClass()->GetName(); Identity.Id = Identity.ActorId; Identity.ClassName = Identity.ActorClassName;
        if (!Manager) return Identity;
        Identity.Instanced = true;
        if (!HandleResolved || !Handle.IsInstanced() || !Handle.GetInstanceComponent()) { Identity.Resolved = false; return Identity; }
        const FSurveyInstanceKey Key = SurveyInstanceKey(Handle);
        Identity.Excluded = ExcludedInstances.Contains(Key);
        if (!LightweightByInstanceBuilt)
        {
            LightweightByInstanceBuilt = true;
            if (Lightweight) for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Pair : Lightweight->GetAllLightweightBuildableInstances())
            {
                UClass* Class = Pair.Key.Get(); if (!Class) continue;
                for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
                {
                    const FRuntimeBuildableInstanceData& Data = Pair.Value[Index]; if (!Data.IsValid()) continue;
                    for (const FInstanceOwnerHandlePtr& InstanceHandle : Data.Handles)
                        if (InstanceHandle.IsValid() && InstanceHandle->IsInstanced() && InstanceHandle->GetInstanceComponent()) LightweightByInstance.Add(SurveyInstanceKey(*InstanceHandle), TPair<UClass*, int32>(Class, Index));
                }
            }
        }
        if (const TPair<UClass*, int32>* Found = LightweightByInstance.Find(Key))
        {
            Identity.ClassPath = Found->Key->GetPathName(); Identity.LightweightIndex = Found->Value; Identity.ClassName = Found->Key->GetName();
            Identity.Id = FString::Printf(TEXT("lw|%s|%d"), *Identity.ClassPath, Found->Value);
            return Identity;
        }
        AActor* Owner = Handle.GetOwner();
        if (Owner && Owner != Manager && !Owner->IsA<AFGLightweightBuildableSubsystem>()) { Identity.Id = Owner->GetName(); Identity.ClassName = Owner->GetClass()->GetName(); return Identity; }
        Identity.Resolved = false;
        return Identity;
    };
    auto IdentifyHit = [&](const FHitResult& Hit)
    {
        AActor* Actor = Hit.GetActor(); AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(Actor); FInstanceHandle Handle;
        const bool HandleResolved = Manager && Cast<ULightweightCollisionComponent>(Hit.GetComponent()) && Manager->ResolveHit(Hit, Handle);
        return Identify(Actor, Manager, HandleResolved, Handle);
    };
    auto IdentifyOverlap = [&](const FOverlapResult& Overlap)
    {
        AActor* Actor = Overlap.GetActor(); AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(Actor); FInstanceHandle Handle;
        const bool HandleResolved = Manager && Cast<ULightweightCollisionComponent>(Overlap.GetComponent()) && Manager->ResolveOverlap(Overlap, Handle);
        return Identify(Actor, Manager, HandleResolved, Handle);
    };

    // A support trace that first meets an excluded instance cannot skip it by
    // actor; that would skip the shared manager and every instance on it. It
    // re-traces with that collision component ignored and traces each other
    // instance body of the component on its own, keeping the nearest hit that is
    // not excluded. Rounds are bounded; an exhausted trace is reported as
    // incomplete and fails the survey rather than being guessed.
    const int32 MaximumTraceRounds = ExcludedInstances.Num() + 2;
    auto TraceSupport = [&](const FVector& Start, const FVector& End, ECollisionChannel Channel, FHitResult& OutHit, FSurveyIdentity& OutIdentity, bool& OutIncomplete)
    {
        FCollisionQueryParams TraceQuery = Query; TArray<UInstancedStaticMeshComponent*> SplitComponents;
        for (int32 Round = 0; Round < MaximumTraceRounds; ++Round)
        {
            FHitResult Nearest; FSurveyIdentity NearestIdentity; bool NearestFound = false; double NearestDistanceSquared = TNumericLimits<double>::Max();
            for (UInstancedStaticMeshComponent* Component : SplitComponents) for (int32 Body = 0; Body < Component->InstanceBodies.Num(); ++Body)
            {
                const FBodyInstance* Instance = Component->InstanceBodies[Body]; FHitResult BodyHit;
                if (!Instance || !Instance->LineTrace(BodyHit, Start, End, TraceQuery.bTraceComplex)) continue;
                const double DistanceSquared = FVector::DistSquared(Start, BodyHit.ImpactPoint); if (DistanceSquared >= NearestDistanceSquared) continue;
                BodyHit.Component = Component; BodyHit.Item = Body; BodyHit.HitObjectHandle = FActorInstanceHandle(Component->GetOwner());
                const FSurveyIdentity Identity = IdentifyHit(BodyHit); if (Identity.Excluded) continue;
                Nearest = BodyHit; NearestIdentity = Identity; NearestDistanceSquared = DistanceSquared; NearestFound = true;
            }
            FHitResult Hit;
            if (World->LineTraceSingleByChannel(Hit, Start, End, Channel, TraceQuery) && FVector::DistSquared(Start, Hit.ImpactPoint) < NearestDistanceSquared)
            {
                const FSurveyIdentity Identity = IdentifyHit(Hit);
                if (!Identity.Excluded) { OutHit = Hit; OutIdentity = Identity; return true; }
                ++IgnoredTraceHits;
                UInstancedStaticMeshComponent* Component = Cast<UInstancedStaticMeshComponent>(Hit.GetComponent());
                if (!Component || SplitComponents.Contains(Component)) break;
                TraceQuery.AddIgnoredComponent(Component); SplitComponents.Add(Component);
                continue;
            }
            if (NearestFound) { OutHit = Nearest; OutIdentity = NearestIdentity; return true; }
            return false;
        }
        OutIncomplete = true;
        return false;
    };

    TMap<FString, int32> SupportIndices; TArray<TSharedPtr<FJsonValue>> SupportIds;
    for (int32 Xi = 0; Xi <= XSamples; ++Xi) for (int32 Yi = 0; Yi <= YSamples; ++Yi)
    {
        const double LocalX = -LengthCm * 0.5 + LengthCm * Xi / XSamples; const double LocalY = -WidthCm * 0.5 + WidthCm * Yi / YSamples;
        const FVector SamplePoint = RequestedPosition + Forward * LocalX + Right * LocalY; FHitResult Hit; FSurveyIdentity Identity; bool Incomplete = false;
        const FVector TraceStart(SamplePoint.X, SamplePoint.Y, TopZ + 5000.0), TraceEnd(SamplePoint.X, SamplePoint.Y, BottomZ - 10000.0);
        bool Found = TraceSupport(TraceStart, TraceEnd, ECC_WorldStatic, Hit, Identity, Incomplete);
        if (!Found && !Incomplete) Found = TraceSupport(TraceStart, TraceEnd, ECC_Visibility, Hit, Identity, Incomplete);
        const TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>(); Sample->SetNumberField(TEXT("x"), SamplePoint.X); Sample->SetNumberField(TEXT("y"), SamplePoint.Y); Sample->SetBoolField(TEXT("found"), Found);
        if (Found)
        {
            const double SurfaceZ = Hit.ImpactPoint.Z; MinimumSurfaceZ = FMath::Min(MinimumSurfaceZ, SurfaceZ); MaximumSurfaceZ = FMath::Max(MaximumSurfaceZ, SurfaceZ);
            const bool PenetratesTop = SurfaceZ > TopZ + SupportToleranceCm; const bool IntersectsVolume = SurfaceZ > BottomZ + SupportToleranceCm && SurfaceZ < TopZ - SupportToleranceCm;
            if (PenetratesTop || IntersectsVolume) ++PenetratingSamples;
            Sample->SetNumberField(TEXT("surfaceZ"), SurfaceZ); Sample->SetBoolField(TEXT("penetratesTop"), PenetratesTop); Sample->SetBoolField(TEXT("intersectsVolume"), IntersectsVolume);
            Sample->SetStringField(TEXT("actorClass"), Identity.ActorClassName); Sample->SetNumberField(TEXT("normalZ"), Hit.ImpactNormal.Z);
            if (Identity.Resolved)
            {
                const int32* Existing = SupportIndices.Find(Identity.Id);
                const int32 SupportIndex = Existing ? *Existing : SupportIndices.Add(Identity.Id, SupportIds.Add(MakeShared<FJsonValueString>(Identity.Id)));
                Sample->SetNumberField(TEXT("supportIndex"), SupportIndex);
            }
            else if (Identity.Instanced) Sample->SetBoolField(TEXT("instanceResolved"), false);
        }
        else
        {
            ++MissingSupportSamples;
            if (Incomplete) { ++IncompleteSamples; Sample->SetBoolField(TEXT("incomplete"), true); }
        }
        Samples.Add(MakeShared<FJsonValueObject>(Sample));
    }
    TArray<FOverlapResult> Overlaps; const FVector Extent(LengthCm * 0.5, WidthCm * 0.5, HeightCm * 0.5);
    World->OverlapMultiByObjectType(Overlaps, RequestedPosition, Rotation.Quaternion(), FCollisionObjectQueryParams::AllObjects, FCollisionShape::MakeBox(Extent), Query);
    TArray<TSharedPtr<FJsonValue>> OverlapActors; TSet<FString> SeenOverlaps;
    for (const FOverlapResult& Overlap : Overlaps) if (Overlap.GetActor())
    {
        const FSurveyIdentity Identity = IdentifyOverlap(Overlap);
        if (Identity.Excluded) { ++IgnoredOverlaps; continue; }
        // Only the exact structure or instance that supported a sample is exempt.
        if (Identity.Resolved && SupportIndices.Contains(Identity.Id)) continue;
        if (SeenOverlaps.Contains(Identity.Id)) continue;
        SeenOverlaps.Add(Identity.Id);
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("id"), Identity.Id); Entry->SetStringField(TEXT("className"), Identity.ClassName);
        Entry->SetStringField(TEXT("actorId"), Identity.ActorId); Entry->SetStringField(TEXT("actorClassName"), Identity.ActorClassName);
        if (Identity.Instanced) Entry->SetBoolField(TEXT("instanceResolved"), Identity.Resolved);
        if (Identity.LightweightIndex != INDEX_NONE) { Entry->SetBoolField(TEXT("lightweight"), true); Entry->SetStringField(TEXT("classPath"), Identity.ClassPath); Entry->SetNumberField(TEXT("lightweightIndex"), Identity.LightweightIndex); }
        OverlapActors.Add(MakeShared<FJsonValueObject>(Entry));
    }
    const int32 TotalSupportSamples = Samples.Num(); const int32 FoundSupportSamples = TotalSupportSamples - MissingSupportSamples;
    const double TerrainCoverage = TotalSupportSamples > 0 ? static_cast<double>(FoundSupportSamples) / static_cast<double>(TotalSupportSamples) : 0.0;
    const bool TerrainObserved = FoundSupportSamples > 0 && TerrainCoverage >= 0.75; const bool TerrainSafe = TerrainObserved && PenetratingSamples == 0; const bool CollisionFree = OverlapActors.IsEmpty();
    const bool TracesComplete = IncompleteSamples == 0;
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("valid"), WithinSafetyRadius && TracesComplete && TerrainObserved && TerrainSafe && CollisionFree);
    Body->SetBoolField(TEXT("dryRun"), true);
    Body->SetNumberField(TEXT("distanceFromPlayerCm"), DistanceCm);
    Body->SetNumberField(TEXT("maximumDistanceCm"), GetMaximumPlacementDistanceCm());
    Body->SetBoolField(TEXT("terrainObserved"), TerrainObserved); Body->SetNumberField(TEXT("terrainCoverage"), TerrainCoverage); Body->SetNumberField(TEXT("foundSupportSamples"), FoundSupportSamples);
    Body->SetBoolField(TEXT("terrainSafe"), TerrainSafe); Body->SetBoolField(TEXT("collisionFree"), CollisionFree);
    Body->SetNumberField(TEXT("bottomZ"), BottomZ); Body->SetNumberField(TEXT("topZ"), TopZ); Body->SetNumberField(TEXT("penetratingSamples"), PenetratingSamples); Body->SetNumberField(TEXT("missingSupportSamples"), MissingSupportSamples);
    Body->SetNumberField(TEXT("minimumSurfaceZ"), MinimumSurfaceZ == TNumericLimits<double>::Max() ? 0.0 : MinimumSurfaceZ); Body->SetNumberField(TEXT("maximumSurfaceZ"), MaximumSurfaceZ == -TNumericLimits<double>::Max() ? 0.0 : MaximumSurfaceZ);
    Body->SetNumberField(TEXT("surfaceVariationCm"), MinimumSurfaceZ == TNumericLimits<double>::Max() ? 0.0 : MaximumSurfaceZ - MinimumSurfaceZ); if (!SummaryOnly) Body->SetArrayField(TEXT("samples"), Samples); Body->SetArrayField(TEXT("overlaps"), OverlapActors);
    Body->SetNumberField(TEXT("sampleCount"), TotalSupportSamples); Body->SetNumberField(TEXT("sampleSpacingCm"), SampleSpacingCm); Body->SetNumberField(TEXT("requestedSampleSpacingCm"), RequestedSampleSpacingCm);
    Body->SetBoolField(TEXT("samplesCapped"), SamplesCapped); Body->SetBoolField(TEXT("summaryOnly"), SummaryOnly); Body->SetNumberField(TEXT("maximumSamples"), MaximumSurveySamples);
    Body->SetArrayField(TEXT("ignoredActorIds"), IgnoredActorIds); Body->SetArrayField(TEXT("unresolvedIgnoreActorIds"), UnresolvedIgnoreActorIds);
    // instance_v1: samples and overlaps carry per-instance identities, support
    // exempts only the exact supporting structure, and lw| exclusions are exact.
    Body->SetStringField(TEXT("identityModel"), TEXT("instance_v1"));
    Body->SetArrayField(TEXT("supportIds"), SupportIds); Body->SetArrayField(TEXT("ignoredInstances"), IgnoredInstances); Body->SetArrayField(TEXT("unresolvedIgnores"), UnresolvedIgnores);
    Body->SetNumberField(TEXT("ignoredTraceHits"), IgnoredTraceHits); Body->SetNumberField(TEXT("ignoredOverlaps"), IgnoredOverlaps); Body->SetNumberField(TEXT("incompleteSamples"), IncompleteSamples);
    Body->SetStringField(TEXT("reason"), !WithinSafetyRadius ? TEXT("outside_safety_radius") : !TracesComplete ? TEXT("instance_exclusion_incomplete") : !TerrainObserved ? TEXT("terrain_not_streamed_or_observed") : !TerrainSafe ? TEXT("terrain_penetration") : !CollisionFree ? TEXT("occupied_volume") : TEXT("site_clear"));
    OnComplete(JsonResponse(Body));
    return true;
}

namespace
{
    /** Points one column-probe request may carry, and its vertical range. */
    constexpr int32 MaximumColumnProbePoints = 256;
    constexpr double MaximumColumnProbeRangeCm = 200000.0;
    /** Surfaces one trace direction records per point before it reports the column as truncated. */
    constexpr int32 MaximumColumnSurfacesPerPass = 64;
    /** Trace attempts one direction may spend per point (surfaces, steps through solids, and skipped buildables). */
    constexpr int32 MaximumColumnTracesPerPass = 256;

    struct FColumnSurface
    {
        double Z = 0.0;
        /** True when the face points up (met by a downward trace): solid lies below it. False for an underside. */
        bool bFacesUp = true;
        FString Kind; FString ActorId; FString ActorClassName; double NormalZ = 0.0;
        /** destructible | deposit | foliage when the player can remove what was hit, otherwise empty. */
        FString Removable;
    };

    const TCHAR* ColumnSurfaceKind(const AActor* Actor)
    {
        if (!Actor) return TEXT("terrain");
        if (Actor->IsA<AFGBuildable>() || Actor->IsA<AAbstractInstanceManager>()) return TEXT("buildable");
        if (Actor->GetClass()->GetName().Contains(TEXT("Landscape"))) return TEXT("landscape");
        return TEXT("terrain");
    }

    /**
     * What the player can clear: nobelisk-destructible actors (rocks, gas pillars), resource deposits (rubble that is
     * mined away), and foliage whose type the game's foliage-removal subsystem reports removable (chainsaw). Foliage
     * types are looked up once per component.
     */
    const TCHAR* ColumnRemovableKind(const FHitResult& Hit, TMap<const UPrimitiveComponent*, bool>& FoliageRemovable)
    {
        const AActor* Actor = Hit.GetActor();
        if (!Actor) return nullptr;
        if (Actor->IsA<AFGDestructibleActor>()) return TEXT("destructible");
        if (Actor->IsA<AFGResourceDeposit>()) return TEXT("deposit");
        const AInstancedFoliageActor* Foliage = Cast<AInstancedFoliageActor>(Actor);
        const UPrimitiveComponent* Component = Hit.GetComponent();
        if (!Foliage || !Component) return nullptr;
        if (const bool* Known = FoliageRemovable.Find(Component)) return *Known ? TEXT("foliage") : nullptr;
        bool bRemovable = false;
        for (const TPair<UFoliageType*, TUniqueObj<FFoliageInfo>>& Pair : Foliage->GetFoliageInfos())
            if (Pair.Key && Pair.Value->GetComponent() == Component) { bRemovable = AFGFoliageRemovalSubsystem::IsRemovable(Pair.Key); break; }
        FoliageRemovable.Add(Component, bRemovable);
        return bRemovable ? TEXT("foliage") : nullptr;
    }
}

/**
 * POST /v1/terrain/column-probe: read-only vertical structure of world collision at caller-chosen (x, y) points.
 * Each column is traced downward from zTop and upward from zBottom, restarting just past every surface met, so
 * the downward pass records faces that point up (tops of solids) and the upward pass records faces that point
 * down (undersides: ceilings, overhangs, arches). A trace that starts inside a solid that reports the start as a
 * hit steps on in growing increments and is counted as an inside step. Walking the merged surfaces from the top,
 * each top enters a solid and each underside leaves one, which separates overhanging rock (an underside with open
 * space below it) from rock that sits in the ground (its underside, if any, lies inside another solid). The bridge
 * reports the layers, the open gaps between them, and any water volume spanning the column; it applies no
 * placement policy.
 */
bool FMKIBridgeModule::HandleColumnProbe(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
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

    const TArray<TSharedPtr<FJsonValue>>* PointValues = nullptr;
    if (!Json->TryGetArrayField(TEXT("points"), PointValues) || !PointValues || PointValues->Num() == 0 || PointValues->Num() > MaximumColumnProbePoints)
    {
        OnComplete(ErrorResponse(TEXT("invalid_points"), FString::Printf(TEXT("points must hold 1 to %d {x, y} objects."), MaximumColumnProbePoints), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    TArray<FVector2D> Points;
    for (const TSharedPtr<FJsonValue>& Value : *PointValues)
    {
        const TSharedPtr<FJsonObject>* Point = nullptr; double PX = 0.0, PY = 0.0;
        if (!Value.IsValid() || !Value->TryGetObject(Point) || !Point || !Point->IsValid() || !(*Point)->TryGetNumberField(TEXT("x"), PX) || !(*Point)->TryGetNumberField(TEXT("y"), PY) || !FMath::IsFinite(PX) || !FMath::IsFinite(PY))
        {
            OnComplete(ErrorResponse(TEXT("invalid_points"), TEXT("Every point needs finite numeric x and y."), EHttpServerResponseCodes::BadRequest));
            return true;
        }
        Points.Add(FVector2D(PX, PY));
    }
    double ZTop = 0.0, ZBottom = 0.0;
    if (!Json->TryGetNumberField(TEXT("zTop"), ZTop) || !Json->TryGetNumberField(TEXT("zBottom"), ZBottom) || !FMath::IsFinite(ZTop) || !FMath::IsFinite(ZBottom) || ZTop <= ZBottom || ZTop - ZBottom > MaximumColumnProbeRangeCm)
    {
        OnComplete(ErrorResponse(TEXT("invalid_range"), FString::Printf(TEXT("Finite zTop above zBottom, at most %.0f cm apart, are required."), MaximumColumnProbeRangeCm), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    FString ChannelName = TEXT("worldStatic"); Json->TryGetStringField(TEXT("channel"), ChannelName);
    ECollisionChannel Channel = ECC_WorldStatic;
    if (ChannelName == TEXT("visibility")) Channel = ECC_Visibility;
    else if (ChannelName != TEXT("worldStatic"))
    {
        OnComplete(ErrorResponse(TEXT("invalid_channel"), TEXT("channel must be worldStatic or visibility."), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    bool bIncludeBuildables = false; Json->TryGetBoolField(TEXT("includeBuildables"), bIncludeBuildables);
    bool bTraceComplex = false; Json->TryGetBoolField(TEXT("traceComplex"), bTraceComplex);
    bool bIncludeSurfaces = true; Json->TryGetBoolField(TEXT("includeSurfaces"), bIncludeSurfaces);
    bool bSkipRemovable = false; Json->TryGetBoolField(TEXT("skipRemovable"), bSkipRemovable);
    TMap<const UPrimitiveComponent*, bool> FoliageRemovable;
    TArray<FString> ExcludeIds; const TArray<TSharedPtr<FJsonValue>>* ExcludeValues = nullptr;
    if (Json->TryGetArrayField(TEXT("excludeIds"), ExcludeValues) && ExcludeValues)
    {
        bool Valid = ExcludeValues->Num() <= 64;
        for (const TSharedPtr<FJsonValue>& Value : *ExcludeValues) { FString Id; if (!Value.IsValid() || !Value->TryGetString(Id) || Id.IsEmpty()) Valid = false; else ExcludeIds.AddUnique(Id); }
        if (!Valid) { OnComplete(ErrorResponse(TEXT("invalid_exclude_list"), TEXT("excludeIds must be at most 64 non-empty actor ID strings."), EHttpServerResponseCodes::BadRequest)); return true; }
    }

    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* PlayerPawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    if (!PlayerPawn)
    {
        OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("No local player is available for bounded observation."), EHttpServerResponseCodes::ServiceUnavail));
        return true;
    }

    FCollisionQueryParams Query(SCENE_QUERY_STAT(MKIBridgeColumnProbe), bTraceComplex); Query.AddIgnoredActor(PlayerPawn);
    TArray<TSharedPtr<FJsonValue>> ExcludedIds, UnresolvedExcludeIds;
    if (ExcludeIds.Num() > 0)
    {
        TSet<FString> Remaining(ExcludeIds);
        for (TActorIterator<AActor> It(World); It && Remaining.Num() > 0; ++It)
            if (Remaining.Remove(It->GetName()) > 0) { Query.AddIgnoredActor(*It); ExcludedIds.Add(MakeShared<FJsonValueString>(It->GetName())); }
        for (const FString& Id : Remaining) UnresolvedExcludeIds.Add(MakeShared<FJsonValueString>(Id));
    }

    // Water volumes are not blocking geometry; each one whose bounds reach the requested range is tested per point.
    TArray<AFGWaterVolume*> WaterVolumes; TArray<FBox> WaterBounds;
    for (TActorIterator<AFGWaterVolume> It(World); It; ++It)
    {
        const FBox Bounds = It->GetComponentsBoundingBox(true);
        if (!Bounds.IsValid || Bounds.Max.Z < ZBottom || Bounds.Min.Z > ZTop) continue;
        WaterVolumes.Add(*It); WaterBounds.Add(Bounds);
    }

    const FVector PlayerPosition = PlayerPawn->GetActorLocation();
    const double ObservationRadiusCm = GetMaximumObservationDistanceCm();
    int32 TotalTraces = 0, TruncatedPoints = 0;
    TArray<TSharedPtr<FJsonValue>> Columns;
    for (const FVector2D& Point : Points)
    {
        const TSharedRef<FJsonObject> Column = MakeShared<FJsonObject>();
        Column->SetNumberField(TEXT("x"), Point.X); Column->SetNumberField(TEXT("y"), Point.Y);
        if (FVector2D::Distance(Point, FVector2D(PlayerPosition.X, PlayerPosition.Y)) > ObservationRadiusCm)
        {
            Column->SetBoolField(TEXT("ok"), false); Column->SetStringField(TEXT("reason"), TEXT("outside_observation_radius"));
            Columns.Add(MakeShared<FJsonValueObject>(Column));
            continue;
        }

        FCollisionQueryParams ColumnQuery = Query;
        TArray<FColumnSurface> Surfaces; int32 InsideStepsDown = 0, InsideStepsUp = 0, EmbeddedHits = 0, SkippedBuildables = 0, SkippedPawns = 0, SkippedRemovables = 0; bool bTruncated = false, bStartedInside = false;
        // Direction +1 traces downward from zTop; -1 traces upward from zBottom.
        for (const int32 Direction : { 1, -1 })
        {
            double Start = Direction > 0 ? ZTop : ZBottom; const double End = Direction > 0 ? ZBottom : ZTop;
            double Step = 50.0; int32 Recorded = 0;
            for (int32 Attempt = 0; ; ++Attempt)
            {
                if ((Direction > 0 && Start <= End) || (Direction < 0 && Start >= End)) break;
                if (Attempt >= MaximumColumnTracesPerPass || Recorded >= MaximumColumnSurfacesPerPass) { bTruncated = true; break; }
                FHitResult Hit; ++TotalTraces;
                if (!World->LineTraceSingleByChannel(Hit, FVector(Point.X, Point.Y, Start), FVector(Point.X, Point.Y, End), Channel, ColumnQuery)) break;
                AActor* HitActor = Hit.GetActor();
                if (!bIncludeBuildables && HitActor && (HitActor->IsA<AFGBuildable>() || HitActor->IsA<AAbstractInstanceManager>()))
                {
                    // Built structures are not terrain. A shared instance manager is skipped by component, which
                    // covers every buildable instance drawn by that component.
                    if (HitActor->IsA<AAbstractInstanceManager>() && Hit.GetComponent()) ColumnQuery.AddIgnoredComponent(Hit.GetComponent());
                    else ColumnQuery.AddIgnoredActor(HitActor);
                    ++SkippedBuildables;
                    continue;
                }
                // Creatures and other pawns are never terrain.
                if (HitActor && HitActor->IsA<APawn>()) { ColumnQuery.AddIgnoredActor(HitActor); ++SkippedPawns; continue; }
                const TCHAR* RemovableKind = ColumnRemovableKind(Hit, FoliageRemovable);
                if (bSkipRemovable && RemovableKind)
                {
                    // Trace as if cleared. Foliage instances share a component per type, so the whole type is skipped.
                    if (HitActor && HitActor->IsA<AInstancedFoliageActor>() && Hit.GetComponent()) ColumnQuery.AddIgnoredComponent(Hit.GetComponent());
                    else ColumnQuery.AddIgnoredActor(HitActor);
                    ++SkippedRemovables;
                    continue;
                }
                if (Hit.bStartPenetrating || Hit.Distance <= 0.0)
                {
                    // The trace began inside a solid that reports its start; step on until it no longer does. A step
                    // must never jump over another component's face (landscape under a sunk trunk, a rock top under
                    // the landscape): trace once ignoring the solid we are in, and when that finds a face before the
                    // next step would land, record it and continue from there.
                    if (Attempt == 0 && Direction > 0) bStartedInside = true;
                    ++(Direction > 0 ? InsideStepsDown : InsideStepsUp);
                    const double NextStart = Start - Direction * Step;
                    if (UPrimitiveComponent* InsideComponent = Hit.GetComponent())
                    {
                        FCollisionQueryParams Beyond = ColumnQuery; Beyond.AddIgnoredComponent(InsideComponent);
                        FHitResult Embedded; ++TotalTraces;
                        AActor* EmbeddedActor = nullptr;
                        if (World->LineTraceSingleByChannel(Embedded, FVector(Point.X, Point.Y, Start), FVector(Point.X, Point.Y, End), Channel, Beyond)
                            && !Embedded.bStartPenetrating && Embedded.Distance > 0.0 && (EmbeddedActor = Embedded.GetActor()) != nullptr
                            && (bIncludeBuildables || (!EmbeddedActor->IsA<AFGBuildable>() && !EmbeddedActor->IsA<AAbstractInstanceManager>()))
                            && !EmbeddedActor->IsA<APawn>()
                            && (Direction > 0 ? Embedded.ImpactPoint.Z >= NextStart : Embedded.ImpactPoint.Z <= NextStart))
                        {
                            const TCHAR* EmbeddedRemovable = ColumnRemovableKind(Embedded, FoliageRemovable);
                            if (!(bSkipRemovable && EmbeddedRemovable))
                            {
                                FColumnSurface Surface; Surface.Z = Embedded.ImpactPoint.Z; Surface.bFacesUp = Direction > 0; Surface.Kind = ColumnSurfaceKind(EmbeddedActor);
                                Surface.ActorId = EmbeddedActor->GetName(); Surface.ActorClassName = EmbeddedActor->GetClass()->GetName(); Surface.NormalZ = Embedded.ImpactNormal.Z;
                                if (EmbeddedRemovable) Surface.Removable = EmbeddedRemovable;
                                Surfaces.Add(Surface); ++Recorded; ++EmbeddedHits;
                                Start = Surface.Z - Direction * 1.0; Step = 50.0;
                                continue;
                            }
                        }
                    }
                    Start = NextStart; Step = FMath::Min(Step * 2.0, 400.0);
                    continue;
                }
                Step = 50.0;
                FColumnSurface Surface; Surface.Z = Hit.ImpactPoint.Z; Surface.bFacesUp = Direction > 0; Surface.Kind = ColumnSurfaceKind(HitActor);
                Surface.ActorId = HitActor ? HitActor->GetName() : FString(); Surface.ActorClassName = HitActor ? HitActor->GetClass()->GetName() : FString(); Surface.NormalZ = Hit.ImpactNormal.Z;
                if (RemovableKind) Surface.Removable = RemovableKind;
                Surfaces.Add(Surface); ++Recorded;
                Start = Surface.Z - Direction * 1.0;
            }
        }
        if (bTruncated) ++TruncatedPoints;

        // Merge from the top: an upward face enters solid, an underside leaves it. The starting depth is whatever
        // keeps the walk from leaving more solids than it entered (the range began inside rock). Landscape is a
        // heightfield that also answers upward traces from below; it has no underside (everything under it is
        // ground), so a landscape hit from below is reported but never leaves a solid.
        auto LeavesSolid = [](const FColumnSurface& Surface) { return !Surface.bFacesUp && Surface.Kind != TEXT("landscape"); };
        Surfaces.Sort([](const FColumnSurface& A, const FColumnSurface& B) { return A.Z > B.Z || (A.Z == B.Z && A.bFacesUp && !B.bFacesUp); });
        int32 Depth = 0, MinimumDepth = 0;
        for (const FColumnSurface& Surface : Surfaces) { if (Surface.bFacesUp) ++Depth; else if (LeavesSolid(Surface)) --Depth; MinimumDepth = FMath::Min(MinimumDepth, Depth); }
        Depth = FMath::Max(-MinimumDepth, bStartedInside ? 1 : 0);
        const bool bRangeStartsInside = Depth > 0;
        TArray<TSharedPtr<FJsonValue>> Layers, Gaps, SurfaceValues;
        TSharedPtr<FJsonObject> OpenLayer;
        double GapTop = ZTop; bool bGapOpen = Depth == 0; int32 EmbeddedSurfaces = 0;
        auto CloseGap = [&](double Bottom, const FColumnSurface* Floor)
        {
            const TSharedRef<FJsonObject> Gap = MakeShared<FJsonObject>();
            Gap->SetNumberField(TEXT("top"), GapTop); Gap->SetNumberField(TEXT("bottom"), Bottom); Gap->SetNumberField(TEXT("heightCm"), GapTop - Bottom);
            Gap->SetBoolField(TEXT("openAbove"), GapTop >= ZTop); Gap->SetBoolField(TEXT("openBelow"), Floor == nullptr);
            if (Floor) { Gap->SetStringField(TEXT("floorKind"), Floor->Kind); Gap->SetStringField(TEXT("floorActorClass"), Floor->ActorClassName); Gap->SetNumberField(TEXT("floorNormalZ"), Floor->NormalZ); }
            Gaps.Add(MakeShared<FJsonValueObject>(Gap));
        };
        if (Depth > 0)
        {
            OpenLayer = MakeShared<FJsonObject>(); OpenLayer->SetNumberField(TEXT("top"), ZTop); OpenLayer->SetBoolField(TEXT("topBeyondRange"), true);
        }
        for (const FColumnSurface& Surface : Surfaces)
        {
            if (bIncludeSurfaces)
            {
                const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetNumberField(TEXT("z"), Surface.Z); Entry->SetStringField(TEXT("facing"), Surface.bFacesUp ? TEXT("up") : TEXT("down"));
                Entry->SetStringField(TEXT("kind"), Surface.Kind); Entry->SetStringField(TEXT("actorId"), Surface.ActorId); Entry->SetStringField(TEXT("actorClass"), Surface.ActorClassName); Entry->SetNumberField(TEXT("normalZ"), Surface.NormalZ);
                if (!Surface.Removable.IsEmpty()) Entry->SetStringField(TEXT("removable"), Surface.Removable);
                SurfaceValues.Add(MakeShared<FJsonValueObject>(Entry));
            }
            if (Surface.bFacesUp)
            {
                if (Depth == 0)
                {
                    if (bGapOpen) CloseGap(Surface.Z, &Surface);
                    bGapOpen = false;
                    OpenLayer = MakeShared<FJsonObject>(); OpenLayer->SetNumberField(TEXT("top"), Surface.Z); OpenLayer->SetBoolField(TEXT("topBeyondRange"), false);
                    OpenLayer->SetStringField(TEXT("topKind"), Surface.Kind); OpenLayer->SetStringField(TEXT("topActorClass"), Surface.ActorClassName); OpenLayer->SetNumberField(TEXT("topNormalZ"), Surface.NormalZ);
                }
                else ++EmbeddedSurfaces;
                ++Depth;
            }
            else if (!LeavesSolid(Surface)) continue;
            else if (Depth > 0)
            {
                --Depth;
                if (Depth == 0 && OpenLayer.IsValid())
                {
                    OpenLayer->SetNumberField(TEXT("bottom"), Surface.Z); OpenLayer->SetBoolField(TEXT("bottomBeyondRange"), false);
                    OpenLayer->SetStringField(TEXT("bottomKind"), Surface.Kind); OpenLayer->SetStringField(TEXT("bottomActorClass"), Surface.ActorClassName);
                    OpenLayer->SetNumberField(TEXT("thicknessCm"), OpenLayer->GetNumberField(TEXT("top")) - Surface.Z);
                    Layers.Add(MakeShared<FJsonValueObject>(OpenLayer.ToSharedRef())); OpenLayer.Reset();
                    GapTop = Surface.Z; bGapOpen = true;
                }
                else ++EmbeddedSurfaces;
            }
        }
        if (OpenLayer.IsValid())
        {
            OpenLayer->SetNumberField(TEXT("bottom"), ZBottom); OpenLayer->SetBoolField(TEXT("bottomBeyondRange"), true);
            OpenLayer->SetNumberField(TEXT("thicknessCm"), OpenLayer->GetNumberField(TEXT("top")) - ZBottom);
            Layers.Add(MakeShared<FJsonValueObject>(OpenLayer.ToSharedRef()));
        }
        else if (bGapOpen) CloseGap(ZBottom, nullptr);

        // Raw open intervals, without any assumption about what lies under the landscape: open space runs from zTop
        // (unless the downward trace started inside a solid) and from every underside, including a landscape hit
        // from below, down to the next upward face. Each end is tagged with what bounds it. Space under a landscape
        // can be a cave roofed by the landscape itself or ground with buried rock; telling them apart needs
        // connectivity to known open air, which the caller decides.
        TArray<TSharedPtr<FJsonValue>> OpenIntervals;
        {
            bool bAir = !bStartedInside; double AirTop = ZTop; const FColumnSurface* AirRoof = nullptr;
            auto AddInterval = [&](double Bottom, const FColumnSurface* Floor)
            {
                const TSharedRef<FJsonObject> Interval = MakeShared<FJsonObject>();
                Interval->SetNumberField(TEXT("top"), AirTop); Interval->SetNumberField(TEXT("bottom"), Bottom); Interval->SetNumberField(TEXT("heightCm"), AirTop - Bottom);
                Interval->SetStringField(TEXT("topBound"), AirRoof ? TEXT("underside") : TEXT("range"));
                if (AirRoof) { Interval->SetStringField(TEXT("topKind"), AirRoof->Kind); Interval->SetStringField(TEXT("topActorClass"), AirRoof->ActorClassName); if (!AirRoof->Removable.IsEmpty()) Interval->SetStringField(TEXT("topRemovable"), AirRoof->Removable); }
                Interval->SetStringField(TEXT("bottomBound"), Floor ? TEXT("floor") : TEXT("range"));
                if (Floor) { Interval->SetStringField(TEXT("bottomKind"), Floor->Kind); Interval->SetStringField(TEXT("bottomActorClass"), Floor->ActorClassName); Interval->SetNumberField(TEXT("bottomNormalZ"), Floor->NormalZ); if (!Floor->Removable.IsEmpty()) Interval->SetStringField(TEXT("bottomRemovable"), Floor->Removable); }
                Interval->SetBoolField(TEXT("underLandscape"), AirRoof && AirRoof->Kind == TEXT("landscape"));
                OpenIntervals.Add(MakeShared<FJsonValueObject>(Interval));
            };
            for (const FColumnSurface& Surface : Surfaces)
            {
                if (Surface.bFacesUp) { if (bAir && AirTop > Surface.Z) AddInterval(Surface.Z, &Surface); bAir = false; }
                else if (!bAir) { bAir = true; AirTop = Surface.Z; AirRoof = &Surface; }
            }
            if (bAir && AirTop > ZBottom) AddInterval(ZBottom, nullptr);
        }

        // Water: every volume containing part of the column, with its surface and bottom found by bisection.
        TArray<TSharedPtr<FJsonValue>> Water; double WaterSurface = -TNumericLimits<double>::Max();
        for (int32 Index = 0; Index < WaterVolumes.Num(); ++Index)
        {
            const FBox& Bounds = WaterBounds[Index]; AFGWaterVolume* Volume = WaterVolumes[Index];
            if (Point.X < Bounds.Min.X || Point.X > Bounds.Max.X || Point.Y < Bounds.Min.Y || Point.Y > Bounds.Max.Y) continue;
            const double Low = FMath::Max(Bounds.Min.Z, ZBottom), High = FMath::Min(Bounds.Max.Z, ZTop);
            double Inside = 0.0; bool bFound = false;
            for (int32 Sample = 0; Sample <= 16 && !bFound; ++Sample)
            {
                const double Z = High - (High - Low) * Sample / 16.0;
                if (Volume->EncompassesPoint(FVector(Point.X, Point.Y, Z))) { Inside = Z; bFound = true; }
            }
            if (!bFound) continue;
            double Wet = Inside, Dry = Bounds.Max.Z + 1.0;
            for (int32 Iteration = 0; Iteration < 24 && Dry - Wet > 0.5; ++Iteration) { const double Mid = (Wet + Dry) * 0.5; if (Volume->EncompassesPoint(FVector(Point.X, Point.Y, Mid))) Wet = Mid; else Dry = Mid; }
            const double Top = Wet;
            Wet = Inside; Dry = Bounds.Min.Z - 1.0;
            for (int32 Iteration = 0; Iteration < 24 && Wet - Dry > 0.5; ++Iteration) { const double Mid = (Wet + Dry) * 0.5; if (Volume->EncompassesPoint(FVector(Point.X, Point.Y, Mid))) Wet = Mid; else Dry = Mid; }
            const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("top"), Top); Entry->SetNumberField(TEXT("bottom"), Wet); Entry->SetStringField(TEXT("volumeId"), Volume->GetName());
            Water.Add(MakeShared<FJsonValueObject>(Entry)); WaterSurface = FMath::Max(WaterSurface, Top);
        }

        Column->SetBoolField(TEXT("ok"), true);
        Column->SetArrayField(TEXT("layers"), Layers); Column->SetArrayField(TEXT("gaps"), Gaps); Column->SetArrayField(TEXT("water"), Water);
        Column->SetArrayField(TEXT("openIntervals"), OpenIntervals);
        if (Water.Num() > 0) Column->SetNumberField(TEXT("waterSurface"), WaterSurface);
        Column->SetBoolField(TEXT("startedInsideSolid"), bRangeStartsInside);
        Column->SetNumberField(TEXT("embeddedSurfaces"), EmbeddedSurfaces); Column->SetNumberField(TEXT("skippedBuildableHits"), SkippedBuildables);
        Column->SetNumberField(TEXT("skippedPawnHits"), SkippedPawns); Column->SetNumberField(TEXT("skippedRemovableHits"), SkippedRemovables);
        Column->SetNumberField(TEXT("insideStepsDown"), InsideStepsDown); Column->SetNumberField(TEXT("insideStepsUp"), InsideStepsUp); Column->SetNumberField(TEXT("embeddedHits"), EmbeddedHits);
        Column->SetBoolField(TEXT("truncated"), bTruncated);
        if (bIncludeSurfaces) Column->SetArrayField(TEXT("surfaces"), SurfaceValues);
        Columns.Add(MakeShared<FJsonValueObject>(Column));
    }

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true); Body->SetBoolField(TEXT("dryRun"), true);
    Body->SetNumberField(TEXT("zTop"), ZTop); Body->SetNumberField(TEXT("zBottom"), ZBottom);
    Body->SetStringField(TEXT("channel"), ChannelName); Body->SetBoolField(TEXT("includeBuildables"), bIncludeBuildables); Body->SetBoolField(TEXT("traceComplex"), bTraceComplex); Body->SetBoolField(TEXT("skipRemovable"), bSkipRemovable);
    Body->SetNumberField(TEXT("maximumPoints"), MaximumColumnProbePoints); Body->SetNumberField(TEXT("maximumRangeCm"), MaximumColumnProbeRangeCm);
    Body->SetNumberField(TEXT("maximumSurfacesPerPass"), MaximumColumnSurfacesPerPass); Body->SetNumberField(TEXT("observationRadiusCm"), ObservationRadiusCm);
    Body->SetNumberField(TEXT("traceCount"), TotalTraces); Body->SetNumberField(TEXT("truncatedPoints"), TruncatedPoints);
    Body->SetArrayField(TEXT("excludedIds"), ExcludedIds); Body->SetArrayField(TEXT("unresolvedExcludeIds"), UnresolvedExcludeIds);
    Body->SetArrayField(TEXT("columns"), Columns);
    OnComplete(JsonResponse(Body));
    return true;
}

namespace
{
    /** Aims one hologram-probe request may carry. */
    constexpr int32 MaximumHologramProbeCases = 256;
    /** Time a probe hologram gets to initialize before it is aimed, in seconds. */
    constexpr float HologramProbeInitializationSeconds = 0.25f;

    bool ReadProbeVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
    {
        const TSharedPtr<FJsonObject>* Value = nullptr; double X = 0.0, Y = 0.0, Z = 0.0;
        if (!Object->TryGetObjectField(Field, Value) || !Value || !Value->IsValid() || !(*Value)->TryGetNumberField(TEXT("x"), X) || !(*Value)->TryGetNumberField(TEXT("y"), Y) || !(*Value)->TryGetNumberField(TEXT("z"), Z)) return false;
        if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z)) return false;
        Out = FVector(X, Y, Z);
        return true;
    }

    TSharedRef<FJsonObject> ProbeVectorJson(const FVector& Value)
    {
        const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("x"), Value.X); Out->SetNumberField(TEXT("y"), Value.Y); Out->SetNumberField(TEXT("z"), Value.Z);
        return Out;
    }
}

/**
 * POST /v1/hologram-probe: where the build gun would put a recipe's hologram for each aim, without building anything.
 * One hologram of the recipe is spawned. Each case traces the build gun's channel (TC_BuildGun, BuildGunTraceChannel) from `eye` through
 * `target`, sets the hologram's scroll rotation, and hands the hit to the hologram's own SetHologramLocationAndRotation
 * and then TrySnapToActor (headless, the first alone does not move the hologram onto its snap target). A lightweight hit
 * is handed over as that instance's temporary buildable, as the build gun does. The response reports the hit, the hologram
 * transform, the building it snapped to, and the native construct validity. The hologram is then destroyed; the world,
 * the player's inventory, and the build gun are unchanged (the control rotation is restored).
 */
bool FMKIBridgeModule::HandleHologramProbe(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    FString RequestText;
    if (!Request.Body.IsEmpty()) { FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num()); RequestText = FString(Converted.Length(), Converted.Get()); }
    TSharedPtr<FJsonObject> Json;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestText);
    if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) { OnComplete(ErrorResponse(TEXT("invalid_json"), TEXT("Request body must be a JSON object."), EHttpServerResponseCodes::BadRequest)); return true; }
    FString RecipePath; Json->TryGetStringField(TEXT("recipePath"), RecipePath); RecipePath.TrimStartAndEndInline();
    FString PathCode, PathMessage;
    if (!IsPlaceableAssetPath(RecipePath, PathCode, PathMessage)) { OnComplete(ErrorResponse(PathCode, PathMessage, PathCode == TEXT("companion_unavailable") ? EHttpServerResponseCodes::Conflict : EHttpServerResponseCodes::BadRequest)); return true; }
    const TArray<TSharedPtr<FJsonValue>>* CaseValues = nullptr;
    if (!Json->TryGetArrayField(TEXT("cases"), CaseValues) || !CaseValues || CaseValues->Num() == 0 || CaseValues->Num() > MaximumHologramProbeCases)
    {
        OnComplete(ErrorResponse(TEXT("invalid_cases"), FString::Printf(TEXT("cases must hold 1 to %d {eye, target, scrollRotation?} objects."), MaximumHologramProbeCases), EHttpServerResponseCodes::BadRequest));
        return true;
    }
    struct FProbeCase { FVector Eye; FVector Target; int32 ScrollRotation = 0; };
    TArray<FProbeCase> Cases;
    for (const TSharedPtr<FJsonValue>& Value : *CaseValues)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr; FProbeCase Case; double Rotation = 0.0;
        if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid() || !ReadProbeVector(*Object, TEXT("eye"), Case.Eye) || !ReadProbeVector(*Object, TEXT("target"), Case.Target)
            || ((*Object)->TryGetNumberField(TEXT("scrollRotation"), Rotation) && (!FMath::IsFinite(Rotation) || FMath::Abs(Rotation) > 360.0)) || Case.Eye.Equals(Case.Target, 1.0))
        {
            OnComplete(ErrorResponse(TEXT("invalid_cases"), TEXT("Every case needs finite eye and target {x, y, z} at least 1 cm apart, and an optional scrollRotation within +-360."), EHttpServerResponseCodes::BadRequest));
            return true;
        }
        Case.ScrollRotation = FMath::RoundToInt(Rotation);
        Cases.Add(Case);
    }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    APawn* Pawn = World ? UGameplayStatics::GetPlayerPawn(World, 0) : nullptr;
    APlayerController* Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    AFGCharacterPlayer* Player = Cast<AFGCharacterPlayer>(Pawn);
    if (!World || !Player || !Controller) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }
    for (const FProbeCase& Case : Cases)
        if (FVector::Distance(Case.Eye, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm() || FVector::Distance(Case.Target, Player->GetActorLocation()) > GetMaximumPlacementDistanceCm())
        {
            OnComplete(ErrorResponse(TEXT("outside_safety_radius"), TEXT("Every eye and target must be within the configured safety radius of the player."), EHttpServerResponseCodes::Forbidden));
            return true;
        }
    UClass* RecipeClass = StaticLoadClass(UFGRecipe::StaticClass(), nullptr, *RecipePath);
    if (!RecipeClass) { OnComplete(ErrorResponse(TEXT("asset_unavailable"), TEXT("Recipe asset unavailable."), EHttpServerResponseCodes::ServerError)); return true; }
    AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(RecipeClass, Controller, Cases[0].Target, Player);
    if (!Hologram) { OnComplete(ErrorResponse(TEXT("hologram_spawn_failed"), TEXT("The recipe's hologram could not be spawned."), EHttpServerResponseCodes::ServerError)); return true; }
    // Let the hologram finish initializing (customization data from the game state, attachment points) before it is
    // aimed, as /v1/place does; until then it carries FGCDInitializing and does not snap.
    Hologram->WaitForGamestateApplyCustomizationData();
    FTimerHandle ProbeTimer;
    World->GetTimerManager().SetTimer(ProbeTimer, [WeakWorld = TWeakObjectPtr<UWorld>(World), WeakController = TWeakObjectPtr<APlayerController>(Controller), WeakPlayer = TWeakObjectPtr<AFGCharacterPlayer>(Player), WeakHologram = TWeakObjectPtr<AFGHologram>(Hologram), Ticket = ActiveTicket, Cases, RecipePath, OnComplete]()
    {
        UWorld* World = WeakWorld.Get(); APlayerController* Controller = WeakController.Get(); AFGCharacterPlayer* Player = WeakPlayer.Get(); AFGHologram* Hologram = WeakHologram.Get();
        if (!World || !Controller || !Player || !Hologram || (Ticket && Ticket->bAbandoned))
        {
            if (Hologram) Hologram->Destroy();
            OnComplete(ErrorResponse(TEXT("deferred_target_unavailable"), TEXT("The world, player, or hologram disappeared, or the request was abandoned, before the probe ran."), EHttpServerResponseCodes::Conflict));
            return;
        }
        const FRotator SavedControlRotation = Controller->GetControlRotation();
        FCollisionQueryParams Query(SCENE_QUERY_STAT(MKIBridgeHologramProbe), true);
        Query.AddIgnoredActor(Player);
        Query.AddIgnoredActor(Hologram);
        TArray<AActor*> HologramChildren; Hologram->GetAllChildActors(HologramChildren, true);
        Query.AddIgnoredActors(HologramChildren);
        TArray<TSharedPtr<FJsonValue>> Results;
        // A lightweight hit gets the instance's temporary buildable through the subsystem's own bookkeeping, as the
        // build gun's converter does: an existing temporary is reused, and one spawned here is removed through
        // RemoveTemporaryBuildableForInstanceIndex once every case has run. Spawning and destroying a raw temporary
        // (SpawnTemporaryBuildable + Destroy) left the instance unable to spawn another, so it refused CanDismantle and
        // stopped snapping until the game restarted (live finding, #148).
        struct FProbeTemporary { TSubclassOf<AFGBuildable> Class; int32 Index = INDEX_NONE; AFGBuildable* Buildable = nullptr; bool bSpawned = false; };
        TMap<FString, FProbeTemporary> Temporaries;
        AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World);
        auto TemporaryFor = [World, Lightweight, &Temporaries](const FString& Id) -> AFGBuildable*
        {
            if (const FProbeTemporary* Known = Temporaries.Find(Id)) return Known->Buildable;
            FProbeTemporary Entry;
            const FRuntimeBuildableInstanceData* Record = Lightweight ? FindLightweightRecord(World, Id, Entry.Class, Entry.Index) : nullptr;
            if (Record && Entry.Class)
            {
                Entry.Buildable = Lightweight->FindTemporaryByBuildableClassAndIndex(Entry.Class, Entry.Index);
                if (!Entry.Buildable)
                    if (FInstanceToTemporaryBuildable* Converted = Lightweight->FindOrSpawnBuildableForRuntimeData(Entry.Class, const_cast<FRuntimeBuildableInstanceData*>(Record), Entry.Index, Entry.bSpawned))
                        Entry.Buildable = Converted->Buildable;
            }
            Temporaries.Add(Id, Entry);
            return Entry.Buildable;
        };
        for (int32 Index = 0; Index < Cases.Num(); ++Index)
        {
            const FProbeCase& Case = Cases[Index];
            const FVector Direction = (Case.Target - Case.Eye).GetSafeNormal();
            const FVector TraceEnd = Case.Eye + Direction * FMath::Max(FVector::Distance(Case.Eye, Case.Target) * 1.5, 10000.0);
            FHitResult Hit;
            const bool Blocked = World->LineTraceSingleByChannel(Hit, Case.Eye, TraceEnd, BuildGunTraceChannel(), Query);
            if (!Blocked) { Hit = FHitResult(); Hit.TraceStart = Case.Eye; Hit.TraceEnd = TraceEnd; Hit.Location = TraceEnd; Hit.ImpactPoint = TraceEnd; }
            // The build gun hands holograms a temporary buildable for a lightweight hit (FGLightweightBuildableSubsystem
            // temporaries), so they snap to it as to an actor. Do the same with the exact instance hit, then drop it.
            const TSharedRef<FJsonObject> HitJson = DescribeSupportHit(World, Hit);
            FString HitId; HitJson->TryGetStringField(TEXT("id"), HitId);
            AFGBuildable* TemporaryBuildable = nullptr;
            if (Blocked && HitId.StartsWith(TEXT("lw|"), ESearchCase::CaseSensitive))
            {
                TemporaryBuildable = TemporaryFor(HitId);
                if (TemporaryBuildable)
                {
                    Hit.HitObjectHandle = FActorInstanceHandle(TemporaryBuildable);
                    Hit.Component = Cast<UPrimitiveComponent>(TemporaryBuildable->GetRootComponent());
                }
            }
            HitJson->SetBoolField(TEXT("temporaryForLightweight"), TemporaryBuildable != nullptr);
            Controller->SetControlRotation(Direction.Rotation());
            Hologram->SetScrollRotateValue(Case.ScrollRotation);
            const bool Usable = Blocked && Hologram->IsValidHitResult(Hit);
            if (Usable) Hologram->SetHologramLocationAndRotation(Hit);
            const FTransform AfterSetLocation = Hologram->GetActorTransform();
            // Headless, SetHologramLocationAndRotation records the snap target but leaves the hologram at the hit
            // (live finding, #148); the explicit TrySnapToActor that /v1/place also uses then applies the native snap.
            const bool TrySnapped = Usable && Hit.GetActor() && Hologram->TrySnapToActor(Hit);
            Hologram->ValidatePlacementAndCost(GetPlayerInventory(World));
            const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("index"), Index);
            Result->SetBoolField(TEXT("hit"), Blocked);
            Result->SetBoolField(TEXT("usableHit"), Usable);
            if (Blocked)
            {
                HitJson->SetObjectField(TEXT("location"), ProbeVectorJson(Hit.Location));
                HitJson->SetObjectField(TEXT("normal"), ProbeVectorJson(Hit.ImpactNormal));
                Result->SetObjectField(TEXT("hitTarget"), HitJson);
            }
            const FTransform Transform = Hologram->GetActorTransform();
            const TSharedRef<FJsonObject> TransformJson = ProbeVectorJson(Transform.GetLocation());
            const FRotator Rotator = Transform.Rotator();
            TransformJson->SetNumberField(TEXT("yaw"), Rotator.Yaw); TransformJson->SetNumberField(TEXT("pitch"), Rotator.Pitch); TransformJson->SetNumberField(TEXT("roll"), Rotator.Roll);
            Result->SetObjectField(TEXT("transform"), TransformJson);
            const TSharedRef<FJsonObject> SetLocationJson = ProbeVectorJson(AfterSetLocation.GetLocation());
            SetLocationJson->SetNumberField(TEXT("yaw"), AfterSetLocation.Rotator().Yaw);
            Result->SetObjectField(TEXT("afterSetLocation"), SetLocationJson);
            Result->SetBoolField(TEXT("trySnapToActor"), TrySnapped);
            Result->SetNumberField(TEXT("scrollRotation"), Hologram->GetScrollRotateValue());
            AFGBuildableHologram* Buildable = Cast<AFGBuildableHologram>(Hologram);
            AFGBuildable* Snapped = Buildable ? Buildable->GetSnappedBuilding() : nullptr;
            Result->SetStringField(TEXT("snappedToId"), Snapped ? Snapped->GetName() : FString());
            Result->SetBoolField(TEXT("canConstruct"), Hologram->CanConstruct());
            TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers; Hologram->GetConstructDisqualifiers(Disqualifiers);
            TArray<TSharedPtr<FJsonValue>> DisqualifierNames;
            for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers) if (Disqualifier) DisqualifierNames.Add(MakeShared<FJsonValueString>(Disqualifier->GetName()));
            Result->SetArrayField(TEXT("disqualifiers"), DisqualifierNames);
            Results.Add(MakeShared<FJsonValueObject>(Result));
        }
        for (const TPair<FString, FProbeTemporary>& Entry : Temporaries)
            if (Entry.Value.bSpawned && Lightweight) Lightweight->RemoveTemporaryBuildableForInstanceIndex(Entry.Value.Class, Entry.Value.Index);
        Controller->SetControlRotation(SavedControlRotation);
        Hologram->Destroy();
        const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
        Body->SetBoolField(TEXT("ok"), true);
        Body->SetStringField(TEXT("recipePath"), RecipePath);
        Body->SetArrayField(TEXT("results"), Results);
        OnComplete(JsonResponse(Body));
    }, HologramProbeInitializationSeconds, false);
    return true;
}

/**
 * GET /v1/region/loaded?x=&y=&z=&radiusCm=: whether the world around a point is loaded enough to build in (#305). The
 * game streams terrain and actors in cells around the player, and work far from the player runs on whatever happens
 * to be loaded there: a pole 400 m away was probed on unloaded terrain. Reports the world partition's own streaming
 * state for a sphere at the point (every cell activated) and whether a downward trace at the point finds ground.
 * `loaded` is both. Read-only.
 */
bool FMKIBridgeModule::HandleRegionLoaded(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request)) { OnComplete(ErrorResponse(TEXT("unauthorized"), TEXT("A valid bearer token is required."), EHttpServerResponseCodes::Denied)); return true; }
    auto Number = [&Request](const TCHAR* Name, double& Out)
    {
        const FString* Text = Request.QueryParams.Find(Name);
        if (!Text || !Text->IsNumeric()) return false;
        Out = FCString::Atod(**Text);
        return FMath::IsFinite(Out);
    };
    double X = 0.0, Y = 0.0, Z = 0.0, RadiusCm = 5000.0;
    if (!Number(TEXT("x"), X) || !Number(TEXT("y"), Y)) { OnComplete(ErrorResponse(TEXT("invalid_region"), TEXT("x and y are required finite numbers."), EHttpServerResponseCodes::BadRequest)); return true; }
    const bool bHasZ = Number(TEXT("z"), Z);
    if (Request.QueryParams.Contains(TEXT("radiusCm")) && (!Number(TEXT("radiusCm"), RadiusCm) || RadiusCm < 100.0 || RadiusCm > 50000.0))
    { OnComplete(ErrorResponse(TEXT("invalid_region"), TEXT("radiusCm must be between 100 and 50000."), EHttpServerResponseCodes::BadRequest)); return true; }
    UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
    if (!World) { OnComplete(ErrorResponse(TEXT("world_unavailable"), TEXT("World is not ready."), EHttpServerResponseCodes::ServiceUnavail)); return true; }

    bool bPartitioned = false, bStreamingCompleted = true;
    if (World->GetWorldPartition())
    {
        if (const UWorldPartitionSubsystem* Partition = World->GetSubsystem<UWorldPartitionSubsystem>())
        {
            bPartitioned = true;
            FWorldPartitionStreamingQuerySource Source(FVector(X, Y, Z));
            Source.bUseGridLoadingRange = false; Source.Radius = RadiusCm;
            bStreamingCompleted = Partition->IsStreamingCompleted(EWorldPartitionRuntimeCellState::Activated, { Source }, false);
        }
    }
    // Ground under the point: a trace from well above (or from the requested height) down through the world floor.
    FHitResult Hit; FCollisionQueryParams Query(SCENE_QUERY_STAT(BridgeRegionLoaded), false);
    if (APawn* Player = UGameplayStatics::GetPlayerPawn(World, 0)) Query.AddIgnoredActor(Player);
    const double TopZ = bHasZ ? Z + 50000.0 : 500000.0, BottomZ = bHasZ ? Z - 50000.0 : -500000.0;
    const bool bGround = World->LineTraceSingleByChannel(Hit, FVector(X, Y, TopZ), FVector(X, Y, BottomZ), ECC_WorldStatic, Query);

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetBoolField(TEXT("ok"), true);
    Body->SetBoolField(TEXT("loaded"), bStreamingCompleted && bGround);
    Body->SetBoolField(TEXT("worldPartition"), bPartitioned);
    Body->SetBoolField(TEXT("streamingCompleted"), bStreamingCompleted);
    Body->SetBoolField(TEXT("groundFound"), bGround);
    if (bGround)
    {
        Body->SetNumberField(TEXT("groundZ"), Hit.ImpactPoint.Z);
        if (const AActor* Actor = Hit.GetActor()) Body->SetStringField(TEXT("groundActorClass"), Actor->GetClass()->GetName());
    }
    Body->SetNumberField(TEXT("radiusCm"), RadiusCm);
    OnComplete(JsonResponse(Body));
    return true;
}

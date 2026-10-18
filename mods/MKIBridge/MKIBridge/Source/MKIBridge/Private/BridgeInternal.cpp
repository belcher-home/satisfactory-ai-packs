#include "BridgeInternal.h"
#include "Components/SplineComponent.h"
#include "Buildables/FGBuildablePole.h"
#include "FGFactoryLegsComponent.h"

// Definitions of the shared helpers declared in BridgeInternal.h.
// Split from the single MKIBridge.cpp without behavior changes (#44).

namespace MKIBridgeInternal
{
    TAutoConsoleVariable<int32> CVarMKIBridgeShowStatus(
        TEXT("MKIBridge.ShowStatus"),
        1,
        TEXT("Show the local MKI Bridge status indicator. 0 disables it; 1 enables it."),
        ECVF_Default);

    TAutoConsoleVariable<float> CVarMaximumPlacementDistanceCm(
        TEXT("MKIBridge.MaximumMutationDistanceCm"),
        200000.0f,
        TEXT("Maximum distance in centimeters from the player for mutating bridge operations."),
        ECVF_Default);

    TAutoConsoleVariable<float> CVarMaximumObservationDistanceCm(
        TEXT("MKIBridge.MaximumObservationDistanceCm"),
        1000000.0f,
        TEXT("Maximum distance in centimeters from the player for spatial bridge observations."),
        ECVF_Default);

    double GetMaximumPlacementDistanceCm()
    {
        const double Configured = static_cast<double>(CVarMaximumPlacementDistanceCm.GetValueOnAnyThread());
        return FMath::IsFinite(Configured) ? FMath::Clamp(Configured, 1000.0, 500000.0) : 200000.0;
    }

    double GetMaximumObservationDistanceCm()
    {
        const double Configured = static_cast<double>(CVarMaximumObservationDistanceCm.GetValueOnAnyThread());
        return FMath::IsFinite(Configured) ? FMath::Clamp(Configured, 1000.0, 2000000.0) : 1000000.0;
    }

    double SnapToIncrement(double Value, double Increment)
    {
        return FMath::RoundToDouble(Value / Increment) * Increment;
    }

    double GetSupportSurfaceToActorOriginOffsetCm(const FString& Type)
    {
        if (Type.Equals(TEXT("splitter"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("merger"), ESearchCase::IgnoreCase)) return 100.0;
        if (Type.Equals(TEXT("smelter"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("constructor"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("storage"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("power_pole"), ESearchCase::IgnoreCase) ||
            Type.Equals(TEXT("biomass_generator"), ESearchCase::IgnoreCase)) return 0.0;
        return 0.0;
    }

    const TArray<FCompanionDeclaration>& CompanionDeclarations()
    {
        static const TArray<FCompanionDeclaration> Declarations = { { TEXT("MKIRoads"), { TEXT("road_network") } } };
        return Declarations;
    }

    TSharedPtr<FJsonObject> ReadPluginDescriptorJson(const TSharedPtr<IPlugin>& Plugin)
    {
        FString Text; TSharedPtr<FJsonObject> Json;
        if (!Plugin.IsValid() || !FFileHelper::LoadFileToString(Text, *Plugin->GetDescriptorFileName())) return nullptr;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        return FJsonSerializer::Deserialize(Reader, Json) ? Json : nullptr;
    }

    FCompanionStatus ResolveCompanion(const FCompanionDeclaration& Declaration)
    {
        FCompanionStatus Status;
        Status.Name = Declaration.Name; Status.Features = Declaration.Features; Status.MountPoint = FString::Printf(TEXT("/%s/"), Declaration.Name);
        IPluginManager& Plugins = IPluginManager::Get();
        if (const TSharedPtr<FJsonObject> Self = ReadPluginDescriptorJson(Plugins.FindPlugin(BridgePluginName)))
        {
            const TArray<TSharedPtr<FJsonValue>>* References = nullptr;
            if (Self->TryGetArrayField(TEXT("Plugins"), References))
                for (const TSharedPtr<FJsonValue>& Value : *References)
                {
                    const TSharedPtr<FJsonObject>* Reference = nullptr;
                    FString Name;
                    if (!Value.IsValid() || !Value->TryGetObject(Reference) || !(*Reference)->TryGetStringField(TEXT("Name"), Name) || Name != Declaration.Name) continue;
                    Status.bDeclared = (*Reference)->TryGetStringField(TEXT("SemVersion"), Status.RequiredVersion) && !Status.RequiredVersion.IsEmpty();
                    bool bOptional = false;
                    (*Reference)->TryGetBoolField(TEXT("Optional"), bOptional);
                    Status.bRequired = !bOptional;
                }
        }
        if (!Status.bDeclared) { Status.Reason = TEXT("the bridge descriptor declares no versioned dependency on this companion"); return Status; }
        if (!Status.bRequired) { Status.Reason = TEXT("the bridge descriptor declares this companion optional; it must be a required dependency"); return Status; }
        const TSharedPtr<IPlugin> Companion = Plugins.FindPlugin(Declaration.Name);
        Status.bPresent = Companion.IsValid() && Companion->IsEnabled() && Companion->IsMounted();
        if (!Status.bPresent) { Status.Reason = TEXT("not installed or not enabled"); return Status; }
        const TSharedPtr<FJsonObject> CompanionJson = ReadPluginDescriptorJson(Companion);
        if (!CompanionJson.IsValid() || !CompanionJson->TryGetStringField(TEXT("SemVersion"), Status.Version)) { Status.Reason = TEXT("the installed descriptor declares no SemVersion"); return Status; }
        FVersion Version; FVersionRange Range; FString Error;
        if (!Version.ParseVersion(Status.Version, Error)) { Status.Reason = FString::Printf(TEXT("installed version %s is invalid: %s"), *Status.Version, *Error); return Status; }
        if (!Range.ParseVersionRange(Status.RequiredVersion, Error)) { Status.Reason = FString::Printf(TEXT("declared range %s is invalid: %s"), *Status.RequiredVersion, *Error); return Status; }
        Status.bCompatible = Range.Matches(Version);
        if (!Status.bCompatible) Status.Reason = FString::Printf(TEXT("installed version %s does not satisfy %s"), *Status.Version, *Status.RequiredVersion);
        return Status;
    }

    const FCompanionStatus& CachedCompanionStatus(const FCompanionDeclaration& Declaration)
    {
        static TMap<FString, FCompanionStatus> Cache;
        if (const FCompanionStatus* Found = Cache.Find(Declaration.Name)) return *Found;
        return Cache.Add(Declaration.Name, ResolveCompanion(Declaration));
    }

    ECollisionChannel BuildGunTraceChannel()
    {
        if (const UCollisionProfile* Profile = UCollisionProfile::Get())
            for (int32 Channel = ECC_GameTraceChannel1; Channel <= ECC_GameTraceChannel18; ++Channel)
                if (Profile->ReturnChannelNameFromContainerIndex(Channel) == FName(TEXT("BuildGun"))) return static_cast<ECollisionChannel>(Channel);
        return ECC_GameTraceChannel5;
    }

    bool IsPlaceableAssetPath(const FString& Path, FString& OutCode, FString& OutMessage)
    {
        if (Path.StartsWith(FactoryGameAssetRoot)) return true;
        for (const FCompanionDeclaration& Declaration : CompanionDeclarations())
        {
            if (!Path.StartsWith(FString::Printf(TEXT("/%s/"), Declaration.Name))) continue;
            const FCompanionStatus& Status = CachedCompanionStatus(Declaration);
            if (Status.bCompatible) return true;
            OutCode = TEXT("companion_unavailable");
            OutMessage = FString::Printf(TEXT("%s belongs to companion mod %s (requires %s), which is unavailable: %s."), *Path, *Status.Name, Status.RequiredVersion.IsEmpty() ? TEXT("an undeclared version") : *Status.RequiredVersion, *Status.Reason);
            return false;
        }
        OutCode = TEXT("invalid_asset_path");
        OutMessage = TEXT("Generic placement requires classPath and recipePath values from the observed FactoryGame structure catalog or a compatible first-party companion mod.");
        return false;
    }

    UFGInventoryComponent* GetPlayerInventory(UWorld* World)
    {
        AFGCharacterPlayer* Player = World ? Cast<AFGCharacterPlayer>(UGameplayStatics::GetPlayerPawn(World, 0)) : nullptr;
        return Player ? Player->GetInventory() : nullptr;
    }

    bool CanAffordRecipe(UWorld* World, TSubclassOf<UFGRecipe> Recipe, int32 Multiplier, FString& Missing)
    {
        UFGInventoryComponent* Inventory = GetPlayerInventory(World);
        if (!Inventory || !Recipe || Multiplier < 1)
        {
            Missing = TEXT("Player inventory or build recipe is unavailable.");
            return false;
        }
        if (Inventory->GetNoBuildCost()) return true;
        for (const FItemAmount& Cost : UFGRecipe::GetIngredients(World, Recipe))
        {
            const int32 Required = Cost.Amount * Multiplier;
            if (!Cost.ItemClass || !Inventory->HasItems(Cost.ItemClass, Required))
            {
                Missing = FString::Printf(TEXT("Missing %d x %s."), Required,
                    Cost.ItemClass ? *Cost.ItemClass->GetName() : TEXT("unknown item"));
                return false;
            }
        }
        return true;
    }

    bool ConsumeRecipeCost(UWorld* World, TSubclassOf<UFGRecipe> Recipe, int32 Multiplier, FString& Missing)
    {
        if (!CanAffordRecipe(World, Recipe, Multiplier, Missing)) return false;
        UFGInventoryComponent* Inventory = GetPlayerInventory(World);
        if (Inventory->GetNoBuildCost()) return true;
        for (const FItemAmount& Cost : UFGRecipe::GetIngredients(World, Recipe))
        {
            Inventory->Remove(Cost.ItemClass, Cost.Amount * Multiplier);
        }
        return true;
    }

    void RefundRecipeCost(UWorld* World, TSubclassOf<UFGRecipe> Recipe, int32 Multiplier)
    {
        UFGInventoryComponent* Inventory = GetPlayerInventory(World);
        if (!Inventory || !Recipe || Multiplier < 1) return;
        if (Inventory->GetNoBuildCost()) return;
        for (const FItemAmount& Cost : UFGRecipe::GetIngredients(World, Recipe))
        {
            if (Cost.ItemClass) Inventory->AddStack(FInventoryStack(Cost.Amount * Multiplier, Cost.ItemClass), true);
        }
    }

    bool IsLightweightSubsystemActor(const AFGBuildable* Buildable)
    {
        if (!Buildable) return false;
        if (Buildable->GetIsLightweightTemporary()) return true;
        return Buildable->GetActorLocation().Equals(FLightweightBuildablePool::PoolTransform.GetLocation(), 1.0);
    }

    bool ParseCanonicalLightweightId(const FString& Id, FString& OutClassPath, int32& OutIndex)
    {
        TArray<FString> Parts;
        Id.ParseIntoArray(Parts, TEXT("|"), false);
        if (Parts.Num() != 3 || !Parts[0].Equals(TEXT("lw"), ESearchCase::CaseSensitive) || !Parts[1].StartsWith(TEXT("/"), ESearchCase::CaseSensitive)) return false;
        for (const TCHAR Character : Parts[1]) if (FChar::IsWhitespace(Character)) return false;
        const FString& IndexText = Parts[2];
        if (IndexText.IsEmpty() || IndexText.Len() > 10 || (IndexText.Len() > 1 && IndexText[0] == TEXT('0'))) return false;
        int64 Value = 0;
        for (const TCHAR Character : IndexText)
        {
            if (Character < TEXT('0') || Character > TEXT('9')) return false;
            Value = Value * 10 + (Character - TEXT('0'));
        }
        if (Value > MAX_int32) return false;
        OutClassPath = Parts[1];
        OutIndex = static_cast<int32>(Value);
        return true;
    }

    bool LightweightCanDismantle(AFGLightweightBuildableSubsystem* Lightweight, TSubclassOf<AFGBuildable> Class, int32 Index, const FRuntimeBuildableInstanceData* Record)
    {
        if (!Lightweight || !Class || !Record) return false;
        AFGBuildable* Temporary = Lightweight->FindTemporaryByBuildableClassAndIndex(Class, Index);
        bool bSpawned = false;
        if (!Temporary)
            if (FInstanceToTemporaryBuildable* Converted = Lightweight->FindOrSpawnBuildableForRuntimeData(Class, const_cast<FRuntimeBuildableInstanceData*>(Record), Index, bSpawned))
                Temporary = Converted->Buildable;
        const bool bCanDismantle = IsValid(Temporary) && IFGDismantleInterface::Execute_CanDismantle(Temporary);
        if (bSpawned) Lightweight->RemoveTemporaryBuildableForInstanceIndex(Class, Index);
        return bCanDismantle;
    }

    AFGBuildable* ResolveLightweightAnchor(UWorld* World, const FString& AnchorId, AActor*& OutTemporaryAnchor)
    {
        OutTemporaryAnchor = nullptr;
        // Only the canonical form is accepted: a malformed index never falls back to instance 0.
        FString ClassPath;
        int32 RuntimeIndex = INDEX_NONE;
        if (!ParseCanonicalLightweightId(AnchorId, ClassPath, RuntimeIndex)) return nullptr;
        AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World);
        if (!Lightweight) return nullptr;
        for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            UClass* BuildableClass = Pair.Key.Get();
            if (!BuildableClass || !BuildableClass->GetPathName().Equals(ClassPath, ESearchCase::CaseSensitive) || !Pair.Value.IsValidIndex(RuntimeIndex) || !Pair.Value[RuntimeIndex].IsValid()) continue;
            FLightweightBuildableInstanceRef Reference;
            Reference.Initialize(Lightweight, Pair.Key, RuntimeIndex);
            if (!Reference.IsValid()) return nullptr;
            if (!Reference.ResolveBuildableInstanceData()) return nullptr;
            AFGBuildable* Temporary = Reference.SpawnTemporaryBuildable();
            if (!IsValid(Temporary)) return nullptr;
            Temporary->SetActorTransform(Reference.GetBuildableTransform());
            OutTemporaryAnchor = Temporary;
            return Temporary;
        }
        return nullptr;
    }

    FSurveyInstanceKey SurveyInstanceKey(const FInstanceHandle& Handle)
    {
        return FSurveyInstanceKey(Handle.GetInstanceComponent(), Handle.GetHandleID());
    }

    TSharedRef<FJsonObject> DescribeSupportHit(UWorld* World, const FHitResult& Hit)
    {
        const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        AActor* Actor = Hit.GetActor();
        Out->SetStringField(TEXT("actorId"), Actor ? Actor->GetName() : FString());
        Out->SetStringField(TEXT("actorClassName"), Actor ? Actor->GetClass()->GetName() : FString());
        AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(Actor);
        Out->SetBoolField(TEXT("instanced"), Manager != nullptr);
        if (!Manager) { Out->SetStringField(TEXT("id"), Actor ? Actor->GetName() : FString()); Out->SetBoolField(TEXT("instanceResolved"), true); return Out; }
        FInstanceHandle Handle;
        const bool HandleResolved = Cast<ULightweightCollisionComponent>(Hit.GetComponent()) && Manager->ResolveHit(Hit, Handle) && Handle.IsInstanced() && Handle.GetInstanceComponent();
        if (HandleResolved)
        {
            const FSurveyInstanceKey Key = SurveyInstanceKey(Handle);
            if (AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World))
                for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Pair : Lightweight->GetAllLightweightBuildableInstances())
                {
                    UClass* Class = Pair.Key.Get(); if (!Class) continue;
                    for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
                    {
                        const FRuntimeBuildableInstanceData& Data = Pair.Value[Index]; if (!Data.IsValid()) continue;
                        for (const FInstanceOwnerHandlePtr& InstanceHandle : Data.Handles)
                            if (InstanceHandle.IsValid() && InstanceHandle->IsInstanced() && InstanceHandle->GetInstanceComponent() && SurveyInstanceKey(*InstanceHandle) == Key)
                            {
                                Out->SetStringField(TEXT("id"), FString::Printf(TEXT("lw|%s|%d"), *Class->GetPathName(), Index));
                                Out->SetStringField(TEXT("classPath"), Class->GetPathName());
                                Out->SetNumberField(TEXT("lightweightIndex"), Index);
                                Out->SetBoolField(TEXT("instanceResolved"), true);
                                return Out;
                            }
                    }
                }
            AActor* Owner = Handle.GetOwner();
            if (Owner && Owner != Manager && !Owner->IsA<AFGLightweightBuildableSubsystem>()) { Out->SetStringField(TEXT("id"), Owner->GetName()); Out->SetBoolField(TEXT("instanceResolved"), true); return Out; }
        }
        Out->SetStringField(TEXT("id"), Actor->GetName());
        Out->SetBoolField(TEXT("instanceResolved"), false);
        return Out;
    }

    TSharedRef<FJsonObject> FactoryCustomizationJson(const FFactoryCustomizationData& Customization)
    {
        const TSharedRef<FJsonObject> CustomizationJson = MakeShared<FJsonObject>();
        auto SetClassPath = [&CustomizationJson](const TCHAR* Field, UClass* Class)
        {
            if (Class) CustomizationJson->SetStringField(Field, Class->GetPathName());
        };
        SetClassPath(TEXT("swatchDescriptorPath"), Customization.SwatchDesc.Get());
        SetClassPath(TEXT("patternDescriptorPath"), Customization.PatternDesc.Get());
        SetClassPath(TEXT("materialDescriptorPath"), Customization.MaterialDesc.Get());
        SetClassPath(TEXT("skinDescriptorPath"), Customization.SkinDesc.Get());
        CustomizationJson->SetNumberField(TEXT("patternRotation"), Customization.PatternRotation);
        auto ColorJson = [](const FLinearColor& Color)
        {
            const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("r"), Color.R); O->SetNumberField(TEXT("g"), Color.G);
            O->SetNumberField(TEXT("b"), Color.B); O->SetNumberField(TEXT("a"), Color.A);
            return O;
        };
        const TSharedRef<FJsonObject> OverrideJson = MakeShared<FJsonObject>();
        OverrideJson->SetObjectField(TEXT("primary"), ColorJson(Customization.OverrideColorData.PrimaryColor));
        OverrideJson->SetObjectField(TEXT("secondary"), ColorJson(Customization.OverrideColorData.SecondaryColor));
        if (Customization.OverrideColorData.PaintFinish) OverrideJson->SetStringField(TEXT("paintFinishDescriptorPath"), Customization.OverrideColorData.PaintFinish->GetPathName());
        CustomizationJson->SetObjectField(TEXT("overrideColors"), OverrideJson);
        const FResolvedFactoryCustomizationData ResolvedCustomization(Customization);
        const TSharedRef<FJsonObject> ResolvedJson = MakeShared<FJsonObject>();
        ResolvedJson->SetNumberField(TEXT("patternId"), ResolvedCustomization.PatternID);
        ResolvedJson->SetNumberField(TEXT("materialId"), ResolvedCustomization.MaterialID);
        ResolvedJson->SetNumberField(TEXT("colorSlot"), ResolvedCustomization.ColorSlot);
        ResolvedJson->SetObjectField(TEXT("primary"), ColorJson(ResolvedCustomization.PrimaryColor));
        ResolvedJson->SetObjectField(TEXT("secondary"), ColorJson(ResolvedCustomization.SecondaryColor));
        ResolvedJson->SetNumberField(TEXT("roughness"), ResolvedCustomization.Roughness);
        ResolvedJson->SetNumberField(TEXT("metallic"), ResolvedCustomization.Metallic);
        CustomizationJson->SetObjectField(TEXT("resolved"), ResolvedJson);
        return CustomizationJson;
    }

    FString SerializeCondensed(const TSharedRef<FJsonObject>& Object)
    {
        FString Body;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
        FJsonSerializer::Serialize(Object, Writer);
        return Body;
    }

    TUniquePtr<FHttpServerResponse> JsonResponse(const TSharedRef<FJsonObject>& Object, EHttpServerResponseCodes Code)
    {
        auto Response = FHttpServerResponse::Create(SerializeCondensed(Object), TEXT("application/json"));
        Response->Code = Code;
        Response->Headers.Add(TEXT("Cache-Control"), {TEXT("no-store")});
        return Response;
    }

    bool IsRetryableError(const FString& Error, EHttpServerResponseCodes Code)
    {
        return Code == EHttpServerResponseCodes::ServiceUnavail || Error == TEXT("request_in_flight") || Error == TEXT("mutation_abandoned");
    }

    TSharedRef<FJsonObject> ErrorBody(const FString& Error, const FString& Message, bool bRetryable)
    {
        const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
        Body->SetBoolField(TEXT("ok"), false);
        Body->SetStringField(TEXT("error"), Error);
        Body->SetStringField(TEXT("message"), Message);
        Body->SetBoolField(TEXT("retryable"), bRetryable);
        return Body;
    }

    TUniquePtr<FHttpServerResponse> ErrorResponse(const FString& Error, const FString& Message, EHttpServerResponseCodes Code)
    {
        return JsonResponse(ErrorBody(Error, Message, IsRetryableError(Error, Code)), Code);
    }

    bool ReadJsonBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& OutJson, FString* OutText)
    {
        FString Text;
        if (!Request.Body.IsEmpty())
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
            Text = FString(Converted.Length(), Converted.Get());
        }
        if (OutText) *OutText = Text;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
    }

    bool IsLoopbackAddressText(FString Address)
    {
        Address.TrimStartAndEndInline();
        Address.RemoveFromStart(TEXT("["));
        Address.RemoveFromEnd(TEXT("]"));
        Address = Address.ToLower();
        return Address.StartsWith(TEXT("127.")) || Address == TEXT("::1") || Address == TEXT("0:0:0:0:0:0:0:1") || Address.StartsWith(TEXT("::ffff:127."));
    }

    const FString& BridgeModVersion()
    {
        static FString Version;
        if (Version.IsEmpty())
        {
            if (const TSharedPtr<FJsonObject> Descriptor = ReadPluginDescriptorJson(IPluginManager::Get().FindPlugin(BridgePluginName)))
            {
                if (!Descriptor->TryGetStringField(TEXT("SemVersion"), Version) || Version.IsEmpty()) Descriptor->TryGetStringField(TEXT("VersionName"), Version);
            }
            if (Version.IsEmpty()) Version = TEXT("unknown");
        }
        return Version;
    }

    const FRuntimeBuildableInstanceData* FindLightweightRecord(UWorld* World, const FString& Id, TSubclassOf<AFGBuildable>& OutClass, int32& OutIndex)
    {
        FString ClassPath;
        if (!ParseCanonicalLightweightId(Id, ClassPath, OutIndex)) return nullptr;
        AFGLightweightBuildableSubsystem* Lightweight = AFGLightweightBuildableSubsystem::Get(World);
        if (!Lightweight) return nullptr;
        for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Pair : Lightweight->GetAllLightweightBuildableInstances())
        {
            UClass* Class = Pair.Key.Get();
            if (!Class || !Class->GetPathName().Equals(ClassPath, ESearchCase::CaseSensitive)) continue;
            if (!Pair.Value.IsValidIndex(OutIndex) || !Pair.Value[OutIndex].IsValid()) return nullptr;
            OutClass = Pair.Key;
            return &Pair.Value[OutIndex];
        }
        return nullptr;
    }

    UFGFactoryConnectionComponent* FindFreeFactoryConnection(AActor* Actor, EFactoryConnectionDirection Direction, const FString& Port)
    {
        if (!Actor) return nullptr;
        TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Actor);
        for (UFGFactoryConnectionComponent* Connection : Connections)
            if (Connection && Connection->GetDirection() == Direction && !Connection->IsConnected() && (Port.IsEmpty() || Connection->GetName().Equals(Port, ESearchCase::IgnoreCase))) return Connection;
        return nullptr;
    }

    UFGFactoryConnectionComponent* FindConveyorSupportSnap(AActor* Actor, const FString& Port)
    {
        if (!Actor || Actor->IsA<AFGBuildableConveyorBase>()) return nullptr;
        TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Actor);
        UFGFactoryConnectionComponent* Snap = nullptr;
        for (UFGFactoryConnectionComponent* Connection : Connections)
        {
            if (!Connection) continue;
            const EFactoryConnectionDirection Direction = Connection->GetDirection();
            if (Direction == EFactoryConnectionDirection::FCD_INPUT || Direction == EFactoryConnectionDirection::FCD_OUTPUT || Direction == EFactoryConnectionDirection::FCD_ANY) return nullptr;
            if (!Snap && Direction == EFactoryConnectionDirection::FCD_SNAP_ONLY && (Port.IsEmpty() || Connection->GetName().Equals(Port, ESearchCase::IgnoreCase))) Snap = Connection;
        }
        return Snap;
    }

    UFGFactoryConnectionComponent* FindBeltEndAtSupport(UWorld* World, const UFGFactoryConnectionComponent* Snap, EFactoryConnectionDirection Direction)
    {
        AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
        if (!Subsystem || !Snap) return nullptr;
        const FVector SnapLocation = Snap->GetConnectorLocation(false);
        UFGFactoryConnectionComponent* Best = nullptr; double BestDistance = ConveyorSupportSnapToleranceCm;
        for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
        {
            AFGBuildableConveyorBelt* Belt = Cast<AFGBuildableConveyorBelt>(Buildable);
            if (!Belt) continue;
            UFGFactoryConnectionComponent* End = Direction == EFactoryConnectionDirection::FCD_OUTPUT ? Belt->GetConnection1() : Belt->GetConnection0();
            if (!End || End->IsConnected() || End->GetDirection() != Direction) continue;
            const double Distance = FVector::Distance(End->GetConnectorLocation(false), SnapLocation);
            if (Distance <= BestDistance) { Best = End; BestDistance = Distance; }
        }
        return Best;
    }

    const TArray<const TCHAR*>& BridgeEventTypes()
    {
        static const TArray<const TCHAR*> Types = {
            TEXT("receipt_completed"), TEXT("buildable_added"), TEXT("buildable_changed"), TEXT("buildable_removed"), TEXT("buildables_bulk_change"),
            TEXT("machine_state_changed"), TEXT("power_circuit_tripped"), TEXT("power_circuit_restored"),
        };
        return Types;
    }

    const TArray<const TCHAR*>& BridgeNativeFeatures()
    {
        static const TArray<const TCHAR*> Features = {
            TEXT("idempotent_replay"), TEXT("site_survey_summary"), TEXT("power_connections_bounded"),
            TEXT("lightweight_dismantle_checked"), TEXT("belt_support_endpoints"), TEXT("buildables_query"), TEXT("placement_final_ids"),
            TEXT("batch"), TEXT("machine_state"), TEXT("power_circuits"), TEXT("player_inventory"), TEXT("catalog"), TEXT("progression"), TEXT("resource_nodes_v2"),
            TEXT("events"), TEXT("persistent_receipts"), TEXT("terrain_column_probe"), TEXT("player_safety"), TEXT("hologram_probe"), TEXT("vehicles"),
            TEXT("vehicle_path_validation"), TEXT("build_receipts"), TEXT("region_loaded"), TEXT("conveyor_contents"), TEXT("game_rules"), TEXT("machine_clock"), TEXT("rail_to_point"), TEXT("spline_geometry"), TEXT("splitter_rules"), TEXT("blueprint_place"),
        };
        return Features;
    }

    TSharedRef<FJsonObject> BridgeVectorJson(const FVector& V)
    {
        const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
        return O;
    }

    TSharedRef<FJsonObject> BridgeRotationJson(const FRotator& Rotation)
    {
        const TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
        RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw); RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch); RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
        return RotationJson;
    }

    FVector BuildableLocation(const AFGBuildable* Buildable)
    {
        if (const AFGBuildableWire* Wire = Cast<AFGBuildableWire>(Buildable))
        {
            const UFGCircuitConnectionComponent* A = Wire->GetConnection(0);
            const UFGCircuitConnectionComponent* B = Wire->GetConnection(1);
            if (IsValid(A) && IsValid(B)) return (A->GetComponentLocation() + B->GetComponentLocation()) * 0.5;
        }
        return Buildable->GetActorLocation();
    }
    /** The factory connections a buildable keeps in an array property named Name (by reflection; none when it has none). */
    TSet<const UActorComponent*> ConnectionArray(const AFGBuildable* Buildable, const TCHAR* Name)
    {
        TSet<const UActorComponent*> Out;
        const FArrayProperty* Array = FindFProperty<FArrayProperty>(Buildable->GetClass(), Name);
        const FObjectPropertyBase* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
        if (!Inner) return Out;
        FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Buildable));
        for (int32 Index = 0; Index < Helper.Num(); ++Index) if (const UActorComponent* Component = Cast<UActorComponent>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)))) Out.Add(Component);
        return Out;
    }

    /**
     * Port semantics (#303): what a conveyor port takes, where it sits on the building, and what it is attached to, read from
     * the game's own data rather than from class names. accepts: "fuel" for a station's or generator's fuel input (its
     * mFuelConnections, a connection feeding its mFuelInventory, or any belt input of a fuel generator), "cargo" for a station's storage inputs, else "any";
     * side: front, back, right, left, top or bottom from the facing in the building's frame; level: how many same-direction
     * ports stand directly below it (a Truck Station's stacked inputs); connectedBuildableId/connectedComponent.
     */
    void AddPortSemantics(const TSharedRef<FJsonObject>& Json, const AFGBuildable* Buildable, const UFGFactoryConnectionComponent* Connection,
        const TArray<UFGFactoryConnectionComponent*>& All, const TSet<const UActorComponent*>& FuelPorts, const TSet<const UActorComponent*>& CargoPorts, const UFGInventoryComponent* FuelInventory)
    {
        const bool bInput = Connection->GetDirection() == EFactoryConnectionDirection::FCD_INPUT;
        const TCHAR* Accepts = TEXT("any");
        // A fuel generator's belt inputs feed only its fuel inventory, but it never ties them to it (#327), so every one is a fuel input.
        if (bInput && (FuelPorts.Contains(Connection) || (FuelInventory && Connection->GetInventory() == FuelInventory) || Buildable->IsA<AFGBuildableGeneratorFuel>())) Accepts = TEXT("fuel");
        else if (bInput && CargoPorts.Contains(Connection)) Accepts = TEXT("cargo");
        Json->SetStringField(TEXT("accepts"), Accepts);
        const FTransform& Frame = Buildable->GetActorTransform();
        const FVector Facing = Frame.InverseTransformVectorNoScale(Connection->GetConnectorNormal());
        const FVector Abs = Facing.GetAbs();
        const TCHAR* Side = Abs.Z >= Abs.X && Abs.Z >= Abs.Y ? (Facing.Z >= 0.0 ? TEXT("top") : TEXT("bottom"))
            : Abs.X >= Abs.Y ? (Facing.X >= 0.0 ? TEXT("front") : TEXT("back")) : (Facing.Y >= 0.0 ? TEXT("right") : TEXT("left"));
        Json->SetStringField(TEXT("side"), Side);
        const FVector Here = Frame.InverseTransformPosition(Connection->GetConnectorLocation(false));
        int32 Level = 0;
        for (const UFGFactoryConnectionComponent* Other : All)
        {
            if (!Other || Other == Connection || Other->GetDirection() != Connection->GetDirection()) continue;
            const FVector There = Frame.InverseTransformPosition(Other->GetConnectorLocation(false));
            if (FVector2D::Distance(FVector2D(Here.X, Here.Y), FVector2D(There.X, There.Y)) <= 50.0 && There.Z < Here.Z - 10.0) ++Level;
        }
        Json->SetNumberField(TEXT("level"), Level);
        if (const UFGFactoryConnectionComponent* Other = Connection->GetConnection())
        {
            if (const AActor* OtherOwner = Other->GetOwner()) Json->SetStringField(TEXT("connectedBuildableId"), OtherOwner->GetName());
            Json->SetStringField(TEXT("connectedComponent"), Other->GetName());
        }
    }

    TSharedRef<FJsonObject> BuildReceiptJson(AFGBuildable* Built, const TArray<TSharedPtr<FJsonValue>>& Deltas)
    {
        const TSharedRef<FJsonObject> Full = ActorBuildableJson(Built, EBuildableFields::Full);
        const TSharedRef<FJsonObject> Receipt = MakeShared<FJsonObject>();
        static const TCHAR* Copied[] = { TEXT("id"), TEXT("className"), TEXT("position"), TEXT("rotation"), TEXT("legOffsetsCm"), TEXT("lift") };
        for (const TCHAR* Field : Copied) if (const TSharedPtr<FJsonValue> Value = Full->TryGetField(Field)) Receipt->SetField(Field, Value);
        const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
        Receipt->SetArrayField(TEXT("ports"), Full->TryGetArrayField(TEXT("connections"), Connections) ? *Connections : TArray<TSharedPtr<FJsonValue>>());
        if (const AFGBuildablePole* Pole = Cast<AFGBuildablePole>(Built)) Receipt->SetNumberField(TEXT("heightCm"), Pole->mHeight);
        else if (const AFGBuildableConveyorLift* Lift = Cast<AFGBuildableConveyorLift>(Built)) Receipt->SetNumberField(TEXT("heightCm"), Lift->GetHeight());
        Receipt->SetBoolField(TEXT("matchesRequest"), Deltas.Num() == 0);
        Receipt->SetArrayField(TEXT("deltas"), Deltas);
        return Receipt;
    }

    TSharedPtr<FJsonValue> ReceiptDelta(const TCHAR* Field, double Requested, double Built, double ToleranceCm)
    {
        const TSharedRef<FJsonObject> Delta = MakeShared<FJsonObject>();
        Delta->SetStringField(TEXT("field"), Field); Delta->SetNumberField(TEXT("requested"), Requested); Delta->SetNumberField(TEXT("built"), Built); Delta->SetNumberField(TEXT("toleranceCm"), ToleranceCm);
        return MakeShared<FJsonValueObject>(Delta);
    }

    TSharedRef<FJsonObject> ActorBuildableJson(AFGBuildable* Buildable, EBuildableFields Fields)
    {
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), Buildable->GetName());
        Item->SetStringField(TEXT("className"), Buildable->GetClass()->GetName());
        Item->SetStringField(TEXT("classPath"), Buildable->GetClass()->GetPathName());
        if (Buildable->GetBuiltWithRecipe()) Item->SetStringField(TEXT("recipePath"), Buildable->GetBuiltWithRecipe()->GetPathName());
        const FVector Location = BuildableLocation(Buildable);
        const FBox Bounds = Buildable->GetComponentsBoundingBox(true);
        auto VectorJson = [](const FVector& V) { return BridgeVectorJson(V); };
        Item->SetObjectField(TEXT("location"), VectorJson(Location));
        Item->SetObjectField(TEXT("position"), VectorJson(Location));
        Item->SetObjectField(TEXT("rotation"), BridgeRotationJson(Buildable->GetActorRotation()));
        Item->SetObjectField(TEXT("boundsMin"), VectorJson(Bounds.Min));
        Item->SetObjectField(TEXT("boundsMax"), VectorJson(Bounds.Max));
        if (Fields == EBuildableFields::Compact) return Item;

        // The legs a building stood on when placed (#321): each foot's offset below the building, cm; none reported for
        // buildings without legs. A station on a deck should report zeros.
        if (const UFGFactoryLegsComponent* Legs = Buildable->FindComponentByClass<UFGFactoryLegsComponent>())
        {
            TArray<TSharedPtr<FJsonValue>> Feet;
            for (const FFeetOffset& Foot : Legs->GetCachedFeetOffsets()) Feet.Add(MakeShared<FJsonValueNumber>(Foot.OffsetZ));
            Item->SetArrayField(TEXT("legOffsetsCm"), Feet);
        }

        TArray<TSharedPtr<FJsonValue>> Ports;
        TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Buildable);
        for (UFGFactoryConnectionComponent* Connection : Connections)
        {
            if (!Connection) continue;
            const TSharedRef<FJsonObject> PortJson = MakeShared<FJsonObject>();
            PortJson->SetStringField(TEXT("name"), Connection->GetName());
            const EFactoryConnectionDirection PortDirection = Connection->GetDirection();
            PortJson->SetStringField(TEXT("direction"), PortDirection == EFactoryConnectionDirection::FCD_INPUT ? TEXT("input") :
                PortDirection == EFactoryConnectionDirection::FCD_OUTPUT ? TEXT("output") :
                PortDirection == EFactoryConnectionDirection::FCD_ANY ? TEXT("any") : TEXT("snap_only"));
            PortJson->SetBoolField(TEXT("connected"), Connection->IsConnected());
            PortJson->SetObjectField(TEXT("location"), VectorJson(Connection->GetConnectorLocation(false)));
            PortJson->SetObjectField(TEXT("normal"), VectorJson(Connection->GetConnectorNormal()));
            Ports.Add(MakeShared<FJsonValueObject>(PortJson));
        }
        Item->SetArrayField(TEXT("ports"), Ports);
        if (AFGBuildableConveyorLift* Lift = Cast<AFGBuildableConveyorLift>(Buildable))
        {
            UFGFactoryConnectionComponent* Bottom = nullptr;
            UFGFactoryConnectionComponent* Top = nullptr;
            for (UFGFactoryConnectionComponent* Connection : Connections)
            {
                if (!Connection) continue;
                if (!Bottom || Connection->GetConnectorLocation(false).Z < Bottom->GetConnectorLocation(false).Z) Bottom = Connection;
                if (!Top || Connection->GetConnectorLocation(false).Z > Top->GetConnectorLocation(false).Z) Top = Connection;
            }
            auto EndpointJson = [&VectorJson](UFGFactoryConnectionComponent* Connection)
            {
                const TSharedRef<FJsonObject> Endpoint = MakeShared<FJsonObject>();
                if (!Connection) return Endpoint;
                const EFactoryConnectionDirection Direction = Connection->GetDirection();
                Endpoint->SetStringField(TEXT("component"), Connection->GetName());
                Endpoint->SetStringField(TEXT("role"), Direction == EFactoryConnectionDirection::FCD_INPUT ? TEXT("input") :
                    Direction == EFactoryConnectionDirection::FCD_OUTPUT ? TEXT("output") : TEXT("any"));
                Endpoint->SetObjectField(TEXT("position"), VectorJson(Connection->GetConnectorLocation(false)));
                Endpoint->SetObjectField(TEXT("facing"), VectorJson(Connection->GetConnectorNormal()));
                Endpoint->SetBoolField(TEXT("connected"), Connection->IsConnected());
                return Endpoint;
            };
            const TSharedRef<FJsonObject> LiftJson = MakeShared<FJsonObject>();
            LiftJson->SetObjectField(TEXT("bottom"), EndpointJson(Bottom));
            LiftJson->SetObjectField(TEXT("top"), EndpointJson(Top));
            LiftJson->SetNumberField(TEXT("heightCm"), Lift->GetHeight());
            LiftJson->SetStringField(TEXT("flowDirection"), Lift->IsFlowUpwards() ? TEXT("bottom_to_top") : TEXT("top_to_bottom"));
            Item->SetObjectField(TEXT("lift"), LiftJson);
        }
        if (Fields == EBuildableFields::Ports) return Item;

        Item->SetBoolField(TEXT("playingBuildEffect"), Buildable->IsPlayingBuildEffect());
        Item->SetBoolField(TEXT("hasBuildEffectActor"), Buildable->GetBuildEffectActor() != nullptr);
        Item->SetObjectField(TEXT("customization"), FactoryCustomizationJson(Buildable->GetCustomizationData_Native()));
        TArray<TSharedPtr<FJsonValue>> GenericConnections;
        auto AddConnectionTransform = [&VectorJson, Buildable](const TSharedRef<FJsonObject>& Json, USceneComponent* Component, const FVector& WorldPosition, const FVector& WorldFacing)
        {
            Json->SetStringField(TEXT("component"), Component->GetName());
            Json->SetStringField(TEXT("componentClass"), Component->GetClass()->GetPathName());
            Json->SetObjectField(TEXT("worldPosition"), VectorJson(WorldPosition));
            Json->SetObjectField(TEXT("worldFacing"), VectorJson(WorldFacing));
            Json->SetObjectField(TEXT("localPosition"), VectorJson(Buildable->GetActorTransform().InverseTransformPosition(WorldPosition)));
            Json->SetObjectField(TEXT("localFacing"), VectorJson(Buildable->GetActorTransform().InverseTransformVectorNoScale(WorldFacing)));
        };
        const TSet<const UActorComponent*> FuelPorts = ConnectionArray(Buildable, TEXT("mFuelConnections")), CargoPorts = ConnectionArray(Buildable, TEXT("mStorageInputConnections"));
        const FObjectPropertyBase* FuelInventoryProperty = FindFProperty<FObjectPropertyBase>(Buildable->GetClass(), TEXT("mFuelInventory"));
        const UFGInventoryComponent* FuelInventory = FuelInventoryProperty ? Cast<UFGInventoryComponent>(FuelInventoryProperty->GetObjectPropertyValue_InContainer(Buildable)) : nullptr;
        const TArray<UFGFactoryConnectionComponent*> AllConnections(Connections);
        for (UFGFactoryConnectionComponent* Connection : Connections)
        {
            if (!Connection) continue;
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("kind"), TEXT("conveyor"));
            AddPortSemantics(Json, Buildable, Connection, AllConnections, FuelPorts, CargoPorts, FuelInventory);
            const EFactoryConnectionDirection Direction = Connection->GetDirection();
            Json->SetStringField(TEXT("direction"), Direction == EFactoryConnectionDirection::FCD_INPUT ? TEXT("input") :
                Direction == EFactoryConnectionDirection::FCD_OUTPUT ? TEXT("output") :
                Direction == EFactoryConnectionDirection::FCD_ANY ? TEXT("any") : TEXT("snap_only"));
            Json->SetBoolField(TEXT("connected"), Connection->IsConnected());
            AddConnectionTransform(Json, Connection, Connection->GetConnectorLocation(false), Connection->GetConnectorNormal());
            GenericConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        TInlineComponentArray<UFGPipeConnectionComponentBase*> PipeConnections(Buildable);
        for (UFGPipeConnectionComponentBase* Connection : PipeConnections)
        {
            if (!Connection) continue;
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("kind"), Connection->IsA<UFGPipeConnectionComponentHyper>() ? TEXT("hypertube") : TEXT("fluid"));
            if (!Connection->IsA<UFGPipeConnectionComponentHyper>()) Json->SetStringField(TEXT("accepts"), TEXT("fluid"));
            const EPipeConnectionType Type = Connection->GetPipeConnectionType();
            Json->SetStringField(TEXT("direction"), Type == EPipeConnectionType::PCT_CONSUMER ? TEXT("input") :
                Type == EPipeConnectionType::PCT_PRODUCER ? TEXT("output") :
                Type == EPipeConnectionType::PCT_SNAP_ONLY ? TEXT("snap_only") : TEXT("any"));
            Json->SetBoolField(TEXT("connected"), Connection->IsConnected());
            Json->SetNumberField(TEXT("clearanceCm"), Connection->GetConnectorClearance());
            if (UFGPipeConnectionComponent* Fluid = Cast<UFGPipeConnectionComponent>(Connection))
            {
                Json->SetNumberField(TEXT("networkId"), Fluid->GetPipeNetworkID());
                if (Fluid->GetFluidDescriptor()) Json->SetStringField(TEXT("fluidDescriptor"), Fluid->GetFluidDescriptor()->GetPathName());
            }
            if (UFGPipeConnectionComponentBase* Other = Connection->GetConnection())
            {
                if (AActor* OtherOwner = Other->GetOwner()) Json->SetStringField(TEXT("connectedBuildableId"), OtherOwner->GetName());
                Json->SetStringField(TEXT("connectedComponent"), Other->GetName());
            }
            AddConnectionTransform(Json, Connection, Connection->GetConnectorLocation(false), Connection->GetConnectorNormal());
            GenericConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        TInlineComponentArray<UFGCircuitConnectionComponent*> CircuitConnections(Buildable);
        for (UFGCircuitConnectionComponent* Connection : CircuitConnections)
        {
            if (!Connection) continue;
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("kind"), Connection->IsA<UFGPowerConnectionComponent>() ? TEXT("power") : TEXT("circuit"));
            Json->SetBoolField(TEXT("connected"), Connection->IsConnected() != 0);
            Json->SetBoolField(TEXT("hidden"), Connection->IsHidden());
            Json->SetNumberField(TEXT("circuitId"), Connection->GetCircuitID());
            Json->SetNumberField(TEXT("connectionCount"), Connection->GetNumConnections());
            Json->SetNumberField(TEXT("maxConnections"), Connection->GetMaxNumConnections());
            Json->SetNumberField(TEXT("freeConnections"), Connection->GetNumFreeConnections());
            AddConnectionTransform(Json, Connection, Connection->GetComponentLocation(), Connection->GetForwardVector());
            GenericConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        TInlineComponentArray<UFGRailroadTrackConnectionComponent*> RailConnections(Buildable);
        for (UFGRailroadTrackConnectionComponent* Connection : RailConnections)
        {
            if (!Connection) continue;
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("kind"), TEXT("rail"));
            Json->SetBoolField(TEXT("connected"), Connection->IsConnected());
            Json->SetNumberField(TEXT("connectionCount"), Connection->GetConnections().Num());
            Json->SetNumberField(TEXT("switchPositions"), Connection->GetNumSwitchPositions());
            Json->SetNumberField(TEXT("switchPosition"), Connection->GetSwitchPosition());
            AddConnectionTransform(Json, Connection, Connection->GetConnectorLocation(), Connection->GetConnectorNormal());
            GenericConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        TInlineComponentArray<UFGTrainPlatformConnection*> PlatformConnections(Buildable);
        for (UFGTrainPlatformConnection* Connection : PlatformConnections)
        {
            if (!Connection) continue;
            const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("kind"), TEXT("train_platform"));
            Json->SetBoolField(TEXT("connected"), Connection->IsConnected());
            const ETrainPlatformConnectionType Type = Connection->GetConnectionType();
            Json->SetStringField(TEXT("direction"), Type == ETrainPlatformConnectionType::ETPC_In ? TEXT("input") :
                Type == ETrainPlatformConnectionType::ETPC_Out ? TEXT("output") : TEXT("neutral"));
            AddConnectionTransform(Json, Connection, Connection->GetComponentLocation(), Connection->GetForwardVector());
            GenericConnections.Add(MakeShared<FJsonValueObject>(Json));
        }
        Item->SetArrayField(TEXT("connections"), GenericConnections);
        // #17: a spline buildable's own geometry (belts, pipes, hypertubes, rails, spline roads), so a route's shape can be
        // compared after a save and reload: each point's world location and tangents, at most MaximumSplinePointsListed.
        if (Fields == EBuildableFields::Full)
        {
            if (const USplineComponent* Spline = Buildable->FindComponentByClass<USplineComponent>())
            {
                TArray<TSharedPtr<FJsonValue>> Points;
                const int32 Count = FMath::Min(Spline->GetNumberOfSplinePoints(), MaximumSplinePointsListed);
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    const TSharedRef<FJsonObject> Point = MakeShared<FJsonObject>();
                    Point->SetObjectField(TEXT("location"), BridgeVectorJson(Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::World)));
                    Point->SetObjectField(TEXT("arriveTangent"), BridgeVectorJson(Spline->GetArriveTangentAtSplinePoint(Index, ESplineCoordinateSpace::World)));
                    Point->SetObjectField(TEXT("leaveTangent"), BridgeVectorJson(Spline->GetLeaveTangentAtSplinePoint(Index, ESplineCoordinateSpace::World)));
                    Points.Add(MakeShared<FJsonValueObject>(Point));
                }
                Item->SetArrayField(TEXT("splinePoints"), Points);
                Item->SetNumberField(TEXT("splineLength"), Spline->GetSplineLength());
            }
        }
        return Item;
    }

    TSharedRef<FJsonObject> LightweightBuildableJson(UClass* Class, int32 Index, const FRuntimeBuildableInstanceData& Data, EBuildableFields Fields)
    {
        auto VectorJson = [](const FVector& V) { return BridgeVectorJson(V); };
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("id"), FString::Printf(TEXT("lw|%s|%d"), *Class->GetPathName(), Index));
        Item->SetStringField(TEXT("className"), Class->GetName());
        Item->SetStringField(TEXT("classPath"), Class->GetPathName());
        if (Data.BuiltWithRecipe) Item->SetStringField(TEXT("recipePath"), Data.BuiltWithRecipe->GetPathName());
        Item->SetBoolField(TEXT("lightweight"), true);
        Item->SetNumberField(TEXT("lightweightIndex"), Index);
        const FVector Location = Data.Transform.GetLocation(); const FRotator Rotation = Data.Transform.Rotator();
        Item->SetObjectField(TEXT("location"), VectorJson(Location));
        Item->SetObjectField(TEXT("position"), VectorJson(Location));
        const TSharedRef<FJsonObject> RotationJson = BridgeRotationJson(Rotation);
        Item->SetObjectField(TEXT("rotation"), RotationJson);
        // The record's box is local to its transform. Report world bounds only when the box is
        // valid; an unset box is omitted rather than reported as a degenerate point.
        if (Data.BoundingBox.IsValid)
        {
            const FBox WorldBounds = Data.BoundingBox.TransformBy(Data.Transform);
            Item->SetObjectField(TEXT("boundsMin"), VectorJson(WorldBounds.Min));
            Item->SetObjectField(TEXT("boundsMax"), VectorJson(WorldBounds.Max));
        }
        if (Fields == EBuildableFields::Compact) return Item;
        Item->SetArrayField(TEXT("ports"), TArray<TSharedPtr<FJsonValue>>{});
        if (Fields == EBuildableFields::Ports) return Item;
        Item->SetObjectField(TEXT("customization"), FactoryCustomizationJson(Data.CustomizationData));
        Item->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>{});
        return Item;
    }

    uint32 BuildableFingerprint(const FString& ClassPath, const FTransform& Transform, uint32 ConnectionBits)
    {
        const FVector L = Transform.GetLocation(); const FRotator R = Transform.Rotator();
        const FString Key = FString::Printf(TEXT("%s|%lld|%lld|%lld|%lld|%lld|%lld|%u"), *ClassPath,
            FMath::RoundToInt64(L.X * 10.0), FMath::RoundToInt64(L.Y * 10.0), FMath::RoundToInt64(L.Z * 10.0),
            FMath::RoundToInt64(R.Yaw * 100.0), FMath::RoundToInt64(R.Pitch * 100.0), FMath::RoundToInt64(R.Roll * 100.0), ConnectionBits);
        return FCrc::StrCrc32(*Key);
    }

    uint32 ActorConnectionBits(AFGBuildable* Buildable)
    {
        uint32 Bits = 2166136261u;
        auto Mix = [&Bits](uint32 Value) { Bits = (Bits ^ Value) * 16777619u; };
        TInlineComponentArray<UFGFactoryConnectionComponent*> Factory(Buildable);
        for (UFGFactoryConnectionComponent* Connection : Factory) if (Connection) Mix(Connection->IsConnected() ? 1u : 2u);
        TInlineComponentArray<UFGPipeConnectionComponentBase*> Pipes(Buildable);
        for (UFGPipeConnectionComponentBase* Connection : Pipes) if (Connection) Mix(Connection->IsConnected() ? 3u : 4u);
        TInlineComponentArray<UFGCircuitConnectionComponent*> Circuits(Buildable);
        for (UFGCircuitConnectionComponent* Connection : Circuits) if (Connection) Mix(16u + static_cast<uint32>(Connection->GetNumConnections()));
        return Bits;
    }

    bool ParseNumberList(const FString& Text, int32 Expected, TArray<double>& Out)
    {
        TArray<FString> Parts; Text.ParseIntoArray(Parts, TEXT(","), false);
        if (Parts.Num() != Expected) return false;
        Out.Reset();
        for (FString Part : Parts)
        {
            Part.TrimStartAndEndInline();
            if (Part.IsEmpty() || !Part.IsNumeric()) return false;
            const double Value = FCString::Atod(*Part);
            if (!FMath::IsFinite(Value)) return false;
            Out.Add(Value);
        }
        return true;
    }

    const TCHAR* BridgeMachineState(UWorld* World, AFGBuildableFactory* Machine)
    {
        AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine);
        UFGPowerInfoComponent* PowerInfo = Machine->GetPowerInfo();
        const bool bHasPower = PowerInfo ? PowerInfo->HasPower() : false;
        bool bInputsPresent = true;
        if (Manufacturer && Manufacturer->GetCurrentRecipe() && Manufacturer->GetInputInventory())
            for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(World, Manufacturer->GetCurrentRecipe()))
                if (Ingredient.ItemClass && Manufacturer->GetInputInventory()->GetNumItems(Ingredient.ItemClass) < Ingredient.Amount) bInputsPresent = false;
        return Machine->IsProductionPaused() ? TEXT("standby") : !bHasPower ? TEXT("unpowered") : Machine->IsProducing() ? TEXT("producing")
            : (Manufacturer && !Manufacturer->GetCurrentRecipe()) ? TEXT("idle")
            : (Manufacturer && !bInputsPresent) ? TEXT("input_starved")
            : Manufacturer ? TEXT("output_blocked") : TEXT("idle");
    }
}

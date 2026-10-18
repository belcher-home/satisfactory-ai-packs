#include "BridgeInternal.h"

// Bearer-token authentication: the loopback token file and request authorization.
// Split from the single MKIBridge.cpp without behavior changes (#44).

bool FMKIBridgeModule::IsAuthorized(const FHttpServerRequest& Request) const
{
    const TArray<FString>* Values = Request.Headers.Find(TEXT("Authorization"));
    if (!Values)
    {
        // Header parsing normally preserves this spelling, but accept lowercase
        // to avoid coupling authentication to a client's header casing.
        Values = Request.Headers.Find(TEXT("authorization"));
    }
    if (!Values || Values->IsEmpty())
    {
        return false;
    }

    FString Supplied = (*Values)[0];
    Supplied.RemoveFromStart(TEXT("Bearer "), ESearchCase::IgnoreCase);
    if (AuthenticationToken.IsEmpty()) return false;
    // Constant-time comparison: every character of the longer string is visited and differences are accumulated,
    // so response timing does not reveal how much of a guessed token matched.
    const int32 Length = FMath::Max(Supplied.Len(), AuthenticationToken.Len());
    uint32 Difference = static_cast<uint32>(Supplied.Len() ^ AuthenticationToken.Len());
    for (int32 Index = 0; Index < Length; ++Index)
    {
        const uint32 A = Index < Supplied.Len() ? static_cast<uint32>(Supplied[Index]) : 0u;
        const uint32 B = Index < AuthenticationToken.Len() ? static_cast<uint32>(AuthenticationToken[Index]) : 0u;
        Difference |= A ^ B;
    }
    return Difference == 0;
}

bool FMKIBridgeModule::LoadOrCreateAuthenticationToken()
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MKIBridge"));
    const FString TokenPath = FPaths::Combine(Directory, TEXT("token.txt"));
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    // Earlier names of this mod kept the token, scopes, audit log and receipts under Saved/SatisfactoryMKIBridge (an interim
    // name) or Saved/SatisfactoryAIBridge (0.4.4 and earlier). The first start under this name copies the newest of them
    // over, so clients keep their token and the owner's scopes stand. The old directories are left in place.
    for (const TCHAR* LegacyName : { TEXT("SatisfactoryMKIBridge"), TEXT("SatisfactoryAIBridge") })
    {
        const FString LegacyDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), LegacyName);
        if (PlatformFile.DirectoryExists(*Directory) || !PlatformFile.DirectoryExists(*LegacyDirectory)) continue;
        if (PlatformFile.CopyDirectoryTree(*Directory, *LegacyDirectory, false))
        {
            UE_LOG(LogMKIBridge, Display, TEXT("Copied the bridge state from %s to %s"), *LegacyDirectory, *Directory);
        }
        else
        {
            UE_LOG(LogMKIBridge, Warning, TEXT("Could not copy the bridge state from %s"), *LegacyDirectory);
        }
    }
    PlatformFile.CreateDirectoryTree(*Directory);

    if (FFileHelper::LoadFileToString(AuthenticationToken, *TokenPath))
    {
        AuthenticationToken.TrimStartAndEndInline();
    }
    const bool bNewInstall = AuthenticationToken.IsEmpty();
    if (AuthenticationToken.IsEmpty())
    {
        AuthenticationToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (!FFileHelper::SaveStringToFile(AuthenticationToken, *TokenPath))
        {
            AuthenticationToken.Empty();
            return false;
        }
    }
    // #7: a new installation is read-only until its owner grants scopes in scopes.txt. An installation that already had a
    // token keeps the full access it had before scopes existed, written down so the owner can narrow it.
    const FString ScopesPath = FPaths::Combine(Directory, TEXT("scopes.txt"));
    if (!PlatformFile.FileExists(*ScopesPath))
    {
        FFileHelper::SaveStringToFile(bNewInstall ? FString(TEXT("observe\n")) : FString(TEXT("all\n")), *ScopesPath);
        UE_LOG(LogMKIBridge, Display, TEXT("Wrote %s (%s); edit it to grant: build connect configure interact move dismantle admin, or all"), *ScopesPath, bNewInstall ? TEXT("read-only, new installation") : TEXT("all, existing installation"));
    }
    UE_LOG(LogMKIBridge, Display, TEXT("Granted scopes: %s"), *FString::Join(GrantedScopes().Array(), TEXT(" ")));

    UE_LOG(LogMKIBridge, Display, TEXT("Authentication token ready at %s"), *TokenPath);
    return true;
}

/** Per-second request limits (#8): generous for a planner's bursts (probe chunks, listings), finite for a runaway loop. */
static constexpr int32 MaximumReadsPerSecond = 300;
static constexpr int32 MaximumMutationsPerSecond = 120; // a 64-operation batch dispatches each operation as a mutation

bool FMKIBridgeModule::AdmitRequest(bool bMutation, int32& RetryAfterMs)
{
    const double Now = FPlatformTime::Seconds();
    if (Now - RateWindowStart >= 1.0) { RateWindowStart = Now; RateWindowReads = 0; RateWindowMutations = 0; }
    int32& Count = bMutation ? RateWindowMutations : RateWindowReads;
    if (Count >= (bMutation ? MaximumMutationsPerSecond : MaximumReadsPerSecond)) { RetryAfterMs = FMath::Max(1, FMath::CeilToInt((1.0 - (Now - RateWindowStart)) * 1000.0)); return false; }
    ++Count;
    return true;
}

void FMKIBridgeModule::AppendAudit(const FString& Route, const FString& RequestId, int32 Status)
{
    const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MKIBridge"), TEXT("audit.log"));
    const FString Line = FString::Printf(TEXT("%s\t%s\t%s\t%d\n"), *FDateTime::UtcNow().ToIso8601(), *Route, RequestId.IsEmpty() ? TEXT("-") : *RequestId.Left(128), Status);
    FFileHelper::SaveStringToFile(Line, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

FString FMKIBridgeModule::RequiredScope(const FString& Path)
{
    if (Path == TEXT("/v1/place") || Path == TEXT("/v1/place-spline-road") || Path == TEXT("/v1/build-gun") || Path == TEXT("/v1/batch")) return TEXT("build");
    if (Path == TEXT("/v1/connect")) return TEXT("connect");
    if (Path == TEXT("/v1/set-machine-recipe") || Path == TEXT("/v1/machine-clock") || Path == TEXT("/v1/splitter-rules") || Path == TEXT("/v1/docking-station") || Path == TEXT("/v1/vehicle-route") || Path == TEXT("/v1/game-rules/fuel") || Path == TEXT("/v1/vehicle-paths/revalidate")) return TEXT("configure");
    if (Path == TEXT("/v1/inventory/transfer") || Path == TEXT("/v1/player/respawn")) return TEXT("interact");
    if (Path == TEXT("/v1/player-transform")) return TEXT("move");
    if (Path == TEXT("/v1/dismantle")) return TEXT("dismantle");
    return TEXT("admin");
}

TSet<FString> FMKIBridgeModule::GrantedScopes() const
{
    FString Text; TSet<FString> Scopes;
    if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MKIBridge"), TEXT("scopes.txt")))) return Scopes;
    TArray<FString> Words; Text.Replace(TEXT(","), TEXT(" ")).ParseIntoArrayWS(Words);
    for (const FString& Word : Words) Scopes.Add(Word.ToLower());
    return Scopes;
}

bool FMKIBridgeModule::HasScope(const FString& Scope) const
{
    const TSet<FString> Scopes = GrantedScopes();
    return Scopes.Contains(TEXT("all")) || Scopes.Contains(Scope);
}

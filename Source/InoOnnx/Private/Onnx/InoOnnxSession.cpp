// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Onnx/InoOnnxSession.h"

#include "InoOnnxInternal.h"
#include "InoOnnx.h"
#include "InoOnnx.h"  // for LogInoOnnx

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
    // Needed for the OrtDmlApi vtable + OrtSessionOptionsAppendExecutionProvider_DML.
    // Header transitively #includes <d3d12.h> and <DirectML.h>, both present
    // in Windows SDK 22621+ on any machine that can build UE 5.7. No extra
    // third-party header dependency.
    //
    // UE's own <Windows.h> inclusion (via CoreMinimal) #defines OPTIONAL
    // which dml_provider_factory.h's "#undef OPTIONAL" line handles. Order
    // matters: include it AFTER core UE headers so the undef is effective.
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include "dml_provider_factory.h"
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
    using namespace InoOnnx;
    using namespace InoOnnx::Internal;

    // ========================================================================
    //  Small helpers
    // ========================================================================

    bool CheckStatus(OrtStatus* Status, const TCHAR* Op, FString* OutError = nullptr)
    {
        return CheckOrtStatus(Status, Op, OutError);
    }

    /**
     * Build a fresh OrtSessionOptions from FInoOnnxSessionOptions and
     * register the requested execution providers. Returns the OrtSessionOptions*
     * (caller owns; release via OrtApi::ReleaseSessionOptions) and
     * populates OutRegistered with the providers that actually registered.
     *
     * On error returns nullptr and writes OutError.
     */
    OrtSessionOptions* BuildOrtSessionOptions(
        const OrtApi* Api,
        const FInoOnnxSessionOptions& Options,
        TArray<EInoOnnxProvider>& OutRegistered,
        FString* OutError)
    {
        OrtSessionOptions* Opts = nullptr;
        if (!CheckStatus(Api->CreateSessionOptions(&Opts),
                         TEXT("CreateSessionOptions"), OutError))
        {
            return nullptr;
        }

        // Graph optimization level.
        if (!CheckStatus(
                Api->SetSessionGraphOptimizationLevel(
                    Opts, OptLevelToOrt(Options.GraphOptimization)),
                TEXT("SetSessionGraphOptimizationLevel"), OutError))
        {
            Api->ReleaseSessionOptions(Opts);
            return nullptr;
        }

        // Log-severity override. -1 = use ORT default (WARNING). Lower
        // values are chattier; 0 (VERBOSE) exposes DML kernel fallback
        // decisions, graph-transformer rewrites, and the exact op-desc
        // field that failed validation when DML rejects something.
        // Indispensable when diagnosing DML op-support issues.
        if (Options.LogSeverityLevel >= 0)
        {
            if (!CheckStatus(
                    Api->SetSessionLogSeverityLevel(Opts, Options.LogSeverityLevel),
                    TEXT("SetSessionLogSeverityLevel"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }

        // Thread counts (0 = ORT default — don't set explicitly).
        if (Options.IntraOpThreadCount > 0)
        {
            if (!CheckStatus(
                    Api->SetIntraOpNumThreads(Opts, Options.IntraOpThreadCount),
                    TEXT("SetIntraOpNumThreads"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }
        if (Options.InterOpThreadCount > 0)
        {
            if (!CheckStatus(
                    Api->SetInterOpNumThreads(Opts, Options.InterOpThreadCount),
                    TEXT("SetInterOpNumThreads"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }

        // Custom session-config entries.
        for (const auto& Pair : Options.SessionConfig)
        {
            const FTCHARToUTF8 Key(*Pair.Key);
            const FTCHARToUTF8 Val(*Pair.Value);
            if (!CheckStatus(
                    Api->AddSessionConfigEntry(Opts, Key.Get(), Val.Get()),
                    TEXT("AddSessionConfigEntry"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }

        // Profiling.
        if (Options.bEnableProfiling)
        {
            // ORT takes a file prefix; it appends a timestamp + ".json".
            // We use an absolute path under Saved/Logs/ so the output is
            // easy to find in both editor and packaged builds.
            const FString ProfilePrefix = FPaths::Combine(
                FPaths::ProjectSavedDir(),
                TEXT("Logs"),
                TEXT("onnxruntime_profile_"));

#if PLATFORM_WINDOWS
            // Windows wants wide-char strings for file paths in the ORT API.
            if (!CheckStatus(
                    Api->EnableProfiling(Opts, (const ORTCHAR_T*)*ProfilePrefix),
                    TEXT("EnableProfiling"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
#else
            const FTCHARToUTF8 ProfileUtf8(*ProfilePrefix);
            if (!CheckStatus(
                    Api->EnableProfiling(Opts, ProfileUtf8.Get()),
                    TEXT("EnableProfiling"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
#endif
        }

        // Optimized model output (optional).
        if (!Options.OptimizedModelOutputPath.IsEmpty())
        {
#if PLATFORM_WINDOWS
            if (!CheckStatus(
                    Api->SetOptimizedModelFilePath(
                        Opts, (const ORTCHAR_T*)*Options.OptimizedModelOutputPath),
                    TEXT("SetOptimizedModelFilePath"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
#else
            const FTCHARToUTF8 PathUtf8(*Options.OptimizedModelOutputPath);
            if (!CheckStatus(
                    Api->SetOptimizedModelFilePath(Opts, PathUtf8.Get()),
                    TEXT("SetOptimizedModelFilePath"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
#endif
        }

        // Apply user-requested execution mode + memory tunables.
        const ExecutionMode UserExecMode =
            (Options.ExecutionMode == EInoOnnxExecutionMode::Parallel)
                ? ORT_PARALLEL : ORT_SEQUENTIAL;
        if (!CheckStatus(Api->SetSessionExecutionMode(Opts, UserExecMode),
                         TEXT("SetSessionExecutionMode"), OutError))
        {
            Api->ReleaseSessionOptions(Opts);
            return nullptr;
        }

        if (Options.bEnableMemPattern)
        {
            if (!CheckStatus(Api->EnableMemPattern(Opts),
                             TEXT("EnableMemPattern"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }
        else
        {
            if (!CheckStatus(Api->DisableMemPattern(Opts),
                             TEXT("DisableMemPattern"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }

        if (Options.bEnableCpuMemArena)
        {
            if (!CheckStatus(Api->EnableCpuMemArena(Opts),
                             TEXT("EnableCpuMemArena"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }
        else
        {
            if (!CheckStatus(Api->DisableCpuMemArena(Opts),
                             TEXT("DisableCpuMemArena"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }

        if (!Options.SessionLogId.IsEmpty())
        {
            const FTCHARToUTF8 Id(*Options.SessionLogId);
            if (!CheckStatus(Api->SetSessionLogId(Opts, Id.Get()),
                             TEXT("SetSessionLogId"), OutError))
            {
                Api->ReleaseSessionOptions(Opts);
                return nullptr;
            }
        }

        // DirectML mandates two settings whenever DML is the effective
        // provider. Microsoft's DML EP docs require:
        //   1. DisableMemPattern — DML uses D3D12 resource binding that
        //      conflicts with mem-pattern's contiguous-host assumption;
        //      without this, DML sessions crash or produce wrong output
        //      on the second inference.
        //   2. ExecutionMode::ORT_SEQUENTIAL — DML doesn't support
        //      parallel multi-op graph execution.
        //
        // Applied unconditionally if DML is in the provider list, even
        // before we know whether DML registration will succeed. If the
        // user requested incompatible settings we override + log; the
        // alternative would be a two-pass register-then-configure flow,
        // and these settings are cheap so single-pass wins.
        const bool bDmlRequested =
            Options.ExecutionProviders.Contains(EInoOnnxProvider::DirectMl);
        if (bDmlRequested)
        {
            if (Options.ExecutionMode != EInoOnnxExecutionMode::Sequential)
            {
                UE_LOG(LogInoOnnx, Warning,
                       TEXT("Onnx: Session: DML requires Sequential execution; ")
                       TEXT("overriding user-requested Parallel mode"));
                CheckStatus(Api->SetSessionExecutionMode(Opts, ORT_SEQUENTIAL),
                            TEXT("SetSessionExecutionMode (DML override)"), nullptr);
            }
            if (Options.bEnableMemPattern)
            {
                UE_LOG(LogInoOnnx, Warning,
                       TEXT("Onnx: Session: DML requires MemPattern off; ")
                       TEXT("overriding user-requested bEnableMemPattern=true"));
                CheckStatus(Api->DisableMemPattern(Opts),
                            TEXT("DisableMemPattern (DML override)"), nullptr);
            }
        }

        // Register execution providers in priority order. Each provider
        // that fails to register is skipped (logged as warning); we
        // keep going so the caller still gets a functional session as
        // long as at least one provider succeeds.
        //
        // CPU is always registered by ORT as the baseline, so even if
        // every requested provider fails, Run() still works — just
        // slower than it might have been.
        //
        // Defensive de-dup: ORT errors out on the second registration
        // of the same provider with "Provider X has already been
        // registered" and the whole session creation aborts — even
        // though the user's *intent* (priority list) is satisfied by
        // the first registration. Some callers thread provider lists
        // through fallback chains and accidentally repeat an entry
        // (e.g. [Xnnpack, Nnapi, Xnnpack, Cpu] where Xnnpack is also
        // a fallback after NNAPI fails). Skip second-and-later
        // appearances with a Verbose log instead of failing.
        TSet<EInoOnnxProvider> SeenProviders;
        OutRegistered.Reset();
        for (const EInoOnnxProvider Provider : Options.ExecutionProviders)
        {
            const TCHAR* ProviderName = ProviderToString(Provider);

            bool bAlreadySeen = false;
            SeenProviders.Add(Provider, &bAlreadySeen);
            if (bAlreadySeen)
            {
                UE_LOG(LogInoOnnx, Verbose,
                       TEXT("Onnx: Provider: %s appears more than once in the priority list; ")
                       TEXT("skipping duplicate (ORT rejects re-registration of the same EP)."),
                       ProviderName);
                continue;
            }

            OrtStatus* RegStatus = nullptr;

            switch (Provider)
            {
                case EInoOnnxProvider::Cpu:
                    // CPU provider is always registered implicitly. Adding
                    // it explicitly via the "OrtSessionOptionsAppendExecutionProvider_CPU"
                    // extension API has no effect other than enabling the
                    // per-thread arena allocator, which ORT uses by default
                    // anyway. Treat as always-succeeds.
                    OutRegistered.Add(Provider);
                    UE_LOG(LogInoOnnx, Log,
                           TEXT("Onnx: Provider: registered %s (implicit fallback)"),
                           ProviderName);
                    continue;

                case EInoOnnxProvider::Xnnpack:
                    RegStatus = Api->SessionOptionsAppendExecutionProvider(
                        Opts, "XNNPACK", /*keys=*/nullptr, /*vals=*/nullptr, /*num_entries=*/0);
                    break;

                case EInoOnnxProvider::Nnapi:
                {
                    // Build provider-options key/val arrays from the
                    // typed NNAPI fields. Empty arrays = NNAPI defaults
                    // (FP32, NHWC, CPU fallback enabled).
                    TArray<const char*> NnapiKeys;
                    TArray<const char*> NnapiVals;
                    if (Options.bNnapiUseFp16)
                    {
                        NnapiKeys.Add("use_fp16");     NnapiVals.Add("1");
                    }
                    if (Options.bNnapiUseNchw)
                    {
                        NnapiKeys.Add("use_nchw");     NnapiVals.Add("1");
                    }
                    if (Options.bNnapiCpuDisabled)
                    {
                        NnapiKeys.Add("cpu_disabled"); NnapiVals.Add("1");
                    }

                    RegStatus = Api->SessionOptionsAppendExecutionProvider(
                        Opts, "NNAPI",
                        NnapiKeys.GetData(),
                        NnapiVals.GetData(),
                        (size_t)NnapiKeys.Num());
                }
                break;

                case EInoOnnxProvider::WebGpu:
                    RegStatus = Api->SessionOptionsAppendExecutionProvider(
                        Opts, "WebGPU", nullptr, nullptr, 0);
                    break;

                case EInoOnnxProvider::DirectMl:
#if PLATFORM_WINDOWS
                    // DML uses a dedicated OrtDmlApi vtable, NOT the generic
                    // string-based SessionOptionsAppendExecutionProvider path
                    // (which Microsoft does not document for "DmlExecutionProvider"
                    // and which silently produces a CPU-only session in
                    // practice). Fetch the vtable via GetExecutionProviderApi
                    // and call SessionOptionsAppendExecutionProvider_DML
                    // with the caller's adapter index.
                    {
                        const OrtDmlApi* DmlApi = nullptr;
                        OrtStatus* VTableStatus = Api->GetExecutionProviderApi(
                            "DML", ORT_API_VERSION,
                            reinterpret_cast<const void**>(&DmlApi));

                        if (VTableStatus != nullptr)
                        {
                            // GetExecutionProviderApi returning an error means
                            // the DML EP isn't compiled into this ORT build.
                            // Log + skip; the session still builds with
                            // remaining providers (CPU fallback).
                            const char* ErrMsg = Api->GetErrorMessage(VTableStatus);
                            UE_LOG(LogInoOnnx, Warning,
                                   TEXT("Onnx: Provider: GetExecutionProviderApi(\"DML\") FAILED: %s. ")
                                   TEXT("The loaded ONNX Runtime does not include the DML EP. ")
                                   TEXT("Re-run Plugins/InoOnnx/OnnxRuntime/scripts/setup-onnxruntime.ps1 ")
                                   TEXT("to stage the DirectML-flavored ORT build."),
                                   UTF8_TO_TCHAR(ErrMsg));
                            Api->ReleaseStatus(VTableStatus);
                            continue;
                        }
                        if (DmlApi == nullptr)
                        {
                            UE_LOG(LogInoOnnx, Warning,
                                   TEXT("Onnx: Provider: GetExecutionProviderApi(\"DML\") returned null vtable; skipping DirectML"));
                            continue;
                        }

                        UE_LOG(LogInoOnnx, Verbose,
                               TEXT("Onnx: Provider: DirectML vtable resolved; appending DML EP (adapter=%d)"),
                               Options.DirectMlAdapterIndex);

                        RegStatus = DmlApi->SessionOptionsAppendExecutionProvider_DML(
                            Opts, Options.DirectMlAdapterIndex);
                    }
                    break;
#else
                    UE_LOG(LogInoOnnx, Warning,
                           TEXT("Onnx: Provider: DirectML is Windows-only; skipping on this platform"));
                    continue;
#endif

                case EInoOnnxProvider::Cuda:
                    RegStatus = Api->SessionOptionsAppendExecutionProvider(
                        Opts, "CUDAExecutionProvider", nullptr, nullptr, 0);
                    break;

                case EInoOnnxProvider::TensorRt:
                    RegStatus = Api->SessionOptionsAppendExecutionProvider(
                        Opts, "TensorrtExecutionProvider", nullptr, nullptr, 0);
                    break;

                default:
                    UE_LOG(LogInoOnnx, Warning,
                           TEXT("Onnx: Provider: unknown provider enum value %d; skipping"),
                           (int32)Provider);
                    continue;
            }

            if (RegStatus == nullptr)
            {
                OutRegistered.Add(Provider);
                if (Provider == EInoOnnxProvider::DirectMl)
                {
                    UE_LOG(LogInoOnnx, Log,
                           TEXT("Onnx: Provider: registered %s (adapter=%d)"),
                           ProviderName, Options.DirectMlAdapterIndex);
                }
                else
                {
                    UE_LOG(LogInoOnnx, Log,
                           TEXT("Onnx: Provider: registered %s"),
                           ProviderName);
                }
            }
            else
            {
                // Registration failed. Log at warning and continue —
                // the session can still be built with remaining providers.
                const char* ErrMsg = Api->GetErrorMessage(RegStatus);
                UE_LOG(LogInoOnnx, Warning,
                       TEXT("Onnx: Provider: %s FAILED to register (will fall back to remaining providers): %s"),
                       ProviderName, UTF8_TO_TCHAR(ErrMsg));
                Api->ReleaseStatus(RegStatus);
            }
        }

        return Opts;
    }

    /** Convert a UE model-file path to the OS-native form ORT wants.
     *  On Windows ORT's CreateSession expects wchar_t*; elsewhere utf-8.
     *  Returned FTCHARToUTF8 is only used on non-Windows platforms. */
    FString NormalizeModelPath(const FString& In)
    {
        FString Out = In;
        FPaths::NormalizeFilename(Out);
        return Out;
    }

    /** Format a provider list for logging. */
    FString JoinProviderList(const TArray<EInoOnnxProvider>& Providers)
    {
        FString Out;
        for (const EInoOnnxProvider P : Providers)
        {
            if (!Out.IsEmpty()) Out += TEXT(", ");
            Out += ProviderToString(P);
        }
        return Out.IsEmpty() ? FString(TEXT("(none)")) : Out;
    }

    /** Format a shape array for logging. */
    FString JoinShape(const TArray<int64>& Shape)
    {
        FString S = TEXT("[");
        for (int32 i = 0; i < Shape.Num(); ++i)
        {
            if (i > 0) S += TEXT(", ");
            S += FString::Printf(TEXT("%lld"), Shape[i]);
        }
        S += TEXT("]");
        return S;
    }

    /** Log caller-requested options at Verbose. */
    void LogRequestedOptions(const FInoOnnxSessionOptions& Options)
    {
        UE_LOG(LogInoOnnx, Verbose,
               TEXT("Onnx: Session: requested providers=[%s] intra=%d inter=%d opt=%d profiling=%s dml_adapter=%d cfg_entries=%d log_sev=%d"),
               *JoinProviderList(Options.ExecutionProviders),
               Options.IntraOpThreadCount,
               Options.InterOpThreadCount,
               (int32)Options.GraphOptimization,
               Options.bEnableProfiling ? TEXT("yes") : TEXT("no"),
               Options.DirectMlAdapterIndex,
               Options.SessionConfig.Num(),
               Options.LogSeverityLevel);
    }

    // ReadIOMeta was here but needed access to the private
    // FInoOnnxSession::FIOMeta nested type — anonymous-namespace free
    // functions can't touch class privates. It's now a private static
    // member function on FInoOnnxSession, defined further down this file.
}

bool FInoOnnxSession::ReadIOMeta(
    const OrtApi* Api,
    OrtSession* Session,
    bool bInput,
    size_t Index,
    TArray<FInoOnnxSession::FIOMeta>& OutArr)
{
    using namespace InoOnnx::Internal;

    // Allocator for name strings.
    OrtAllocator* Allocator = nullptr;
    if (!CheckOrtStatus(
            Api->GetAllocatorWithDefaultOptions(&Allocator),
            TEXT("GetAllocatorWithDefaultOptions"), nullptr))
    {
        return false;
    }

    // --- Name ---
    char* NamePtr = nullptr;
    OrtStatus* NameStatus = bInput
        ? Api->SessionGetInputName(Session, Index, Allocator, &NamePtr)
        : Api->SessionGetOutputName(Session, Index, Allocator, &NamePtr);
    if (!CheckOrtStatus(NameStatus,
                        bInput ? TEXT("SessionGetInputName") : TEXT("SessionGetOutputName"), nullptr))
    {
        return false;
    }
    FString Name = FString(UTF8_TO_TCHAR(NamePtr));
    // Free the ORT-allocated name string. AllocatorFree returns OrtStatus*
    // and is tagged [[nodiscard]] on clang (Android); MSVC is lenient.
    // In practice this free can't meaningfully fail, but we have to handle
    // the return to keep -Werror happy.
    if (OrtStatus* FreeStatus = Api->AllocatorFree(Allocator, NamePtr))
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Session: OrtApi::AllocatorFree returned an error (ignored): %s"),
               UTF8_TO_TCHAR(Api->GetErrorMessage(FreeStatus)));
        Api->ReleaseStatus(FreeStatus);
    }

    // --- Type info ---
    OrtTypeInfo* TypeInfo = nullptr;
    OrtStatus* TypeStatus = bInput
        ? Api->SessionGetInputTypeInfo(Session, Index, &TypeInfo)
        : Api->SessionGetOutputTypeInfo(Session, Index, &TypeInfo);
    if (!CheckOrtStatus(TypeStatus,
                        bInput ? TEXT("SessionGetInputTypeInfo") : TEXT("SessionGetOutputTypeInfo"),
                        nullptr))
    {
        return false;
    }

    const OrtTensorTypeAndShapeInfo* TensorInfo = nullptr;
    if (!CheckOrtStatus(
            Api->CastTypeInfoToTensorInfo(TypeInfo, &TensorInfo),
            TEXT("CastTypeInfoToTensorInfo"), nullptr))
    {
        Api->ReleaseTypeInfo(TypeInfo);
        return false;
    }

    // CastTypeInfoToTensorInfo can set *TensorInfo to null for non-
    // tensor model I/O (maps, sequences). We don't support those yet —
    // log and skip.
    if (TensorInfo == nullptr)
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Session: model %s[%zu] (%s) is not a tensor type; skipping metadata"),
               bInput ? TEXT("input") : TEXT("output"), Index, *Name);
        Api->ReleaseTypeInfo(TypeInfo);
        FInoOnnxSession::FIOMeta Stub;
        Stub.Name = Name;
        OutArr.Add(MoveTemp(Stub));
        return true;
    }

    // Dtype.
    ONNXTensorElementDataType OrtDtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    CheckOrtStatus(Api->GetTensorElementType(TensorInfo, &OrtDtype),
                   TEXT("GetTensorElementType"), nullptr);

    // Shape.
    size_t DimCount = 0;
    TArray<int64> Shape;
    if (CheckOrtStatus(Api->GetDimensionsCount(TensorInfo, &DimCount),
                       TEXT("GetDimensionsCount"), nullptr) && DimCount > 0)
    {
        Shape.SetNumUninitialized((int32)DimCount);
        // reinterpret_cast: UE int64 (long long) vs ORT int64_t (long on
        // Android). Same representation, different type names.
        CheckOrtStatus(Api->GetDimensions(TensorInfo,
                                          reinterpret_cast<int64_t*>(Shape.GetData()),
                                          DimCount),
                       TEXT("GetDimensions"), nullptr);
    }

    Api->ReleaseTypeInfo(TypeInfo);

    FInoOnnxSession::FIOMeta Entry;
    Entry.Name  = MoveTemp(Name);
    Entry.Shape = MoveTemp(Shape);
    Entry.Dtype = OrtToDtype(OrtDtype);
    OutArr.Add(MoveTemp(Entry));
    return true;
}

// ============================================================================
//  FInoOnnxSession — static factories
// ============================================================================

TUniquePtr<FInoOnnxSession> FInoOnnxSession::Create(
    const FString& ModelPath,
    const FInoOnnxSessionOptions& Options,
    FString* OutError)
{
    const double StartTime = FPlatformTime::Seconds();

    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Session: Create — model=%s, requested providers=[%s]"),
           *ModelPath,
           *JoinProviderList(Options.ExecutionProviders));
    LogRequestedOptions(Options);

    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        const FString Err(TEXT("ONNX Runtime is not initialized (GetApi() == nullptr)."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: Create FAILED: %s"), *Err);
        return nullptr;
    }

    OrtEnv* Env = InoOnnx::Internal::GetGlobalOrtEnv();
    if (Env == nullptr)
    {
        const FString Err(TEXT("Failed to obtain global OrtEnv."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: Create FAILED: %s"), *Err);
        return nullptr;
    }

    // Verify the file exists up-front — ORT's error for a missing file
    // is not terribly informative.
    const FString FullPath = NormalizeModelPath(ModelPath);
    if (!IFileManager::Get().FileExists(*FullPath))
    {
        const FString Err = FString::Printf(
            TEXT("Model file does not exist: %s"), *FullPath);
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: Create FAILED: %s"), *Err);
        return nullptr;
    }

    TArray<EInoOnnxProvider> RegisteredProviders;
    OrtSessionOptions* Opts = BuildOrtSessionOptions(Api, Options, RegisteredProviders, OutError);
    if (Opts == nullptr)
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: Create FAILED — BuildOrtSessionOptions returned null"));
        return nullptr;
    }

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Session: calling ORT CreateSession on %s"), *FullPath);

    // ORT's CreateSession signature on Windows takes wchar_t*; on other
    // platforms it takes char* (utf-8). Our PLATFORM_TCHAR_IS_WCHAR
    // handling keeps this cleanish.
    OrtSession* Native = nullptr;
#if PLATFORM_WINDOWS
    OrtStatus* CreateStatus = Api->CreateSession(Env, (const ORTCHAR_T*)*FullPath, Opts, &Native);
#else
    const FTCHARToUTF8 PathUtf8(*FullPath);
    OrtStatus* CreateStatus = Api->CreateSession(Env, PathUtf8.Get(), Opts, &Native);
#endif

    if (!CheckStatus(CreateStatus, TEXT("CreateSession"), OutError))
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: CreateSession FAILED for %s"), *FullPath);
        Api->ReleaseSessionOptions(Opts);
        return nullptr;
    }

    // Wrap into our class.
    TUniquePtr<FInoOnnxSession> Session = TUniquePtr<FInoOnnxSession>(new FInoOnnxSession());
    Session->NativeSession = Native;
    Session->NativeOptions = Opts;

    if (!Session->FinishConstruction(Options, MoveTemp(RegisteredProviders), OutError))
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: FinishConstruction FAILED for %s"), *FullPath);
        // FinishConstruction cleans up on failure via our destructor.
        return nullptr;
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Session: loaded '%s' (%d inputs, %d outputs, active=[%s], %.1f ms)"),
           *FPaths::GetCleanFilename(FullPath),
           Session->GetInputCount(),
           Session->GetOutputCount(),
           *JoinProviderList(Session->GetActiveProviders()),
           ElapsedMs);

    return Session;
}

TUniquePtr<FInoOnnxSession> FInoOnnxSession::CreateFromMemory(
    TArrayView<const uint8> ModelBytes,
    const FInoOnnxSessionOptions& Options,
    FString* OutError)
{
    const double StartTime = FPlatformTime::Seconds();

    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Session: CreateFromMemory — %d bytes, requested providers=[%s]"),
           ModelBytes.Num(),
           *JoinProviderList(Options.ExecutionProviders));
    LogRequestedOptions(Options);

    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        const FString Err(TEXT("ONNX Runtime is not initialized (GetApi() == nullptr)."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: CreateFromMemory FAILED: %s"), *Err);
        return nullptr;
    }

    OrtEnv* Env = InoOnnx::Internal::GetGlobalOrtEnv();
    if (Env == nullptr)
    {
        const FString Err(TEXT("Failed to obtain global OrtEnv."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: CreateFromMemory FAILED: %s"), *Err);
        return nullptr;
    }

    if (ModelBytes.Num() == 0)
    {
        const FString Err(TEXT("ModelBytes is empty."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: CreateFromMemory FAILED: %s"), *Err);
        return nullptr;
    }

    TArray<EInoOnnxProvider> RegisteredProviders;
    OrtSessionOptions* Opts = BuildOrtSessionOptions(Api, Options, RegisteredProviders, OutError);
    if (Opts == nullptr)
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: CreateFromMemory FAILED — BuildOrtSessionOptions returned null"));
        return nullptr;
    }

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Session: calling ORT CreateSessionFromArray (%d bytes)"),
           ModelBytes.Num());

    OrtSession* Native = nullptr;
    OrtStatus* CreateStatus = Api->CreateSessionFromArray(
        Env, ModelBytes.GetData(), (size_t)ModelBytes.Num(), Opts, &Native);

    if (!CheckStatus(CreateStatus, TEXT("CreateSessionFromArray"), OutError))
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: CreateSessionFromArray FAILED"));
        Api->ReleaseSessionOptions(Opts);
        return nullptr;
    }

    TUniquePtr<FInoOnnxSession> Session = TUniquePtr<FInoOnnxSession>(new FInoOnnxSession());
    Session->NativeSession = Native;
    Session->NativeOptions = Opts;

    if (!Session->FinishConstruction(Options, MoveTemp(RegisteredProviders), OutError))
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: FinishConstruction FAILED after CreateSessionFromArray"));
        return nullptr;
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Session: loaded from memory (%d bytes, %d inputs, %d outputs, active=[%s], %.1f ms)"),
           ModelBytes.Num(),
           Session->GetInputCount(),
           Session->GetOutputCount(),
           *JoinProviderList(Session->GetActiveProviders()),
           ElapsedMs);

    return Session;
}

FInoOnnxSession::~FInoOnnxSession()
{
    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        // Shutdown order issue: ORT already torn down. Can't release
        // safely; process is probably exiting anyway. Leak is unavoidable.
        return;
    }

    if (NativeSession)
    {
        Api->ReleaseSession(NativeSession);
        NativeSession = nullptr;
    }
    if (NativeOptions)
    {
        Api->ReleaseSessionOptions(NativeOptions);
        NativeOptions = nullptr;
    }
}

bool FInoOnnxSession::FinishConstruction(
    const FInoOnnxSessionOptions& Options,
    TArray<EInoOnnxProvider> RegisteredProviders,
    FString* OutError)
{
    const OrtApi* Api = InoOnnx::GetApi();
    ActiveProviders = MoveTemp(RegisteredProviders);

    // Populate input metadata.
    size_t InputCount = 0;
    if (!CheckStatus(Api->SessionGetInputCount(NativeSession, &InputCount),
                     TEXT("SessionGetInputCount"), OutError))
    {
        return false;
    }
    InputMeta.Reserve((int32)InputCount);
    for (size_t i = 0; i < InputCount; ++i)
    {
        if (!ReadIOMeta(Api, NativeSession, /*bInput=*/true, i, InputMeta))
        {
            if (OutError) *OutError = FString::Printf(
                TEXT("Failed to read metadata for input %zu"), i);
            return false;
        }
    }

    // Populate output metadata.
    size_t OutputCount = 0;
    if (!CheckStatus(Api->SessionGetOutputCount(NativeSession, &OutputCount),
                     TEXT("SessionGetOutputCount"), OutError))
    {
        return false;
    }
    OutputMeta.Reserve((int32)OutputCount);
    for (size_t i = 0; i < OutputCount; ++i)
    {
        if (!ReadIOMeta(Api, NativeSession, /*bInput=*/false, i, OutputMeta))
        {
            if (OutError) *OutError = FString::Printf(
                TEXT("Failed to read metadata for output %zu"), i);
            return false;
        }
    }

    // Pre-cache UTF-8 representations of every input/output name and
    // build the const-char* arrays Run() will hand to ORT verbatim.
    // Two-pass: populate every NameUtf8 buffer first, then take pointers.
    // (Pointers into a TArray<ANSICHAR> are stable until that TArray
    // reallocates; we never modify NameUtf8 after this loop, and the
    // FIOMeta entries themselves don't move because InputMeta /
    // OutputMeta are past their Reserve+Add population step.)
    auto BakeNameBuffers = [](TArray<FIOMeta>& Metas)
    {
        for (FIOMeta& Meta : Metas)
        {
            const FTCHARToUTF8 Conv(*Meta.Name);
            const int32 ByteLen = Conv.Length();
            Meta.NameUtf8.Reset(ByteLen + 1);
            Meta.NameUtf8.Append(reinterpret_cast<const ANSICHAR*>(Conv.Get()), ByteLen);
            Meta.NameUtf8.Add('\0');
        }
    };
    BakeNameBuffers(InputMeta);
    BakeNameBuffers(OutputMeta);

    InputNamePtrs.Reset(InputMeta.Num());
    for (const FIOMeta& Meta : InputMeta)
    {
        InputNamePtrs.Add(Meta.NameUtf8.GetData());
    }
    OutputNamePtrs.Reset(OutputMeta.Num());
    for (const FIOMeta& Meta : OutputMeta)
    {
        OutputNamePtrs.Add(Meta.NameUtf8.GetData());
    }

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Session: metadata cache populated (%d inputs, %d outputs)"),
           InputMeta.Num(), OutputMeta.Num());

    (void)Options; // currently no post-Create Options post-processing
    return true;
}

// ============================================================================
//  Metadata accessors
// ============================================================================

FString FInoOnnxSession::GetInputName(int32 Index) const
{
    return InputMeta.IsValidIndex(Index) ? InputMeta[Index].Name : FString();
}

FString FInoOnnxSession::GetOutputName(int32 Index) const
{
    return OutputMeta.IsValidIndex(Index) ? OutputMeta[Index].Name : FString();
}

TArray<int64> FInoOnnxSession::GetInputShape(int32 Index) const
{
    return InputMeta.IsValidIndex(Index) ? InputMeta[Index].Shape : TArray<int64>{};
}

TArray<int64> FInoOnnxSession::GetOutputShape(int32 Index) const
{
    return OutputMeta.IsValidIndex(Index) ? OutputMeta[Index].Shape : TArray<int64>{};
}

EInoOnnxDtype FInoOnnxSession::GetInputDtype(int32 Index) const
{
    return InputMeta.IsValidIndex(Index) ? InputMeta[Index].Dtype : EInoOnnxDtype::Undefined;
}

EInoOnnxDtype FInoOnnxSession::GetOutputDtype(int32 Index) const
{
    return OutputMeta.IsValidIndex(Index) ? OutputMeta[Index].Dtype : EInoOnnxDtype::Undefined;
}

void FInoOnnxSession::LogMetadata() const
{
    using namespace InoOnnx::Internal;

    FString ProvList;
    for (const EInoOnnxProvider P : ActiveProviders)
    {
        if (!ProvList.IsEmpty()) ProvList += TEXT(", ");
        ProvList += ProviderToString(P);
    }
    UE_LOG(LogInoOnnx, Log, TEXT("Onnx: Session: active providers: %s"),
           ProvList.IsEmpty() ? TEXT("(none)") : *ProvList);

    auto DumpShape = [](const TArray<int64>& Shape) {
        FString S = TEXT("[");
        for (int32 i = 0; i < Shape.Num(); ++i)
        {
            if (i > 0) S += TEXT(", ");
            S += FString::Printf(TEXT("%lld"), Shape[i]);
        }
        S += TEXT("]");
        return S;
    };

    UE_LOG(LogInoOnnx, Log, TEXT("Onnx: Session: %d inputs:"), InputMeta.Num());
    for (int32 i = 0; i < InputMeta.Num(); ++i)
    {
        UE_LOG(LogInoOnnx, Verbose, TEXT("Onnx: Session:   input[%d] %s : dtype=%d shape=%s"),
               i, *InputMeta[i].Name, (int32)InputMeta[i].Dtype, *DumpShape(InputMeta[i].Shape));
    }

    UE_LOG(LogInoOnnx, Log, TEXT("Onnx: Session: %d outputs:"), OutputMeta.Num());
    for (int32 i = 0; i < OutputMeta.Num(); ++i)
    {
        UE_LOG(LogInoOnnx, Verbose, TEXT("Onnx: Session:   output[%d] %s : dtype=%d shape=%s"),
               i, *OutputMeta[i].Name, (int32)OutputMeta[i].Dtype, *DumpShape(OutputMeta[i].Shape));
    }
}

// ============================================================================
//  Inference
// ============================================================================

bool FInoOnnxSession::Run(
    TArrayView<const FInoOnnxTensor> Inputs,
    TArray<FInoOnnxTensor>& OutOutputs,
    FString* OutError)
{
    const double RunStart = FPlatformTime::Seconds();

    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        const FString Err(TEXT("ONNX Runtime is not initialized."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: Run FAILED: %s"), *Err);
        return false;
    }

    if (NativeSession == nullptr)
    {
        const FString Err(TEXT("Session is null."));
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: Run FAILED: %s"), *Err);
        return false;
    }

    if (Inputs.Num() != InputMeta.Num())
    {
        const FString Err = FString::Printf(
            TEXT("Inputs.Num() == %d but model expects %d"),
            Inputs.Num(), InputMeta.Num());
        if (OutError) *OutError = Err;
        UE_LOG(LogInoOnnx, Error, TEXT("Onnx: Session: Run FAILED: %s"), *Err);
        return false;
    }

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Session: Run — %d inputs, %d outputs"),
           Inputs.Num(), OutputMeta.Num());
    for (int32 i = 0; i < Inputs.Num(); ++i)
    {
        UE_LOG(LogInoOnnx, Verbose,
               TEXT("Onnx: Session:   in[%d] %s : dtype=%d shape=%s"),
               i,
               *InputMeta[i].Name,
               (int32)Inputs[i].GetDtype(),
               *JoinShape(Inputs[i].GetShape()));
    }

    // Names come from the cached InputNamePtrs / OutputNamePtrs arrays
    // baked at FinishConstruction time. The only per-Run allocation is
    // for the input-OrtValue* array — small + cheap.
    TArray<const OrtValue*> InputValuesRaw;
    InputValuesRaw.Reserve(Inputs.Num());
    for (int32 i = 0; i < Inputs.Num(); ++i)
    {
        InputValuesRaw.Add(Inputs[i].GetNativeHandle());
    }

    TArray<OrtValue*> OutputValuesRaw;
    OutputValuesRaw.SetNumZeroed(OutputMeta.Num());

    // Run. RunOptions == nullptr means default ORT options (no
    // cancellation, default verbosity, etc.) — appropriate for nearly
    // every caller. If we ever need cancellation, switch to creating
    // one per inference.
    OrtStatus* Status = Api->Run(
        NativeSession,
        /*run_options=*/ nullptr,
        InputNamePtrs.GetData(),
        InputValuesRaw.GetData(),
        (size_t)InputValuesRaw.Num(),
        OutputNamePtrs.GetData(),
        (size_t)OutputNamePtrs.Num(),
        OutputValuesRaw.GetData());

    if (!CheckStatus(Status, TEXT("Run"), OutError))
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Session: Run FAILED (%.2f ms elapsed)"),
               (FPlatformTime::Seconds() - RunStart) * 1000.0);
        // Any partial output tensors ORT managed to allocate before
        // erroring need to be released so we don't leak.
        for (OrtValue* V : OutputValuesRaw)
        {
            if (V) Api->ReleaseValue(V);
        }
        return false;
    }

    // Wrap outputs in FInoOnnxTensors. Each Adopt takes ownership of
    // the OrtValue; shape/dtype come from the cached output metadata
    // (ORT guarantees outputs have the declared dtype, and the shape
    // is dynamic-dim-bound to actual values — we refresh from the
    // OrtValue to get concrete dims instead of the -1 placeholders).
    const int32 OutputBaseIndex = OutOutputs.Num();
    OutOutputs.Reserve(OutOutputs.Num() + OutputMeta.Num());
    for (int32 i = 0; i < OutputValuesRaw.Num(); ++i)
    {
        OutOutputs.Add(FInoOnnxTensor::Adopt(
            OutputValuesRaw[i],
            OutputMeta[i].Dtype,
            /*Shape=*/ {}));
        // Adopt() calls RefreshShapeAndDtype when shape is empty, so
        // the concrete runtime shape is populated.
        OutputValuesRaw[i] = nullptr;  // transferred ownership
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - RunStart) * 1000.0;
    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Session: Run complete in %.2f ms (%d outputs)"),
           ElapsedMs, OutputMeta.Num());
    for (int32 i = 0; i < OutputMeta.Num(); ++i)
    {
        const int32 OutIdx = OutputBaseIndex + i;
        if (OutOutputs.IsValidIndex(OutIdx))
        {
            UE_LOG(LogInoOnnx, Verbose,
                   TEXT("Onnx: Session:   out[%d] %s : dtype=%d shape=%s"),
                   i,
                   *OutputMeta[i].Name,
                   (int32)OutOutputs[OutIdx].GetDtype(),
                   *JoinShape(OutOutputs[OutIdx].GetShape()));
        }
    }

    return true;
}

bool FInoOnnxSession::Warmup(
    TArrayView<const FInoOnnxTensor> DummyInputs,
    FString* OutError)
{
    const double WarmT0 = FPlatformTime::Seconds();

    TArray<FInoOnnxTensor> DiscardOutputs;
    if (!Run(DummyInputs, DiscardOutputs, OutError))
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Session: warmup failed — first real inference will pay the JIT cost"));
        return false;
    }

    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Session: warmup ok in %.1f ms"),
           (FPlatformTime::Seconds() - WarmT0) * 1000.0);
    return true;
}

void FInoOnnxSession::RunAsync(
    TArray<FInoOnnxTensor>&& Inputs,
    TFunction<void(TArray<FInoOnnxTensor>, FString)> OnComplete)
{
    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Session: RunAsync dispatched to ThreadPool (%d inputs)"),
           Inputs.Num());

    // Move captures are important: we move Inputs into the lambda so
    // the caller's tensors become invalid (preventing use-after-move
    // races) and they get destroyed only when Run returns.
    Async(EAsyncExecution::ThreadPool,
        [this, MovedInputs = MoveTemp(Inputs), OnComplete = MoveTemp(OnComplete)]() mutable
        {
            TArray<FInoOnnxTensor> Outputs;
            FString Error;

            const bool bOk = this->Run(
                TArrayView<const FInoOnnxTensor>(MovedInputs.GetData(), MovedInputs.Num()),
                Outputs,
                &Error);

            if (!bOk)
            {
                Outputs.Reset();
                if (Error.IsEmpty())
                {
                    Error = TEXT("Unknown Run error");
                }
            }

            // Marshal completion to the game thread so Blueprint / UI
            // callbacks don't have to worry about thread safety. This
            // mirrors the LiteRT-LM worker's completion dispatch pattern
            // (see InoLiteRtLmConversationWorker.cpp).
            //
            // Move the outputs array + error into the game-thread
            // lambda; they'll be destroyed after OnComplete returns.
            AsyncTask(ENamedThreads::GameThread,
                [Outputs = MoveTemp(Outputs), Error = MoveTemp(Error), OnComplete = MoveTemp(OnComplete)]() mutable
                {
                    if (OnComplete)
                    {
                        OnComplete(MoveTemp(Outputs), MoveTemp(Error));
                    }
                });
        });
}

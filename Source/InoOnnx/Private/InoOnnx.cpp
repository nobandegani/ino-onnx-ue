// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoOnnx.h"

#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
    // For GetModuleHandleW / GetModuleFileNameW — used to verify which
    // DLL Windows' base-name cache actually served when we asked to
    // load a full-path DLL. Without this we can't distinguish "our
    // InoOnnxRuntime.dll / DirectML.dll / etc. got loaded" from "UE's
    // already-cached copy at a different path was returned instead."
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <windows.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

#if PLATFORM_IOS
    // For dlsym(RTLD_DEFAULT, "OrtGetApiBase"). On iOS, Microsoft's
    // ORT distribution is a STATIC FRAMEWORK (Unix `ar` archive wrapped
    // in a fat header, NOT a Mach-O dylib). InoOnnx.Build.cs links it
    // statically into the iOS executable via PublicAdditionalFrameworks
    // with bCopyFramework=false, so by the time this Init runs every
    // Ort* symbol is already in the main executable's global symbol
    // table. dlsym(RTLD_DEFAULT, ...) finds it there. We never call
    // GetDllHandle — there's no separate runtime dylib to open.
    #include <dlfcn.h>
#endif

// ONNX Runtime C API. Included for the struct / function-type definitions
// (OrtApi, OrtApiBase, OrtStatus, OrtGetApiBase signature, etc.). We do
// NOT link against the ORT import library:
//   Windows: we GetProcAddress "OrtGetApiBase" on the renamed
//            InoOnnxRuntime.dll at runtime.
//   Android: same path via dlsym.
//
// We deliberately stay on the C API (not onnxruntime_cxx_api.h) so we
// remain exception-free (UE modules default bEnableExceptions=false).
#include "onnxruntime_c_api.h"

// Single definition for the shared log category declared in InoOnnx.h.
DEFINE_LOG_CATEGORY(LogInoOnnx);

// =============================================================
// InoOnnx — DLL loader + global OrtApi accessor
// =============================================================

namespace InoOnnx
{

namespace
{
    /** Cached OrtApi vtable. Populated by Init(), cleared by Shutdown().
     *  Accessed via GetApi() from all ORT-consuming .cpps in any consumer
     *  plugin. */
    const OrtApi* GOrtApi = nullptr;

    /**
     * Per-platform library path we feed to FPlatformProcess::GetDllHandle.
     *
     * Windows: our renamed DLL at an absolute path. We resolve the full
     *   path via IPluginManager so the loader can't be confused with any
     *   other onnxruntime.dll on the system.
     *
     * Android: bare soname. Android's dynamic linker resolves this via
     *   the APK's lib/<abi>/ dir (which is in LD_LIBRARY_PATH for the
     *   process). We can't build an absolute path because the APK's
     *   on-device lib dir ("/data/app/.../lib/arm64-v8a") isn't known
     *   at build time. The UPL's soLoadLibrary preload has already
     *   mapped the .so into the process by this point, so dlopen just
     *   returns the existing handle.
     *
     * Mac: same pattern as Windows — full absolute path resolved via
     *   IPluginManager, pointing at our renamed
     *   Source/ThirdParty/Mac/libInoOnnxRuntime.dylib. dyld handles the
     *   load via dlopen on the absolute path; @rpath logic doesn't apply
     *   because we're loading the dylib by full filesystem path, not by
     *   install-name lookup.
     *
     * iOS: returns empty as a sentinel. Microsoft's iOS ORT is a STATIC
     *   FRAMEWORK — InoOnnx.Build.cs links it into the iOS executable
     *   at build time (PublicAdditionalFrameworks with
     *   bCopyFramework=false), so there's no separate runtime dylib to
     *   load. Init() detects the empty path and skips straight to
     *   symbol resolution against RTLD_DEFAULT via dlsym, which finds
     *   the statically-linked Ort* symbols in the main executable's
     *   global symbol table.
     */
    FString ResolveOnnxLibraryName()
    {
#if PLATFORM_WINDOWS
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoOnnx"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Source/ThirdParty/Win64"),
            TEXT("InoOnnxRuntime.dll"));
#elif PLATFORM_ANDROID
        return FString(TEXT("libInoOnnxRuntime.so"));
#elif PLATFORM_MAC
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoOnnx"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Source/ThirdParty/Mac"),
            TEXT("libInoOnnxRuntime.dylib"));
#elif PLATFORM_IOS
        // Sentinel: empty path tells Init() to skip dlopen and go
        // straight to dlsym(RTLD_DEFAULT) for OrtGetApiBase resolution.
        return FString();
#else
        return FString();
#endif
    }

#if PLATFORM_WINDOWS
    /**
     * Windows-only: query Windows for the full on-disk path of an
     * already-loaded DLL (by its base name). Returns empty if the DLL
     * isn't loaded at all, or a sentinel string on API failure.
     *
     * Critical for verifying that our preloaded copies actually won
     * the base-name cache race vs other DLLs with the same name that
     * UE or other plugins may have loaded first.
     */
    FString GetActualLoadedModulePath(const TCHAR* BaseName)
    {
        HMODULE Handle = GetModuleHandleW(BaseName);
        if (Handle == nullptr)
        {
            return FString(TEXT("(not loaded)"));
        }
        WCHAR PathBuf[MAX_PATH + 1] = {};
        const DWORD Len = GetModuleFileNameW(Handle, PathBuf, MAX_PATH);
        if (Len == 0 || Len >= MAX_PATH)
        {
            return FString(TEXT("(GetModuleFileName failed)"));
        }
        return FString(PathBuf);
    }

    /**
     * Verify that a loaded DLL came from the path we expected. Logs a
     * Warning if Windows' base-name cache served a different copy
     * (common signal: another plugin loaded its own DirectML.dll
     * before us, pinning that version into the process).
     */
    void VerifyLoadedPath(const TCHAR* BaseName, const FString& ExpectedFullPath)
    {
        const FString ActualPath = GetActualLoadedModulePath(BaseName);

        // Windows paths are case-insensitive and may use mixed separators.
        // Normalise both sides to forward-slash lower-case for comparison.
        auto Normalize = [](const FString& In) -> FString
        {
            FString Out = In;
            Out.ReplaceInline(TEXT("\\"), TEXT("/"));
            return Out.ToLower();
        };

        if (Normalize(ActualPath) == Normalize(ExpectedFullPath))
        {
            UE_LOG(LogInoOnnx, Log,
                   TEXT("Onnx: Module: verified %s is loaded from %s"),
                   BaseName, *ActualPath);
        }
        else
        {
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Onnx: Module: BASE-NAME CACHE COLLISION — %s loaded from %s, ")
                   TEXT("but we wanted %s. Our preload didn't win the race (another plugin ")
                   TEXT("loaded a different %s first). Symptoms may include version-skew ")
                   TEXT("bugs at runtime."),
                   BaseName, *ActualPath, *ExpectedFullPath, BaseName);
        }
    }

    /**
     * Windows-only: our Source/ThirdParty/Win64 directory. Used to build
     * full paths for the InoDml.dll + shared-providers preloads below.
     * Returns empty if the plugin can't be resolved.
     */
    FString ResolveWin64BinDir()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("InoOnnx"));
        if (!Plugin.IsValid())
        {
            return FString();
        }
        return FPaths::Combine(
            Plugin->GetBaseDir(),
            TEXT("Source/ThirdParty/Win64"));
    }

    /**
     * Pre-load a sibling DLL by full path so Windows' loaded-modules
     * cache is populated with OUR copy for that base name.
     *
     * Historical context: DirectML.dll is a DELAY-LOAD dependency of
     * our ORT (confirmed via pefile parse — DIRECTORY_ENTRY_DELAY_IMPORT,
     * not static). The current architecture RENAMES DirectML.dll to
     * InoDml.dll and patches our ORT DLL's delay-import table
     * (patch-ort-dml-import.py) to match — so no other plugin looks
     * for "InoDml.dll" and no cache collision is possible. This preload
     * is now belt-and-braces: it ensures our InoDml.dll is in the cache
     * under a known full path before ORT's delay-load stub fires.
     *
     * Also pre-loads onnxruntime_providers_shared.dll for the same
     * defensive reason — ORT LoadLibrary's it lazily for certain
     * shared EPs.
     *
     * Failures are logged but non-fatal — if a preload DLL genuinely
     * isn't staged (shouldn't happen post-setup-script) we log clearly
     * and let the subsequent InoOnnxRuntime.dll load fail naturally
     * with a load error.
     */
    void PreloadWin64Deps()
    {
        const FString BinDir = ResolveWin64BinDir();
        if (BinDir.IsEmpty())
        {
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Onnx: Module: cannot resolve plugin bin dir; InoDml.dll preload skipped"));
            return;
        }

        UE_LOG(LogInoOnnx, Verbose,
               TEXT("Onnx: Module: preloading Win64 sibling DLLs from %s"),
               *BinDir);

        // Preload order doesn't matter for the two sibling DLLs — neither
        // imports the other. What matters is that both are loaded by full
        // path BEFORE InoOnnxRuntime.dll, so their base-name cache entries
        // are ours. "InoDml.dll" is OUR rename of DirectML.dll — see the
        // patch-ort-dml-import.py script for the import-table rewrite
        // that ties this together.
        struct FPreload { const TCHAR* Name; bool bRequired; };
        const FPreload Preloads[] = {
            { TEXT("InoDml.dll"),                      true  },
            { TEXT("onnxruntime_providers_shared.dll"), false },
        };

        for (const FPreload& P : Preloads)
        {
            const FString FullPath = FPaths::Combine(BinDir, P.Name);
            UE_LOG(LogInoOnnx, Verbose,
                   TEXT("Onnx: Module: attempting preload of %s (full path=%s, required=%s)"),
                   P.Name, *FullPath, P.bRequired ? TEXT("yes") : TEXT("no"));
            void* Handle = FPlatformProcess::GetDllHandle(*FullPath);
            if (Handle != nullptr)
            {
                UE_LOG(LogInoOnnx, Log,
                       TEXT("Onnx: Module: pre-loaded %s"), P.Name);

                VerifyLoadedPath(P.Name, FullPath);

                // We deliberately DON'T FreeDllHandle — we want the module
                // to stay resident until process exit, holding the cache
                // entry for the whole lifetime of the game.
            }
            else if (P.bRequired)
            {
                UE_LOG(LogInoOnnx, Error,
                       TEXT("Onnx: Module: REQUIRED preload failed: %s (path=%s). ")
                       TEXT("DirectML support will not work — our ORT's delay-load import ")
                       TEXT("of %s will fail at first DML call. Run ")
                       TEXT("Plugins/InoOnnx/OnnxRuntime/scripts/setup-onnxruntime.ps1 ")
                       TEXT("to stage the binary."),
                       P.Name, *FullPath, P.Name);
            }
            else
            {
                UE_LOG(LogInoOnnx, Verbose,
                       TEXT("Onnx: Module: optional preload not found: %s (path=%s)"),
                       P.Name, *FullPath);
            }
        }
    }
#endif // PLATFORM_WINDOWS

    /**
     * Call OrtApi::GetAvailableProviders and log the returned provider
     * list. Runs once at Init() as proof the vtable is callable.
     */
    void LogAvailableProviders(const OrtApi* Api)
    {
        if (Api == nullptr)
        {
            return;
        }

        char** ProvidersPtr = nullptr;
        int    NumProviders = 0;
        OrtStatus* Status = Api->GetAvailableProviders(&ProvidersPtr, &NumProviders);

        if (Status != nullptr)
        {
            const char* ErrMsg = Api->GetErrorMessage(Status);
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Onnx: Module: OrtApi::GetAvailableProviders failed: %s"),
                   UTF8_TO_TCHAR(ErrMsg));
            Api->ReleaseStatus(Status);
            return;
        }

        FString Joined;
        for (int i = 0; i < NumProviders; ++i)
        {
            if (!Joined.IsEmpty())
            {
                Joined += TEXT(", ");
            }
            if (ProvidersPtr != nullptr && ProvidersPtr[i] != nullptr)
            {
                Joined += FString(UTF8_TO_TCHAR(ProvidersPtr[i]));
            }
        }

        UE_LOG(LogInoOnnx, Log,
               TEXT("Onnx: Module: available providers (%d): %s"),
               NumProviders,
               Joined.IsEmpty() ? TEXT("(none)") : *Joined);

        // ReleaseAvailableProviders is declared with warn_unused_result
        // (Android clang enforces this; MSVC is more forgiving). Capture
        // and release any returned OrtStatus.
        if (OrtStatus* ReleaseStatus = Api->ReleaseAvailableProviders(ProvidersPtr, NumProviders))
        {
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Onnx: Module: OrtApi::ReleaseAvailableProviders returned an error (ignored): %s"),
                   UTF8_TO_TCHAR(Api->GetErrorMessage(ReleaseStatus)));
            Api->ReleaseStatus(ReleaseStatus);
        }
    }

    /**
     * Resolve OrtGetApiBase -> OrtApi* via the given API-base pointer.
     * Logs + returns nullptr on version mismatch.
     */
    const OrtApi* SelectOrtApi(const OrtApiBase* ApiBase)
    {
        if (ApiBase == nullptr)
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Onnx: Module: OrtGetApiBase returned nullptr. The loaded ONNX Runtime is broken."));
            return nullptr;
        }

        const OrtApi* Api = ApiBase->GetApi(ORT_API_VERSION);
        if (Api == nullptr)
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Onnx: Module: OrtApiBase::GetApi(ORT_API_VERSION=%u) returned nullptr — ")
                   TEXT("the loaded ONNX Runtime does not implement this API version. ")
                   TEXT("Expected our pinned build (see Plugins/InoOnnx/OnnxRuntime/ONNXRUNTIME_VERSION)."),
                   (uint32)ORT_API_VERSION);
            return nullptr;
        }

        UE_LOG(LogInoOnnx, Verbose,
               TEXT("Onnx: Module: OrtApiBase::GetApi(ORT_API_VERSION=%u) resolved OrtApi vtable"),
               (uint32)ORT_API_VERSION);
        return Api;
    }
}

void* Init()
{
    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Module: Init — loading ONNX Runtime DLLs (compiled-against ORT_API_VERSION=%u)"),
           (uint32)ORT_API_VERSION);

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_MAC || PLATFORM_IOS
    // Unified dlopen + dlsym path across every platform we support. We
    // deliberately DO NOT link libUnreal against our ORT binary on
    // Windows / Android / Mac — see InoOnnx.Build.cs for the rationale
    // (base-name cache collision on Windows; symbol-version collision
    // on Android with marketplace plugins shipping older ORT;
    // dyld-binding hygiene on Mac).
    //
    // iOS is the sole exception: the framework is statically declared
    // via PublicAdditionalFrameworks, so dyld auto-loads it at app
    // launch and OrtGetApiBase ends up in the process's global symbol
    // namespace before this Init() runs. We resolve it via dlsym
    // RTLD_DEFAULT instead of going through GetDllHandle.

#if PLATFORM_WINDOWS
    PreloadWin64Deps();
#endif

    const FString LibName = ResolveOnnxLibraryName();

#if !PLATFORM_IOS
    // Win64 / Android / Mac: explicit dlopen on the resolved path /
    // bare soname. iOS skips this entirely and uses dlsym below.
    if (LibName.IsEmpty())
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Module: could not resolve ONNX Runtime library name (IPluginManager failed?)."));
        return nullptr;
    }

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Module: resolved library path: %s"),
           *LibName);

    void* Handle = FPlatformProcess::GetDllHandle(*LibName);
    if (Handle == nullptr)
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Module: failed to load %s. ")
               TEXT("Did you run Plugins/InoOnnx/OnnxRuntime/scripts/setup-onnxruntime.ps1 ")
               TEXT("and re-package?"),
               *LibName);
        return nullptr;
    }

    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Module: loaded %s"),
           *LibName);

#if PLATFORM_WINDOWS
    VerifyLoadedPath(TEXT("InoOnnxRuntime.dll"), LibName);
#endif

    // Resolve the single entry-point symbol we need.
    void* EntryPoint = FPlatformProcess::GetDllExport(Handle, TEXT("OrtGetApiBase"));
    if (EntryPoint == nullptr)
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Module: %s does not export OrtGetApiBase. ")
               TEXT("The library is malformed or the setup script picked up the wrong file."),
               *LibName);
        FPlatformProcess::FreeDllHandle(Handle);
        return nullptr;
    }
#else
    // iOS path — Ort* symbols are statically linked into the main
    // executable (Microsoft ships ORT for iOS as a static framework;
    // see InoOnnx.Build.cs's IOS branch). No GetDllHandle, no dlopen.
    // Sentinel handle (0x1) so the Shutdown path can distinguish
    // "successfully initialized on iOS" from "Init returned nullptr".
    // FreeDllHandle is never called on this sentinel; dlclose on
    // RTLD_DEFAULT is illegal anyway.
    if (!LibName.IsEmpty())
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Module: iOS Init received non-empty LibName '%s'; ignoring ")
               TEXT("(iOS uses statically-linked framework + RTLD_DEFAULT, not explicit dlopen)."),
               *LibName);
    }
    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Module: iOS — Ort* symbols statically linked from ")
           TEXT("InoOnnxRuntime.framework; resolving OrtGetApiBase via dlsym(RTLD_DEFAULT)."));

    void* EntryPoint = dlsym(RTLD_DEFAULT, "OrtGetApiBase");
    if (EntryPoint == nullptr)
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Module: dlsym(RTLD_DEFAULT, \"OrtGetApiBase\") returned null. ")
               TEXT("Ort* symbols are not in the iOS executable's global namespace — check ")
               TEXT("that InoOnnx.Build.cs's PublicAdditionalFrameworks call reached the link ")
               TEXT("step. (Strip-unused-symbols passes can also remove OrtGetApiBase if no ")
               TEXT("UE code reaches it via a non-static-link path; verify -ObjC / ")
               TEXT("-force_load if you see this on a stripped Release build.)"));
        return nullptr;
    }
    void* Handle = reinterpret_cast<void*>(0x1);
#endif // !PLATFORM_IOS

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Module: OrtGetApiBase resolved at %p"),
           EntryPoint);

    using OrtGetApiBaseFn = const OrtApiBase* (*)();
    const OrtApiBase* ApiBase = reinterpret_cast<OrtGetApiBaseFn>(EntryPoint)();
    if (ApiBase != nullptr && ApiBase->GetVersionString != nullptr)
    {
        const char* RuntimeVer = ApiBase->GetVersionString();
        UE_LOG(LogInoOnnx, Log,
               TEXT("Onnx: Module: ORT runtime version: %s (compiled-against ORT_API_VERSION=%u)"),
               UTF8_TO_TCHAR(RuntimeVer != nullptr ? RuntimeVer : "<null>"),
               (uint32)ORT_API_VERSION);
    }

    GOrtApi = SelectOrtApi(ApiBase);
    if (GOrtApi == nullptr)
    {
#if !PLATFORM_IOS
        FPlatformProcess::FreeDllHandle(Handle);
#endif
        return nullptr;
    }

    LogAvailableProviders(GOrtApi);

    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Module: Init complete"));
    return Handle;

#else
    // Linux: no library staged for this platform yet.
    UE_LOG(LogInoOnnx, Warning,
           TEXT("Onnx: Module: ONNX Runtime is not yet available on this platform."));
    return nullptr;
#endif
}

void Shutdown(void* Handle)
{
    UE_LOG(LogInoOnnx, Log,
           TEXT("Onnx: Module: Shutdown — unloading DLL"));

    // NOTE: any OrtEnv that consumer plugins (InoAgents, etc.) created
    // lazily via the OrtApi vtable should be released BEFORE we get here.
    // The InoAgents InoOnnxInternal::ReleaseGlobalOrtEnv() helper handles
    // this on the InoAgents side; consumer plugins that own their own
    // OrtEnv instances are responsible for releasing them in their own
    // ShutdownModule before this PreLoadingScreen module tears down.

    // Clear the cached OrtApi pointer so any late callers of GetApi()
    // see nullptr rather than a vtable belonging to a DLL we are about
    // to unload. Happens-before ordering matters here.
    GOrtApi = nullptr;

#if PLATFORM_WINDOWS || PLATFORM_ANDROID || PLATFORM_MAC
    if (Handle != nullptr)
    {
        FPlatformProcess::FreeDllHandle(Handle);
        UE_LOG(LogInoOnnx, Verbose,
               TEXT("Onnx: Module: FreeDllHandle released ONNX Runtime handle"));
    }
#elif PLATFORM_IOS
    // iOS Handle is the 0x1 sentinel — we never called GetDllHandle, so
    // there is nothing to free. dlclose on RTLD_DEFAULT is illegal, and
    // we don't own the framework's lifetime anyway (dyld loaded it at
    // app launch and will release it at process exit).
    (void)Handle;
    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Module: iOS — framework owned by dyld, no FreeDllHandle call."));
#else
    (void)Handle;
#endif
}

const OrtApi* GetApi()
{
    return GOrtApi;
}

} // namespace InoOnnx

// =============================================================
// FInoOnnxModule — UE module class
// =============================================================

void FInoOnnxModule::StartupModule()
{
    OnnxRuntimeHandle = InoOnnx::Init();
}

void FInoOnnxModule::ShutdownModule()
{
    InoOnnx::Shutdown(OnnxRuntimeHandle);
    OnnxRuntimeHandle = nullptr;
}

IMPLEMENT_MODULE(FInoOnnxModule, InoOnnx)

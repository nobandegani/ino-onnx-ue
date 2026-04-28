// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

// Forward-declare so this header doesn't pull in onnxruntime_c_api.h.
// The definition comes from that header in the .cpp consumers.
struct OrtApi;

/**
 * Shared log category for the InoOnnx plugin and the ONNX Runtime
 * loader code that ships with it. Consumer plugins (InoAgents, etc.)
 * have their own log categories and continue to use them; only the
 * runtime DLL load / unload logs to LogInoOnnx.
 */
INOONNX_API DECLARE_LOG_CATEGORY_EXTERN(LogInoOnnx, Log, All);

/**
 * InoOnnx plugin runtime module.
 *
 * Loads at LoadingPhase=PreLoadingScreen so the ONNX Runtime DLL/.so is
 * mapped and callable before any consumer plugin's Default-phase
 * StartupModule runs. Consumers (InoAgents today, future ones tomorrow)
 * declare InoOnnx in their .uplugin's Plugins array and "InoOnnx" in
 * their Build.cs PublicDependencyModuleNames, then `#include "InoOnnx.h"`
 * and call `InoAgents::Onnx::GetApi()` to reach the OrtApi vtable.
 *
 * StartupModule loads the ORT DLL/.so, resolves OrtGetApiBase via
 * GetProcAddress / dlsym, caches the OrtApi vtable, and runs a smoke
 * test (OrtApi::GetAvailableProviders) so the log shows whether ORT is
 * callable end-to-end. Failure here is non-fatal — Init() logs its own
 * error; consumers null-check InoAgents::Onnx::GetApi() before use.
 */
class FInoOnnxModule : public IModuleInterface
{
public:
    //~ IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    //~ End of IModuleInterface

private:
    /** Handle to InoOnnxRuntime.dll / libInoOnnxRuntime.so, returned by
     *  InoAgents::Onnx::Init(). nullptr on unsupported platforms or if
     *  the load failed. Passed to Shutdown() at module teardown. */
    void* OnnxRuntimeHandle = nullptr;
};

/**
 * ONNX Runtime DLL loader + global OrtApi accessor.
 *
 * The namespace is `InoAgents::Onnx::` for historical reasons (this code
 * was extracted from the InoAgents plugin). Consumer code that already
 * spells `InoAgents::Onnx::GetApi()` continues to work unchanged.
 *
 * Init() / Shutdown() are called by FInoOnnxModule (above). Consumer
 * plugins should NOT call them directly — the lifecycle is owned by
 * the InoOnnx plugin's PreLoadingScreen module. Consumers only need
 * `GetApi()` to reach the OrtApi vtable.
 *
 * Init() on Windows:
 *   - FPlatformProcess::GetDllHandle on the renamed
 *     Source/ThirdParty/Win64/InoOnnxRuntime.dll. Renamed (from
 *     onnxruntime.dll) to dodge Windows' base-name DLL caching, which
 *     otherwise returns UE's NNE-bundled older ORT handle instead of ours.
 *   - FPlatformProcess::GetDllExport to resolve "OrtGetApiBase" — the
 *     only symbol we resolve dynamically. Everything else flows through
 *     the OrtApi vtable that GetApiBase()->GetApi(ORT_API_VERSION)
 *     hands back.
 *
 * Init() on Android:
 *   - Bare soname `libInoOnnxRuntime.so`. The UPL <soLoadLibrary> preload
 *     has already mapped it, so dlopen returns the existing handle.
 *   - GetDllExport("OrtGetApiBase") via dlsym, then caches OrtApi*.
 *
 * Init() on iOS / Linux / macOS:
 *   - Warns. GetApi() returns nullptr. Any consumer that calls GetApi()
 *     and checks the return handles this gracefully.
 */
namespace InoAgents::Onnx
{
    INOONNX_API void* Init();
    INOONNX_API void  Shutdown(void* Handle);

    /**
     * Return the cached OrtApi vtable pointer, or nullptr if Init() did
     * not succeed (platform not supported, DLL load failed, API version
     * mismatch, etc.). Callers must null-check.
     *
     * Lifetime: valid from a successful Init() until Shutdown(). Threads
     * may read freely; the pointer itself is set once at module startup
     * and cleared once at module shutdown, with no writes in between.
     */
    INOONNX_API const OrtApi* GetApi();
}

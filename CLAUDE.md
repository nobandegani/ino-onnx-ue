# CLAUDE.md — InoOnnx plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoOnnx/`. The hosting demo project is documented in
`E:/Projects/InoProject/CLAUDE.md`.

## Purpose

`InoOnnx` is an Unreal Engine 5.7 runtime plugin whose job is to
**stage Microsoft's prebuilt ONNX Runtime + DirectML binaries and expose
them as a UE module** that other plugins (currently `InoAgents`) declare
as a dependency. Consumers `#include "InoOnnx.h"` and call
`InoAgents::Onnx::GetApi()` to reach the `OrtApi*` vtable; ORT sessions /
tensors are then driven through that vtable.

Unlike LiteRT-LM (built from source by the sibling `InoLiteRT` plugin),
ORT is consumed pre-built — Microsoft ships an industry-standard binary
for every platform we target, so building from source would be effort
without payoff.

The plugin is target-platform-aware: **Windows (Win64)** and **Android
(arm64-v8a)** ship today. iOS, Linux, and macOS would slot in by adding
platform branches to `InoOnnx.Build.cs` plus the matching prebuilt
download in `setup-onnxruntime.ps1`.

## Why a separate runtime from LiteRT-LM?

LiteRT-LM is TFLite-based and purpose-built for on-device LLM inference;
it does not cleanly run arbitrary ONNX models. ONNX Runtime is the
industry-standard runtime for ONNX graphs and has first-class prebuilt
binaries for every platform we target. Using each runtime for what it's
best at:

- **LLM inference** → LiteRT-LM (Gemma 4 today; via the InoLiteRT plugin)
- **Everything else in ONNX** → ONNX Runtime (Chatterbox Turbo TTS today,
  NeuTTS Nano's NeuCodec decoder, future vision/audio models)

Two small focused runtimes is cleaner than one mega-runtime trying to
cover every case.

## Layout

```
Plugins/InoOnnx/
├── InoOnnx.uplugin                  ← UE plugin manifest (LoadingPhase=PreLoadingScreen)
│
├── OnnxRuntime/                     ← setup workspace
│   ├── ONNXRUNTIME_VERSION          ← pinned ORT version (e.g. "1.24.3")
│   ├── DIRECTML_VERSION             ← pinned DirectML version (e.g. "1.15.4")
│   ├── scripts/
│   │   ├── setup-onnxruntime.ps1    ← downloads + stages prebuilt binaries
│   │   └── patch-ort-dml-import.py  ← patches ORT's PE import table
│   │                                  to look for InoDml.dll
│   └── .cache/                      ← downloaded NuGets/AARs (gitignored)
│
└── Source/
    ├── InoOnnx/                     ← The single UE module
    │   ├── InoOnnx.Build.cs         ← embeds third-party wiring
    │   │                              (RuntimeDependencies, UPL, includes)
    │   ├── InoOnnx_UPL_Android.xml  ← APK packaging directives
    │   ├── Public/InoOnnx.h         ← FInoOnnxModule + namespace InoAgents::Onnx
    │   │                              + LogInoOnnx category
    │   └── Private/InoOnnx.cpp      ← StartupModule loads DLL + smoke test
    │
    └── ThirdParty/                  ← staged setup outputs (consumed by UE)
        ├── Public/                  ← ORT C / C++ API headers
        │   └── (onnxruntime_c_api.h + dml_provider_factory.h + …)
        ├── Win64/                   ← Windows DLLs
        │   ├── InoOnnxRuntime.dll              (RENAMED from onnxruntime.dll, ~13 MB)
        │   ├── InoDml.dll                      (RENAMED from DirectML.dll, ~18 MB)
        │   └── onnxruntime_providers_shared.dll (original name, ~200 KB)
        └── Android/arm64-v8a/
            └── libInoOnnxRuntime.so            (RENAMED from libonnxruntime.so, ~25 MB)
```

## How other plugins consume this

In a consumer plugin's `Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[] {
    "InoOnnx",   // exposes <onnxruntime_c_api.h> + <InoOnnx.h>
                 // (provides InoAgents::Onnx::GetApi() accessor for the
                 //  OrtApi vtable). Stages the runtime DLLs/.so for cook.
});
```

In its `.uplugin`:

```json
"Plugins": [
    { "Name": "InoOnnx", "Enabled": true }
]
```

That's it. `FInoOnnxModule::StartupModule` runs at
`LoadingPhase=PreLoadingScreen`, strictly before any consumer's
`Default`-phase `StartupModule`, so by the time consumer code runs the
DLL/.so is loaded and the `OrtApi` vtable is cached. A
`OrtApi::GetAvailableProviders` smoke test at startup confirms the link.

Consumer code:
```cpp
#include "InoOnnx.h"  // for InoAgents::Onnx::GetApi()

const OrtApi* Api = InoAgents::Onnx::GetApi();
if (Api == nullptr) { /* graceful fallback */ return; }
// ... use Api->CreateSession etc.
```

The first and currently only consumer is `Plugins/InoAgents/`.

## Target platforms

| Platform              | Status      | Artifacts |
|---|---|---|
| **Windows (Win64)**   | ✅ shipping | `InoOnnxRuntime.dll` + `InoDml.dll` + `onnxruntime_providers_shared.dll` |
| **Android (arm64-v8a)** | ✅ shipping | `libInoOnnxRuntime.so` (CPU + XNNPACK + NNAPI + WebGPU EPs baked in) |
| iOS / Linux / macOS   | ⏳ not staged | No prebuilt download; consumers' `OrtApi*` is null on these platforms. |

## Why we rename the DLLs / .so (do NOT undo)

UE 5.7 ships **multiple** unrelated copies of ONNX Runtime AND DirectML
via bundled plugins:

- `Engine/Plugins/NNE/NNERuntimeORT/.../onnxruntime.dll` (UE's NNE, ORT 1.19.x)
- Various Marketplace plugins shipping their own `onnxruntime.dll` /
  `libonnxruntime.so` at older ORT versions
- `Engine/Binaries/Win64/DML/x64/DirectML.dll` (UE's bundled DirectML,
  loaded early by NNE / lip-sync via `LoadLibraryA`)

If we shipped under default names, three things break:

1. **Windows ORT** — `LoadLibrary` caches DLLs by base name, so
   `FPlatformProcess::GetDllHandle` returns whichever `onnxruntime.dll`
   was already loaded into the process (usually NNE's older 1.19.x).
   Our `OrtApi::GetApi(ORT_API_VERSION=24)` then returns nullptr because
   that older DLL doesn't implement API 24.
2. **Windows DirectML** — `InoOnnxRuntime.dll` has `DirectML.dll` as a
   delay-load dependency. UE's plugins load their copy first, winning
   the cache. Our delay-load stub then binds to UE's DirectML version on
   first call, causing kernel-validation failures on fp16 attention and
   silent numerical corruption.
3. **Android** — clang's linker at `libUnreal.so` build time resolves
   `OrtGetApiBase` against whichever `libonnxruntime.so` it sees first
   (often a Marketplace plugin's 1.19.2), recording a versioned symbol
   reference `OrtGetApiBase@VERS_1.19.2`. At runtime our 1.24.3 .so
   tagged `VERS_1.24.3` can't satisfy that, and the dynamic linker
   aborts the process during `libUnreal.so` init — silently, before
   UE's logger is up.

The fix:

- **Rename** the conflicting DLLs to base names no other plugin uses
  (`InoOnnxRuntime.dll`, `InoDml.dll`, `libInoOnnxRuntime.so`).
- **Patch** `InoOnnxRuntime.dll`'s PE delay-import table so its
  `DirectML.dll` reference becomes `InoDml.dll`. See
  `OnnxRuntime/scripts/patch-ort-dml-import.py`.
- **Dynamic load only** — `InoOnnx.Build.cs` does NOT use
  `PublicAdditionalLibraries` or `PublicDelayLoadDLLs`. The runtime
  resolves `OrtGetApiBase` via `GetProcAddress` / `dlsym` on the renamed
  binary, then drives everything through the returned `OrtApi*` vtable.
  No static-linker reference to ORT exists in `libUnreal.so`.

The combined effect: `libUnreal.so` has zero `Ort*` symbol references,
our `InoOnnxRuntime.dll`'s only delay-load DML target is `InoDml.dll`
(a base name no other plugin owns), and base-name cache collisions
become structurally impossible.

## How this relates to UE's NNE

UE 5.7 ships the **Neural Network Engine (NNE)** which bundles ONNX
Runtime for its own use cases (learned animation, style transfer, small
CNN inference). We do not use NNE for InoOnnx because:

- NNE's high-level API is designed for "tensor in, tensor out" — one
  forward pass. Our workloads (autoregressive TTS generation, KV-cache
  management) want direct `Ort::Session` access.
- Pinning ORT via our own script lets us pick a specific version + EP
  set independent of whatever UE's NNE ships with.
- The two copies coexist fine because the renames keep them isolated.

## Setup

```powershell
cd Plugins/InoOnnx/OnnxRuntime/scripts
./setup-onnxruntime.ps1
```

The script is idempotent (safe to re-run). It:

1. Reads pinned versions from `OnnxRuntime/ONNXRUNTIME_VERSION` and
   `OnnxRuntime/DIRECTML_VERSION`.
2. Skips early if a stamp file shows both are already staged.
3. Downloads three artifacts to `OnnxRuntime/.cache/` (cached if present):
   - `Microsoft.ML.OnnxRuntime.DirectML` NuGet (DML-flavored ORT)
   - `Microsoft.AI.DirectML` NuGet (DirectML runtime DLL)
   - `onnxruntime-android-<ver>.aar` from Maven Central
4. Extracts + renames + stages binaries into `Source/ThirdParty/`.
5. Runs `patch-ort-dml-import.py` on the ORT DLL (patches
   `DirectML.dll` → `InoDml.dll` in the delay-import table).
6. Writes the version stamp.

Python 3 + `pip install pefile` is required (one-time, for the patch
script).

## Bumping the pin

Both Windows + Android must have the target version published before
bumping — Maven Central typically lags GitHub releases by a few days,
NuGet sometimes lags by a week. The newest version with **both Win64
NuGet AND Android AAR** published is what to pin.

```powershell
# 1. Edit ONNXRUNTIME_VERSION (e.g. 1.25.0)
# 2. Re-run setup
cd Plugins/InoOnnx/OnnxRuntime/scripts
./setup-onnxruntime.ps1

# 3. Run smoke tests in PIE:
#      Ino.Onnx.ProvidersTest
#      Ino.Onnx.SessionFromFileTest <abs-path-to-model.onnx>

# 4. Commit ONNXRUNTIME_VERSION bump + any Build.cs tweaks.
```

DirectML version is pinned independently in `DIRECTML_VERSION` because
the ORT NuGet only declares a *minimum* compatible version — explicit
pin prevents a transitive bump from surprising us.

## Execution providers

| Platform | EPs available |
|---|---|
| Windows | CPU, **DirectML** (D3D12 GPU + NPU on 24H2+) |
| Android | CPU, XNNPACK, NNAPI, WebGPU |

DirectML is the only Windows accelerator we ship. CUDA / TensorRT / ROCm
are intentionally not shipped — they require 150+ MB of vendor-specific
runtime libs per game; DirectML covers NVIDIA, AMD, Intel, and NPUs on
a single D3D12 path.

DirectML has known kernel-level bugs that show up on real models (e.g.
the Chatterbox `language_model` and `speech_encoder` sessions hit
`E_INVALIDARG` on `MultiHeadAttention` / `Slice`). Consumer plugins
default to CPU and let the caller flip individual sessions to GPU after
verifying — the matrix lives in InoAgents'
`FInoChatterboxPerformanceOptions`, not here.

## Troubleshooting

- **`Invoke-WebRequest` fails with TLS / proxy error**: the setup script
  uses `-UseBasicParsing` but needs outbound HTTPS to NuGet + Maven. On
  corporate networks, set `$env:HTTPS_PROXY` before running.
- **`patch-ort-dml-import.py` fails**: ensure Python 3 is on PATH and
  `pip install pefile`.
- **`Expand-Archive` says "file not found"**: AAR / NuGet files are
  zips in disguise. The script copies to `.zip` before extracting. If
  you see this on re-run, delete `OnnxRuntime/.cache/` and retry.
- **Stamp file drift loops**: if staging fails partway and the
  `.ort_version` stamp is stale, delete it and re-run.
- **Smoke test reports DirectML not available**: check that
  `InoDml.dll` is actually in `Source/ThirdParty/Win64/`, and that
  `Ino.Onnx.ProvidersTest` log line lists `DmlExecutionProvider`. If
  not, the import-table patch likely failed — re-run setup.

## Authoritative references

- ONNX Runtime: https://github.com/microsoft/onnxruntime
- DirectML: https://github.com/microsoft/DirectML
- ORT C API header (staged copy consumers `#include`):
  `Source/ThirdParty/Public/onnxruntime_c_api.h`
- `OrtApi` accessor: `Source/InoOnnx/Public/InoOnnx.h` →
  `InoAgents::Onnx::GetApi()`
- Pinned versions: `OnnxRuntime/ONNXRUNTIME_VERSION` and
  `OnnxRuntime/DIRECTML_VERSION`

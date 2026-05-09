# CLAUDE.md — InoOnnx plugin

This file provides guidance to Claude Code (claude.ai/code) when working
inside `Plugins/InoOnnx/`. The hosting demo project is documented in
`E:/Projects/InoProject/CLAUDE.md`.

## Purpose

`InoOnnx` is an Unreal Engine 5.7 runtime plugin whose job is to
**stage Microsoft's prebuilt ONNX Runtime + DirectML binaries and expose
them as a UE module** that other plugins (currently `InoAgents`) declare
as a dependency. Consumers have two API levels to choose from:

- **Raw C API** — `#include "InoOnnx.h"` and call `InoOnnx::GetApi()`
  for the `OrtApi*` vtable; drive sessions and tensors directly through
  it. Maximum control, zero abstraction overhead.
- **Generic C++ wrapper** — `#include "InoOnnxSession.h"` /
  `InoOnnxTensor.h` for `FInoOnnxSession`, `FInoOnnxTensor`, and the
  `EInoOnnxDtype` / `EInoOnnxProvider` enums. Move-only RAII, async
  runs, dtype-safe data access. The right default for any new ORT
  integration unless you specifically need raw-C-API control. See
  "Generic ORT C++ wrapper" below.

Unlike LiteRT-LM (built from source by the sibling `InoLiteRT` plugin),
ORT is consumed pre-built — Microsoft ships an industry-standard binary
for every platform we target, so building from source would be effort
without payoff.

The plugin is target-platform-aware: **Windows (Win64)**, **Android
(arm64-v8a)**, **macOS (Apple Silicon)**, and **iOS (arm64 device)**
ship today. Linux would slot in by adding a platform branch to
`InoOnnx.Build.cs` plus the matching prebuilt download in
`setup-onnxruntime.ps1`.

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
│   │   ├── patch-ort-dml-import.py  ← patches ORT's PE import table
│   │   │                              to look for InoDml.dll (pefile)
│   │   ├── patch-ort-android-soname.py ← patches the Android .so's
│   │   │                              DT_SONAME to libInoOnnxRuntime.so (lief)
│   │   └── patch-ort-apple.py       ← patches Mac dylib LC_ID_DYLIB +
│   │                                  renames iOS framework end-to-end
│   │                                  (lief + plistlib)
│   └── .cache/                      ← downloaded NuGets/AARs (gitignored)
│
└── Source/
    ├── InoOnnx/                     ← The single UE module
    │   ├── InoOnnx.Build.cs         ← embeds third-party wiring
    │   │                              (RuntimeDependencies, UPL,
    │   │                               PublicAdditionalFrameworks)
    │   ├── InoOnnx_UPL_Android.xml  ← APK packaging directives
    │   ├── Public/
    │   │   ├── InoOnnx.h            ← FInoOnnxModule + namespace InoOnnx
    │   │   │                          (Init/Shutdown/GetApi) + LogInoOnnx
    │   │   └── Onnx/                ← generic, model-agnostic ORT C++ wrapper
    │   │       ├── InoOnnxTypes.h   ← EInoOnnxDtype, EInoOnnxProvider,
    │   │       │                      EInoOnnxGraphOptimizationLevel,
    │   │       │                      FInoOnnxSessionOptions
    │   │       ├── InoOnnxSession.h ← FInoOnnxSession (load .onnx, run sync/async)
    │   │       └── InoOnnxTensor.h  ← FInoOnnxTensor (move-only OrtValue wrapper)
    │   └── Private/
    │       ├── InoOnnx.cpp          ← StartupModule: DLL load + smoke test
    │       └── Onnx/                ← wrapper impl
    │           ├── InoOnnxInternal.{h,cpp}  ← shared helpers (status check,
    │           │                              dtype mapping, global OrtEnv)
    │           ├── InoOnnxSession.cpp
    │           └── InoOnnxTensor.cpp
    │
    └── ThirdParty/                  ← staged setup outputs (consumed by UE)
        ├── Public/                  ← ORT C / C++ API headers
        │   └── (onnxruntime_c_api.h + dml_provider_factory.h +
        │       coreml_provider_factory.h + …)
        ├── Win64/                   ← Windows DLLs
        │   ├── InoOnnxRuntime.dll              (RENAMED from onnxruntime.dll, ~17 MB)
        │   ├── InoDml.dll                      (RENAMED from DirectML.dll, ~18 MB)
        │   └── onnxruntime_providers_shared.dll (original name, ~22 KB)
        ├── Android/arm64-v8a/
        │   └── libInoOnnxRuntime.so            (RENAMED from libonnxruntime.so, ~25 MB)
        ├── Mac/
        │   └── libInoOnnxRuntime.dylib         (RENAMED from libonnxruntime.dylib,
        │                                        Apple Silicon arm64; CPU + CoreML EP, ~12 MB)
        └── IOS/
            ├── InoOnnxRuntime.framework/       (RENAMED from onnxruntime.framework;
            │   ├── InoOnnxRuntime              iOS device arm64; CPU + CoreML, ~14 MB)
            │   ├── Info.plist                  (CFBundle{Executable,Name,Identifier} patched)
            │   └── Headers/
            └── Simulator/
                └── InoOnnxRuntime.framework/   (iOS simulator arm64+x86_64 fat;
                                                 dev only — not wired into Build.cs)
```

## How other plugins consume this

In a consumer plugin's `Build.cs`:

```csharp
PublicDependencyModuleNames.AddRange(new string[] {
    "InoOnnx",   // exposes <onnxruntime_c_api.h> + <InoOnnx.h>
                 // (provides InoOnnx::GetApi() accessor for the OrtApi
                 //  vtable). Also exposes <InoOnnxSession.h> + friends
                 //  for the generic C++ wrapper. Stages the runtime
                 //  DLLs/.so for cook.
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

Consumer code — pick one of two API levels:

```cpp
// Level 1: raw ORT C API. Maximum control; the original entry point
// callers have used since the plugin was extracted from InoAgents.
#include "InoOnnx.h"  // for InoOnnx::GetApi()

const OrtApi* Api = InoOnnx::GetApi();
if (Api == nullptr) { /* graceful fallback */ return; }
// ... use Api->CreateSession etc.
```

```cpp
// Level 2: generic C++ wrapper. Owned types, move-only RAII, dtype
// safety, async runs. Use this for any new ORT integration unless
// you specifically need raw-C-API control.
#include "InoOnnxSession.h"
#include "InoOnnxTensor.h"

FInoOnnxSessionOptions Options;
Options.ExecutionProviders = { EInoOnnxProvider::Cpu };  // or DirectMl, Nnapi, ...
TUniquePtr<FInoOnnxSession> Session = FInoOnnxSession::Create(ModelPath, Options);
if (!Session) { /* graceful fallback */ return; }

TArray<FInoOnnxTensor> Outputs;
Session->Run(Inputs, Outputs);
```

The first and currently only consumer is `Plugins/InoAgents/`.

## Target platforms

| Platform                 | Status        | Artifacts |
|---|---|---|
| **Windows (Win64)**      | ✅ shipping   | `InoOnnxRuntime.dll` + `InoDml.dll` + `onnxruntime_providers_shared.dll`. EPs: CPU + DirectML. |
| **Android (arm64-v8a)**  | ✅ shipping   | `libInoOnnxRuntime.so` (single .so). EPs: CPU + XNNPACK + NNAPI + WebGPU. |
| **macOS (arm64)**        | ✅ shipping   | `libInoOnnxRuntime.dylib` (Apple Silicon only — Microsoft drops Intel Mac in modern ORT NuGets). EPs: CPU + CoreML. |
| **iOS (arm64 device)**   | ✅ shipping   | `InoOnnxRuntime.framework` (renamed from `onnxruntime.framework`, embedded via `PublicAdditionalFrameworks`). EPs: CPU + CoreML + XNNPACK. |
| **iOS Simulator**        | ⚙️ staged     | Framework staged at `Source/ThirdParty/IOS/Simulator/InoOnnxRuntime.framework/` (arm64+x86_64 fat) for dev iteration in Xcode simulator, but `InoOnnx.Build.cs` only wires the device slice into shipped iOS builds. Flip the framework path or branch on `Target.Architecture` if you need simulator. |
| Linux                    | ⏳ not staged | No prebuilt download; consumers' `OrtApi*` is null on this platform. |

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
3. **Android** — two failure modes, one at link time and one at load
   time:
   - **Link time**: clang's linker at `libUnreal.so` build time resolves
     `OrtGetApiBase` against whichever `libonnxruntime.so` it sees first
     (often a Marketplace plugin's 1.19.2), recording a versioned
     symbol reference `OrtGetApiBase@VERS_1.19.2`. At runtime our 1.24.3
     .so tagged `VERS_1.24.3` can't satisfy that, and the dynamic
     linker aborts the process during `libUnreal.so` init — silently,
     before UE's logger is up.
   - **Load time / SONAME aliasing**: Android's dynamic linker dedupes
     loaded libraries by `DT_SONAME` (the embedded ELF entry), NOT by
     filename. Microsoft's onnxruntime-android AAR ships every ABI's
     .so with `DT_SONAME = "libonnxruntime.so"`. Renaming only the
     FILE to `libInoOnnxRuntime.so` leaves the SONAME unchanged, so if
     any other plugin (e.g. RuntimeMetaHumanLipSync) loads a real
     `libonnxruntime.so` first, the linker registers it under SONAME
     `libonnxruntime.so` and our subsequent `dlopen("libInoOnnxRuntime.so")`
     reads our file's SONAME, finds it already loaded, and **returns
     the marketplace plugin's older handle**. `OrtApi::GetApi(24)` then
     returns nullptr against that 1.19.x copy.

The fix:

- **Rename** the conflicting DLLs to base names no other plugin uses
  (`InoOnnxRuntime.dll`, `InoDml.dll`, `libInoOnnxRuntime.so`).
- **Patch** `InoOnnxRuntime.dll`'s PE delay-import table so its
  `DirectML.dll` reference becomes `InoDml.dll`. See
  `OnnxRuntime/scripts/patch-ort-dml-import.py`.
- **Patch** the Android `.so`'s `DT_SONAME` to match the renamed
  filename (`libInoOnnxRuntime.so`). File rename alone is insufficient
  — see "Load time / SONAME aliasing" above. Done via
  `OnnxRuntime/scripts/patch-ort-android-soname.py` (uses lief; one-time
  `pip install lief` required). The new SONAME is longer than the
  original, so unlike the Windows DML patch this needs a real ELF
  editor that can extend the dynamic string table.
- **Patch** the Mac dylib's `LC_ID_DYLIB` install_name to
  `@rpath/libInoOnnxRuntime.dylib` and the iOS framework end-to-end
  (rename framework dir + binary, patch LC_ID_DYLIB to
  `@rpath/InoOnnxRuntime.framework/InoOnnxRuntime`, rewrite Info.plist's
  `CFBundleExecutable` / `CFBundleName` / `CFBundleIdentifier`). Done
  via `OnnxRuntime/scripts/patch-ort-apple.py` (lief Mach-O editor +
  Python's stdlib `plistlib`). Apple platforms don't have Windows'
  base-name DLL cache or Android's SONAME aliasing — dyld looks up by
  full @rpath + LC_ID_DYLIB install_name — so collision risk is much
  lower here than on those two platforms. We rename anyway for
  defence-in-depth + naming consistency, and because Apple's
  code-signing rejects framework-name / Info.plist mismatches at
  packaging time (so once we rename the binary we MUST also patch the
  plist to match).
- **Dynamic load only** (Win64 / Android / Mac) — `InoOnnx.Build.cs`
  does NOT use `PublicAdditionalLibraries` or `PublicDelayLoadDLLs` on
  these platforms. The runtime resolves `OrtGetApiBase` via
  `GetProcAddress` / `dlsym` on the renamed binary, then drives
  everything through the returned `OrtApi*` vtable. No static-linker
  reference to ORT exists in `libUnreal.so` / `libUnreal.dylib`.

**iOS is the exception** — `PublicAdditionalFrameworks` does double
duty (adds `-framework InoOnnxRuntime` to the link command AND embeds
`InoOnnxRuntime.framework` into the `.app`'s `Frameworks/` directory at
packaging time). iOS has no reliable equivalent of `dlopen`-by-full-path
that works across all supported iOS versions and signing modes (App
Store, ad-hoc, dev). The framework is auto-loaded by dyld at app launch
before any UE module runs; `InoOnnx.cpp`'s iOS Init resolves
`OrtGetApiBase` via `dlsym(RTLD_DEFAULT, ...)` so the consumer-facing
API stays uniform across platforms — only the load mechanism differs.

The combined effect (Win64 / Android / Mac): `libUnreal.so` /
`libUnreal.dylib` has zero `Ort*` symbol references, our
`InoOnnxRuntime.dll`'s only delay-load DML target is `InoDml.dll` (a
base name no other plugin owns), our Android `.so` has a unique SONAME
so it can't alias to a marketplace plugin's `libonnxruntime.so`, our
Mac dylib has a unique `LC_ID_DYLIB` install_name and lives at a
unique path under `Source/ThirdParty/Mac/`, and filename-cache
(Windows), SONAME-cache (Android), and dyld-cache (Mac) collisions
become structurally impossible. iOS dodges the whole question by
having dyld pre-load the framework at app launch — there is nothing
for our code to load or alias.

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
2. Skips early if a stamp file shows the matching versions are already
   staged AND every required output file is present (Win64 DLLs,
   Android `.so`, Mac dylib, iOS device + simulator framework binaries).
3. Downloads four artifacts to `OnnxRuntime/.cache/` (cached if present):
   - `Microsoft.ML.OnnxRuntime.DirectML` NuGet (DML-flavored ORT for Win64)
   - `Microsoft.AI.DirectML` NuGet (DirectML runtime DLL)
   - `onnxruntime-android-<ver>.aar` from Maven Central
   - `Microsoft.ML.OnnxRuntime` NuGet (CPU-flavored — Mac + iOS slices)
4. Extracts + renames + stages binaries into `Source/ThirdParty/`.
5. Runs the three patcher scripts to apply the rename surgery:
   - `patch-ort-dml-import.py` — Windows DML PE import table
     (`DirectML.dll` → `InoDml.dll` in delay-imports).
   - `patch-ort-android-soname.py` — Android ELF DT_SONAME
     (`libonnxruntime.so` → `libInoOnnxRuntime.so`).
   - `patch-ort-apple.py` — Mac/iOS Mach-O LC_ID_DYLIB + iOS framework
     dir / binary / Info.plist rename (one script handles both modes).
6. Writes the version stamp.

Python 3 + `pip install pefile lief` is required (one-time):
- `pefile` — for the Windows DML import-table patch.
- `lief` — for the Android `.so` SONAME patch AND the Mac/iOS Mach-O
  LC_ID_DYLIB rewrites.
- `plistlib` — Python stdlib (no install); used by `patch-ort-apple.py`
  to rewrite the iOS framework Info.plist.

## Bumping the pin

All four platforms must have the target version published before
bumping — Maven Central typically lags GitHub releases by a few days,
NuGet sometimes lags by a week. The newest version with **all of
DirectML NuGet AND Android AAR AND CPU NuGet (Mac + iOS slices)
published** is what to pin. In practice the DirectML and CPU NuGets
publish together since they're both part of the same Microsoft release
flow; Maven Central is the usual long pole.

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
| Windows  | CPU, **DirectML** (D3D12 GPU + NPU on 24H2+) |
| Android  | CPU, XNNPACK, NNAPI, WebGPU |
| macOS    | CPU, **CoreML** (CPU/GPU/ANE on Apple Silicon) |
| iOS      | CPU, **CoreML**, XNNPACK |

DirectML is the only Windows accelerator we ship. CUDA / TensorRT / ROCm
are intentionally not shipped — they require 150+ MB of vendor-specific
runtime libs per game; DirectML covers NVIDIA, AMD, Intel, and NPUs on
a single D3D12 path.

CoreML is the Apple-platform analog. It's statically compiled into
Microsoft's prebuilt osx-arm64 dylib and ios xcframework — no separate
download or activation step. The provider self-registers when the
binary is mapped (Mac dlopen / iOS dyld auto-load). Request it via
`FInoOnnxSessionOptions::ExecutionProviders = { EInoOnnxProvider::CoreMl,
EInoOnnxProvider::Cpu }` for Apple-Silicon Macs and iOS devices that
ship the Apple Neural Engine.

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
- **`patch-ort-android-soname.py` fails**: ensure Python 3 is on PATH
  and `pip install lief`. Lief is a separate dependency from pefile.
- **`patch-ort-apple.py` fails**: same dependency as the Android script
  (`pip install lief`). On a Windows host the script can still patch
  Mach-O binaries via lief; install_name_tool is NOT required (and
  isn't available on Windows anyway). If the iOS framework ends up
  with mismatched Info.plist + binary names, Apple's codesign rejects
  the bundle at packaging time — re-run the setup script to re-stage.
- **iOS code-signing fails with "binary identity does not match"**:
  the Info.plist's CFBundleExecutable / CFBundleName / CFBundleIdentifier
  must match the framework dir name and binary file name. Our patcher
  rewrites all three to `InoOnnxRuntime` / `io.inoland.InoOnnxRuntime`
  in lockstep. If a manual edit got out of sync, delete
  `Source/ThirdParty/IOS/InoOnnxRuntime.framework/` and re-run setup.
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
- CoreML EP docs: https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html

Upstream release artifacts we consume (per `ONNXRUNTIME_VERSION`):
- Win64 ORT + DirectML EP: `Microsoft.ML.OnnxRuntime.DirectML` NuGet
- Win64 DirectML runtime: `Microsoft.AI.DirectML` NuGet (pinned via
  `DIRECTML_VERSION`)
- Android: `onnxruntime-android-<ver>.aar` from Maven Central
- Mac (osx-arm64) + iOS xcframework: `Microsoft.ML.OnnxRuntime` NuGet
  (the regular CPU NuGet, NOT the DirectML variant)

- ORT C API header (staged copy consumers `#include`):
  `Source/ThirdParty/Public/onnxruntime_c_api.h`
- Raw C API accessor: `Source/InoOnnx/Public/InoOnnx.h` →
  `InoOnnx::GetApi()`
- Generic C++ wrapper:
  - `Source/InoOnnx/Public/Onnx/InoOnnxSession.h` → `FInoOnnxSession`
  - `Source/InoOnnx/Public/Onnx/InoOnnxTensor.h`  → `FInoOnnxTensor`
  - `Source/InoOnnx/Public/Onnx/InoOnnxTypes.h`   → `EInoOnnxDtype`,
    `EInoOnnxProvider`, `FInoOnnxSessionOptions`, etc.
- Pinned versions: `OnnxRuntime/ONNXRUNTIME_VERSION` and
  `OnnxRuntime/DIRECTML_VERSION`

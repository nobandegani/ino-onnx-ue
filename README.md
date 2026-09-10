# InoOnnx

**[ONNX Runtime](https://github.com/microsoft/onnxruntime) packaged for Unreal Engine 5.** Run
ONNX models in-process on Windows, Android, macOS and iOS — with DirectML GPU/NPU acceleration on
Windows, CoreML on Apple, and NNAPI / XNNPACK / WebGPU on Android.

Unlike the other runtime plugins in this set, InoOnnx offers a wrapped API alongside the raw one:
`FInoOnnxSession` and `FInoOnnxTensor` give you a UE-idiomatic, RAII, move-safe surface over ORT's
C API instead of leaving you to manage `OrtSession*` by hand.

> ### ⚠️ Two things to know before shipping
>
> **The shipped `InoOnnxRuntime.dll` is binary-patched, so Microsoft's code signature on it is no
> longer valid.** And `InoDml.dll` is Microsoft's **proprietary** DirectML, not open source.
> Both are deliberate and both are explained in [Licensing](#licensing) and
> [Why everything is renamed](#why-everything-is-renamed).

---

## Contents

- [What consumers get](#what-consumers-get)
- [Requirements](#requirements)
- [Install](#install)
- [Staging the runtime](#staging-the-runtime)
- [Why everything is renamed](#why-everything-is-renamed)
- [Platform support](#platform-support)
- [Smoke tests](#smoke-tests)
- [Troubleshooting](#troubleshooting)
- [Licensing](#licensing)

---

## What consumers get

Declare `InoOnnx` in your `.uplugin` `Plugins` array and your `Build.cs`
`PublicDependencyModuleNames`. There are **two API levels** — pick one.

### Level 1 — raw ORT C API

Maximum control, zero abstraction:

```cpp
#include "InoOnnx.h"

const OrtApi* Api = InoOnnx::GetApi();
if (Api == nullptr)
{
    return;   // unsupported platform, or the runtime failed to load
}
// drive Api->CreateSession etc. directly
```

### Level 2 — the C++ wrapper (recommended)

RAII, move-safe, dtype-checked. The right default unless you specifically need the raw vtable:

```cpp
#include "Onnx/InoOnnxSession.h"

FInoOnnxSessionOptions Options;

// Priority-ordered, with graceful fallthrough: entries that aren't
// registered in this build are skipped with a warning, and CPU is always
// the implicit final fallback. So one list can ship to every platform.
Options.ExecutionProviders = {
    EInoOnnxProvider::DirectMl,   // Windows GPU/NPU
    EInoOnnxProvider::CoreMl,     // Apple
    EInoOnnxProvider::Nnapi,      // Android
    EInoOnnxProvider::Xnnpack,    // fast CPU kernels everywhere
};

FString Error;
TUniquePtr<FInoOnnxSession> Session =
    FInoOnnxSession::Create(ModelPath, Options, &Error);
if (!Session)
{
    return;   // Error carries ORT's message; it is also logged to LogInoOnnx
}

// Pay the JIT / kernel-selection cost up front, off the hot path
Session->Warmup(DummyInputs);

TArray<FInoOnnxTensor> Outputs;
if (Session->Run(Inputs, Outputs, &Error))
{
    // Outputs own their buffers
}
```

`CreateFromMemory` takes an in-memory model if you'd rather not touch disk.

**Design notes.** `FInoOnnxSession` is heap-only via `TUniquePtr` — copy *and* move are deleted,
so a session can never be silently relocated out from under ORT's internal pointers. `Warmup()`
exists because the first `Run()` on a non-trivial graph is typically 2–10× slower than steady
state; calling it at load time moves that cost off the user-visible path. Every failure path
returns `false`, fills `OutError` when you pass one, and logs to `LogInoOnnx` either way.

`FInoOnnxSessionOptions` is `BlueprintType`, alongside `EInoOnnxProvider`, `EInoOnnxDtype`,
`EInoOnnxExecutionMode` and `EInoOnnxGraphOptimizationLevel`, so session configuration can be
exposed to designers. It also carries `IntraOpThreadCount` / `InterOpThreadCount` (0 = ORT
default; capping at 2–4 per session usually beats letting every session claim all cores),
`ExecutionMode`, and `DirectMlAdapterIndex`.

> `ExecutionMode = Parallel` is incompatible with DirectML. If DML is anywhere in the provider
> list, Sequential is forced automatically — you don't have to remember it.

Not every enum value is available on every platform — see
[Platform support](#platform-support) for what each build actually carries. `Cuda` and `TensorRt`
in particular are declared but **not shipped**; listing one is harmless (it's skipped with a
warning) but it won't do anything.

---

## Requirements

**To consume it:** Unreal Engine 5 (developed against 5.7). Prebuilt runtimes for all four
platforms are committed, so a clean clone compiles with no toolchain.

**To re-stage the runtime:** PowerShell, Python 3, and `pip install pefile lief` — the staging
step rewrites binaries (see below).

---

## Install

```bash
cd YourProject/Plugins
git clone https://github.com/nobandegani/ino-onnx-ue.git InoOnnx
```

No submodules — the runtime arrives as NuGet/AAR packages at staging time, and the staged
binaries are committed. Add `InoOnnx` to your `.uproject` `Plugins` array, regenerate project
files, and build.

---

## Staging the runtime

`OnnxRuntime/scripts/setup-onnxruntime.ps1` downloads and stages everything. Pinned versions:

| Component | Version | Source |
|---|---|---|
| ONNX Runtime | **1.24.3** (`ONNXRUNTIME_VERSION`) | `Microsoft.ML.OnnxRuntime.DirectML` NuGet |
| DirectML | **1.15.4** (`DIRECTML_VERSION`) | `Microsoft.AI.DirectML` NuGet |

Three helper scripts do the binary rewriting:

| Script | Platform | What it rewrites |
|---|---|---|
| `patch-ort-dml-import.py` | Win64 | ORT's PE delay-import table: `DirectML.dll` → `InoDml.dll` (in-place byte edit via `pefile`) |
| `patch-ort-android-soname.py` | Android | ELF `DT_SONAME` → `libInoOnnxRuntime.so` (via `lief`, since the new name is longer) |
| `patch-ort-apple.py` | macOS / iOS | macOS: Mach-O `LC_ID_DYLIB` install name. iOS: framework dir + binary rename + `Info.plist` identity — no install-name edit, since the binary is a static archive (via `lief` + `plistlib`) |

---

## Why everything is renamed

This is the most important thing to understand about this plugin, and it is not arbitrary.

**The problem: you are not the only ONNX Runtime in the process.** UE ships ORT 1.19.x under
`Engine/Plugins/NNE/NNERuntimeORT/` for its Neural Network Engine, its own DirectML under
`Engine/Binaries/Win64/DML/x64/`, and marketplace plugins bring more — `RuntimeMetaHumanLipSync`
carries ORT 1.19.2, for instance.

Every major platform dedupes native libraries by a *name*, and that name is not the filename:

| Platform | Dedupes by | Failure if you don't rename |
|---|---|---|
| **Windows** (ORT) | DLL **base name** | `GetDllHandle` returns whichever `onnxruntime.dll` loaded first — usually NNE's 1.19.x. `OrtGetApi(24)` returns NULL. |
| **Windows** (DirectML) | DLL **base name** | Another plugin `LoadLibrary`s `DirectML.dll` first, so your delay-load stub binds UE's different build. Result: kernel-validation failures and **silent fp16 numerical corruption**. |
| **Android** (load time) | ELF **`DT_SONAME`** | `dlopen("libInoOnnxRuntime.so")` returns the *other* plugin's handle — a file rename doesn't change the SONAME. `OrtGetApi(24)` returns NULL. |
| **Android** (link time) | **Versioned symbols** | Worse: the linker can bind `OrtGetApiBase@VERS_1.19.2` from another plugin's `.so` at `libUnreal.so` build time. At runtime your 1.24.3 copy is tagged `VERS_1.24.3`, can't satisfy it, and **the dynamic linker aborts the process during init — before UE's logger exists**, so you get no message at all. |
| **macOS** | `@rpath` + `LC_ID_DYLIB` | Much lower risk; dyld resolves by full install name. Renamed for defence-in-depth. |
| **iOS** | n/a | No runtime library to alias — symbols are static-linked. Renamed because Apple's codesign rejects framework-name / `Info.plist` mismatches. |

The Windows/Android cases are avoided entirely by resolving `OrtGetApiBase` dynamically
(`GetProcAddress` / `dlsym`) rather than via `PublicAdditionalLibraries` or
`PublicDelayLoadDLLs`, so `libUnreal.so` carries **zero** `Ort*` symbol references.

So a file rename alone is insufficient on Windows and Android — the *name recorded inside the
binary* has to change too. Hence the patch scripts. After patching, no other plugin is looking for
`InoDml.dll` or `libInoOnnxRuntime.so`, so the caches can't alias.

The silent-fp16-corruption case is why this matters more than it looks: the failure mode isn't a
crash, it's wrong numbers.

> **Consequence: Microsoft's Authenticode signature on `InoOnnxRuntime.dll` is invalidated** by
> the import-table edit. The bytes change; the signature no longer matches. Expect that
> aggressive AV or SmartScreen may flag it, and that environments enforcing signed-binary
> policies will reject it. `InoDml.dll` is **only renamed on disk** — its bytes are untouched, so
> its signature survives.

---

## Platform support

| Platform | Artifact | Execution providers | Linkage |
|---|---|---|---|
| **Windows x64** | `InoOnnxRuntime.dll`, `InoDml.dll`, `onnxruntime_providers_shared.dll` | CPU, **DirectML** (D3D12 GPU + NPU on 24H2+) | `GetProcAddress`, no static ref |
| **Android** arm64-v8a | `libInoOnnxRuntime.so` | CPU, XNNPACK, NNAPI, WebGPU | `dlsym`, no static ref |
| **macOS** Apple Silicon | `libInoOnnxRuntime.dylib` | CPU, **CoreML** (CPU/GPU/ANE) | `dlsym`, no static ref |
| **iOS** device (+ simulator, staged) | `InoOnnxRuntime.framework` | CPU, **CoreML**, XNNPACK | **Static archive**, linked in |

**macOS is Apple Silicon only** — Microsoft dropped Intel Mac from modern ORT NuGets. The iOS
Simulator slice is staged for dev iteration but is not wired into `Build.cs`; only the device
slice ships.

**iOS works differently from every other platform.** Microsoft ships ORT for iOS as a Unix `ar`
static archive wrapped in a fat header — not a dylib. It's linked with
`PublicAdditionalFrameworks` and `bCopyFramework=false`, so `Ort*` symbols end up baked into the
app executable and there is no runtime library to load at all. `dlsym(RTLD_DEFAULT, …)` then finds
them in the main binary, so the call site is identical to the other platforms even though the
mechanism underneath is completely different.

**CUDA, TensorRT and ROCm are intentionally not shipped** — each needs 150 MB+ of vendor-specific
runtime per game, and DirectML covers NVIDIA, AMD, Intel and NPUs on one D3D12 path. The enum
still declares them so the API won't change if they're ever added; requesting one logs a warning
and falls through.

On Windows, `FInoOnnxSessionOptions::DirectMlAdapterIndex` selects the adapter, and the
enumeration order matches DirectML's own `device_id` indexing — use `Ino.Onnx.ListDmlAdapters` to
see it.

> **DirectML has real kernel bugs on real models, independent of any version skew.**
> `MultiHeadAttention` and `Slice` have been observed failing with `E_INVALIDARG` on production
> graphs even with correctly matched binaries. Default your sessions to CPU and promote individual
> ones to DirectML only after verifying that specific model — don't assume GPU works graph-wide.

---

## Smoke tests

From the editor's Output Log:

```
Ino.Onnx.ProvidersTest          which execution providers ORT actually registered
Ino.Onnx.SessionFromFileTest    end-to-end session create + run from a model file
Ino.Onnx.ListDmlAdapters        enumerate DirectML adapters in device_id order
```

`ProvidersTest` is the quickest way to confirm DirectML really registered rather than silently
falling back to CPU.

---

## Troubleshooting

Everything logs under **`LogInoOnnx`**.

| Symptom | Cause |
|---|---|
| `OrtGetApi` returns NULL | A different, older ORT won the name race. Confirm the SONAME/base-name patching actually applied. |
| `E_INVALIDARG` from `MultiHeadAttention` or `Slice` | Version skew — your ORT is talking to a mismatched DirectML. This is exactly what the rename prevents. |
| fp16 outputs subtly wrong, no error | Same cause, worse symptom. Check which DirectML is loaded before anything else. |
| DirectML missing from `ProvidersTest` | `InoDml.dll` not staged, or the import patch didn't apply. |
| First inference far slower than later ones | Expected. Call `Warmup()` at load time. |
| AV flags `InoOnnxRuntime.dll` | Expected — the import-table patch invalidates Microsoft's signature. See above. |
| Staging fails on `pefile` / `lief` | `pip install pefile lief`, and make sure Python 3 is on PATH. |

---

## Licensing

This plugin is **Apache-2.0** ([`LICENSE`](LICENSE)); all source files carry matching headers.

Per-path detail is in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The two items that
matter:

> **⚠️ `Source/ThirdParty/Win64/InoDml.dll` is Microsoft's proprietary DirectML** (renamed from
> `DirectML.dll`, bytes unmodified), redistributed from the `Microsoft.AI.DirectML` NuGet package
> under Microsoft's own terms — not an OSI license. It is required **only** for the DirectML
> execution provider; on CPU it is unused and can be dropped from staging. Confirm the NuGet
> package's redistribution terms for your distribution, including whether renaming the file is
> within them.

> **⚠️ `InoOnnxRuntime.dll` is a modified binary.** ONNX Runtime is MIT, which permits
> modification, but be aware you are shipping a Microsoft-built DLL whose bytes — and therefore
> whose signature — no longer match what Microsoft published. The modification is one
> import-table string, documented above and reproducible from the scripts in this repo.

**ONNX model weights are not in this repository.** Any model you ship carries its own license.

# OnnxRuntime/ — ONNX Runtime integration for the InoAgents plugin

This directory holds the setup, version pin, and scripts for the **ONNX
Runtime** half of the plugin. It is parallel to (and independent of) the
`LiteRtLm/` directory that hosts the TFLite-based LLM stack.

ONNX Runtime is the foundation any ONNX-based model will use inside the
plugin. The first consumer is **Chatterbox Turbo** (Text-To-Speech with
zero-shot voice cloning), but the session infrastructure is model-agnostic
— any future ONNX model (codec decoders, speaker encoders, vision
adapters, etc.) links against the same runtime.

## Why a separate runtime from LiteRT-LM?

LiteRT-LM is TFLite-based; it is purpose-built for on-device LLM inference
and does not (cleanly) run arbitrary ONNX models. ONNX Runtime is the
industry-standard runtime for ONNX models and has first-class prebuilt
binaries for Win64 and Android. Using each runtime for what it's best at
means:

- LLM inference → LiteRT-LM (Gemma 4 today)
- Everything else in ONNX → ONNX Runtime (Chatterbox, future TTS models,
  future vision/audio models)

Two small, well-maintained third-party libraries is cleaner than one
mega-runtime trying to cover every case.

## Directory layout

```
OnnxRuntime/
├── ONNXRUNTIME_VERSION         ← pinned ORT version (e.g. "1.24.3")
├── scripts/
│   ├── setup-onnxruntime.ps1   ← downloads + stages prebuilt binaries
│   └── (future) update-onnxruntime.ps1  ← version bump helper
├── README.md                   ← this file
└── .cache/                     ← download cache (gitignored)
```

After `setup-onnxruntime.ps1` runs, the staging destinations under the
plugin are populated:

```
Source/ThirdParty/InoOnnxRuntime/
├── Public/                     ← ORT C/C++ API headers
├── Win64/
│   └── onnxruntime.lib         ← MSVC import library
└── .ort_version                ← matches ONNXRUNTIME_VERSION; used for drift detection

Binaries/ThirdParty/InoOnnxRuntime/
├── Win64/
│   ├── onnxruntime.dll                        (~13 MB, CPU EP)
│   └── onnxruntime_providers_shared.dll       (present on newer ORT releases)
└── Android/arm64-v8a/
    └── libonnxruntime.so                      (~15 MB, CPU + XNNPACK EPs)
```

Everything under `Binaries/ThirdParty/InoOnnxRuntime/` is **gitignored** —
the binaries are downloaded per-developer by the setup script, same
pattern as `Binaries/ThirdParty/InoAgentsLibrary/`.

## Getting started

```powershell
cd Plugins/InoAgents/OnnxRuntime/scripts
./setup-onnxruntime.ps1
```

The script:

1. Reads the pinned version from `OnnxRuntime/ONNXRUNTIME_VERSION`.
2. Downloads two artifacts to `OnnxRuntime/.cache/` (idempotent — cached
   if already present):
   - Windows: `onnxruntime-win-x64-<ver>.zip` from GitHub Releases.
   - Android: `onnxruntime-android-<ver>.aar` from Maven Central.
3. Extracts + stages into the paths shown above.
4. Writes a `.ort_version` stamp file so re-running the script is cheap.

Safe to re-run anytime. If you delete `Source/ThirdParty/InoOnnxRuntime/`
or `Binaries/ThirdParty/InoOnnxRuntime/`, re-running the script restores them.

## Bumping the version

1. Edit `ONNXRUNTIME_VERSION` to the new triple (e.g. `1.25.0`).
2. Delete `Source/ThirdParty/InoOnnxRuntime/.ort_version` (or let the
   script detect drift — it compares staged version to pinned version).
3. Re-run `setup-onnxruntime.ps1`.
4. Run the smoke tests (see Phase 5 in the plan) on both platforms.
5. Commit the `ONNXRUNTIME_VERSION` bump + any Build.cs tweaks required
   by API changes between ORT versions.

Check both platforms have a release at the target version before bumping
— Android releases on Maven Central sometimes lag the GitHub Windows
release by a week. As of initial setup the latest matched pair was
**1.24.3** (newer Windows 1.24.4 existed without an Android companion).

## Execution providers (current + future)

**Current: CPU-only on Windows, CPU + XNNPACK on Android.**

This keeps the initial footprint small and matches the common starting
point for TTS workloads — a modern gaming CPU handles Chatterbox Turbo
on Windows at roughly 1–2 s first-chunk latency; XNNPACK provides the
ARM-optimized fast path on Android.

**Future (separate follow-up phase): DirectML on Windows.** For
GPU-accelerated inference on Windows, we will add the DirectML provider
separately via the Microsoft.AI.DirectML NuGet or equivalent redistributable.
DirectML is the right GPU story because it rides the game's existing D3D12
device — no CUDA runtime to ship, no Vulkan stack collision with the
renderer. The CPU-only start today does not block that — `FInoOnnxSession`
will be provider-selectable from day one.

Why NOT CUDA / TensorRT: both require shipping large CUDA runtime
libraries (~150 MB), only work on NVIDIA hardware, and clash with how
the game already has a D3D12 renderer. DirectML covers the same
ground via D3D12 for every GPU vendor UE supports.

Why NOT NNAPI on Android: NNAPI delegation is inconsistent across
vendors and sometimes silently falls back to CPU with no diagnostic.
XNNPACK is deterministic and fast on every arm64 phone we would ship to.

## How this relates to UE's NNE

UE 5.7 ships the **Neural Network Engine (NNE)** which bundles ONNX
Runtime under the hood for its own use cases (learned animation, style
transfer, small CNN inference in gameplay systems). We do not use NNE
for the InoAgents plugin. Reasons:

- NNE's high-level API is designed for "tensor in, tensor out" — one
  forward pass. Our workloads (autoregressive TTS generation with
  streaming output, KV-cache management across tokens) want direct
  `Ort::Session` access.
- Pinning ORT via our own script lets us pick a specific version and
  provider set independent of whatever UE's NNE happens to ship with
  in a given engine release.
- NNE can keep doing whatever the rest of the project uses it for —
  the two copies of ORT coexist fine because they are loaded into
  different addresses at runtime and isolated by their wrapping
  modules.

## What lives where

| Concern | Owned by |
|---|---|
| Pinning, downloading, staging ORT binaries | `OnnxRuntime/scripts/setup-onnxruntime.ps1` |
| UBT module definition (link lib, stage DLLs, UPL for Android) | `Source/ThirdParty/InoOnnxRuntime/` (Phase 2) |
| ORT module startup (DLL preload on Windows) | `Source/InoAgents/Private/Onnx/InoOnnxModule.cpp` (Phase 3) |
| Generic C++ session wrapper | `Source/InoAgents/{Public,Private}/Onnx/InoOnnxSession.*` (Phase 4) |
| Smoke tests (console commands) | `Source/InoAgents/Private/SmokeTests/InoOnnxTest.cpp` (Phase 5) |
| TTS-specific code (Chatterbox and later) | Separate `Source/InoAgents/{Public,Private}/Tts/...` (follow-up plan) |

## Troubleshooting

- **`Invoke-WebRequest` fails with TLS / proxy error**: the setup script
  uses `-UseBasicParsing` but depends on outbound HTTPS to GitHub
  Releases and Maven Central. On corporate networks, set
  `$env:HTTPS_PROXY` before running.
- **Expand-Archive says "file not found"**: AAR files are zips in
  disguise. The script copies to `.zip` before extracting. If you see
  this error on a re-run, delete `OnnxRuntime/.cache/` and retry.
- **`.ort_version` drift loops**: if repeatedly staging fails partway
  and the stamp file is stale, delete it and re-run.

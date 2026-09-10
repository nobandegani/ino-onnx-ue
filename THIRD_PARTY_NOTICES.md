# Third-Party Notices

InoOnnx is distributed under the [Apache License 2.0](LICENSE), Copyright 2026 Inoland.

That grant covers Inoland's own code — `Source/InoOnnx/` and `OnnxRuntime/scripts/`. This
repository also redistributes prebuilt Microsoft binaries under `Source/ThirdParty/`, one of which
is **proprietary**, and another of which has been **modified**. The table below is authoritative
where it disagrees with any other document here.

---

## Summary

| Path | Component | License | Modified? |
|---|---|---|---|
| `Source/InoOnnx/`, `OnnxRuntime/scripts/` | InoOnnx | Apache-2.0 | — |
| `Source/ThirdParty/*/Headers/`, `Public/` | ONNX Runtime C/C++ API headers | MIT | No |
| `Source/ThirdParty/Win64/InoOnnxRuntime.dll` | ONNX Runtime 1.24.3 | MIT | **Yes — import table** |
| `Source/ThirdParty/Win64/onnxruntime_providers_shared.dll` | ONNX Runtime 1.24.3 | MIT | No |
| `Source/ThirdParty/Win64/InoDml.dll` | **DirectML 1.15.4** | **Microsoft proprietary** | No — renamed only |
| `Source/ThirdParty/Android/arm64-v8a/libInoOnnxRuntime.so` | ONNX Runtime 1.24.3 | MIT | **Yes — DT_SONAME** |
| `Source/ThirdParty/Mac/libInoOnnxRuntime.dylib` | ONNX Runtime 1.24.3 (arm64) | MIT | **Yes — LC_ID_DYLIB** |
| `Source/ThirdParty/IOS/InoOnnxRuntime.framework` (device + Simulator) | ONNX Runtime 1.24.3 | MIT | **Yes — renamed + Info.plist** |

---

## 1. ONNX Runtime — Microsoft Corporation

**License:** MIT. **Upstream:** https://github.com/microsoft/onnxruntime
**Version:** 1.24.3 (`OnnxRuntime/ONNXRUNTIME_VERSION`)
**Source package:** `Microsoft.ML.OnnxRuntime.DirectML` NuGet,
`https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.3`

MIT requires that the copyright notice and permission notice accompany copies of the software.
This file serves that purpose for the binaries under `Source/ThirdParty/`.

### Modifications made by Inoland

MIT permits modification. These changes are disclosed here so that anyone shipping these binaries
knows they are not byte-identical to Microsoft's published artifacts, and can reproduce them.

| Platform | Original | Shipped as | Change |
|---|---|---|---|
| Win64 | `onnxruntime.dll` | `InoOnnxRuntime.dll` | PE **delay-import** table string `DirectML.dll` → `InoDml.dll`, in-place byte edit |
| Android | `libonnxruntime.so` | `libInoOnnxRuntime.so` | ELF `DT_SONAME` → `libInoOnnxRuntime.so` |
| macOS (arm64) | `libonnxruntime.dylib` | `libInoOnnxRuntime.dylib` | Mach-O `LC_ID_DYLIB` install name → `@rpath/libInoOnnxRuntime.dylib` |
| iOS | `onnxruntime.framework` | `InoOnnxRuntime.framework` | Framework dir + binary renamed; `Info.plist` `CFBundleExecutable`/`CFBundleName`/`CFBundleIdentifier` rewritten to match. **No `LC_ID_DYLIB` edit** — see below |

> The iOS framework binary is **not** a Mach-O dylib. Microsoft ships ORT for iOS as a Unix `ar`
> static archive wrapped in a fat header (Apple's "static framework" convention). The patcher
> detects the `!<arch>` magic and skips the install-name rewrite, because a static archive has no
> install name to patch — only the directory, binary filename and `Info.plist` identity change,
> which is what Apple's codesign requires to accept the renamed bundle. At link time the `Ort*`
> symbols are pulled directly into the app executable, so there is no iOS dylib at runtime at
> all.

No ONNX Runtime source code was altered and no functional behaviour was changed. Every edit is a
**library-identity** change, applied to avoid name collisions with other ONNX Runtime copies in
the same process — see [README → Why everything is renamed](README.md#why-everything-is-renamed)
for the failure modes this prevents, including silent fp16 numerical corruption on Windows.

The scripts that perform these edits are `OnnxRuntime/scripts/patch-ort-dml-import.py`,
`patch-ort-android-soname.py` and `patch-ort-apple.py`. They are the complete and reproducible
record of the modifications; re-running `setup-onnxruntime.ps1` regenerates the staged binaries
from the pinned NuGet/AAR packages.

> **⚠️ Code signature.** The Win64 import-table edit changes bytes inside a Microsoft-signed DLL,
> so **Microsoft's Authenticode signature on `InoOnnxRuntime.dll` is no longer valid.** Anti-virus
> and SmartScreen may flag it, and environments that enforce signed-binary policies may reject it.
> Sign it yourself if your distribution requires a valid signature. The Apple and Android edits
> likewise invalidate any upstream signature on those artifacts; for iOS, UE re-signs embedded
> frameworks with your distribution identity at packaging time.

---

## 2. DirectML — Microsoft Corporation

**Version:** 1.15.4 (`OnnxRuntime/DIRECTML_VERSION`)
**Source package:** `Microsoft.AI.DirectML` NuGet,
`https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4`
**Shipped as:** `Source/ThirdParty/Win64/InoDml.dll`

> **DirectML is proprietary Microsoft software, not open source.** It is redistributed under the
> license attached to the `Microsoft.AI.DirectML` NuGet package, not under MIT and not under this
> repository's Apache-2.0 grant.

`InoDml.dll` is `DirectML.dll` **renamed on disk only** — the binary is byte-identical to
Microsoft's, and its signature is intact. The rename is necessary because Windows' loader caches
DLLs by base name, and UE ships its own `DirectML.dll` under
`Engine/Binaries/Win64/DML/x64/`; without a unique name, ORT 1.24.3 ends up bound to UE's
different DirectML build. The corresponding import-table edit is applied to **ONNX Runtime**, not
to DirectML.

> **⚠️ Before shipping**, confirm the redistribution terms on the `Microsoft.AI.DirectML` package
> version you are using — including whether renaming the file is permitted by them. If those
> terms are not acceptable for your distribution, DirectML is needed **only** for the DirectML
> execution provider: run on the CPU provider instead and `InoDml.dll` can be dropped from
> staging entirely. Every other platform (CoreML on Apple, NNAPI/XNNPACK on Android) is
> unaffected.

DirectML is Windows-only and D3D12-based. CUDA, TensorRT and ROCm execution providers are
deliberately not shipped.

---

## 3. Build-time Python dependencies

The staging scripts require `pefile` (PE import-table editing) and `lief` (ELF/Mach-O editing).
These are **developer tooling only** — not redistributed, not linked, and not present in any
shipped build. `pefile` is MIT; `lief` is Apache-2.0.

---

## 4. Model weights

**No ONNX models are in this repository.** Models are supplied by the consuming application and
remain subject to their own licenses, which for many published models include use restrictions
that MIT and Apache-2.0 do not. Check the license of any model you ship.

---

## Reporting a problem with these notices

If a component is misattributed or a notice is missing, please open an issue at
https://github.com/nobandegani/ino-onnx-ue/issues and it will be corrected.

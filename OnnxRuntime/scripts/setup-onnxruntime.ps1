# setup-onnxruntime.ps1
#
# One-time setup (idempotent) for the InoOnnx plugin.
#
# Downloads Microsoft's prebuilt ONNX Runtime binaries for Win64, Android
# arm64-v8a, macOS (Apple Silicon), and iOS (device + simulator), stages
# everything (headers + DLLs/.so/.dylib/.framework) under a flat tree:
#   Plugins/InoOnnx/Source/ThirdParty/
#     Public/                          C / C++ API headers
#     Win64/                           InoOnnxRuntime.dll, InoDml.dll, providers_shared
#     Android/arm64-v8a/               libInoOnnxRuntime.so
#     Mac/                             libInoOnnxRuntime.dylib (Apple Silicon arm64)
#     IOS/InoOnnxRuntime.framework/    iOS device (arm64)
#     IOS/Simulator/InoOnnxRuntime.framework/ iOS simulator (arm64 + x86_64 fat)
#
# Pinned versions live in two files:
#   Plugins/InoOnnx/OnnxRuntime/ONNXRUNTIME_VERSION   (e.g. "1.24.3")
#   Plugins/InoOnnx/OnnxRuntime/DIRECTML_VERSION      (e.g. "1.15.4")
# Bump either + re-run this script to update the corresponding binaries.
#
# Windows sources:
#   Microsoft.ML.OnnxRuntime.DirectML NuGet (DML-flavored ORT build)
#     -> onnxruntime.dll                    (RENAMED to InoOnnxRuntime.dll)
#     -> onnxruntime_providers_shared.dll   (original name — not statically
#        imported by our ORT DLL, some shared-EP paths LoadLibrary it by
#        basename; safer to ship than regret)
#     -> headers under build/native/include/
#   Microsoft.AI.DirectML NuGet (DirectML runtime)
#     -> DirectML.dll                       (ORIGINAL NAME — see below)
#
# Why the DirectML NuGet pair and not the plain GitHub Releases ZIP:
#   The GitHub-published `onnxruntime-win-x64-<ver>.zip` is CPU-only — the
#   DirectML Execution Provider is NOT compiled in. To target D3D12 GPUs
#   (any modern Windows GPU including NPUs on Win11 24H2+) we need the
#   DML-flavored build, which Microsoft ships only via NuGet. DirectML.dll
#   itself comes from a separate NuGet because the ORT NuGet doesn't
#   bundle it — dependency relationship, not a bundling one.
#
# Why we also rename DirectML.dll -> InoDml.dll:
#   Empirically confirmed via our own diagnostic logging that UE 5.7's
#   bundled plugins (RuntimeMetaHumanLipSync, NNE plugin family) LoadLibrary
#   their own DirectML.dll early in editor startup — BEFORE our module's
#   StartupModule runs. Windows' base-name cache serves THAT copy when our
#   InoOnnxRuntime.dll later resolves its static DirectML.dll import. The
#   UE-bundled DirectML is a different build than what our ORT 1.24.3 was
#   compiled against (confirmed by a 10KB file-size difference), and the
#   version skew causes kernel-validation failures (MultiHeadAttention /
#   Slice E_INVALIDARG) and silent numerical corruption on the fp16 LM.
#
#   A full-path preload doesn't fix it — Windows' cache is keyed by base
#   name, so once UE's DirectML.dll is loaded, every subsequent resolution
#   of "DirectML.dll" returns that handle regardless of the path we pass.
#
#   The fix: rename the base name. DirectML.dll is a STATIC import in our
#   onnxruntime.dll's PE import table, so we can't just rename the file on
#   disk — the import entry still says "DirectML.dll". We patch the import
#   table with patch-ort-dml-import.py (pefile-based in-place byte edit)
#   so our ORT's import says "InoDml.dll", then rename the DLL file to
#   match. No other plugin looks for "InoDml.dll" → no cache collision,
#   full version isolation.
#
#   Side-effect: Microsoft's Authenticode signature on InoOnnxRuntime.dll
#   is invalidated by the byte edit. For a shipped game this is a
#   non-issue (UE game DLLs aren't expected to carry MS signatures).
#   Enterprise AV on a dev machine may occasionally flag "MS-signed DLL
#   with broken signature" — documented as a known caveat.
#
# Python dependencies (one-time, all pure-Python):
#   pefile  — Windows DML import-table patch (patch-ort-dml-import.py)
#   lief    — Android SONAME + Apple Mach-O LC_ID_DYLIB patches
#             (patch-ort-android-soname.py, patch-ort-apple.py)
#   plistlib — iOS framework Info.plist rewrite (Python stdlib; no install)
#
#   pip install pefile lief
#
# Required once before first run; stays installed for subsequent runs.
#   Run once before first setup; stays installed for subsequent runs.
#
# Why no GPU mega-bundle / CUDA:
#   Microsoft's GPU ORT build includes CUDA + TensorRT (~300 MB of NVIDIA
#   runtime libs we don't want to ship with a UE game). DirectML covers
#   the same ground for NVIDIA, AMD, Intel, and (on 24H2+) NPUs via one
#   API — 20 MB of DLLs total. Clear winner for general-purpose Windows
#   deployment.
#
# Android:
#   Unchanged. The Android AAR ships one unified libonnxruntime.so per ABI
#   with CPU + XNNPACK + NNAPI + WebGPU baked in. DirectML is Windows-only
#   (D3D12-based) so nothing to do here. The .so keeps the libInoOnnxRuntime.so
#   rename for the same link-time symbol-version-collision reason the old
#   Windows rename dodged; see the original-file comments below preserved
#   from Phase 4.
#
# Mac + iOS (sourced from the regular Microsoft.ML.OnnxRuntime CPU NuGet —
# NOT the DirectML variant; DML is Windows-only):
#   The CPU NuGet ships:
#     runtimes/osx-arm64/native/libonnxruntime.dylib    (Apple Silicon Mac)
#     runtimes/ios/native/onnxruntime.xcframework.zip   (nested zip)
#   The inner xcframework contains three slices we care about:
#     ios-arm64/onnxruntime.framework/                  (iOS device — arm64)
#     ios-arm64_x86_64-simulator/onnxruntime.framework/ (iOS simulator — fat)
#   (A maccatalyst slice exists too; we ignore it — Mac Catalyst isn't a
#    UE 5.7 target.)
#
#   Microsoft does not ship Intel-Mac (osx-x64) or universal2 binaries
#   in modern ORT NuGets — only Apple Silicon. Intel Macs would need a
#   GitHub-Releases osx-x86_64 tarball; not staged today.
#
#   Same defensive-isolation rename pattern as Win64 / Android, applied
#   via patch-ort-apple.py (lief-based Mach-O LC_ID_DYLIB rewrite +
#   plistlib Info.plist rewrite for the iOS frameworks):
#     libonnxruntime.dylib   -> libInoOnnxRuntime.dylib
#                               (LC_ID_DYLIB -> @rpath/libInoOnnxRuntime.dylib)
#     onnxruntime.framework  -> InoOnnxRuntime.framework
#                               (binary renamed onnxruntime -> InoOnnxRuntime,
#                                LC_ID_DYLIB -> @rpath/InoOnnxRuntime.framework/InoOnnxRuntime,
#                                CFBundleExecutable / CFBundleName / CFBundleIdentifier patched)
#
#   Apple platforms don't have Windows' base-name DLL cache or Android's
#   SONAME aliasing — dyld is stricter and looks up by full @rpath +
#   LC_ID_DYLIB install_name — so collision risk is much lower here than
#   on those two platforms. We rename anyway for defence-in-depth + naming
#   consistency with the Win64 / Android rename pattern.
#
# Artifacts on disk after this runs (assuming ORT 1.24.3 + DML 1.15.4):
#
#   Source/ThirdParty/
#     Public/                              (C / C++ API headers + dml_provider_factory.h)
#     Win64/
#       InoOnnxRuntime.dll                 (~13 MB — RENAMED from onnxruntime.dll)
#       InoDml.dll                         (~18 MB — RENAMED from DirectML.dll)
#       onnxruntime_providers_shared.dll   (~200 KB, original name)
#     Android/arm64-v8a/
#       libInoOnnxRuntime.so               (~25 MB, CPU + XNNPACK)
#     Mac/
#       libInoOnnxRuntime.dylib            (~12 MB, Apple Silicon CPU + CoreML)
#     IOS/InoOnnxRuntime.framework/
#       InoOnnxRuntime                     (~14 MB, iOS device CPU + CoreML)
#       Info.plist + Headers/
#     IOS/Simulator/InoOnnxRuntime.framework/
#       InoOnnxRuntime                     (~28 MB, fat arm64+x86_64 simulator)
#       Info.plist + Headers/

$ErrorActionPreference = "Stop"

$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$OnnxRtDir    = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$PluginDir    = (Resolve-Path (Join-Path $OnnxRtDir "..")).Path
$VersionFile    = Join-Path $OnnxRtDir "ONNXRUNTIME_VERSION"
$DmlVersionFile = Join-Path $OnnxRtDir "DIRECTML_VERSION"
$CacheDir     = Join-Path $OnnxRtDir ".cache"

# Staging destinations. Everything (headers + Win64 DLLs + Android .so)
# lives under Source/ThirdParty/ in a flat layout — Public/, Win64/,
# Android/<arch>/ — matching the InoLiteRT plugin's pattern. No
# Binaries/ThirdParty/ tree.
#
# Note there is no Win64 "lib" directory — dynamic loading (GetProcAddress
# on the renamed DLL) means we never link against the ORT import library
# at UE build time.
$ThirdPartyDir       = Join-Path $PluginDir "Source\ThirdParty"
$PublicIncDir        = Join-Path $ThirdPartyDir "Public"
$Win64BinStageDir    = Join-Path $ThirdPartyDir "Win64"
$Arm64BinStageDir    = Join-Path $ThirdPartyDir "Android\arm64-v8a"
# macOS — Apple Silicon only (Microsoft drops Intel Mac in modern ORT NuGets).
# Flat dylib at the Mac/ root; no .framework wrapper needed for executable
# bundles. UE's Mac packaging copies the dylib next to the binary at cook time.
$MacStageDir         = Join-Path $ThirdPartyDir "Mac"
# iOS — split into device + simulator slices, each in its own renamed
# .framework dir (PublicAdditionalFrameworks expects framework dirs, not
# loose dylibs). Device slice is what ships in App Store builds; simulator
# slice exists for dev convenience and is currently NOT wired into Build.cs.
$IosStageRoot        = Join-Path $ThirdPartyDir "IOS"
$IosFrameworkDir     = Join-Path $IosStageRoot "InoOnnxRuntime.framework"
$IosSimStageRoot     = Join-Path $IosStageRoot "Simulator"
$IosSimFrameworkDir  = Join-Path $IosSimStageRoot "InoOnnxRuntime.framework"

#---------------------------------------------------------------------
# 1. Load pinned versions
#---------------------------------------------------------------------
if (-not (Test-Path $VersionFile)) {
    Write-Error "ONNXRUNTIME_VERSION file not found at $VersionFile"
}
$Version = (Get-Content $VersionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "ONNXRUNTIME_VERSION must be a plain semver triple like '1.24.3'. Got: '$Version'"
}

if (-not (Test-Path $DmlVersionFile)) {
    Write-Error "DIRECTML_VERSION file not found at $DmlVersionFile"
}
$DmlVersion = (Get-Content $DmlVersionFile -Raw).Trim()
if ($DmlVersion -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "DIRECTML_VERSION must be a plain semver triple like '1.15.4'. Got: '$DmlVersion'"
}

Write-Host ""
Write-Host "=== ONNX Runtime + DirectML setup ===" -ForegroundColor Cyan
Write-Host "ORT version:       $Version (from ONNXRUNTIME_VERSION)"
Write-Host "DirectML version:  $DmlVersion (from DIRECTML_VERSION)"
Write-Host "Plugin dir:    $PluginDir"
Write-Host "OnnxRuntime:   $OnnxRtDir"
Write-Host "Cache dir:     $CacheDir"
Write-Host ""

#---------------------------------------------------------------------
# 2. Resolve download URLs + target cache paths
#---------------------------------------------------------------------
# Windows: DML-flavored ORT NuGet. NuGet's flat HTTP API serves raw .nupkg
# (which is just a ZIP) from v2/package/<id>/<version>. Saving with .nupkg
# extension; Expand-Archive requires .zip — we copy-then-rename at extract
# time.
$OrtNupkgName = "Microsoft.ML.OnnxRuntime.DirectML.$Version.nupkg"
$OrtNupkgUrl  = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$Version"
$OrtNupkgPath = Join-Path $CacheDir $OrtNupkgName

# Windows: DirectML.dll comes from a separate NuGet (Microsoft.AI.DirectML).
# Version pinned independently because ORT NuGet only declares a minimum
# compatible DirectML version — we want an explicit pin so a transitive
# bump doesn't surprise us.
$DmlNupkgName = "Microsoft.AI.DirectML.$DmlVersion.nupkg"
$DmlNupkgUrl  = "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$DmlVersion"
$DmlNupkgPath = Join-Path $CacheDir $DmlNupkgName

# Android AAR is published to Maven Central (not GitHub Releases / NuGet).
$AndroidAarName = "onnxruntime-android-$Version.aar"
$AndroidAarUrl  = "https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/$Version/$AndroidAarName"
$AndroidAarPath = Join-Path $CacheDir $AndroidAarName

# Mac + iOS: the regular Microsoft.ML.OnnxRuntime CPU NuGet ships
# osx-arm64 + ios xcframework. (The DML NuGet we use for Win64 has only
# Windows artifacts; the CPU NuGet has every other platform.)
$OrtCpuNupkgName = "Microsoft.ML.OnnxRuntime.$Version.nupkg"
$OrtCpuNupkgUrl  = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/$Version"
$OrtCpuNupkgPath = Join-Path $CacheDir $OrtCpuNupkgName

#---------------------------------------------------------------------
# 3. Idempotency: if staged binaries already match both versions, skip
#---------------------------------------------------------------------
# Stamp format: "<ort_version>+<dml_version>". Single-line + marker so
# either version bump triggers re-stage.
$StampFile    = Join-Path $ThirdPartyDir ".ort_version"
$ExpectedStamp = "$Version+$DmlVersion"

if ((Test-Path $StampFile) -and `
    (Test-Path (Join-Path $Win64BinStageDir "InoOnnxRuntime.dll")) -and `
    (Test-Path (Join-Path $Win64BinStageDir "InoDml.dll")) -and `
    (Test-Path (Join-Path $Arm64BinStageDir "libInoOnnxRuntime.so")) -and `
    (Test-Path (Join-Path $MacStageDir "libInoOnnxRuntime.dylib")) -and `
    (Test-Path (Join-Path $IosFrameworkDir "InoOnnxRuntime")) -and `
    (Test-Path (Join-Path $IosSimFrameworkDir "InoOnnxRuntime"))) {
    $StampValue = (Get-Content $StampFile -Raw).Trim()
    if ($StampValue -eq $ExpectedStamp) {
        Write-Host "--- Already up to date ---" -ForegroundColor Green
        Write-Host "  ORT $Version + DirectML $DmlVersion staged."
        Write-Host "  Delete '$StampFile' or bump ONNXRUNTIME_VERSION / DIRECTML_VERSION to force re-stage."
        exit 0
    } else {
        Write-Host "--- Version drift detected: staged=$StampValue, pinned=$ExpectedStamp. Re-staging. ---" -ForegroundColor Yellow
    }
}

#---------------------------------------------------------------------
# 4. Preflight: create directories + helper function
#---------------------------------------------------------------------
# Invoke-WebRequest + Expand-Archive are built into PowerShell 5.1+, so no
# external dependencies required. The NuGet packages and Android AAR are
# all zips in disguise; Expand-Archive handles them once we have a .zip
# extension on disk.

foreach ($d in @($CacheDir, $PublicIncDir, $Win64BinStageDir, $Arm64BinStageDir,
                 $MacStageDir, $IosStageRoot, $IosSimStageRoot)) {
    if (-not (Test-Path $d)) {
        New-Item -ItemType Directory -Path $d -Force | Out-Null
    }
}

function Download-IfMissing {
    param([string]$Url, [string]$Dest, [string]$Label, [int]$MinSizeMb = 1)
    if ((Test-Path $Dest) -and ((Get-Item $Dest).Length -gt ($MinSizeMb * 1MB))) {
        Write-Host "  [CACHED] $Label ($(Split-Path $Dest -Leaf))"
        return
    }
    Write-Host "  [DOWNLOAD] $Label"
    Write-Host "             $Url"
    # UseBasicParsing avoids depending on IE rendering, which is absent on
    # Server Core / some CI runners.
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    $sizeMb = [math]::Round((Get-Item $Dest).Length / 1MB, 1)
    Write-Host "             -> $Dest ($sizeMb MB)"
}

# Expand-Archive insists on a .zip extension. NuGet packages are ZIPs with
# .nupkg — copy to .zip first, then extract.
function Expand-Nupkg {
    param([string]$NupkgPath, [string]$DestDir, [string]$Label)

    if (Test-Path $DestDir) {
        Remove-Item -Recurse -Force $DestDir
    }
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null

    $AsZip = [System.IO.Path]::ChangeExtension($NupkgPath, ".zip")
    Copy-Item -Path $NupkgPath -Destination $AsZip -Force
    Expand-Archive -Path $AsZip -DestinationPath $DestDir -Force
    Remove-Item -Path $AsZip -Force

    Write-Host "  [EXTRACT] $Label -> $DestDir"
}

#---------------------------------------------------------------------
# 5. Download
#---------------------------------------------------------------------
Write-Host "--- Downloading artifacts ---" -ForegroundColor Yellow
Download-IfMissing -Url $OrtNupkgUrl     -Dest $OrtNupkgPath     -Label "ORT DirectML NuGet"     -MinSizeMb 10
Download-IfMissing -Url $DmlNupkgUrl     -Dest $DmlNupkgPath     -Label "DirectML NuGet"         -MinSizeMb 100
Download-IfMissing -Url $AndroidAarUrl   -Dest $AndroidAarPath   -Label "Android AAR"            -MinSizeMb 5
Download-IfMissing -Url $OrtCpuNupkgUrl  -Dest $OrtCpuNupkgPath  -Label "ORT CPU NuGet (Mac+iOS)" -MinSizeMb 5
Write-Host ""

#---------------------------------------------------------------------
# 6. Extract + stage Windows ORT (DML-flavored NuGet)
#---------------------------------------------------------------------
Write-Host "--- Extracting + staging Windows ORT ---" -ForegroundColor Yellow

$OrtExtractDir = Join-Path $CacheDir "ort-dml-extract-$Version"
Expand-Nupkg -NupkgPath $OrtNupkgPath -DestDir $OrtExtractDir -Label "ORT DML NuGet"

# NuGet layout:
#   runtimes/win-x64/native/onnxruntime.dll
#   runtimes/win-x64/native/onnxruntime_providers_shared.dll
#   build/native/include/*.h   (onnxruntime_c_api.h + dml_provider_factory.h + ...)
$OrtNativeDir = Join-Path $OrtExtractDir "runtimes\win-x64\native"
$OrtIncludeDir = Join-Path $OrtExtractDir "build\native\include"

if (-not (Test-Path $OrtNativeDir))  { Write-Error "Missing runtimes/win-x64/native/ in the ORT NuGet at $OrtExtractDir" }
if (-not (Test-Path $OrtIncludeDir)) { Write-Error "Missing build/native/include/ in the ORT NuGet at $OrtExtractDir" }

# Stage headers. Copy every .h from the NuGet's include/ — they're the
# public API surface (onnxruntime_c_api.h, dml_provider_factory.h, plus
# several smaller helper headers we may never reference but that the
# main ones include).
Write-Host "  [STAGE] Headers -> $PublicIncDir"
Get-ChildItem -Path $OrtIncludeDir -File | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination (Join-Path $PublicIncDir $_.Name) -Force
}

# Stage core ORT DLL — RENAMED from onnxruntime.dll to InoOnnxRuntime.dll,
# AND patched in the PE import table to reference "InoDml.dll" instead
# of "DirectML.dll".
#
# The rename alone fixes the cache collision between our ORT and UE's
# bundled ORT (different versions of the same onnxruntime.dll base name).
# The import-table patch fixes the collision with UE's bundled DirectML.dll
# at Engine/Binaries/Win64/DML/x64/ — UE's NNE plugin / RuntimeMetaHumanLipSync
# / etc. LoadLibrary DirectML.dll early in editor startup, so when our
# ORT DLL later tries to resolve its static DirectML.dll import, Windows'
# base-name cache serves UE's copy (different version than our ORT was
# built against). Symptoms of that version skew: MultiHeadAttention /
# Slice kernel validation failures (E_INVALIDARG) and fp16 silent
# numerical corruption in the Chatterbox LM.
#
# After patching, our ORT DLL imports "InoDml.dll" — a base name no other
# plugin uses, so base-name cache collisions become structurally impossible.
$OrtDllSrc = Join-Path $OrtNativeDir "onnxruntime.dll"
if (-not (Test-Path $OrtDllSrc)) { Write-Error "onnxruntime.dll not found at $OrtDllSrc" }

$PatchScript = Join-Path $ScriptDir "patch-ort-dml-import.py"
if (-not (Test-Path $PatchScript)) { Write-Error "patch-ort-dml-import.py not found at $PatchScript" }

$PatchedOrtPath = Join-Path $Win64BinStageDir "InoOnnxRuntime.dll"

Write-Host "  [PATCH] InoOnnxRuntime.dll import table: DirectML.dll -> InoDml.dll"
& python $PatchScript $OrtDllSrc $PatchedOrtPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "patch-ort-dml-import.py failed (exit code $LASTEXITCODE). Ensure Python 3 is on PATH and 'pip install pefile' was run."
}
Write-Host "  [STAGE] onnxruntime.dll -> InoOnnxRuntime.dll (renamed + import-table patched)"

# Stage shared providers DLL — ORIGINAL NAME. Not a static import of the
# core ORT DLL (confirmed via dumpbin), so the base-name collision concern
# doesn't apply here — any other plugin with the same-named DLL would be
# loading its own copy into its own address space. Our ORT may LoadLibrary
# it at session-create time for certain shared execution providers (the
# "shared" half is what lets multiple sessions sharing one provider work).
# Ship it under the original name so ORT's internal discovery finds it.
$OrtSharedSrc = Join-Path $OrtNativeDir "onnxruntime_providers_shared.dll"
if (-not (Test-Path $OrtSharedSrc)) { Write-Error "onnxruntime_providers_shared.dll not found at $OrtSharedSrc" }
Copy-Item -Path $OrtSharedSrc -Destination (Join-Path $Win64BinStageDir "onnxruntime_providers_shared.dll") -Force
Write-Host "  [STAGE] onnxruntime_providers_shared.dll -> $Win64BinStageDir (original name, non-statically-imported)"

Write-Host ""

#---------------------------------------------------------------------
# 7. Extract + stage DirectML.dll (from Microsoft.AI.DirectML NuGet)
#---------------------------------------------------------------------
Write-Host "--- Extracting + staging DirectML runtime ---" -ForegroundColor Yellow

$DmlExtractDir = Join-Path $CacheDir "directml-extract-$DmlVersion"
Expand-Nupkg -NupkgPath $DmlNupkgPath -DestDir $DmlExtractDir -Label "DirectML NuGet"

# Microsoft.AI.DirectML layout:
#   bin/x64-win/DirectML.dll      (~18 MB — this is what we want)
#   bin/x64-win/DirectML.Debug.dll (debug build, skip)
#   bin/x64-win/DirectML.lib       (import library, skip — dynamic load only)
#   bin/arm64-win/DirectML.dll     (skip — Windows on ARM64, not our target yet)
#   bin/arm64ec-win/DirectML.dll   (skip — ARM64EC, not our target)
#   bin/x86-win/DirectML.dll       (skip — 32-bit Windows, not supported by UE)
#   bin/x64-xbox-scarlett-*/DirectML.dll (skip — Xbox GDK builds)
#   include/DirectML.h + DirectMLConfig.h (optional; Windows SDK already has DirectML.h)
$DmlDllSrc = Join-Path $DmlExtractDir "bin\x64-win\DirectML.dll"
if (-not (Test-Path $DmlDllSrc)) { Write-Error "DirectML.dll not found at $DmlDllSrc" }

# Stage DirectML.dll as "InoDml.dll" — renamed to match the import-table
# patch we apply to InoOnnxRuntime.dll above. UE 5.7's NNE plugin and
# marketplace plugins (RuntimeMetaHumanLipSync, etc.) LoadLibrary their
# own DirectML.dll early during editor startup — our copy would lose
# the base-name cache race and our ORT would end up statically bound to
# their DirectML version (often a different 1.15.x build than what our
# ORT 1.24.3 was compiled against). The rename makes the whole thing
# structurally impossible: no plugin looks for "InoDml.dll", so no
# collision can happen.
#
# Also delete any stale "DirectML.dll" in the staging dir from previous
# setup runs — this is a one-way migration, we never use the original
# name again.
Copy-Item -Path $DmlDllSrc -Destination (Join-Path $Win64BinStageDir "InoDml.dll") -Force
$DmlDllSize = [math]::Round((Get-Item $DmlDllSrc).Length / 1MB, 1)
Write-Host "  [STAGE] DirectML.dll ($DmlDllSize MB) -> InoDml.dll (renamed for base-name isolation)"

$StaleDml = Join-Path $Win64BinStageDir "DirectML.dll"
if (Test-Path $StaleDml) {
    Remove-Item -Path $StaleDml -Force
    Write-Host "  [CLEANUP] removed stale $StaleDml from previous pre-rename setup run"
}

Write-Host ""

#---------------------------------------------------------------------
# 8. Extract Android AAR (unchanged from Phase 4)
#---------------------------------------------------------------------
# An .aar is a zip. PowerShell's Expand-Archive is strict about the
# extension, so we copy to a .zip first, then extract.
Write-Host "--- Extracting + staging Android artifacts ---" -ForegroundColor Yellow

$AarAsZip = Join-Path $CacheDir "onnxruntime-android-$Version.zip"
Copy-Item -Path $AndroidAarPath -Destination $AarAsZip -Force

$AndroidExtractDir = Join-Path $CacheDir "android-extract-$Version"
if (Test-Path $AndroidExtractDir) {
    Remove-Item -Recurse -Force $AndroidExtractDir
}
New-Item -ItemType Directory -Path $AndroidExtractDir -Force | Out-Null

Expand-Archive -Path $AarAsZip -DestinationPath $AndroidExtractDir -Force
Remove-Item -Path $AarAsZip -Force

# AAR layout: jni/<abi>/libonnxruntime.so. We RENAME to libInoOnnxRuntime.so
# when staging — same rationale as the Windows onnxruntime.dll rename.
#
# IMPORTANT: a file rename alone is NOT sufficient on Android. Microsoft's
# AAR ships the .so with DT_SONAME = "libonnxruntime.so" embedded inside
# the ELF, and Android's dynamic linker dedupes loaded libraries by
# SONAME — not by filename. If any other plugin in the same APK also
# ships a libonnxruntime.so (e.g. RuntimeMetaHumanLipSync at ORT 1.19.2),
# the linker's loaded-libraries cache aliases both files under the same
# SONAME, and dlopen("libInoOnnxRuntime.so") returns whichever .so
# loaded first. So we ALSO patch the SONAME inside the ELF via
# patch-ort-android-soname.py (uses lief; pip install lief).
$ArmSoSrc      = Join-Path $AndroidExtractDir "jni\arm64-v8a\libonnxruntime.so"
$ArmSoStaged   = Join-Path $Arm64BinStageDir   "libInoOnnxRuntime.so"
$ArmSoTargetSoname = "libInoOnnxRuntime.so"
if (-not (Test-Path $ArmSoSrc)) {
    Write-Error "libonnxruntime.so not found at expected path inside AAR: $ArmSoSrc"
}

Copy-Item -Path $ArmSoSrc -Destination $ArmSoStaged -Force
Write-Host "  [STAGE] jni/arm64-v8a/libonnxruntime.so -> $ArmSoStaged (renamed for link-time version isolation)"

# Patch DT_SONAME in-place so SONAME matches the new filename. Idempotent —
# running the script after a successful patch is a no-op.
$AndroidPatchScript = Join-Path $ScriptDir "patch-ort-android-soname.py"
if (-not (Test-Path $AndroidPatchScript)) {
    Write-Error "patch-ort-android-soname.py not found at $AndroidPatchScript"
}
Write-Host "  [PATCH] DT_SONAME -> $ArmSoTargetSoname"
& python $AndroidPatchScript $ArmSoStaged $ArmSoStaged $ArmSoTargetSoname
if ($LASTEXITCODE -ne 0) {
    Write-Error "patch-ort-android-soname.py failed (exit $LASTEXITCODE). Did you run 'pip install lief'?"
}

Write-Host ""

#---------------------------------------------------------------------
# 9. Extract + stage macOS + iOS (regular CPU NuGet)
#---------------------------------------------------------------------
# The DirectML NuGet we used above ships only Windows artifacts. The
# regular Microsoft.ML.OnnxRuntime CPU NuGet ships every other platform
# Microsoft supports — we extract two from it:
#   runtimes/osx-arm64/native/libonnxruntime.dylib    (Apple Silicon Mac)
#   runtimes/ios/native/onnxruntime.xcframework.zip   (iOS, nested zip)
#
# Each goes through patch-ort-apple.py (lief-based) for the rename.
Write-Host "--- Extracting + staging macOS + iOS ORT ---" -ForegroundColor Yellow

$ApplePatchScript = Join-Path $ScriptDir "patch-ort-apple.py"
if (-not (Test-Path $ApplePatchScript)) {
    Write-Error "patch-ort-apple.py not found at $ApplePatchScript"
}

$OrtCpuExtractDir = Join-Path $CacheDir "ort-cpu-extract-$Version"
Expand-Nupkg -NupkgPath $OrtCpuNupkgPath -DestDir $OrtCpuExtractDir -Label "ORT CPU NuGet"

# ---- Mac (Apple Silicon dylib) ----
# Microsoft does not ship Intel-Mac (osx-x64) or universal2 in modern ORT
# NuGets — only osx-arm64. If a future need for Intel Mac arises, point
# this at the GitHub-Releases osx-x86_64 tarball and lipo/-merge with the
# arm64 dylib into a universal2 binary.
$MacDylibSrc = Join-Path $OrtCpuExtractDir "runtimes\osx-arm64\native\libonnxruntime.dylib"
if (-not (Test-Path $MacDylibSrc)) {
    Write-Error "Expected Mac dylib at $MacDylibSrc inside the CPU NuGet (Microsoft may have changed the runtimes/ layout)."
}

$MacDylibStaged = Join-Path $MacStageDir "libInoOnnxRuntime.dylib"
$MacInstallName = "@rpath/libInoOnnxRuntime.dylib"
Write-Host "  [PATCH] Mac dylib: install_name -> $MacInstallName"
& python $ApplePatchScript --mode dylib `
    --input $MacDylibSrc `
    --output $MacDylibStaged `
    --install-name $MacInstallName
if ($LASTEXITCODE -ne 0) {
    Write-Error "patch-ort-apple.py (dylib mode) failed (exit $LASTEXITCODE). Did you 'pip install lief'?"
}
$MacSizeMb = [math]::Round((Get-Item $MacDylibStaged).Length / 1MB, 1)
Write-Host "  [STAGE] libInoOnnxRuntime.dylib ($MacSizeMb MB) -> $MacDylibStaged"

# ---- iOS (xcframework, nested zip inside the NuGet) ----
# Extract the inner zip into a scratch dir; then run patch-ort-apple.py
# in framework mode against the device + simulator slices.
$IosXcfZipSrc = Join-Path $OrtCpuExtractDir "runtimes\ios\native\onnxruntime.xcframework.zip"
if (-not (Test-Path $IosXcfZipSrc)) {
    Write-Error "Expected iOS xcframework zip at $IosXcfZipSrc inside the CPU NuGet."
}

$IosXcfExtractDir = Join-Path $CacheDir "ort-ios-xcframework-$Version"
if (Test-Path $IosXcfExtractDir) {
    Remove-Item -Recurse -Force $IosXcfExtractDir
}
New-Item -ItemType Directory -Path $IosXcfExtractDir -Force | Out-Null
Expand-Archive -Path $IosXcfZipSrc -DestinationPath $IosXcfExtractDir -Force

# The inner zip extracts to onnxruntime.xcframework/ at the root of the
# scratch dir. Locate it defensively (Microsoft's nesting could shift).
$XcfRootCandidates = @(Get-ChildItem -Path $IosXcfExtractDir -Filter "onnxruntime.xcframework" -Directory -Recurse)
if ($XcfRootCandidates.Count -eq 0) {
    Write-Error "onnxruntime.xcframework not found inside $IosXcfZipSrc — Microsoft may have changed the inner-zip layout. Inspect $IosXcfExtractDir."
}
$XcfRoot = $XcfRootCandidates[0].FullName
Write-Host "  iOS xcframework root: $XcfRoot"

# Helper: patch + stage one slice's onnxruntime.framework into a renamed
# InoOnnxRuntime.framework at the destination. The Python script handles
# the binary rename, LC_ID_DYLIB rewrite, and Info.plist rewrite.
function Stage-IosFramework {
    param(
        [Parameter(Mandatory)] [string]$SliceDir,    # e.g. <xcf-root>/ios-arm64
        [Parameter(Mandatory)] [string]$DestFwDir,   # e.g. Source/ThirdParty/IOS/InoOnnxRuntime.framework
        [Parameter(Mandatory)] [string]$Label        # for logging
    )

    if (-not (Test-Path $SliceDir)) {
        Write-Error "[$Label] slice not found: $SliceDir (xcframework upstream layout may have changed)"
    }

    $SrcFw = Join-Path $SliceDir "onnxruntime.framework"
    if (-not (Test-Path $SrcFw)) {
        Write-Error "[$Label] onnxruntime.framework missing inside slice: $SrcFw"
    }

    Write-Host "  [PATCH] $Label framework: rename onnxruntime -> InoOnnxRuntime"
    & python $script:ApplePatchScript --mode framework `
        --input $SrcFw `
        --output $DestFwDir `
        --new-name InoOnnxRuntime
    if ($LASTEXITCODE -ne 0) {
        Write-Error "patch-ort-apple.py (framework mode, $Label) failed (exit $LASTEXITCODE). Did you 'pip install lief'?"
    }

    $StagedBin = Join-Path $DestFwDir "InoOnnxRuntime"
    if (-not (Test-Path $StagedBin -PathType Leaf)) {
        Write-Error "[$Label] post-patch binary missing at $StagedBin"
    }
    $sizeMb = [math]::Round((Get-Item $StagedBin).Length / 1MB, 1)
    Write-Host "  [STAGE] $Label/InoOnnxRuntime ($sizeMb MB)"
}

# 1. iOS device (arm64 only — Apple dropped 32-bit + simulator-on-device
#    long ago).
Stage-IosFramework `
    -SliceDir (Join-Path $XcfRoot "ios-arm64") `
    -DestFwDir $IosFrameworkDir `
    -Label "iOS device (arm64)"

# 2. iOS simulator (arm64 + x86_64 fat). arm64 covers Apple Silicon Mac
#    hosts running the simulator; x86_64 covers Intel Mac hosts. Devs
#    iterating in Xcode simulator need this slice. Currently NOT wired
#    into Build.cs (matches InoLlama's pattern; dev convenience only).
Stage-IosFramework `
    -SliceDir (Join-Path $XcfRoot "ios-arm64_x86_64-simulator") `
    -DestFwDir $IosSimFrameworkDir `
    -Label "iOS simulator (arm64 + x86_64)"

Write-Host ""

#---------------------------------------------------------------------
# 10. Write version stamp
#---------------------------------------------------------------------
Set-Content -Path $StampFile -Value $ExpectedStamp -NoNewline -Encoding ASCII

Write-Host "=== ORT $Version + DirectML $DmlVersion staged successfully ===" -ForegroundColor Green
Write-Host ""
Write-Host "Staged binaries:"
Write-Host "  Windows ORT core:    $Win64BinStageDir\InoOnnxRuntime.dll  (renamed + import-patched)"
Write-Host "  Windows ORT shared:  $Win64BinStageDir\onnxruntime_providers_shared.dll"
Write-Host "  DirectML runtime:    $Win64BinStageDir\InoDml.dll          (renamed)"
Write-Host "  Android ORT:         $Arm64BinStageDir\libInoOnnxRuntime.so"
Write-Host "  Mac ORT:             $MacStageDir\libInoOnnxRuntime.dylib  (Apple Silicon)"
Write-Host "  iOS device ORT:      $IosFrameworkDir\InoOnnxRuntime"
Write-Host "  iOS simulator ORT:   $IosSimFrameworkDir\InoOnnxRuntime    (dev only — not wired into Build.cs)"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Verify Ino.Onnx.ProvidersTest inside PIE now lists DmlExecutionProvider (Win64),"
Write-Host "     CoreMLExecutionProvider (Mac/iOS), or NnapiExecutionProvider (Android)."
Write-Host "  2. Flip Chatterbox Performance.bPreferDirectMl on (default true after Phase D+)."

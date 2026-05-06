# setup-onnxruntime.ps1
#
# One-time setup (idempotent) for the InoOnnx plugin.
#
# Downloads Microsoft's prebuilt ONNX Runtime binaries for Win64 and Android
# arm64-v8a, stages everything (headers + DLLs/.so) under a flat tree:
#   Plugins/InoOnnx/Source/ThirdParty/
#     Public/                          C / C++ API headers
#     Win64/                           InoOnnxRuntime.dll, InoDml.dll, providers_shared
#     Android/arm64-v8a/               libInoOnnxRuntime.so
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
# Python + pefile dependency:
#   The patch script needs Python 3 on PATH and `pip install pefile`.
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
$ThirdPartyDir    = Join-Path $PluginDir "Source\ThirdParty"
$PublicIncDir     = Join-Path $ThirdPartyDir "Public"
$Win64BinStageDir = Join-Path $ThirdPartyDir "Win64"
$Arm64BinStageDir = Join-Path $ThirdPartyDir "Android\arm64-v8a"

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
    (Test-Path (Join-Path $Arm64BinStageDir "libInoOnnxRuntime.so"))) {
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

foreach ($d in @($CacheDir, $PublicIncDir, $Win64BinStageDir, $Arm64BinStageDir)) {
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
# 9. Write version stamp
#---------------------------------------------------------------------
Set-Content -Path $StampFile -Value $ExpectedStamp -NoNewline -Encoding ASCII

Write-Host "=== ORT $Version + DirectML $DmlVersion staged successfully ===" -ForegroundColor Green
Write-Host ""
Write-Host "Staged binaries:"
Write-Host "  Windows ORT core:   $Win64BinStageDir\InoOnnxRuntime.dll  (renamed + import-patched)"
Write-Host "  Windows ORT shared: $Win64BinStageDir\onnxruntime_providers_shared.dll"
Write-Host "  DirectML runtime:   $Win64BinStageDir\InoDml.dll          (renamed)"
Write-Host "  Android ORT:        $Arm64BinStageDir\libInoOnnxRuntime.so"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Verify Ino.Onnx.ProvidersTest inside PIE now lists DmlExecutionProvider."
Write-Host "  2. Flip Chatterbox Performance.bPreferDirectMl on (default true after Phase D+)."

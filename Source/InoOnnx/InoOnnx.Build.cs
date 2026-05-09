// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

public class InoOnnx : ModuleRules
{
	public InoOnnx(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Public/ that holds the C++ wrapper
				// headers (InoOnnxSession.h, InoOnnxTensor.h, InoOnnxTypes.h).
				// Exposed as a Public include path so consumers can use
				// the bare #include "InoOnnxSession.h" without the
				// "Onnx/" subdir prefix.
				Path.Combine(ModuleDirectory, "Public", "Onnx"),
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// Subdirectory of Private/ that holds the wrapper impl
				// files (InoOnnxInternal.{h,cpp}, InoOnnxSession.cpp,
				// InoOnnxTensor.cpp). Lets the .cpps include the internal
				// helper without a relative path.
				Path.Combine(ModuleDirectory, "Private", "Onnx"),
			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",

				// IPluginManager — used in StartupModule to resolve our
				// plugin's install path so we can load DLLs by full absolute path.
				"Projects",
			}
			);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);

		// =====================================================================
		// ONNX Runtime + DirectML third-party integration
		// =====================================================================
		// This is the only UE module in the plugin, so it owns the third-party
		// wiring directly (no separate external module). Build artifacts are
		// produced by Plugins/InoOnnx/OnnxRuntime/scripts/setup-onnxruntime.ps1
		// which downloads Microsoft's prebuilt NuGets / Maven AAR and stages
		// them into the consolidated tree:
		//
		//     Source/ThirdParty/Public/                       ORT C / C++ API headers
		//     Source/ThirdParty/Win64/InoOnnxRuntime.dll      RENAMED from onnxruntime.dll
		//     Source/ThirdParty/Win64/InoDml.dll              RENAMED from DirectML.dll
		//     Source/ThirdParty/Win64/onnxruntime_providers_shared.dll
		//     Source/ThirdParty/Android/arm64-v8a/libInoOnnxRuntime.so
		//
		// Companion file in this same module directory:
		//     InoOnnx_UPL_Android.xml             Android packaging directives
		//
		// Consumers #include "onnxruntime_c_api.h" for the type definitions
		// (OrtApi, OrtSession, OrtStatus, etc.) but do NOT call the exported
		// functions directly — all ORT calls go through the OrtApi vtable
		// returned by GetApiBase()->GetApi(ORT_API_VERSION) at runtime.
		//
		// Why no import library / no PublicDelayLoadDLLs:
		//   UE 5.7 ships multiple conflicting copies of "onnxruntime.dll"
		//   (NNERuntimeORT plugin, RuntimeMetaHumanLipSync plugin, etc.).
		//   Windows LoadLibrary caches DLLs by BASE NAME — whichever
		//   onnxruntime.dll gets loaded into the process first wins, and
		//   subsequent GetDllHandle calls with a different full path still
		//   return the cached older-version handle. Our OrtApi::GetApi(24)
		//   then returns nullptr because UE's bundled ORT is 1.19.x.
		//
		//   Fix: we rename our DLL to "InoOnnxRuntime.dll" (no other code
		//   knows that name) and load it via GetProcAddress on the single
		//   exported entry point "OrtGetApiBase" at module startup. The
		//   returned OrtApi vtable drives everything else — no static
		//   linker dependency on the ORT export table at all, and no
		//   cache collision possible.
		//
		//   Android does not need the rename for symbol-collision reasons,
		//   but we keep it anyway — clang's linker at libUnreal.so build
		//   time would otherwise resolve OrtGetApiBase against whichever
		//   libonnxruntime.so it sees first (often a Marketplace plugin's
		//   1.19.2), recording a versioned symbol reference that fails at
		//   runtime against our 1.24.3 .so. Renaming + dlopen + dlsym at
		//   runtime sidesteps the link entirely.
		//
		// DirectML Execution Provider (Windows only, D3D12-based GPU/NPU
		// acceleration) is included via the Microsoft.ML.OnnxRuntime.DirectML
		// NuGet build. Two additional Windows runtime dependencies ship
		// alongside InoOnnxRuntime.dll:
		//
		//   InoDml.dll                      RENAMED from DirectML.dll — our
		//                                   ORT's PE delay-import table is
		//                                   patched to match (see
		//                                   patch-ort-dml-import.py)
		//   onnxruntime_providers_shared.dll  ORIGINAL NAME — shared-EP
		//                                   infrastructure, LoadLibrary'd
		//                                   by ORT on demand
		//
		// CUDA / TensorRT / ROCm are intentionally NOT shipped — DirectML
		// covers NVIDIA, AMD, Intel, and NPUs on a D3D12 path with one
		// 20 MB redist, vs CUDA's 300+ MB NVIDIA-only mega-bundle.

		string ThirdPartyDir  = Path.Combine(PluginDirectory, "Source", "ThirdParty");
		string PublicDir      = Path.Combine(ThirdPartyDir, "Public");
		string Win64Dir       = Path.Combine(ThirdPartyDir, "Win64");
		string AndroidBaseDir = Path.Combine(ThirdPartyDir, "Android");
		string MacDir         = Path.Combine(ThirdPartyDir, "Mac");
		string IosFwDir       = Path.Combine(ThirdPartyDir, "IOS", "InoOnnxRuntime.framework");

		// Public headers — consumers do
		//     #include "onnxruntime_c_api.h"
		PublicSystemIncludePaths.Add(PublicDir);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows: dynamic loading only. NO PublicAdditionalLibraries,
			// NO PublicDelayLoadDLLs. The runtime consumer resolves
			// OrtGetApiBase via GetProcAddress on the renamed
			// "InoOnnxRuntime.dll".
			//
			// We still need RuntimeDependencies so UE's packaging step
			// copies the three DLLs to the staged output alongside the
			// game executable. Without them the shipped build would ship
			// without the ORT runtime and every session-creation call
			// would fail.

			// 1. The core ORT DLL (renamed for base-name isolation +
			//    delay-import table patched to point at InoDml.dll).
			RuntimeDependencies.Add(Path.Combine(Win64Dir, "InoOnnxRuntime.dll"));

			// 2. onnxruntime_providers_shared.dll (original name).
			//    ORT LoadLibrary's this at session-create time for certain
			//    shared providers. Not a static import of InoOnnxRuntime.dll
			//    (verified via dumpbin).
			RuntimeDependencies.Add(Path.Combine(Win64Dir, "onnxruntime_providers_shared.dll"));

			// 3. InoDml.dll (RENAMED from DirectML.dll). DirectML.dll is a
			//    delay-load dependency of InoOnnxRuntime.dll; UE 5.7 ships
			//    its own DirectML.dll at Engine/Binaries/Win64/DML/x64/ and
			//    its NNE / RuntimeMetaHumanLipSync plugins LoadLibrary that
			//    copy early in startup, winning Windows' base-name cache.
			//    Our ORT's delay-load stub would then bind to UE's different-
			//    version DirectML, causing kernel validation failures on
			//    fp16 attention and silent numerical corruption.
			//
			//    Fix: rename DirectML.dll -> InoDml.dll on disk and patch
			//    InoOnnxRuntime.dll's delay-import table to match (via
			//    patch-ort-dml-import.py in the setup script). No plugin
			//    looks for "InoDml.dll" so collisions become structurally
			//    impossible.
			RuntimeDependencies.Add(Path.Combine(Win64Dir, "InoDml.dll"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			// Android arm64-v8a artifact staged by setup-onnxruntime.ps1
			// under Source/ThirdParty/Android/arm64-v8a/, RENAMED from
			// libonnxruntime.so to libInoOnnxRuntime.so.
			//
			// Like Windows, we do NOT use PublicAdditionalLibraries on
			// Android. UE 5.7 ships a Marketplace plugin (the
			// RuntimeMetaHumanLipSync variant) that distributes its own
			// libonnxruntime.so for Android arm64 at ORT v1.19.2. If it's
			// enabled in the project, UBT adds THAT .so to the link path
			// for libUnreal.so, recording a versioned symbol reference
			// (e.g. OrtGetApiBase@VERS_1.19.2) that fails at runtime
			// against our 1.24.3 .so — silent process abort before UE's
			// logger is up.
			//
			// Fix: isolate. Rename our .so, drop it from the link command
			// entirely, and dlopen + dlsym at runtime.
			string Arm64Dir = Path.Combine(AndroidBaseDir, "arm64-v8a");
			string SoPath   = Path.Combine(Arm64Dir, "libInoOnnxRuntime.so");
			if (File.Exists(SoPath))
			{
				// RuntimeDependencies keeps the .so in the UE packaging
				// manifest (cook-time staging into Saved/). The actual
				// APK inclusion happens via the UPL XML's <resourceCopies>.
				RuntimeDependencies.Add(SoPath);
			}

			// Apply the UPL (Unreal Plugin Language) XML that tells UE's
			// APK packager to copy libInoOnnxRuntime.so into lib/arm64-v8a/
			// and emit a System.loadLibrary("InoOnnxRuntime") call so the
			// .so is resident by the time the consumer's StartupModule runs.
			AdditionalPropertiesForReceipt.Add(
				"AndroidPlugin",
				Path.Combine(ModuleDirectory, "InoOnnx_UPL_Android.xml"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// Mac: dynamic loading only — same isolation rationale as Win64
			// and Android. Even though no UE 5.7 plugin currently ships its
			// own libonnxruntime.dylib, keeping the load explicit (dlopen
			// by full path resolved via IPluginManager) means we can never
			// accidentally dyld-bind to a future Marketplace plugin's copy
			// at app launch. Apple Silicon only — Microsoft drops Intel
			// Mac in modern ORT NuGets.
			//
			// Staged by setup-onnxruntime.ps1 from the regular CPU NuGet
			// (NOT the DirectML one — DML is Windows-only):
			//   runtimes/osx-arm64/native/libonnxruntime.dylib
			//     -> Source/ThirdParty/Mac/libInoOnnxRuntime.dylib
			// LC_ID_DYLIB rewritten to @rpath/libInoOnnxRuntime.dylib via
			// patch-ort-apple.py (lief-based) so the install_name matches
			// the renamed file. CoreML EP is statically compiled into this
			// build and self-registers when the dylib is mapped.
			string MacDylib = Path.Combine(MacDir, "libInoOnnxRuntime.dylib");
			if (File.Exists(MacDylib))
			{
				// RuntimeDependencies stages the dylib alongside the
				// packaged executable. UE's Mac packaging copies it into
				// <Game>.app/Contents/UE/<plugin-relative-path>/ — our
				// IPluginManager-resolved Init path picks it up there.
				RuntimeDependencies.Add(MacDylib);
			}
		}
		else if (Target.Platform == UnrealTargetPlatform.IOS)
		{
			// iOS: Microsoft ships ORT for iOS as a STATIC FRAMEWORK —
			// the binary inside InoOnnxRuntime.framework/ is a Unix `ar`
			// archive (wrapped in a fat header), NOT a Mach-O dylib. This
			// is Apple's "static framework" convention, common for
			// security-sensitive code on iOS.
			//
			// PublicAdditionalFrameworks adds `-framework InoOnnxRuntime`
			// to the iOS link command. Because the framework is static,
			// every Ort* symbol the linker pulls ends up baked into the
			// final iOS executable's binary — there is NO separate
			// runtime dylib for dyld to load at app launch.
			//
			// bCopyFramework = false because nothing needs to be embedded
			// in the .app/Frameworks/ directory. With a real dynamic
			// framework we'd want true here so dyld can find the dylib at
			// app launch; with a static archive there's no dylib + the
			// archive's bytes are already linked into the main executable,
			// so embedding would duplicate the code in the .ipa AND
			// confuse code-signing (codesign expects a Mach-O dylib at
			// the framework's binary path, not an `ar` archive).
			//
			// At runtime, InoOnnx.cpp resolves OrtGetApiBase via
			// dlsym(RTLD_DEFAULT, ...) — for iOS that resolves against
			// the main executable's global symbol table, which contains
			// the statically-linked Ort* symbols. Same call site as Mac
			// / Win64 / Android, different mechanism underneath.
			//
			// Staged by setup-onnxruntime.ps1 from the CPU NuGet:
			//   runtimes/ios/native/onnxruntime.xcframework.zip (nested)
			//     -> ios-arm64/onnxruntime.framework slice
			//     -> renamed end-to-end via patch-ort-apple.py:
			//          - framework dir   onnxruntime.framework -> InoOnnxRuntime.framework
			//          - binary name     onnxruntime           -> InoOnnxRuntime
			//          - LC_ID_DYLIB     skipped — static `ar` archive has
			//                            no install_name to patch
			//          - Info.plist      CFBundleExecutable / Name / Identifier
			//
			// Currently we only ship the device slice (arm64). The
			// simulator slice exists at Source/ThirdParty/IOS/Simulator/
			// for development convenience but is not wired in here —
			// UE 5.7's iOS toolchain targets device builds in shipped
			// game flow, and devs running in simulator can flip this
			// path or branch on Target.Architecture. (Future follow-up;
			// not blocking initial iOS support.)
			if (Directory.Exists(IosFwDir))
			{
				PublicAdditionalFrameworks.Add(new Framework(
					"InoOnnxRuntime",
					IosFwDir,
					/*CopyBundledAssets*/ null,
					/*bCopyFramework*/ false));
			}
		}
		// Linux: not yet implemented. Linking succeeds because no static
		// references; runtime calls fail gracefully when the dynamic load
		// can't find the library.
	}
}

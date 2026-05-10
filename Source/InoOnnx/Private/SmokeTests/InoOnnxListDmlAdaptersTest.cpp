// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

// ============================================================================
// Ino.Onnx.ListDmlAdapters
// ============================================================================
//
// Windows-only diagnostic: enumerates D3D12 adapters visible to
// IDXGIFactory::EnumAdapters and logs each one's index, description,
// vendor / device ID, and memory stats. The index printed here is the
// exact value to pass as FInoOnnxSessionOptions::DirectMlAdapterIndex
// to pick a specific GPU / iGPU / NPU / WARP for DirectML inference.
//
// Why this exists: DirectML's OrtSessionOptionsAppendExecutionProvider_DML
// takes a `device_id` parameter that matches IDXGIFactory::EnumAdapters
// order exactly, but there's no ergonomic way to know what's at each
// index without writing DXGI code. This command is the ergonomic way.
//
// On Windows the DXGI adapter order is driver / OS defined, typically:
//
//   [0] Primary display adapter (often the dGPU on desktops with a
//       discrete card + monitor plugged into it; often the iGPU on
//       laptops with integrated-only graphics).
//   [1..N] Other hardware adapters (secondary GPU, iGPU alongside a
//       dGPU, NPU on Windows 11 24H2+ with a driver that enumerates it
//       as a D3D12 adapter).
//   [last] Microsoft Basic Render Driver (WARP software rasterizer).
//
// Not PIE-gated; works from the editor's main menu console too.
//
// Invoke:
//     Ino.Onnx.ListDmlAdapters
// ============================================================================

#include "CoreMinimal.h"

#include "HAL/IConsoleManager.h"

#include "InoOnnx.h"              // for LogInoOnnx

#if PLATFORM_WINDOWS
    #include "Windows/AllowWindowsPlatformTypes.h"
    #include <dxgi1_4.h>
    #include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{

#if PLATFORM_WINDOWS
    /** Friendly vendor name for the most common DXGI vendor IDs. Falls
     *  back to a raw hex string for unknowns. Vendor IDs come from PCI
     *  SIG assignments; the short list here covers every GPU / iGPU /
     *  NPU you'd realistically see in a UE target environment. */
    FString VendorString(uint32 VendorId)
    {
        switch (VendorId)
        {
            case 0x10DE: return TEXT("NVIDIA");
            case 0x1002: return TEXT("AMD");
            case 0x8086: return TEXT("Intel");
            case 0x1414: return TEXT("Microsoft (WARP / Basic Render)");
            case 0x5143: return TEXT("Qualcomm");
            case 0x106B: return TEXT("Apple");
            default:     return FString::Printf(TEXT("0x%04X"), VendorId);
        }
    }
#endif

    void RunOnnxListDmlAdapters(const TArray<FString>& /*Args*/)
    {
#if PLATFORM_WINDOWS
        IDXGIFactory4* Factory = nullptr;
        const HRESULT HrFactory = CreateDXGIFactory1(IID_PPV_ARGS(&Factory));
        if (FAILED(HrFactory) || Factory == nullptr)
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.ListDmlAdapters: CreateDXGIFactory1 failed ")
                   TEXT("(HRESULT=0x%08X). DXGI support is broken on this system."),
                   (uint32)HrFactory);
            return;
        }

        UE_LOG(LogInoOnnx, Log,
               TEXT("Ino.Onnx.ListDmlAdapters: enumerating D3D12 adapters ")
               TEXT("(IDXGIFactory::EnumAdapters1 order — matches DirectML's device_id indexing)..."));

        UINT            AdapterIndex = 0;
        IDXGIAdapter1*  Adapter      = nullptr;

        while (Factory->EnumAdapters1(AdapterIndex, &Adapter) != DXGI_ERROR_NOT_FOUND)
        {
            if (Adapter != nullptr)
            {
                DXGI_ADAPTER_DESC1 Desc;
                FMemory::Memzero(&Desc, sizeof(Desc));
                if (SUCCEEDED(Adapter->GetDesc1(&Desc)))
                {
                    const uint64 DedicatedVramMB = (uint64)Desc.DedicatedVideoMemory  / (1024ULL * 1024ULL);
                    const uint64 DedicatedSysMB  = (uint64)Desc.DedicatedSystemMemory / (1024ULL * 1024ULL);
                    const uint64 SharedSysMB     = (uint64)Desc.SharedSystemMemory    / (1024ULL * 1024ULL);
                    const bool   bIsSoftware     = (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

                    // Description is TCHAR[128] per DXGI_ADAPTER_DESC1.
                    UE_LOG(LogInoOnnx, Log,
                           TEXT("  [%u] %s  (vendor=%s, device=0x%04X, rev=%u)"),
                           AdapterIndex,
                           Desc.Description,
                           *VendorString(Desc.VendorId),
                           Desc.DeviceId,
                           Desc.Revision);
                    UE_LOG(LogInoOnnx, Log,
                           TEXT("         memory: dedicated_vram=%llu MB, dedicated_sys=%llu MB, shared_sys=%llu MB%s"),
                           DedicatedVramMB, DedicatedSysMB, SharedSysMB,
                           bIsSoftware ? TEXT("  [SOFTWARE WARP — slow; fallback only]") : TEXT(""));
                }
                else
                {
                    UE_LOG(LogInoOnnx, Warning,
                           TEXT("  [%u] <GetDesc1 failed>"), AdapterIndex);
                }

                Adapter->Release();
                Adapter = nullptr;
            }
            ++AdapterIndex;
        }

        if (AdapterIndex == 0)
        {
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Ino.Onnx.ListDmlAdapters: no D3D12 adapters found. ")
                   TEXT("Verify D3D12 is installed and at least one DX-capable GPU / iGPU / WARP is present."));
        }
        else
        {
            UE_LOG(LogInoOnnx, Log,
                   TEXT("Ino.Onnx.ListDmlAdapters: %u adapter(s) total. Pass the index you want ")
                   TEXT("to FInoOnnxSessionOptions::DirectMlAdapterIndex."),
                   AdapterIndex);
        }

        Factory->Release();
#else
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Ino.Onnx.ListDmlAdapters: Windows-only command. DirectML itself is ")
               TEXT("D3D12-based and has no equivalent on this platform."));
#endif
    }

    FAutoConsoleCommand GOnnxListDmlAdaptersCmd(
        TEXT("Ino.Onnx.ListDmlAdapters"),
        TEXT("List D3D12 adapters visible to IDXGIFactory::EnumAdapters1, in the ")
        TEXT("exact order DirectML uses for device_id. Pair the printed index with ")
        TEXT("FInoOnnxSessionOptions::DirectMlAdapterIndex to pick a specific ")
        TEXT("GPU / iGPU / NPU / WARP adapter for DirectML inference. Windows-only."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunOnnxListDmlAdapters));
}

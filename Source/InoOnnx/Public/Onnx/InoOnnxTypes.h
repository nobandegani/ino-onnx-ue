// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

#include "InoOnnxTypes.generated.h"

/**
 * ==========================================================================
 *  InoOnnx — generic ONNX Runtime wrapper
 * ==========================================================================
 *
 * This header declares the model-agnostic types used by
 * FInoOnnxSession and FInoOnnxTensor. None of it is specific to any model
 * (Chatterbox, Gemma, vision encoders, classifiers, etc.). It is the
 * foundation the rest of the ONNX-consuming code in the plugin builds on.
 *
 * No Blueprint exposure in this layer — ONNX tensors don't map cleanly
 * to Blueprint types (dynamic shapes, dozens of dtypes, zero-copy
 * buffer semantics). Per-model layers (UInoChatterboxTtsSubsystem etc.)
 * are the right place to add Blueprint-friendly wrappers that hide
 * this machinery.
 */

// ============================================================================
//  Data types
// ============================================================================

/**
 * ONNX tensor element data types. Matches ONNXTensorElementDataType
 * from onnxruntime_c_api.h, but re-declared here as a UE-friendly enum
 * so public headers don't need to drag in ORT headers.
 *
 * The subset here covers every type any current or planned plugin
 * consumer needs (TTS codecs, vision encoders, LLM token tensors).
 * Types we've never had a need for (complex64, string, float8*,
 * int4 groupings) are intentionally omitted — add them when a first
 * consumer arrives.
 */
enum class EInoOnnxDtype : uint8
{
    Undefined = 0,
    Float32,
    Float16,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Bool,
};

/**
 * ORT execution providers the plugin knows how to request.
 *
 * Cpu is always available (ORT's reference implementation).
 * Xnnpack is always available on arm64 and typically on x86_64 too
 * (ships with every ORT build we've looked at).
 * Nnapi / WebGpu / DirectMl / CoreMl / Cuda / TensorRt are available
 * only on specific platforms and specific ORT builds — if a caller
 * requests one that isn't registered in the current ORT runtime, we
 * silently skip it and fall through to the next entry in the priority
 * list.
 *
 * The enum order here is also the RECOMMENDED priority for a generic
 * workload on each platform — put your preferred accelerator first.
 * FInoOnnxSession::GetActiveProviders() returns the providers that
 * actually successfully registered.
 */
UENUM(BlueprintType)
enum class EInoOnnxProvider : uint8
{
    Cpu       UMETA(DisplayName = "CPU"),         ///< reference CPU implementation; always works
    Xnnpack   UMETA(DisplayName = "XNNPACK"),     ///< Google XNNPACK CPU kernels (ARM NEON / AVX); very fast on both platforms
    Nnapi     UMETA(DisplayName = "NNAPI"),       ///< Android Neural Networks API (vendor NPU/GPU/DSP); Android only
    WebGpu    UMETA(DisplayName = "WebGPU"),      ///< ONNX WebGPU provider (GPU compute); Android + future Windows
    DirectMl  UMETA(DisplayName = "DirectML"),    ///< Windows GPU via D3D12 (matches UE's renderer); Windows only
    CoreMl    UMETA(DisplayName = "CoreML"),      ///< Apple CoreML (CPU/GPU/ANE on Apple Silicon); macOS + iOS only
    Cuda      UMETA(DisplayName = "CUDA"),        ///< NVIDIA CUDA; Windows / Linux with CUDA toolkit
    TensorRt  UMETA(DisplayName = "TensorRT"),    ///< NVIDIA TensorRT (on top of CUDA); Windows / Linux, NVIDIA only
};

/**
 * Graph optimization level passed to OrtSessionOptions. Same semantics
 * as ORT's GraphOptimizationLevel enum. Higher levels do more aggressive
 * operator fusion / constant folding at session-create time; the
 * per-inference cost does not change.
 *
 * Default (All) is almost always what you want. The only reason to
 * lower this is if your model hits a known bug in a fused-op
 * implementation, or you need inspection of the mid-level graph.
 */
UENUM(BlueprintType)
enum class EInoOnnxGraphOptimizationLevel : uint8
{
    Disabled  UMETA(DisplayName = "Disabled"),
    Basic     UMETA(DisplayName = "Basic"),
    Extended  UMETA(DisplayName = "Extended"),
    All       UMETA(DisplayName = "All"),
};

/**
 * Op-level execution mode. Sequential runs ops one at a time; Parallel
 * can run independent graph branches concurrently using InterOpThreadCount.
 * For nearly all models Sequential is the right answer. DirectML
 * REQUIRES Sequential (the EP doesn't support parallel multi-op
 * scheduling); the session builder forces it on if DML is in the
 * provider list.
 */
UENUM(BlueprintType)
enum class EInoOnnxExecutionMode : uint8
{
    Sequential  UMETA(DisplayName = "Sequential"),
    Parallel    UMETA(DisplayName = "Parallel"),
};

// ============================================================================
//  Session options
// ============================================================================

/**
 * Configuration for FInoOnnxSession::Create. All fields have sensible
 * defaults — a default-constructed FInoOnnxSessionOptions loads with
 * CPU-only, default ORT thread pool, full graph optimization, no
 * profiling. That's the right starting point for 90 % of use cases.
 *
 * ExecutionProviders: priority-ordered list. The session builder
 *   registers them in order; if one fails (e.g. NNAPI requested on
 *   non-Android), it is skipped with a log message and the next
 *   provider takes priority. If ALL fail, ORT falls back to CPU
 *   internally — but you should always include EInoOnnxProvider::Cpu
 *   somewhere in the list to make that explicit.
 *
 * IntraOpThreadCount: number of threads inside one op (e.g. matmul
 *   parallelism). 0 = ORT default (typically one per physical core).
 *   For LLM workloads where multiple sessions run concurrently,
 *   setting this to 2-4 per session gives better aggregate throughput
 *   than letting each session claim all cores.
 *
 * InterOpThreadCount: number of threads spanning different ops in a
 *   single inference (parallel graph execution). Usually 0 (sequential)
 *   is fine unless you have known parallel-able graph sections.
 *
 * GraphOptimization: see enum docs. Default (All) is correct.
 *
 * SessionConfig: passes arbitrary key-value pairs through to
 *   OrtApi::AddSessionConfigEntry. The full key list is at
 *   onnxruntime/core/session/session_options_config_keys.h.
 *   Examples:
 *     "session.disable_prepacking" -> "1"
 *     "session.intra_op.allow_spinning" -> "0"
 *   Most callers leave this empty.
 *
 * OptimizedModelOutputPath: if non-empty, ORT will write the
 *   post-optimization model to this path after session creation
 *   (useful for inspecting what the graph actually looks like after
 *   fusion / folding). Empty means don't save.
 *
 * bEnableProfiling: if true, ORT writes per-op timing data to a file
 *   named onnxruntime_profile_*.json next to the working directory.
 *   Costs ~5-10 % runtime overhead. Off by default.
 */
USTRUCT(BlueprintType)
struct INOONNX_API FInoOnnxSessionOptions
{
    GENERATED_BODY()

    // ====================================================================
    //  Provider selection
    // ====================================================================

    /**
     * Priority-ordered list of execution providers. The session builder
     * registers them in order; ones that fail (e.g. NNAPI on Windows) are
     * skipped with a warning. CPU is always available as the implicit
     * fallback even if every entry fails.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Provider")
    TArray<EInoOnnxProvider> ExecutionProviders = { EInoOnnxProvider::Cpu };

    // ====================================================================
    //  Threading
    // ====================================================================

    /**
     * Threads inside one op (matmul parallelism). 0 = ORT default
     * (typically one per physical core). For multi-session apps where
     * sessions run concurrently, capping at 2-4 per session typically
     * gives better aggregate throughput than letting each claim all cores.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Threading")
    int32 IntraOpThreadCount = 0;

    /**
     * Threads spanning different ops (parallel graph execution). Only
     * has effect with ExecutionMode = Parallel. 0 = ORT default.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Threading")
    int32 InterOpThreadCount = 0;

    /**
     * Sequential = ops run one at a time; Parallel = independent branches
     * can run concurrently using InterOpThreadCount. DirectML requires
     * Sequential (forced automatically if DML is in the provider list).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Threading")
    EInoOnnxExecutionMode ExecutionMode = EInoOnnxExecutionMode::Sequential;

    // ====================================================================
    //  Graph optimization
    // ====================================================================

    /**
     * Graph optimization level applied at session-create. Higher levels
     * fuse more ops + fold more constants once at load; per-inference
     * cost does not change. All is the recommended default.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Optimization")
    EInoOnnxGraphOptimizationLevel GraphOptimization = EInoOnnxGraphOptimizationLevel::All;

    /**
     * ORT's memory-pattern optimizer. Reuses host allocations across
     * inferences when input shapes are stable. Auto-disabled if
     * DirectML is in the provider list (DML uses D3D12 resource binding
     * that conflicts with mem-pattern's contiguous-host assumption).
     * Default: ON.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Optimization")
    bool bEnableMemPattern = true;

    /**
     * ORT's CPU memory arena (pooled allocator). Default ON — strongly
     * recommended for any workload that runs more than one inference,
     * since it avoids per-Run heap traffic for ORT internal scratch
     * buffers. Disable only if you've measured arena retention as a
     * memory-footprint problem.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Optimization")
    bool bEnableCpuMemArena = true;

    // ====================================================================
    //  Provider-specific options
    // ====================================================================

    /**
     * DirectML adapter index (IDXGIFactory::EnumAdapters order). 0 =
     * default adapter (typically the primary display GPU). On a system
     * with an integrated + discrete GPU, adapter 0 is often the
     * integrated one (power-aware systems) or the discrete one
     * (performance-aware) — check dxdiag if you need to pick deliberately.
     * Negative values invalid; ignored on non-Windows.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|DirectML")
    int32 DirectMlAdapterIndex = 0;

    /**
     * NNAPI: enable FP16 on FP32 models. Vendor NPUs / GPUs typically
     * accelerate FP16 — matches what mobile silicon actually runs
     * natively. Off by default to preserve numerical fidelity; flip on
     * for speed if your model tolerates FP16 quality loss.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|NNAPI")
    bool bNnapiUseFp16 = false;

    /**
     * NNAPI: prefer NCHW (channel-first) layout. Some NPUs only accept
     * NCHW; vendor drivers will silently fall back to CPU otherwise.
     * Off by default — most modern Android NPUs handle NHWC fine.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|NNAPI")
    bool bNnapiUseNchw = false;

    /**
     * NNAPI: refuse CPU fallback within NNAPI. If the model can't fit
     * entirely on the accelerator, registration fails (logged); ORT
     * then falls through to the next provider in the priority list.
     * Useful when the CPU EP would be faster than NNAPI's CPU runtime.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|NNAPI")
    bool bNnapiCpuDisabled = false;

    // ====================================================================
    //  Diagnostics
    // ====================================================================

    /**
     * If non-empty, ORT writes the post-optimization model graph to this
     * path after session create — useful for inspecting the result of
     * fusion / folding / layout transformation. Empty = don't save.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Diagnostics")
    FString OptimizedModelOutputPath;

    /**
     * If true, ORT writes per-op timing data to a file named
     * onnxruntime_profile_*.json under <Project>/Saved/Logs/. Costs
     * ~5–10 % runtime overhead. Off by default.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Diagnostics")
    bool bEnableProfiling = false;

    /**
     * Override ORT's session log severity level. -1 = use ORT default
     * (Warning). Valid override values match OrtLoggingLevel:
     *   0 = Verbose, 1 = Info, 2 = Warning, 3 = Error, 4 = Fatal.
     * Set 0 when diagnosing DML kernel fallbacks / op-support gaps;
     * the verbose log shows which DML_OPERATOR_*_DESC field failed
     * validation instead of just a generic E_INVALIDARG. Costs ~5 %.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Diagnostics")
    int32 LogSeverityLevel = -1;

    /**
     * Optional log id for this session. Useful when multiple sessions
     * run concurrently to distinguish their lines in ORT's log stream.
     * Empty = don't set.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Diagnostics")
    FString SessionLogId;

    // ====================================================================
    //  Advanced — pass arbitrary key/value pairs through to ORT
    // ====================================================================

    /**
     * Arbitrary OrtApi::AddSessionConfigEntry pairs. Use this when you
     * need an option not exposed as a typed field above. Full key list:
     *   onnxruntime/core/session/session_options_config_keys.h
     * Examples:
     *   "session.disable_prepacking" -> "1"
     *   "session.intra_op.allow_spinning" -> "0"
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InoOnnx|Advanced")
    TMap<FString, FString> SessionConfig;
};

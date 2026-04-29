// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"

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
 * Nnapi / WebGpu / DirectMl / Cuda / TensorRt are available only on
 * specific platforms and specific ORT builds — if a caller requests
 * one that isn't registered in the current ORT runtime, we silently
 * skip it and fall through to the next entry in the priority list.
 *
 * The enum order here is also the RECOMMENDED priority for a generic
 * workload on each platform — put your preferred accelerator first.
 * FInoOnnxSession::GetActiveProviders() returns the providers that
 * actually successfully registered.
 */
enum class EInoOnnxProvider : uint8
{
    Cpu = 0,       ///< reference CPU implementation; always works
    Xnnpack,       ///< Google XNNPACK CPU kernels (ARM NEON / AVX); very fast on both platforms
    Nnapi,         ///< Android Neural Networks API (vendor NPU/GPU/DSP); Android only
    WebGpu,        ///< ONNX WebGPU provider (GPU compute); Android + future Windows
    DirectMl,      ///< Windows GPU via D3D12 (matches UE's renderer); Windows only
    Cuda,          ///< NVIDIA CUDA; Windows / Linux with CUDA toolkit
    TensorRt,      ///< NVIDIA TensorRT (on top of CUDA); Windows / Linux, NVIDIA only
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
enum class EInoOnnxGraphOptimizationLevel : uint8
{
    Disabled = 0,
    Basic,
    Extended,
    All,
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
struct FInoOnnxSessionOptions
{
    TArray<EInoOnnxProvider> ExecutionProviders = { EInoOnnxProvider::Cpu };
    int32 IntraOpThreadCount = 0;
    int32 InterOpThreadCount = 0;
    EInoOnnxGraphOptimizationLevel GraphOptimization = EInoOnnxGraphOptimizationLevel::All;
    TMap<FString, FString> SessionConfig;
    FString OptimizedModelOutputPath;
    bool bEnableProfiling = false;

    /**
     * DirectML adapter index (IDXGIFactory::EnumAdapters order). Only used
     * when EInoOnnxProvider::DirectMl is in ExecutionProviders. 0 = default
     * adapter, which is typically the primary display GPU. On a system with
     * an integrated GPU + discrete GPU, adapter 0 is often the integrated
     * one (power-aware systems) or the discrete one (performance-aware).
     * Check dxdiag if you need to pick deliberately.
     *
     * Negative values are invalid per ORT's DML EP. Ignored on non-Windows.
     */
    int32 DirectMlAdapterIndex = 0;

    /**
     * Set ORT's session log severity level. Values (match
     * OrtLoggingLevel in onnxruntime_c_api.h):
     *   0 = ORT_LOGGING_LEVEL_VERBOSE  (most chatty; shows every op
     *       the provider registers, every graph transformation, etc.)
     *   1 = ORT_LOGGING_LEVEL_INFO
     *   2 = ORT_LOGGING_LEVEL_WARNING  (ORT default)
     *   3 = ORT_LOGGING_LEVEL_ERROR
     *   4 = ORT_LOGGING_LEVEL_FATAL
     *
     * Use value 0 when diagnosing DML kernel fallbacks, op-support
     * gaps, or metacommand routing — the verbose log tells you
     * exactly which DML_OPERATOR_*_DESC field validation failed
     * instead of just a generic E_INVALIDARG line/column. Costs ~5%
     * runtime (log-print overhead) so leave at 2 for production.
     *
     * -1 = don't call SetSessionLogSeverityLevel at all (use ORT's
     * built-in default). This is the initial value to preserve
     * behaviour for callers that don't care about log level.
     */
    int32 LogSeverityLevel = -1;
};

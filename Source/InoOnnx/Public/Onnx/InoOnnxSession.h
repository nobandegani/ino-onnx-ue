// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Onnx/InoOnnxTypes.h"
#include "Onnx/InoOnnxTensor.h"

// Forward declare ORT's opaque session handle so this public header
// doesn't pull in onnxruntime_c_api.h.
struct OrtSession;
struct OrtSessionOptions;

/**
 * ==========================================================================
 *  FInoOnnxSession — generic ONNX Runtime inference session
 * ==========================================================================
 *
 * Loads an ONNX model from disk (or memory), registers the requested
 * execution providers, caches input/output metadata, and runs inference
 * either synchronously or asynchronously with completion callback.
 *
 * Not model-specific. Every future plugin consumer that needs ORT
 * (Chatterbox Turbo TTS backbone + codec decoder, future image
 * encoders, vision classifiers, whatever) constructs one of these
 * and drives it. The session knows nothing about tokenizers, audio
 * streaming, generation loops, or any higher-level concept — those
 * belong in per-model consumer layers.
 *
 * Ownership:
 *   Move-only, heap-allocated via TUniquePtr. Use the static Create()
 *   factories to obtain an instance. Destructor releases the
 *   underlying OrtSession + OrtSessionOptions cleanly.
 *
 * Thread-safety:
 *   Run() is safe to call from multiple threads concurrently on the
 *   same session (ORT guarantees this for all reasonable session
 *   configurations). The metadata accessors (GetInput*, GetOutput*)
 *   read from an immutable cache populated at Create time — also
 *   concurrent-read-safe.
 *
 * Ordering conventions:
 *   Run(Inputs, Outputs) takes inputs in the order the model declares
 *   them (GetInputName(i) for i in [0, GetInputCount())). Outputs
 *   come back in the same declared order. If you need name-based
 *   mapping instead, do it at the caller — a trivial few-line
 *   wrapper. The positional API is the hot path; adding a hash
 *   lookup per input on the AR token loop would cost measurable
 *   latency.
 */
class INOONNX_API FInoOnnxSession
{
public:
    // ========================================================================
    //  Construction
    // ========================================================================

    /**
     * Load an ONNX model from the given filesystem path.
     *
     * Returns a valid TUniquePtr on success, nullptr on failure. On
     * failure, *OutError (if non-null) gets the diagnostic from ORT,
     * and a detailed log line is written to LogInoOnnx.
     *
     * ModelPath is a UE-style path (forward or backward slashes, both
     * work). Expected to be an absolute path to a .onnx file; on
     * Android the path typically comes from FPaths::ProjectPersistentDownloadDir()
     * since .onnx models download at runtime there.
     *
     * Typical causes of failure:
     *   - File does not exist
     *   - File is not a valid ONNX model
     *   - Requested execution provider is not registered in the
     *     current ORT runtime (logged as WARNING and fallback to
     *     next; session creation still succeeds if at least one
     *     provider registers; the CPU EP is always present)
     *   - Model uses operators the ORT build does not support
     *     (upgrade the ORT pin in OnnxRuntime/ONNXRUNTIME_VERSION)
     */
    static TUniquePtr<FInoOnnxSession> Create(
        const FString& ModelPath,
        const FInoOnnxSessionOptions& Options = {},
        FString* OutError = nullptr);

    /**
     * Load an ONNX model from a byte buffer already in memory. The
     * buffer does not need to outlive the call — ORT copies its
     * internal graph representation out during session construction.
     *
     * Useful for models downloaded into memory (e.g. via FHttpRequest
     * and kept as TArray<uint8>) when you want to avoid a round-trip
     * through disk.
     */
    static TUniquePtr<FInoOnnxSession> CreateFromMemory(
        TArrayView<const uint8> ModelBytes,
        const FInoOnnxSessionOptions& Options = {},
        FString* OutError = nullptr);

    ~FInoOnnxSession();

    FInoOnnxSession(const FInoOnnxSession&) = delete;
    FInoOnnxSession& operator=(const FInoOnnxSession&) = delete;
    FInoOnnxSession(FInoOnnxSession&&) = delete;             // heap-only via TUniquePtr
    FInoOnnxSession& operator=(FInoOnnxSession&&) = delete;

    // ========================================================================
    //  Metadata (cached at Create; immutable afterward)
    // ========================================================================

    int32 GetInputCount() const  { return InputMeta.Num(); }
    int32 GetOutputCount() const { return OutputMeta.Num(); }

    /** Input / output name as declared by the model graph. */
    FString GetInputName(int32 Index) const;
    FString GetOutputName(int32 Index) const;

    /**
     * Declared input/output shape. Entries of -1 mean a dynamic
     * dimension (e.g. batch size, sequence length) — must be bound
     * to a concrete value at inference time by the shape of the
     * corresponding input tensor.
     */
    TArray<int64> GetInputShape(int32 Index) const;
    TArray<int64> GetOutputShape(int32 Index) const;

    EInoOnnxDtype GetInputDtype(int32 Index) const;
    EInoOnnxDtype GetOutputDtype(int32 Index) const;

    /** Providers that successfully registered at Create time, in the
     *  order they were attempted. A provider requested by the caller
     *  that failed to register (e.g. NNAPI on non-Android) is NOT in
     *  this list. Useful for diagnostics; safe to ignore. */
    const TArray<EInoOnnxProvider>& GetActiveProviders() const { return ActiveProviders; }

    /** Human-readable dump of the session's I/O metadata + active
     *  providers. Writes to LogInoOnnx at Log level. Useful for
     *  smoke tests and bug reports. */
    void LogMetadata() const;

    // ========================================================================
    //  Inference
    // ========================================================================

    /**
     * Run inference synchronously. Blocks the calling thread until
     * ORT returns. Safe for off-game-thread use (in fact preferred —
     * inference can take tens of milliseconds at the fast end).
     *
     * Inputs: positional. Must have GetInputCount() entries. Each
     *   entry's dtype/shape must match the model's corresponding
     *   input signature (with -1 dims bound to the tensor's
     *   concrete size). Validation is done by ORT; an invalid shape
     *   yields a false return + a descriptive error in *OutError.
     *
     * OutOutputs: appended to. Existing contents are NOT cleared
     *   (caller may want to reuse the array across multiple Run
     *   calls by Reset-ing first, or not). On success, Num() inputs
     *   added equals GetOutputCount().
     *
     * OutError: if non-null, populated with ORT's error string on
     *   failure. Pass nullptr if you don't care; the error is always
     *   logged to LogInoOnnx regardless.
     *
     * Returns true on success, false on any failure (log + OutError
     * carry the diagnostic).
     */
    bool Run(
        TArrayView<const FInoOnnxTensor> Inputs,
        TArray<FInoOnnxTensor>& OutOutputs,
        FString* OutError = nullptr);

    /**
     * Run a single discardable inference to pay the JIT / kernel-
     * selection / memory-pattern setup costs upfront. The first Run()
     * after Create() is typically 2-10x slower than steady state on
     * any non-trivial graph; calling Warmup once at session-load time
     * moves that cost off the user-visible inference hot path.
     *
     * Pass dummy inputs that match the model's input signature —
     * typically minimum-size zero tensors. The caller knows what
     * "small but valid" looks like for their model. Outputs are
     * discarded internally; only the timing log on success is
     * useful externally.
     *
     * Failure is non-fatal: the first real Run() will simply pay the
     * setup cost itself. Returns false (with *OutError populated if
     * non-null) so the caller can choose to log or surface.
     */
    bool Warmup(
        TArrayView<const FInoOnnxTensor> DummyInputs,
        FString* OutError = nullptr);

    /**
     * Run inference asynchronously on the UE thread pool. Completion
     * callback fires on the GAME THREAD via AsyncTask(GameThread).
     *
     * Inputs are moved into the worker (invalidating the caller's
     * copies) so the async boundary can't be broken by the caller
     * freeing tensors mid-inference.
     *
     * OnComplete signature: (TArray<FInoOnnxTensor> Outputs, FString Error).
     * On success Outputs contains GetOutputCount() tensors, Error is
     * empty. On failure Outputs is empty and Error is non-empty.
     *
     * Thread safety: it is safe to have multiple concurrent RunAsync
     * calls in flight on the same session. ORT handles the serialization.
     */
    void RunAsync(
        TArray<FInoOnnxTensor>&& Inputs,
        TFunction<void(TArray<FInoOnnxTensor> Outputs, FString Error)> OnComplete);

private:
    FInoOnnxSession() = default;

    /** Per-I/O cached metadata. Populated once in Create; immutable.
     *  Nested-private so callers can't see ORT-flavored internals. */
    struct FIOMeta
    {
        FString       Name;
        TArray<int64> Shape;        ///< may contain -1 for dynamic dims
        EInoOnnxDtype Dtype = EInoOnnxDtype::Undefined;

        /**
         * UTF-8 (null-terminated) representation of Name, kept alive for
         * the session's lifetime. Run() reads from here instead of
         * re-converting from FString every call. ORT's Run takes
         * `const char**` for names, so caching as a stable const char*
         * is the natural representation.
         */
        TArray<ANSICHAR> NameUtf8;
    };

    // Implementation lives in the .cpp; this header deliberately
    // doesn't expose OrtSession* directly.
    OrtSession*        NativeSession = nullptr;
    OrtSessionOptions* NativeOptions = nullptr;

    TArray<FIOMeta>            InputMeta;
    TArray<FIOMeta>            OutputMeta;
    TArray<EInoOnnxProvider>   ActiveProviders;

    /**
     * Pre-cached `const char*` arrays into the FIOMeta::NameUtf8 buffers.
     * Populated once in FinishConstruction; passed straight to OrtApi::Run
     * so each inference avoids the FString -> UTF-8 conversion + tiny
     * TArray allocs the per-call code used to do.
     */
    TArray<const char*> InputNamePtrs;
    TArray<const char*> OutputNamePtrs;

    // Shared impl used by both Create overloads — takes a pre-allocated
    // session ptr and populates everything else. Returns false + error
    // on metadata-cache failure (can't happen if the session is valid,
    // but check anyway).
    bool FinishConstruction(
        const FInoOnnxSessionOptions& Options,
        TArray<EInoOnnxProvider> RegisteredProviders,
        FString* OutError);

    /**
     * Populate a single FIOMeta entry at Index from the session's
     * declared inputs (bInput=true) or outputs (bInput=false). Static
     * because it operates only on the raw OrtSession + OrtApi; doesn't
     * need class state. Declared as a private member instead of a free
     * function so it can construct/access the private FIOMeta nested
     * type. Returns false on ORT API error (logs + optionally sets
     * *OutError is handled by CheckOrtStatus inside).
     */
    static bool ReadIOMeta(
        const struct OrtApi* Api,
        OrtSession* Session,
        bool bInput,
        size_t Index,
        TArray<FIOMeta>& OutArr);
};

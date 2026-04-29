// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Onnx/InoOnnxTypes.h"

// Forward declare so public consumers don't need onnxruntime_c_api.h.
// Real definition comes in via the private impl.
struct OrtValue;

/**
 * Move-only wrapper around ORT's OrtValue for TENSOR types.
 *
 * Holds three things:
 *   - OrtValue* (ORT's opaque handle to the tensor buffer + shape)
 *   - Shape    (cached TArray<int64> so callers can inspect without an
 *               ORT API round-trip on every access)
 *   - Dtype    (cached EInoOnnxDtype)
 *
 * Ownership semantics:
 *   Move-only. Moving transfers the OrtValue*; the moved-from tensor
 *   is left in an IsValid() == false state. No implicit copy — copying
 *   would require a deep tensor clone which we don't want to do
 *   implicitly (surprise cost on the LLM streaming path).
 *
 * Thread-safety:
 *   Not safe for concurrent writes to the same tensor. Reads (GetData,
 *   CopyToArray) after construction are thread-safe for concurrent
 *   readers. Typical usage is single-threaded: one thread constructs,
 *   fills, passes to Session::Run, and consumes the outputs.
 *
 * Integration with non-tensor ORT values (maps, sequences, sparse
 * tensors) is deliberately not covered here — every consumer we've
 * designed for deals only in dense tensors. Add a sibling
 * FInoOnnxMap / FInoOnnxSequence if a consumer ever needs one.
 *
 * Typical usage patterns:
 *
 *   // 1. Zero-allocate output (Session::Run fills these)
 *   FInoOnnxTensor Out;  // default-constructed, IsValid() == false
 *   Session->Run({ Input }, OutOutputs);  // Session::Run adopts outputs
 *
 *   // 2. Input from typed array (copy into owned buffer)
 *   TArray<float> Samples = ...;
 *   FInoOnnxTensor Audio = FInoOnnxTensor::CreateFromBufferCopy<float>(
 *       {1, NumSamples},         // shape: 1 x N
 *       Samples);                // copied into OrtValue-owned buffer
 *
 *   // 3. Mutate in place before passing to Run
 *   FInoOnnxTensor Tokens = FInoOnnxTensor::Create(EInoOnnxDtype::Int64,
 *                                                  {1, SeqLen});
 *   int64* Data = Tokens.GetMutableData<int64>();
 *   for (int i = 0; i < SeqLen; ++i) { Data[i] = TokenIds[i]; }
 *
 *   // 4. Read typed outputs
 *   const float* Logits = Output.GetData<float>();
 *   TArray<float> LogitsArray;
 *   Output.CopyToArray<float>(LogitsArray);
 */
class INOONNX_API FInoOnnxTensor
{
public:
    /** Default-constructed invalid tensor (owns no OrtValue). Used as
     *  a placeholder; fill by moving a real tensor into it. */
    FInoOnnxTensor() = default;

    /** Destructor releases the OrtValue via OrtApi::ReleaseValue. Safe
     *  to call on a moved-from or default-constructed tensor. */
    ~FInoOnnxTensor();

    // Move-only. Copying a tensor would require a deep clone which we
    // don't offer implicitly (use CloneIntoNewTensor if needed later).
    FInoOnnxTensor(FInoOnnxTensor&& Other) noexcept;
    FInoOnnxTensor& operator=(FInoOnnxTensor&& Other) noexcept;
    FInoOnnxTensor(const FInoOnnxTensor&) = delete;
    FInoOnnxTensor& operator=(const FInoOnnxTensor&) = delete;

    // ========================================================================
    //  Factories — allocate a new owned tensor
    // ========================================================================

    /**
     * Allocate a zero-initialized tensor of the given dtype and shape.
     * Returns an invalid tensor on error (logs to LogInoOnnx).
     *
     * Shape values must all be >= 0. Use IsValid() to check success.
     */
    static FInoOnnxTensor Create(EInoOnnxDtype Dtype, const TArray<int64>& Shape);

    /** Convenience: Create(EInoOnnxDtype::Float32, Shape). */
    static FInoOnnxTensor CreateFloat32(const TArray<int64>& Shape);

    /** Convenience: Create(EInoOnnxDtype::Int64, Shape). */
    static FInoOnnxTensor CreateInt64(const TArray<int64>& Shape);

    /**
     * Create a tensor and copy the given data into the newly-allocated
     * buffer. Shape total element count must match Data.Num(); otherwise
     * returns an invalid tensor and logs a warning.
     *
     * Dtype is inferred from the T type:
     *   float    -> Float32
     *   int8_t   -> Int8
     *   uint8_t  -> UInt8
     *   int16_t  -> Int16
     *   uint16_t -> UInt16
     *   int32_t  -> Int32
     *   uint32_t -> UInt32
     *   int64_t  -> Int64
     *   uint64_t -> UInt64
     *   bool     -> Bool
     *
     * Float16 tensors need manual construction (use Create() +
     * GetMutableData<uint16>() and interpret as half-precision bits).
     */
    template<typename T>
    static FInoOnnxTensor CreateFromBufferCopy(
        const TArray<int64>& Shape,
        TArrayView<const T> Data);

    // ========================================================================
    //  Adopt — wrap an already-constructed OrtValue (advanced)
    // ========================================================================

    /**
     * Adopt ownership of an already-constructed OrtValue. Used by
     * FInoOnnxSession::Run to wrap output tensors. The caller transfers
     * ownership — do not release OrtValue* after calling this.
     *
     * Shape and Dtype are not queried from the OrtValue (which would
     * require an ORT round-trip) — pass them if already known. If
     * empty / Undefined, call RefreshShapeAndDtype to populate from
     * the underlying OrtValue.
     */
    static FInoOnnxTensor Adopt(
        OrtValue* Native,
        EInoOnnxDtype Dtype = EInoOnnxDtype::Undefined,
        TArray<int64> Shape = {});

    /** Query the OrtValue to populate Shape and Dtype fields. Usually
     *  called after Adopt() when the caller didn't have shape/dtype
     *  info at hand. Returns false and logs if the query fails. */
    bool RefreshShapeAndDtype();

    // ========================================================================
    //  Metadata
    // ========================================================================

    bool IsValid() const { return NativeValue != nullptr; }

    EInoOnnxDtype GetDtype() const { return Dtype; }

    /** Dimensions; -1 entries possible only for outputs that haven't
     *  been computed yet (rare for tensors adopted from Session::Run). */
    const TArray<int64>& GetShape() const { return Shape; }

    /** Total number of scalar elements (product of Shape dimensions). */
    int64 GetElementCount() const;

    /** Total byte size of the underlying buffer (ElementCount * element size). */
    SIZE_T GetByteSize() const;

    // ========================================================================
    //  Data access (const and mutable)
    // ========================================================================

    /**
     * Typed read-only access to the raw data buffer. Returns nullptr if
     * the tensor is invalid or the template T size doesn't match the
     * tensor's dtype (a warning is logged in that case — treat it as a
     * programming error).
     *
     * Valid T choices:
     *   float / uint16 (for Float16 raw bits) / int8 / uint8 / int16 /
     *   uint16 / int32 / uint32 / int64 / uint64 / bool
     *
     * Pointer lifetime: valid until this FInoOnnxTensor is moved from
     * or destroyed. Do not store across async boundaries unless you
     * keep the tensor alive.
     */
    template<typename T> const T* GetData() const;

    /** Mutable variant of GetData. Same lifetime rules, plus: writes
     *  must complete before any concurrent Session::Run that reads this
     *  tensor. */
    template<typename T> T* GetMutableData();

    /** Copy the entire buffer into a UE TArray of the same element
     *  type. Reallocates Out. Dtype/T mismatch logs and leaves Out
     *  empty. Useful for feeding into Blueprint-visible wrappers. */
    template<typename T> void CopyToArray(TArray<T>& Out) const;

    // ========================================================================
    //  Interop (for code that needs to talk to ORT directly)
    // ========================================================================

    /** Raw OrtValue pointer. Do not release; lifetime tied to this
     *  FInoOnnxTensor. Pass to ORT APIs that need one, e.g.
     *  OrtApi::Run input/output arrays (FInoOnnxSession handles this
     *  internally; direct callers should rarely need it). */
    OrtValue* GetNativeHandle() const { return NativeValue; }

    /**
     * Release ownership of the OrtValue. Returns the native pointer to
     * the caller (who becomes responsible for OrtApi::ReleaseValue).
     * After calling, IsValid() == false. Used by Session::Run internals
     * to hand ownership back to ORT.
     */
    OrtValue* ReleaseNative();

private:
    OrtValue*     NativeValue = nullptr;
    EInoOnnxDtype Dtype       = EInoOnnxDtype::Undefined;
    TArray<int64> Shape;

    // Private ctor used by factories + Adopt. Takes ownership of Native.
    FInoOnnxTensor(OrtValue* Native, EInoOnnxDtype InDtype, TArray<int64>&& InShape);

    // Shared impl used by GetData<T> / GetMutableData<T> to look up the
    // tensor's raw data pointer via the ORT API. Returns nullptr on
    // error (logs the reason).
    void* GetDataPtrRaw(SIZE_T TypeSizeCheck, const TCHAR* TypeName) const;
};

// ============================================================================
//  Template definitions
// ============================================================================

namespace InoOnnx::Detail
{
    // Compile-time dtype deduction from a C++ scalar type. Specializations
    // below cover every plausible match — anything unspecialized fails the
    // static_assert in the caller.
    template<typename T> struct TDtypeOf { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Undefined; };

    template<> struct TDtypeOf<float>    { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Float32; };
    template<> struct TDtypeOf<int8>     { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Int8;    };
    template<> struct TDtypeOf<uint8>    { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::UInt8;   };
    template<> struct TDtypeOf<int16>    { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Int16;   };
    template<> struct TDtypeOf<uint16>   { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::UInt16;  };
    template<> struct TDtypeOf<int32>    { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Int32;   };
    template<> struct TDtypeOf<uint32>   { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::UInt32;  };
    template<> struct TDtypeOf<int64>    { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Int64;   };
    template<> struct TDtypeOf<uint64>   { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::UInt64;  };
    template<> struct TDtypeOf<bool>     { static constexpr EInoOnnxDtype Value = EInoOnnxDtype::Bool;    };
}

template<typename T>
inline FInoOnnxTensor FInoOnnxTensor::CreateFromBufferCopy(
    const TArray<int64>& Shape,
    TArrayView<const T> Data)
{
    using namespace InoOnnx::Detail;
    static_assert(TDtypeOf<T>::Value != EInoOnnxDtype::Undefined,
                  "FInoOnnxTensor::CreateFromBufferCopy: unsupported element type T. "
                  "Supported: float, int8/16/32/64, uint8/16/32/64, bool. "
                  "For Float16 use Create() + GetMutableData<uint16>().");

    FInoOnnxTensor Tensor = Create(TDtypeOf<T>::Value, Shape);
    if (!Tensor.IsValid())
    {
        return Tensor; // invalid; error already logged by Create
    }

    // Shape product must match Data count, else Create would have
    // returned a mismatched buffer. Sanity-check here.
    const int64 ExpectedCount = Tensor.GetElementCount();
    if (ExpectedCount != (int64)Data.Num())
    {
        // Return invalid; leave Tensor intact for error logging
        UE_LOG(LogTemp, Warning,
               TEXT("FInoOnnxTensor::CreateFromBufferCopy: shape product %lld != Data.Num() %d"),
               ExpectedCount, Data.Num());
        return FInoOnnxTensor{};
    }

    if (T* Dst = Tensor.GetMutableData<T>())
    {
        FMemory::Memcpy(Dst, Data.GetData(), (SIZE_T)Data.Num() * sizeof(T));
    }
    return Tensor;
}

template<typename T>
inline const T* FInoOnnxTensor::GetData() const
{
    using namespace InoOnnx::Detail;
    static_assert(TDtypeOf<T>::Value != EInoOnnxDtype::Undefined,
                  "FInoOnnxTensor::GetData<T>: unsupported T.");
    // Hardcoded log name instead of TEXT(__FUNCTION__): on clang the TEXT
    // macro expands to a token-paste (u ## __FUNCTION__) that fails to
    // resolve because __FUNCTION__ is not a string literal at preprocess
    // time. MSVC is more forgiving. Using a literal avoids the whole
    // preprocessor mess — we lose the per-T<> name suffix in log output
    // but that was only a diagnostic convenience.
    return static_cast<const T*>(GetDataPtrRaw(sizeof(T), TEXT("FInoOnnxTensor::GetData")));
}

template<typename T>
inline T* FInoOnnxTensor::GetMutableData()
{
    using namespace InoOnnx::Detail;
    static_assert(TDtypeOf<T>::Value != EInoOnnxDtype::Undefined,
                  "FInoOnnxTensor::GetMutableData<T>: unsupported T.");
    return static_cast<T*>(const_cast<void*>(GetDataPtrRaw(sizeof(T), TEXT("FInoOnnxTensor::GetMutableData"))));
}

template<typename T>
inline void FInoOnnxTensor::CopyToArray(TArray<T>& Out) const
{
    Out.Reset();
    if (!IsValid())
    {
        return;
    }
    const T* Src = GetData<T>();
    if (Src == nullptr)
    {
        return;
    }
    const int64 Count = GetElementCount();
    Out.SetNumUninitialized((int32)Count);
    FMemory::Memcpy(Out.GetData(), Src, (SIZE_T)Count * sizeof(T));
}

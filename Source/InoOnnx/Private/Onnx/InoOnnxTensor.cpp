// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "Onnx/InoOnnxTensor.h"

#include "InoOnnxInternal.h"
#include "InoOnnx.h"
#include "InoOnnx.h"  // for LogInoOnnx

namespace
{
    /**
     * One-liner for every ORT call that might fail inside this TU.
     * Wraps CheckOrtStatus so we don't repeat the OpDescription boilerplate.
     */
    bool CheckTensorStatus(OrtStatus* Status, const TCHAR* Op)
    {
        return InoOnnx::Internal::CheckOrtStatus(Status, Op, /*OutError=*/ nullptr);
    }

    /** Default CPU allocator. ORT owns it; never release. nullptr if
     *  the ORT API isn't ready. */
    OrtAllocator* GetCpuAllocator()
    {
        const OrtApi* Api = InoOnnx::GetApi();
        if (Api == nullptr)
        {
            return nullptr;
        }

        OrtAllocator* Allocator = nullptr;
        OrtStatus* Status = Api->GetAllocatorWithDefaultOptions(&Allocator);
        if (!CheckTensorStatus(Status, TEXT("GetAllocatorWithDefaultOptions")))
        {
            return nullptr;
        }
        return Allocator;
    }

    /** MemoryInfo describing CPU memory. ORT owns it; release via
     *  ReleaseMemoryInfo when done with it. Used when building tensors
     *  that wrap caller-provided buffers. Not used in this file — we
     *  always allocate through OrtAllocator — but kept commented here
     *  as a reference for future zero-copy wrappers. */
}

// ============================================================================
//  FInoOnnxTensor
// ============================================================================

FInoOnnxTensor::FInoOnnxTensor(OrtValue* Native, EInoOnnxDtype InDtype, TArray<int64>&& InShape)
    : NativeValue(Native)
    , Dtype(InDtype)
    , Shape(MoveTemp(InShape))
{
}

FInoOnnxTensor::~FInoOnnxTensor()
{
    if (NativeValue != nullptr)
    {
        const OrtApi* Api = InoOnnx::GetApi();
        if (Api != nullptr)
        {
            Api->ReleaseValue(NativeValue);
        }
        // else: Api torn down before tensor; leak is unavoidable and
        //       one-time at process exit — not worth logging.
        NativeValue = nullptr;
    }
}

FInoOnnxTensor::FInoOnnxTensor(FInoOnnxTensor&& Other) noexcept
    : NativeValue(Other.NativeValue)
    , Dtype(Other.Dtype)
    , Shape(MoveTemp(Other.Shape))
{
    Other.NativeValue = nullptr;
    Other.Dtype       = EInoOnnxDtype::Undefined;
    // Other.Shape is already in the moved-from state from the MoveTemp above.
}

FInoOnnxTensor& FInoOnnxTensor::operator=(FInoOnnxTensor&& Other) noexcept
{
    if (this != &Other)
    {
        // Release our current value (if any).
        if (NativeValue != nullptr)
        {
            const OrtApi* Api = InoOnnx::GetApi();
            if (Api != nullptr)
            {
                Api->ReleaseValue(NativeValue);
            }
        }

        NativeValue = Other.NativeValue;
        Dtype       = Other.Dtype;
        Shape       = MoveTemp(Other.Shape);

        Other.NativeValue = nullptr;
        Other.Dtype       = EInoOnnxDtype::Undefined;
    }
    return *this;
}

// ============================================================================
//  Factories
// ============================================================================

FInoOnnxTensor FInoOnnxTensor::Create(EInoOnnxDtype Dtype, const TArray<int64>& Shape)
{
    using namespace InoOnnx::Internal;

    if (Dtype == EInoOnnxDtype::Undefined)
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Tensor: Create FAILED — Dtype == Undefined; returning invalid tensor"));
        return FInoOnnxTensor{};
    }

    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Tensor: Create FAILED — ONNX Runtime is not initialized"));
        return FInoOnnxTensor{};
    }

    // All dims must be >= 0. ORT accepts -1 (dynamic) only for model
    // inputs/outputs, not for concrete tensor allocation.
    for (int32 i = 0; i < Shape.Num(); ++i)
    {
        if (Shape[i] < 0)
        {
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Onnx: Tensor: Create FAILED — negative dim at index %d (value %lld); ")
                   TEXT("concrete tensors require all-positive shapes"),
                   i, Shape[i]);
            return FInoOnnxTensor{};
        }
    }

    OrtAllocator* Allocator = GetCpuAllocator();
    if (Allocator == nullptr)
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Tensor: Create FAILED — CPU allocator is null"));
        return FInoOnnxTensor{};
    }

    {
        FString ShapeStr = TEXT("[");
        for (int32 i = 0; i < Shape.Num(); ++i)
        {
            if (i > 0) ShapeStr += TEXT(", ");
            ShapeStr += FString::Printf(TEXT("%lld"), Shape[i]);
        }
        ShapeStr += TEXT("]");
        UE_LOG(LogInoOnnx, Verbose,
               TEXT("Onnx: Tensor: Create — dtype=%d shape=%s"),
               (int32)Dtype, *ShapeStr);
    }

    OrtValue* Native = nullptr;
    // UE's int64 is always `long long`; Android's int64_t is `long`. Same
    // bit-width, different types, so direct pointer passing fails on clang.
    // reinterpret_cast is safe — both are 64-bit signed integers with the
    // same representation per the LP64/LLP64 ABIs we target.
    const int64_t* ShapeData = reinterpret_cast<const int64_t*>(Shape.GetData());
    const size_t ShapeCount = (size_t)Shape.Num();

    OrtStatus* Status = Api->CreateTensorAsOrtValue(
        Allocator,
        ShapeData,
        ShapeCount,
        DtypeToOrt(Dtype),
        &Native);

    if (!CheckTensorStatus(Status, TEXT("CreateTensorAsOrtValue")))
    {
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Tensor: CreateTensorAsOrtValue FAILED (dtype=%d)"),
               (int32)Dtype);
        return FInoOnnxTensor{};
    }

    return FInoOnnxTensor(Native, Dtype, TArray<int64>(Shape));
}

FInoOnnxTensor FInoOnnxTensor::CreateFloat32(const TArray<int64>& Shape)
{
    return Create(EInoOnnxDtype::Float32, Shape);
}

FInoOnnxTensor FInoOnnxTensor::CreateInt64(const TArray<int64>& Shape)
{
    return Create(EInoOnnxDtype::Int64, Shape);
}

FInoOnnxTensor FInoOnnxTensor::Adopt(OrtValue* Native, EInoOnnxDtype InDtype, TArray<int64> InShape)
{
    FInoOnnxTensor Tensor(Native, InDtype, MoveTemp(InShape));

    // If caller didn't provide dtype/shape, query the OrtValue.
    if (Tensor.NativeValue != nullptr &&
        (InDtype == EInoOnnxDtype::Undefined || Tensor.Shape.Num() == 0))
    {
        Tensor.RefreshShapeAndDtype();
    }

    return Tensor;
}

bool FInoOnnxTensor::RefreshShapeAndDtype()
{
    using namespace InoOnnx::Internal;

    if (NativeValue == nullptr)
    {
        return false;
    }

    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        return false;
    }

    // Get a TypeAndShapeInfo handle — ORT needs it to read dtype + shape.
    OrtTensorTypeAndShapeInfo* TypeInfo = nullptr;
    if (!CheckTensorStatus(
            Api->GetTensorTypeAndShape(NativeValue, &TypeInfo),
            TEXT("GetTensorTypeAndShape")))
    {
        return false;
    }

    // Query element dtype.
    ONNXTensorElementDataType OrtDtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    bool bOk = CheckTensorStatus(
        Api->GetTensorElementType(TypeInfo, &OrtDtype),
        TEXT("GetTensorElementType"));

    // Query number of dimensions.
    size_t DimCount = 0;
    if (bOk)
    {
        bOk = CheckTensorStatus(
            Api->GetDimensionsCount(TypeInfo, &DimCount),
            TEXT("GetDimensionsCount"));
    }

    // Query the dims themselves.
    if (bOk && DimCount > 0)
    {
        Shape.SetNumUninitialized((int32)DimCount);
        // reinterpret_cast: UE's int64 (long long) vs ORT's int64_t (long
        // on Android). Same representation, different C++ types.
        bOk = CheckTensorStatus(
            Api->GetDimensions(TypeInfo, reinterpret_cast<int64_t*>(Shape.GetData()), DimCount),
            TEXT("GetDimensions"));
    }
    else
    {
        Shape.Reset();
    }

    Api->ReleaseTensorTypeAndShapeInfo(TypeInfo);

    if (bOk)
    {
        Dtype = OrtToDtype(OrtDtype);
    }
    return bOk;
}

// ============================================================================
//  Metadata
// ============================================================================

int64 FInoOnnxTensor::GetElementCount() const
{
    int64 Count = 1;
    for (const int64 Dim : Shape)
    {
        if (Dim < 0)
        {
            // Dynamic dim — element count not computable until bound.
            return 0;
        }
        Count *= Dim;
    }
    return Count;
}

SIZE_T FInoOnnxTensor::GetByteSize() const
{
    using namespace InoOnnx::Internal;
    const int64 Count = GetElementCount();
    const SIZE_T ElemSize = DtypeElementSize(Dtype);
    return (SIZE_T)Count * ElemSize;
}

// ============================================================================
//  Data access
// ============================================================================

void* FInoOnnxTensor::GetDataPtrRaw(SIZE_T TypeSizeCheck, const TCHAR* TypeName) const
{
    using namespace InoOnnx::Internal;

    if (NativeValue == nullptr)
    {
        return nullptr;
    }

    const OrtApi* Api = InoOnnx::GetApi();
    if (Api == nullptr)
    {
        return nullptr;
    }

    const SIZE_T ElemSize = DtypeElementSize(Dtype);
    if (ElemSize != TypeSizeCheck)
    {
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Tensor: %s — template T size (%llu) does not match tensor dtype element size (%llu) ")
               TEXT("(tensor dtype=%d); returning nullptr"),
               TypeName, (uint64)TypeSizeCheck, (uint64)ElemSize, (int32)Dtype);
        return nullptr;
    }

    void* Data = nullptr;
    if (!CheckTensorStatus(
            Api->GetTensorMutableData(NativeValue, &Data),
            TEXT("GetTensorMutableData")))
    {
        return nullptr;
    }
    return Data;
}

OrtValue* FInoOnnxTensor::ReleaseNative()
{
    OrtValue* Released = NativeValue;
    NativeValue = nullptr;
    Dtype       = EInoOnnxDtype::Undefined;
    Shape.Reset();
    return Released;
}

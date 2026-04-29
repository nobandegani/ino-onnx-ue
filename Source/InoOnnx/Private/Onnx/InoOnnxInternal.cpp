// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "InoOnnxInternal.h"
#include "InoOnnx.h"              // for InoOnnx::GetApi() (from sibling InoOnnx plugin)
#include "InoOnnx.h"  // for LogInoOnnx

#include "HAL/CriticalSection.h"

namespace InoOnnx::Internal
{

// ============================================================================
//  Status check
// ============================================================================

bool CheckOrtStatus(OrtStatus* Status, const TCHAR* OpDescription, FString* OutError)
{
    if (Status == nullptr)
    {
        return true;
    }

    const OrtApi* Api = GetApi();
    if (Api == nullptr)
    {
        // Shouldn't happen — if we got an OrtStatus we must have a valid
        // Api pointer. But be defensive: if ORT was torn down between
        // the producing call and here, we can't release the status and
        // can't get the error message. Log and leak (one-time, on
        // teardown path).
        UE_LOG(LogInoOnnx, Error,
               TEXT("Onnx: Internal: %s FAILED but GetApi() is null; cannot extract / release error status."),
               OpDescription ? OpDescription : TEXT("<unknown op>"));
        return false;
    }

    const char* RawMsg = Api->GetErrorMessage(Status);
    FString ErrorStr = RawMsg ? FString(UTF8_TO_TCHAR(RawMsg)) : FString(TEXT("<null error message>"));

    if (OutError != nullptr)
    {
        *OutError = ErrorStr;
    }

    UE_LOG(LogInoOnnx, Error,
           TEXT("Onnx: Internal: %s FAILED: %s"),
           OpDescription ? OpDescription : TEXT("<unknown op>"),
           *ErrorStr);

    Api->ReleaseStatus(Status);
    return false;
}

// ============================================================================
//  Dtype conversions
// ============================================================================

ONNXTensorElementDataType DtypeToOrt(EInoOnnxDtype Dtype)
{
    switch (Dtype)
    {
        case EInoOnnxDtype::Float32:  return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        case EInoOnnxDtype::Float16:  return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        case EInoOnnxDtype::Int8:     return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8;
        case EInoOnnxDtype::UInt8:    return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
        case EInoOnnxDtype::Int16:    return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16;
        case EInoOnnxDtype::UInt16:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16;
        case EInoOnnxDtype::Int32:    return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32;
        case EInoOnnxDtype::UInt32:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32;
        case EInoOnnxDtype::Int64:    return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
        case EInoOnnxDtype::UInt64:   return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64;
        case EInoOnnxDtype::Bool:     return ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
        case EInoOnnxDtype::Undefined:
        default:
            return ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    }
}

EInoOnnxDtype OrtToDtype(ONNXTensorElementDataType Ort)
{
    switch (Ort)
    {
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:    return EInoOnnxDtype::Float32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:  return EInoOnnxDtype::Float16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:     return EInoOnnxDtype::Int8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:    return EInoOnnxDtype::UInt8;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:    return EInoOnnxDtype::Int16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:   return EInoOnnxDtype::UInt16;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:    return EInoOnnxDtype::Int32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:   return EInoOnnxDtype::UInt32;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:    return EInoOnnxDtype::Int64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:   return EInoOnnxDtype::UInt64;
        case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:     return EInoOnnxDtype::Bool;
        default:
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Onnx: Internal: OrtToDtype unknown ONNXTensorElementDataType=%d; returning Undefined"),
                   (int32)Ort);
            return EInoOnnxDtype::Undefined;
    }
}

SIZE_T DtypeElementSize(EInoOnnxDtype Dtype)
{
    switch (Dtype)
    {
        case EInoOnnxDtype::Float32: return 4;
        case EInoOnnxDtype::Float16: return 2;
        case EInoOnnxDtype::Int8:    return 1;
        case EInoOnnxDtype::UInt8:   return 1;
        case EInoOnnxDtype::Int16:   return 2;
        case EInoOnnxDtype::UInt16:  return 2;
        case EInoOnnxDtype::Int32:   return 4;
        case EInoOnnxDtype::UInt32:  return 4;
        case EInoOnnxDtype::Int64:   return 8;
        case EInoOnnxDtype::UInt64:  return 8;
        case EInoOnnxDtype::Bool:    return 1;
        case EInoOnnxDtype::Undefined:
        default:
            return 0;
    }
}

// ============================================================================
//  Provider / graph-optimization enum translations
// ============================================================================

const TCHAR* ProviderToString(EInoOnnxProvider Provider)
{
    switch (Provider)
    {
        case EInoOnnxProvider::Cpu:       return TEXT("CPU");
        case EInoOnnxProvider::Xnnpack:   return TEXT("XNNPACK");
        case EInoOnnxProvider::Nnapi:     return TEXT("NNAPI");
        case EInoOnnxProvider::WebGpu:    return TEXT("WebGPU");
        case EInoOnnxProvider::DirectMl:  return TEXT("DirectML");
        case EInoOnnxProvider::Cuda:      return TEXT("CUDA");
        case EInoOnnxProvider::TensorRt:  return TEXT("TensorRT");
        default:                          return TEXT("Unknown");
    }
}

GraphOptimizationLevel OptLevelToOrt(EInoOnnxGraphOptimizationLevel Level)
{
    switch (Level)
    {
        case EInoOnnxGraphOptimizationLevel::Disabled: return ORT_DISABLE_ALL;
        case EInoOnnxGraphOptimizationLevel::Basic:    return ORT_ENABLE_BASIC;
        case EInoOnnxGraphOptimizationLevel::Extended: return ORT_ENABLE_EXTENDED;
        case EInoOnnxGraphOptimizationLevel::All:
        default:                                       return ORT_ENABLE_ALL;
    }
}

// ============================================================================
//  Global OrtEnv singleton
// ============================================================================

namespace
{
    // Protected by GOrtEnvLock. Access GOrtEnv only while holding.
    FCriticalSection GOrtEnvLock;
    OrtEnv*          GOrtEnv = nullptr;
}

OrtEnv* GetGlobalOrtEnv()
{
    FScopeLock Lock(&GOrtEnvLock);

    if (GOrtEnv != nullptr)
    {
        return GOrtEnv;
    }

    const OrtApi* Api = GetApi();
    if (Api == nullptr)
    {
        // ORT never initialized (e.g. the DLL failed to load). No point
        // trying to make an env without a vtable.
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Internal: GetGlobalOrtEnv called but OrtApi is null; returning nullptr"));
        return nullptr;
    }

    UE_LOG(LogInoOnnx, Verbose,
           TEXT("Onnx: Internal: creating global OrtEnv (log-level=WARNING, logid=\"InoOnnx\")"));

    // Log level: WARNING is the right default for production. ORT emits
    // a fair amount of INFO output at startup (graph-opt decisions,
    // provider registration, etc.) that's noise at runtime. Bump to
    // ORT_LOGGING_LEVEL_INFO transiently if you're debugging session
    // creation.
    OrtStatus* Status = Api->CreateEnv(
        ORT_LOGGING_LEVEL_WARNING,
        "InoOnnx",                   // logid prefix shown in ORT's own log output
        &GOrtEnv);

    if (!CheckOrtStatus(Status, TEXT("CreateEnv"), /*OutError=*/ nullptr))
    {
        GOrtEnv = nullptr;
        return nullptr;
    }

    UE_LOG(LogInoOnnx, Log, TEXT("Onnx: Internal: created global OrtEnv"));
    return GOrtEnv;
}

void ReleaseGlobalOrtEnv()
{
    FScopeLock Lock(&GOrtEnvLock);
    if (GOrtEnv == nullptr)
    {
        return;
    }

    const OrtApi* Api = GetApi();
    if (Api != nullptr)
    {
        Api->ReleaseEnv(GOrtEnv);
    }
    else
    {
        // Api was torn down before us — leak the env. In practice this
        // only happens if ShutdownModule order is pathological; the
        // process is about to exit anyway.
        UE_LOG(LogInoOnnx, Warning,
               TEXT("Onnx: Internal: OrtApi gone during ReleaseGlobalOrtEnv; leaking OrtEnv (process exiting)."));
    }

    GOrtEnv = nullptr;
    UE_LOG(LogInoOnnx, Log, TEXT("Onnx: Internal: released global OrtEnv"));
}

} // namespace InoOnnx::Internal

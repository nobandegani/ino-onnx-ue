// Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#include "InoOnnx.h"              // for InoOnnx::GetApi() + LogInoOnnx
#include "InoOnnxTypes.h"
#include "InoOnnxTensor.h"
#include "InoOnnxSession.h"

#include "onnxruntime_c_api.h"     // for OrtApi::GetAvailableProviders in ProvidersTest

/**
 * Console commands for validating the ONNX Runtime integration end-to-end.
 * Two commands:
 *
 *   Ino.Onnx.ProvidersTest
 *     No model required. Pulls the list of execution providers the
 *     runtime was compiled with and logs them. Doubles the check that
 *     already runs in FInoOnnxModule::StartupModule, useful if you
 *     want to re-run after hot-reload or verify GetApi() is still
 *     valid.
 *
 *   Ino.Onnx.SessionFromFileTest <abs-path-to-model.onnx>
 *     Loads an arbitrary ONNX model from the given absolute path,
 *     dumps its I/O metadata to the log via FInoOnnxSession::LogMetadata.
 *     If all inputs have concrete (non-negative) shapes, also
 *     synthesizes zero-filled input tensors and runs one forward
 *     pass to verify Run() works end-to-end. Outputs get their
 *     shape+dtype dumped too.
 *
 *     Intended for dropping in any ONNX model the user has on hand
 *     and confirming the plugin can load and run it without writing
 *     per-model code. Exercises FInoOnnxSession + FInoOnnxTensor +
 *     internal helpers + the full OrtSession lifecycle.
 */

namespace
{
    // =======================================================================
    //  Ino.Onnx.ProvidersTest — no args
    // =======================================================================

    void RunProvidersTest(const TArray<FString>& /*Args*/)
    {
        const OrtApi* Api = InoOnnx::GetApi();
        if (Api == nullptr)
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.ProvidersTest: ONNX Runtime is not initialized. ")
                   TEXT("Check FInoOnnxModule::StartupModule logs for the reason."));
            return;
        }

        char** ProvidersPtr = nullptr;
        int    NumProviders = 0;
        if (OrtStatus* Status = Api->GetAvailableProviders(&ProvidersPtr, &NumProviders))
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.ProvidersTest: GetAvailableProviders failed: %s"),
                   UTF8_TO_TCHAR(Api->GetErrorMessage(Status)));
            Api->ReleaseStatus(Status);
            return;
        }

        UE_LOG(LogInoOnnx, Log,
               TEXT("Ino.Onnx.ProvidersTest: %d provider%s available:"),
               NumProviders, NumProviders == 1 ? TEXT("") : TEXT("s"));
        for (int i = 0; i < NumProviders; ++i)
        {
            UE_LOG(LogInoOnnx, Log, TEXT("  [%d] %s"),
                   i, ProvidersPtr && ProvidersPtr[i] ? UTF8_TO_TCHAR(ProvidersPtr[i]) : TEXT("(null)"));
        }

        if (OrtStatus* RelStatus = Api->ReleaseAvailableProviders(ProvidersPtr, NumProviders))
        {
            UE_LOG(LogInoOnnx, Warning,
                   TEXT("Ino.Onnx.ProvidersTest: ReleaseAvailableProviders returned error (ignored): %s"),
                   UTF8_TO_TCHAR(Api->GetErrorMessage(RelStatus)));
            Api->ReleaseStatus(RelStatus);
        }
    }

    FAutoConsoleCommand GProvidersTestCmd(
        TEXT("Ino.Onnx.ProvidersTest"),
        TEXT("Log the list of ONNX Runtime execution providers available in the loaded runtime."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunProvidersTest));

    // =======================================================================
    //  Ino.Onnx.SessionFromFileTest <model.onnx>
    // =======================================================================

    /** True if every dim in Shape is >= 0 (so we can allocate a concrete
     *  tensor matching it). Dynamic dims (-1) or empty shapes mean we
     *  can't synthesize zero-fill inputs automatically. */
    bool IsShapeFullyConcrete(const TArray<int64>& Shape)
    {
        if (Shape.Num() == 0) return false;
        for (const int64 D : Shape)
        {
            if (D <= 0) return false;
        }
        return true;
    }

    /** Dump a tensor's post-inference shape to a readable string. */
    FString FormatShape(const TArray<int64>& Shape)
    {
        FString Out = TEXT("[");
        for (int32 i = 0; i < Shape.Num(); ++i)
        {
            if (i > 0) Out += TEXT(", ");
            Out += FString::Printf(TEXT("%lld"), Shape[i]);
        }
        Out += TEXT("]");
        return Out;
    }

    void RunSessionFromFileTest(const TArray<FString>& Args)
    {
        if (Args.Num() < 1)
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.SessionFromFileTest: expected one argument — absolute path to an .onnx model file."));
            return;
        }

        const FString ModelPath = Args[0];
        UE_LOG(LogInoOnnx, Log,
               TEXT("Ino.Onnx.SessionFromFileTest: loading %s"), *ModelPath);

        if (!IFileManager::Get().FileExists(*ModelPath))
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.SessionFromFileTest: file does not exist: %s"), *ModelPath);
            return;
        }

        // Default options — CPU provider, full graph optimization. Any
        // future provider-benchmark console command should construct its
        // own FInoOnnxSessionOptions with alternate ExecutionProviders.
        FInoOnnxSessionOptions Options;
        // Prefer platform-appropriate accelerators if the model supports
        // them; fall through to CPU either way.
#if PLATFORM_ANDROID
        Options.ExecutionProviders = {
            EInoOnnxProvider::Xnnpack,
            EInoOnnxProvider::Cpu,
        };
#else
        Options.ExecutionProviders = { EInoOnnxProvider::Cpu };
#endif

        FString Error;
        const double T0 = FPlatformTime::Seconds();
        TUniquePtr<FInoOnnxSession> Session = FInoOnnxSession::Create(ModelPath, Options, &Error);
        const double TLoad = FPlatformTime::Seconds() - T0;

        if (!Session.IsValid())
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.SessionFromFileTest: Create failed: %s"), *Error);
            return;
        }

        UE_LOG(LogInoOnnx, Log,
               TEXT("Ino.Onnx.SessionFromFileTest: loaded in %.3f s"), TLoad);
        Session->LogMetadata();

        // Check whether we can auto-synthesize inputs. Skip the Run
        // portion if any input has a dynamic dim — the caller needs a
        // per-model harness in that case.
        const int32 InCount = Session->GetInputCount();
        bool bCanSynthesize = InCount > 0;
        for (int32 i = 0; i < InCount && bCanSynthesize; ++i)
        {
            if (!IsShapeFullyConcrete(Session->GetInputShape(i)))
            {
                UE_LOG(LogInoOnnx, Warning,
                       TEXT("Ino.Onnx.SessionFromFileTest: input [%d] %s has dynamic shape %s; ")
                       TEXT("cannot auto-synthesize, skipping Run."),
                       i, *Session->GetInputName(i), *FormatShape(Session->GetInputShape(i)));
                bCanSynthesize = false;
            }
        }

        if (!bCanSynthesize)
        {
            UE_LOG(LogInoOnnx, Log,
                   TEXT("Ino.Onnx.SessionFromFileTest: metadata-only pass complete. ")
                   TEXT("(Model has dynamic inputs — write a per-model harness to exercise Run.)"));
            return;
        }

        // Build zero-initialized input tensors at the exact declared shapes.
        // FInoOnnxTensor::Create allocates CPU memory via OrtAllocator;
        // the underlying buffer is zero-filled.
        TArray<FInoOnnxTensor> Inputs;
        Inputs.Reserve(InCount);
        for (int32 i = 0; i < InCount; ++i)
        {
            FInoOnnxTensor T = FInoOnnxTensor::Create(
                Session->GetInputDtype(i), Session->GetInputShape(i));
            if (!T.IsValid())
            {
                UE_LOG(LogInoOnnx, Error,
                       TEXT("Ino.Onnx.SessionFromFileTest: failed to allocate input tensor [%d]"), i);
                return;
            }
            Inputs.Add(MoveTemp(T));
        }

        TArray<FInoOnnxTensor> Outputs;
        const double T1 = FPlatformTime::Seconds();
        const bool bOk = Session->Run(
            TArrayView<const FInoOnnxTensor>(Inputs.GetData(), Inputs.Num()),
            Outputs,
            &Error);
        const double TRun = FPlatformTime::Seconds() - T1;

        if (!bOk)
        {
            UE_LOG(LogInoOnnx, Error,
                   TEXT("Ino.Onnx.SessionFromFileTest: Run failed: %s"), *Error);
            return;
        }

        UE_LOG(LogInoOnnx, Log,
               TEXT("Ino.Onnx.SessionFromFileTest: Run OK in %.3f s — %d output tensor%s:"),
               TRun, Outputs.Num(), Outputs.Num() == 1 ? TEXT("") : TEXT("s"));
        for (int32 i = 0; i < Outputs.Num(); ++i)
        {
            UE_LOG(LogInoOnnx, Log,
                   TEXT("  [%d] %s  dtype=%d  shape=%s  byte-size=%llu"),
                   i, *Session->GetOutputName(i),
                   (int32)Outputs[i].GetDtype(),
                   *FormatShape(Outputs[i].GetShape()),
                   (uint64)Outputs[i].GetByteSize());
        }

        UE_LOG(LogInoOnnx, Log, TEXT("Ino.Onnx.SessionFromFileTest: PASS"));
    }

    FAutoConsoleCommand GSessionFromFileTestCmd(
        TEXT("Ino.Onnx.SessionFromFileTest"),
        TEXT("Load an .onnx model from disk, dump its I/O metadata, and (if inputs are fully concrete) run one zero-filled forward pass. Argument: absolute path to the .onnx file."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&RunSessionFromFileTest));
}

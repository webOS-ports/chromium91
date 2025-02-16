// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_SERVICES_MACHINE_LEARNING_PUBLIC_CPP_FAKE_SERVICE_CONNECTION_H_
#define CHROMEOS_SERVICES_MACHINE_LEARNING_PUBLIC_CPP_FAKE_SERVICE_CONNECTION_H_

#include <memory>
#include <vector>

#include "base/callback_forward.h"
#include "base/component_export.h"
#include "base/macros.h"
#include "chromeos/services/machine_learning/public/cpp/service_connection.h"
#include "chromeos/services/machine_learning/public/mojom/grammar_checker.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/graph_executor.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/handwriting_recognizer.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/machine_learning_service.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/model.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/tensor.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/text_classifier.mojom.h"
#include "chromeos/services/machine_learning/public/mojom/web_platform_handwriting.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote_set.h"

namespace chromeos {
namespace machine_learning {

// Fake implementation of chromeos::machine_learning::ServiceConnection.
// Handles LoadModel (and Model::CreateGraphExecutor) by binding to itself.
// Handles GraphExecutor::Execute by always returning the value specified by
// a previous call to SetOutputValue.
// Handles TextClassifier::Annotate by always returning the value specified by
// a previous call to SetOutputAnnotation.
// Handles TextClassifier::SuggestSelection by always returning the value
// specified by a previous call to SetOutputSelection.
// For use with ServiceConnection::UseFakeServiceConnectionForTesting().
class COMPONENT_EXPORT(CHROMEOS_MLSERVICE) FakeServiceConnectionImpl
    : public ServiceConnection,
      public mojom::MachineLearningService,
      public mojom::Model,
      public mojom::TextClassifier,
      public mojom::HandwritingRecognizer,
      public mojom::GrammarChecker,
      public mojom::GraphExecutor,
      public mojom::SodaRecognizer,
      public web_platform::mojom::HandwritingRecognizer {
 public:
  FakeServiceConnectionImpl();
  ~FakeServiceConnectionImpl() override;

  // ServiceConnection:
  mojom::MachineLearningService& GetMachineLearningService() override;
  void BindMachineLearningService(
      mojo::PendingReceiver<mojom::MachineLearningService> receiver) override;
  void Initialize() override;

  // mojom::MachineLearningService:
  void Clone(
      mojo::PendingReceiver<mojom::MachineLearningService> receiver) override;

  // It's safe to execute LoadBuiltinModel, LoadFlatBufferModel and
  // LoadTextClassifier for multi times, but all the receivers will be bound to
  // the same instance.
  void LoadBuiltinModel(mojom::BuiltinModelSpecPtr spec,
                        mojo::PendingReceiver<mojom::Model> receiver,
                        mojom::MachineLearningService::LoadBuiltinModelCallback
                            callback) override;
  void LoadFlatBufferModel(
      mojom::FlatBufferModelSpecPtr spec,
      mojo::PendingReceiver<mojom::Model> receiver,
      mojom::MachineLearningService::LoadFlatBufferModelCallback callback)
      override;

  void LoadTextClassifier(
      mojo::PendingReceiver<mojom::TextClassifier> receiver,
      mojom::MachineLearningService::LoadTextClassifierCallback callback)
      override;

  void LoadHandwritingModel(
      mojom::HandwritingRecognizerSpecPtr spec,
      mojo::PendingReceiver<mojom::HandwritingRecognizer> receiver,
      mojom::MachineLearningService::LoadHandwritingModelCallback
          result_callback) override;

  // Will be deprecated and removed soon.
  void LoadHandwritingModelWithSpec(
      mojom::HandwritingRecognizerSpecPtr spec,
      mojo::PendingReceiver<mojom::HandwritingRecognizer> receiver,
      mojom::MachineLearningService::LoadHandwritingModelWithSpecCallback
          result_callback) override;

  // Dedicated HWR API for Web Platform.
  void LoadWebPlatformHandwritingModel(
      web_platform::mojom::HandwritingModelConstraintPtr constraint,
      mojo::PendingReceiver<web_platform::mojom::HandwritingRecognizer>
          receiver,
      LoadWebPlatformHandwritingModelCallback callback) override;

  void LoadGrammarChecker(
      mojo::PendingReceiver<mojom::GrammarChecker> receiver,
      mojom::MachineLearningService::LoadGrammarCheckerCallback callback)
      override;

  void LoadSpeechRecognizer(
      mojom::SodaConfigPtr soda_config,
      mojo::PendingRemote<mojom::SodaClient> soda_client,
      mojo::PendingReceiver<mojom::SodaRecognizer> soda_recognizer,
      mojom::MachineLearningService::LoadSpeechRecognizerCallback callback)
      override;

  // mojom::Model:
  void CreateGraphExecutor(
      mojo::PendingReceiver<mojom::GraphExecutor> receiver,
      mojom::Model::CreateGraphExecutorCallback callback) override;
  void CreateGraphExecutorWithOptions(
      mojom::GraphExecutorOptionsPtr options,
      mojo::PendingReceiver<mojom::GraphExecutor> receiver,
      mojom::Model::CreateGraphExecutorCallback callback) override;

  // mojom::GraphExecutor:
  // Execute() will return the tensor set by SetOutputValue() as the output.
  void Execute(base::flat_map<std::string, mojom::TensorPtr> inputs,
               const std::vector<std::string>& output_names,
               mojom::GraphExecutor::ExecuteCallback callback) override;

  // Useful for simulating a failure at different stage.
  // There are different error codes at each stage, we just randomly pick one.
  void SetLoadModelFailure();
  void SetCreateGraphExecutorFailure();
  void SetExecuteFailure();
  void SetLoadTextClassifierFailure();
  // Reset all the Model related failures and make Execute succeed.
  void SetExecuteSuccess();
  // Reset all the TextClassifier related failures and make LoadTextClassifier
  // succeed.
  // Currently, there are three interfaces related to TextClassifier
  // (|LoadTextClassifier|, |Annotate| and |SuggestSelection|) but only
  // |LoadTextClassifier| can fail.
  void SetTextClassifierSuccess();

  // Call SetOutputValue() before Execute() to set the output tensor.
  void SetOutputValue(const std::vector<int64_t>& shape,
                      const std::vector<double>& value);

  // In async mode, FakeServiceConnectionImpl adds requests like
  // LoadBuiltinModel, CreateGraphExecutor to |pending_calls_| instead of
  // responding immediately. Calls in |pending_calls_| will run when
  // RunPendingCalls() is called.
  // It's useful when an unit test wants to test the async behaviour of real
  // ml-service.
  void SetAsyncMode(bool async_mode);
  void RunPendingCalls();

  // Call SetOutputAnnotation() before Annotate() to set the output annotation.
  void SetOutputAnnotation(
      const std::vector<mojom::TextAnnotationPtr>& annotation);

  // Call SetOutputSelection() before SuggestSelection() to set the output
  // selection.
  void SetOutputSelection(const mojom::CodepointSpanPtr& selection);

  // Call SetOutputLanguages() before FindLanguages() to set the output
  // languages.
  void SetOutputLanguages(const std::vector<mojom::TextLanguagePtr>& languages);

  // Call SetOutputGrammarCheckerResult() before Check() to set the output of
  // grammar checker.
  void SetOutputGrammarCheckerResult(
      const mojom::GrammarCheckerResultPtr& result);

  // Call SetOutputHandwritingRecognizerResult() before Recognize() to set the
  // output of handwriting.
  void SetOutputHandwritingRecognizerResult(
      const mojom::HandwritingRecognizerResultPtr& result);

  // Call SetOutputWebPlatformHandwritingRecognizerResult() before
  // GetPrediction() to set the output of handwriting.
  void SetOutputWebPlatformHandwritingRecognizerResult(
      const std::vector<web_platform::mojom::HandwritingPredictionPtr>&
          predictions);

  // mojom::TextClassifier:
  void Annotate(mojom::TextAnnotationRequestPtr request,
                mojom::TextClassifier::AnnotateCallback callback) override;

  // mojom::TextClassifier:
  void SuggestSelection(
      mojom::TextSuggestSelectionRequestPtr request,
      mojom::TextClassifier::SuggestSelectionCallback callback) override;

  // mojom::TextClassifier:
  void FindLanguages(
      const std::string& text,
      mojom::TextClassifier::FindLanguagesCallback callback) override;

  // mojom::HandwritingRecognizer:
  void Recognize(
      mojom::HandwritingRecognitionQueryPtr query,
      mojom::HandwritingRecognizer::RecognizeCallback callback) override;

  // web_platform::mojom::HandwritingRecognizer
  void GetPrediction(
      std::vector<web_platform::mojom::HandwritingStrokePtr> strokes,
      web_platform::mojom::HandwritingHintsPtr hints,
      web_platform::mojom::HandwritingRecognizer::GetPredictionCallback
          callback) override;

  // mojom::GrammarChecker:
  void Check(mojom::GrammarCheckerQueryPtr query,
             mojom::GrammarChecker::CheckCallback callback) override;

  // mojom::SpeechRecognizer
  void AddAudio(const std::vector<uint8_t>& audio) override;
  void Stop() override;
  void Start() override;
  void MarkDone() override;

 private:
  void ScheduleCall(base::OnceClosure call);
  void HandleLoadBuiltinModelCall(
      mojo::PendingReceiver<mojom::Model> receiver,
      mojom::MachineLearningService::LoadBuiltinModelCallback callback);
  void HandleLoadFlatBufferModelCall(
      mojo::PendingReceiver<mojom::Model> receiver,
      mojom::MachineLearningService::LoadFlatBufferModelCallback callback);
  void HandleCreateGraphExecutorCall(
      mojo::PendingReceiver<mojom::GraphExecutor> receiver,
      mojom::Model::CreateGraphExecutorCallback callback);
  void HandleExecuteCall(mojom::GraphExecutor::ExecuteCallback callback);
  void HandleLoadTextClassifierCall(
      mojo::PendingReceiver<mojom::TextClassifier> receiver,
      mojom::MachineLearningService::LoadTextClassifierCallback callback);
  void HandleAnnotateCall(mojom::TextAnnotationRequestPtr request,
                          mojom::TextClassifier::AnnotateCallback callback);
  void HandleSuggestSelectionCall(
      mojom::TextSuggestSelectionRequestPtr request,
      mojom::TextClassifier::SuggestSelectionCallback callback);
  void HandleFindLanguagesCall(
      std::string text,
      mojom::TextClassifier::FindLanguagesCallback callback);
  void HandleLoadHandwritingModelCall(
      mojo::PendingReceiver<mojom::HandwritingRecognizer> receiver,
      mojom::MachineLearningService::LoadHandwritingModelCallback callback);
  void HandleLoadWebPlatformHandwritingModelCall(
      mojo::PendingReceiver<web_platform::mojom::HandwritingRecognizer>
          receiver,
      mojom::MachineLearningService::LoadHandwritingModelCallback callback);
  void HandleLoadHandwritingModelWithSpecCall(
      mojo::PendingReceiver<mojom::HandwritingRecognizer> receiver,
      mojom::MachineLearningService::LoadHandwritingModelWithSpecCallback
          callback);
  void HandleRecognizeCall(
      mojom::HandwritingRecognitionQueryPtr query,
      mojom::HandwritingRecognizer::RecognizeCallback callback);
  void HandleGetPredictionCall(
      std::vector<web_platform::mojom::HandwritingStrokePtr> strokes,
      web_platform::mojom::HandwritingHintsPtr hints,
      web_platform::mojom::HandwritingRecognizer::GetPredictionCallback
          callback);
  void HandleLoadGrammarCheckerCall(
      mojo::PendingReceiver<mojom::GrammarChecker> receiver,
      mojom::MachineLearningService::LoadGrammarCheckerCallback callback);
  void HandleGrammarCheckerQueryCall(
      mojom::GrammarCheckerQueryPtr query,
      mojom::GrammarChecker::CheckCallback callback);
  void HandleLoadSpeechRecognizerCall(
      mojo::PendingRemote<mojom::SodaClient> soda_client,
      mojo::PendingReceiver<mojom::SodaRecognizer> soda_recognizer,
      mojom::MachineLearningService::LoadSpeechRecognizerCallback callback);

  void HandleStopCall();
  void HandleStartCall();
  void HandleMarkDoneCall();

  // Additional receivers bound via `Clone`.
  mojo::ReceiverSet<mojom::MachineLearningService> clone_ml_service_receivers_;

  mojo::Remote<mojom::MachineLearningService> machine_learning_service_;
  mojo::ReceiverSet<mojom::Model> model_receivers_;
  mojo::ReceiverSet<mojom::GraphExecutor> graph_receivers_;
  mojo::ReceiverSet<mojom::TextClassifier> text_classifier_receivers_;
  mojo::ReceiverSet<mojom::HandwritingRecognizer> handwriting_receivers_;
  mojo::ReceiverSet<web_platform::mojom::HandwritingRecognizer>
      web_platform_handwriting_receivers_;
  mojo::ReceiverSet<mojom::GrammarChecker> grammar_checker_receivers_;
  mojo::ReceiverSet<mojom::SodaRecognizer> soda_recognizer_receivers_;
  mojo::RemoteSet<mojom::SodaClient> soda_client_remotes_;
  mojom::TensorPtr output_tensor_;
  mojom::LoadHandwritingModelResult load_handwriting_model_result_;
  mojom::LoadHandwritingModelResult load_web_platform_handwriting_model_result_;
  mojom::LoadModelResult load_model_result_;
  mojom::LoadModelResult load_text_classifier_result_;
  mojom::LoadModelResult load_soda_result_;
  mojom::CreateGraphExecutorResult create_graph_executor_result_;
  mojom::ExecuteResult execute_result_;
  std::vector<mojom::TextAnnotationPtr> annotate_result_;
  mojom::CodepointSpanPtr suggest_selection_result_;
  std::vector<mojom::TextLanguagePtr> find_languages_result_;
  mojom::HandwritingRecognizerResultPtr handwriting_result_;
  std::vector<web_platform::mojom::HandwritingPredictionPtr>
      web_platform_handwriting_result_;
  mojom::GrammarCheckerResultPtr grammar_checker_result_;

  bool async_mode_;
  std::vector<base::OnceClosure> pending_calls_;

  DISALLOW_COPY_AND_ASSIGN(FakeServiceConnectionImpl);
};

}  // namespace machine_learning
}  // namespace chromeos

#endif  // CHROMEOS_SERVICES_MACHINE_LEARNING_PUBLIC_CPP_FAKE_SERVICE_CONNECTION_H_

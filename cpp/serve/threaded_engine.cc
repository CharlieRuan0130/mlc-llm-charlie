/*!
 *  Copyright (c) 2023 by Contributors
 * \file serve/threaded_engine.cc
 * \brief The implementation for threaded serving engine in MLC LLM.
 */
#include "threaded_engine.h"

#include <tvm/runtime/module.h>
#include <tvm/runtime/packed_func.h>
#include <tvm/runtime/registry.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "engine.h"
#include "request.h"

namespace mlc {
namespace llm {
namespace serve {

using tvm::Device;
using namespace tvm::runtime;

/*! \brief The threaded engine instruction kind. */
enum class InstructionKind : int {
  kAddRequest = 0,
  kAbortRequest = 1,
  kUnloadEngine = 2,
  kReloadEngine = 3,
  kDebugCallFuncOnAllAllWorker = 4,
};

/*! \brief The implementation of ThreadedEngine. */
class ThreadedEngineImpl : public ThreadedEngine {
 public:
  void InitBackgroundEngine(TVMArgs args) final {
    Optional<PackedFunc> request_stream_callback;
    try {
      request_stream_callback = args.At<Optional<PackedFunc>>(4);
    } catch (const dmlc::Error& e) {
      LOG(FATAL) << "ValueError: " << e.what() << kEngineCreationErrorMessage;
    }

    CHECK(request_stream_callback.defined())
        << "ThreadedEngine requires request stream callback function, but it is not given.";
    request_stream_callback_ = request_stream_callback.value();

    auto frequest_stream_callback_wrapper = [this](TVMArgs args, TVMRetValue* ret) {
      ICHECK_EQ(args.size(), 1);
      Array<RequestStreamOutput> delta_outputs = args[0];
      bool need_notify = false;
      {
        std::lock_guard<std::mutex> lock(request_stream_callback_mutex_);
        request_stream_callback_inputs_.push_back(std::move(delta_outputs));
        ++pending_request_stream_callback_cnt_;
        need_notify = stream_callback_waiting_;
      }
      if (need_notify) {
        request_stream_callback_cv_.notify_one();
      }
    };

    std::vector<TVMValue> values{args.values, args.values + args.size()};
    std::vector<int> type_codes{args.type_codes, args.type_codes + args.size()};
    TVMArgsSetter setter(values.data(), type_codes.data());
    request_stream_callback = PackedFunc(frequest_stream_callback_wrapper);
    setter(4, request_stream_callback);
    background_engine_ = CreateEnginePacked(TVMArgs(values.data(), type_codes.data(), args.size()));
  }

  void AddRequest(Request request) final {
    bool need_notify = false;
    {
      std::lock_guard<std::mutex> lock(background_loop_mutex_);
      instruction_queue_.emplace_back(InstructionKind::kAddRequest, request);
      ++pending_request_operation_cnt_;
      need_notify = engine_waiting_;
    }
    if (need_notify) {
      background_loop_cv_.notify_one();
    }
  }

  void AbortRequest(const String& request_id) final {
    bool need_notify = false;
    {
      std::lock_guard<std::mutex> lock(background_loop_mutex_);
      instruction_queue_.emplace_back(InstructionKind::kAbortRequest, request_id);
      ++pending_request_operation_cnt_;
      need_notify = engine_waiting_;
    }
    if (need_notify) {
      background_loop_cv_.notify_one();
    }
  }

  void RunBackgroundLoop() final {
    // The local vectors that load the requests from critical regions.
    std::vector<std::pair<InstructionKind, ObjectRef>> local_instruction_queue;

    while (!exit_now_.load(std::memory_order_relaxed)) {
      {
        std::unique_lock<std::mutex> lock(background_loop_mutex_);
        engine_waiting_ = true;
        background_loop_cv_.wait(lock, [this] {
          return !background_engine_->Empty() || pending_request_operation_cnt_.load() > 0 ||
                 exit_now_.load(std::memory_order_relaxed);
        });
        engine_waiting_ = false;

        local_instruction_queue = instruction_queue_;
        instruction_queue_.clear();
        pending_request_operation_cnt_ = 0;
      }
      for (const auto& [kind, arg] : local_instruction_queue) {
        if (kind == InstructionKind::kAddRequest) {
          background_engine_->AddRequest(Downcast<Request>(arg));
        } else if (kind == InstructionKind::kAbortRequest) {
          background_engine_->AbortRequest(Downcast<String>(arg));
        } else if (kind == InstructionKind::kUnloadEngine) {
          // Todo(mlc-team): implement engine unload
          LOG(FATAL) << "Not implemented yet.";
        } else if (kind == InstructionKind::kReloadEngine) {
          // Todo(mlc-team): implement engine reload
          LOG(FATAL) << "Not implemented yet.";
        } else if (kind == InstructionKind::kDebugCallFuncOnAllAllWorker) {
          background_engine_->DebugCallFuncOnAllAllWorker(Downcast<String>(arg));
        } else {
          LOG(FATAL) << "Cannot reach here";
        }
      }
      background_engine_->Step();
    }
  }

  void RunBackgroundStreamBackLoop() final {
    // The local vectors that load the request stream callback inputs from critical regions.
    std::vector<Array<RequestStreamOutput>> local_request_stream_callback_inputs;
    std::vector<RequestStreamOutput> flattened_callback_inputs;

    while (!exit_now_.load(std::memory_order_relaxed)) {
      {
        std::unique_lock<std::mutex> lock(request_stream_callback_mutex_);
        stream_callback_waiting_ = true;
        request_stream_callback_cv_.wait(lock, [this] {
          return pending_request_stream_callback_cnt_.load() > 0 ||
                 exit_now_.load(std::memory_order_relaxed);
        });
        stream_callback_waiting_ = false;

        local_request_stream_callback_inputs = request_stream_callback_inputs_;
        request_stream_callback_inputs_.clear();
        pending_request_stream_callback_cnt_ = 0;
      }
      for (const Array<RequestStreamOutput>& callback_inputs :
           local_request_stream_callback_inputs) {
        for (const RequestStreamOutput& callback_input : callback_inputs) {
          flattened_callback_inputs.push_back(callback_input);
        }
      }
      if (!flattened_callback_inputs.empty()) {
        request_stream_callback_(Array<RequestStreamOutput>(flattened_callback_inputs));
      }
      flattened_callback_inputs.clear();
    }
  }

  void ExitBackgroundLoop() final {
    {
      std::lock_guard<std::mutex> lock(background_loop_mutex_);
      exit_now_.store(true);
    }
    background_loop_cv_.notify_one();
    request_stream_callback_cv_.notify_one();
  }

  /************** Debug/Profile **************/

  void DebugCallFuncOnAllAllWorker(const String& func_name) final {
    bool need_notify = false;
    {
      std::lock_guard<std::mutex> lock(background_loop_mutex_);
      instruction_queue_.emplace_back(InstructionKind::kDebugCallFuncOnAllAllWorker, func_name);
      ++pending_request_operation_cnt_;
      need_notify = engine_waiting_;
    }
    if (need_notify) {
      background_loop_cv_.notify_one();
    }
  }

 private:
  /*! \brief The background normal engine for request processing. */
  std::unique_ptr<Engine> background_engine_;
  /*! \brief The request stream callback. */
  PackedFunc request_stream_callback_;

  /*! \brief The mutex ensuring only one thread can access critical regions. */
  std::mutex background_loop_mutex_;
  std::mutex request_stream_callback_mutex_;
  /*! \brief The condition variable preventing threaded engine from spinning. */
  std::condition_variable background_loop_cv_;
  std::condition_variable request_stream_callback_cv_;
  /*! \brief A boolean flag denoting if the engine needs to exit background loop. */
  std::atomic<bool> exit_now_ = false;

  /************** Critical Regions **************/
  /*!
   * \brief The instruction queue for the threaded engine.
   * The instructions include:
   *  - requests to add into the background engine,
   *  - requests to abort from the background engine,
   *  - engine unload/reload,
   *  - and other debugging instructions.
   * Elements are sended from other threads and consumed by
   * the threaded engine in the background loop.
   */
  std::vector<std::pair<InstructionKind, ObjectRef>> instruction_queue_;
  /*!
   * \brief The delta outputs to pass through callback.
   * Elements are sended from the background loop thread and
   * consumed by the foreground thread.
   */
  std::vector<Array<RequestStreamOutput>> request_stream_callback_inputs_;
  /*!
   * \brief Number of pending request operations, should be the size of
   * `requests_to_add_` and `requests_to_abort_`.
   */
  std::atomic<int> pending_request_operation_cnt_ = 0;
  /*!
   * \brief Number of pending request stream callback invocations.
   * It should be the size of `request_stream_callback_inputs_`.
   */
  std::atomic<int> pending_request_stream_callback_cnt_ = 0;
  /*! \brief A boolean flag indicating if the engine is waiting for new requests/aborts. */
  bool engine_waiting_ = false;
  /*! \brief A boolean flag indicating if the stream callback loop is waiting. */
  bool stream_callback_waiting_ = false;
};

/*! \brief The implementation of ThreadedEngine. */
class ThreadedEngineModule : public ThreadedEngineImpl, public ModuleNode {
 public:
  TVM_MODULE_VTABLE_BEGIN("mlc.serve.async_threaded_engine");
  TVM_MODULE_VTABLE_ENTRY("add_request", &ThreadedEngineImpl::AddRequest);
  TVM_MODULE_VTABLE_ENTRY("abort_request", &ThreadedEngineImpl::AbortRequest);
  TVM_MODULE_VTABLE_ENTRY("run_background_loop", &ThreadedEngineImpl::RunBackgroundLoop);
  TVM_MODULE_VTABLE_ENTRY("run_background_stream_back_loop",
                          &ThreadedEngineImpl::RunBackgroundStreamBackLoop);
  TVM_MODULE_VTABLE_ENTRY("exit_background_loop", &ThreadedEngineImpl::ExitBackgroundLoop);
  TVM_MODULE_VTABLE_ENTRY("debug_call_func_on_all_worker",
                          &ThreadedEngineImpl::DebugCallFuncOnAllAllWorker);
  if (_name == "init_background_engine") {
    return PackedFunc([_self](TVMArgs args, TVMRetValue* rv) -> void {
      SelfPtr self = static_cast<SelfPtr>(_self.get());
      self->InitBackgroundEngine(args);
    });
  }
  TVM_MODULE_VTABLE_END();
};

TVM_REGISTER_GLOBAL("mlc.serve.create_threaded_engine").set_body_typed([]() {
  return Module(make_object<ThreadedEngineModule>());
});

std::unique_ptr<ThreadedEngine> ThreadedEngine::Create() {
  std::unique_ptr<ThreadedEngineImpl> threaded_engine = std::make_unique<ThreadedEngineImpl>();
  return std::move(threaded_engine);
}

}  // namespace serve
}  // namespace llm
}  // namespace mlc

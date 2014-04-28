/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "instrumentation.h"

#include <sys/uio.h>

#include "atomic.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "interpreter/interpreter.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "nth_caller_visitor.h"
#if !defined(ART_USE_PORTABLE_COMPILER)
#include "entrypoints/quick/quick_entrypoints.h"
#endif
#include "object_utils.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"

namespace art {

namespace instrumentation {

const bool kVerboseInstrumentation = false;

// Do we want to deoptimize for method entry and exit listeners or just try to intercept
// invocations? Deoptimization forces all code to run in the interpreter and considerably hurts the
// application's performance.
static constexpr bool kDeoptimizeForAccurateMethodEntryExitListeners = false;

static bool InstallStubsClassVisitor(mirror::Class* klass, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Instrumentation* instrumentation = reinterpret_cast<Instrumentation*>(arg);
  return instrumentation->InstallStubsForClass(klass);
}

Instrumentation::Instrumentation()
    : instrumentation_stubs_installed_(false), entry_exit_stubs_installed_(false),
      interpreter_stubs_installed_(false),
      interpret_only_(false), forced_interpret_only_(false),
      have_method_entry_listeners_(false), have_method_exit_listeners_(false),
      have_method_unwind_listeners_(false), have_dex_pc_listeners_(false),
      have_field_read_listeners_(false), have_field_write_listeners_(false),
      have_exception_caught_listeners_(false),
      deoptimized_methods_lock_("deoptimized methods lock"),
      deoptimization_enabled_(false),
      interpreter_handler_table_(kMainHandlerTable),
      quick_alloc_entry_points_instrumentation_counter_(0) {
}

bool Instrumentation::InstallStubsForClass(mirror::Class* klass) {
  for (size_t i = 0, e = klass->NumDirectMethods(); i < e; i++) {
    InstallStubsForMethod(klass->GetDirectMethod(i));
  }
  for (size_t i = 0, e = klass->NumVirtualMethods(); i < e; i++) {
    InstallStubsForMethod(klass->GetVirtualMethod(i));
  }
  return true;
}

static void UpdateEntrypoints(mirror::ArtMethod* method, const void* quick_code,
                              const void* portable_code, bool have_portable_code)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  method->SetEntryPointFromPortableCompiledCode(portable_code);
  method->SetEntryPointFromQuickCompiledCode(quick_code);
  bool portable_enabled = method->IsPortableCompiled();
  if (have_portable_code && !portable_enabled) {
    method->SetIsPortableCompiled();
  } else if (portable_enabled) {
    method->ClearIsPortableCompiled();
  }
  if (!method->IsResolutionMethod()) {
    if (quick_code == GetQuickToInterpreterBridge() ||
        quick_code == GetQuickToInterpreterBridgeTrampoline(Runtime::Current()->GetClassLinker()) ||
        (quick_code == GetQuickResolutionTrampoline(Runtime::Current()->GetClassLinker()) &&
         Runtime::Current()->GetInstrumentation()->IsForcedInterpretOnly()
         && !method->IsNative() && !method->IsProxyMethod())) {
      if (kIsDebugBuild) {
        if (quick_code == GetQuickToInterpreterBridge()) {
          DCHECK(portable_code == GetPortableToInterpreterBridge());
        } else if (quick_code == GetQuickResolutionTrampoline(Runtime::Current()->GetClassLinker())) {
          DCHECK(portable_code == GetPortableResolutionTrampoline(Runtime::Current()->GetClassLinker()));
        }
      }
      DCHECK(!method->IsNative()) << PrettyMethod(method);
      DCHECK(!method->IsProxyMethod()) << PrettyMethod(method);
      method->SetEntryPointFromInterpreter(art::interpreter::artInterpreterToInterpreterBridge);
    } else {
      method->SetEntryPointFromInterpreter(art::artInterpreterToCompiledCodeBridge);
    }
  }
}

void Instrumentation::InstallStubsForMethod(mirror::ArtMethod* method) {
  if (method->IsAbstract() || method->IsProxyMethod()) {
    // Do not change stubs for these methods.
    return;
  }
  const void* new_portable_code;
  const void* new_quick_code;
  bool uninstall = !entry_exit_stubs_installed_ && !interpreter_stubs_installed_;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  bool is_class_initialized = method->GetDeclaringClass()->IsInitialized();
  bool have_portable_code = false;
  if (uninstall) {
    if ((forced_interpret_only_ || IsDeoptimized(method)) && !method->IsNative()) {
      new_portable_code = GetPortableToInterpreterBridge();
      new_quick_code = GetQuickToInterpreterBridge();
    } else if (is_class_initialized || !method->IsStatic() || method->IsConstructor()) {
      new_portable_code = class_linker->GetPortableOatCodeFor(method, &have_portable_code);
      new_quick_code = class_linker->GetQuickOatCodeFor(method);
    } else {
      new_portable_code = GetPortableResolutionTrampoline(class_linker);
      new_quick_code = GetQuickResolutionTrampoline(class_linker);
    }
  } else {  // !uninstall
    if ((interpreter_stubs_installed_ || IsDeoptimized(method)) && !method->IsNative()) {
      new_portable_code = GetPortableToInterpreterBridge();
      new_quick_code = GetQuickToInterpreterBridge();
    } else {
      // Do not overwrite resolution trampoline. When the trampoline initializes the method's
      // class, all its static methods code will be set to the instrumentation entry point.
      // For more details, see ClassLinker::FixupStaticTrampolines.
      if (is_class_initialized || !method->IsStatic() || method->IsConstructor()) {
        // Do not overwrite interpreter to prevent from posting method entry/exit events twice.
        new_portable_code = class_linker->GetPortableOatCodeFor(method, &have_portable_code);
        new_quick_code = class_linker->GetQuickOatCodeFor(method);
        DCHECK(new_quick_code != GetQuickToInterpreterBridgeTrampoline(class_linker));
        if (entry_exit_stubs_installed_ && new_quick_code != GetQuickToInterpreterBridge()) {
          DCHECK(new_portable_code != GetPortableToInterpreterBridge());
          new_portable_code = GetPortableToInterpreterBridge();
          new_quick_code = GetQuickInstrumentationEntryPoint();
        }
      } else {
        new_portable_code = GetPortableResolutionTrampoline(class_linker);
        new_quick_code = GetQuickResolutionTrampoline(class_linker);
      }
    }
  }
  UpdateEntrypoints(method, new_quick_code, new_portable_code, have_portable_code);
}

// Places the instrumentation exit pc as the return PC for every quick frame. This also allows
// deoptimization of quick frames to interpreter frames.
// Since we may already have done this previously, we need to push new instrumentation frame before
// existing instrumentation frames.
static void InstrumentationInstallStack(Thread* thread, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  struct InstallStackVisitor : public StackVisitor {
    InstallStackVisitor(Thread* thread, Context* context, uintptr_t instrumentation_exit_pc)
        : StackVisitor(thread, context),  instrumentation_stack_(thread->GetInstrumentationStack()),
          existing_instrumentation_frames_count_(instrumentation_stack_->size()),
          instrumentation_exit_pc_(instrumentation_exit_pc),
          reached_existing_instrumentation_frames_(false), instrumentation_stack_depth_(0),
          last_return_pc_(0) {
    }

    virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      mirror::ArtMethod* m = GetMethod();
      if (GetCurrentQuickFrame() == NULL) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Ignoring a shadow frame. Frame " << GetFrameId()
                    << " Method=" << PrettyMethod(m);
        }
        return true;  // Ignore shadow frames.
      }
      if (m == NULL) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Skipping upcall. Frame " << GetFrameId();
        }
        last_return_pc_ = 0;
        return true;  // Ignore upcalls.
      }
      if (m->IsRuntimeMethod()) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Skipping runtime method. Frame " << GetFrameId();
        }
        last_return_pc_ = GetReturnPc();
        return true;  // Ignore unresolved methods since they will be instrumented after resolution.
      }
      if (kVerboseInstrumentation) {
        LOG(INFO) << "  Installing exit stub in " << DescribeLocation();
      }
      uintptr_t return_pc = GetReturnPc();
      if (return_pc == instrumentation_exit_pc_) {
        // We've reached a frame which has already been installed with instrumentation exit stub.
        // We should have already installed instrumentation on previous frames.
        reached_existing_instrumentation_frames_ = true;

        CHECK_LT(instrumentation_stack_depth_, instrumentation_stack_->size());
        const InstrumentationStackFrame& frame = instrumentation_stack_->at(instrumentation_stack_depth_);
        CHECK_EQ(m, frame.method_) << "Expected " << PrettyMethod(m)
                                   << ", Found " << PrettyMethod(frame.method_);
        return_pc = frame.return_pc_;
        if (kVerboseInstrumentation) {
          LOG(INFO) << "Ignoring already instrumented " << frame.Dump();
        }
      } else {
        CHECK_NE(return_pc, 0U);
        CHECK(!reached_existing_instrumentation_frames_);
        InstrumentationStackFrame instrumentation_frame(GetThisObject(), m, return_pc, GetFrameId(),
                                                        false);
        if (kVerboseInstrumentation) {
          LOG(INFO) << "Pushing frame " << instrumentation_frame.Dump();
        }

        // Insert frame before old ones so we do not corrupt the instrumentation stack.
        auto it = instrumentation_stack_->end() - existing_instrumentation_frames_count_;
        instrumentation_stack_->insert(it, instrumentation_frame);
        SetReturnPc(instrumentation_exit_pc_);
      }
      dex_pcs_.push_back(m->ToDexPc(last_return_pc_));
      last_return_pc_ = return_pc;
      ++instrumentation_stack_depth_;
      return true;  // Continue.
    }
    std::deque<InstrumentationStackFrame>* const instrumentation_stack_;
    const size_t existing_instrumentation_frames_count_;
    std::vector<uint32_t> dex_pcs_;
    const uintptr_t instrumentation_exit_pc_;
    bool reached_existing_instrumentation_frames_;
    size_t instrumentation_stack_depth_;
    uintptr_t last_return_pc_;
  };
  if (kVerboseInstrumentation) {
    std::string thread_name;
    thread->GetThreadName(thread_name);
    LOG(INFO) << "Installing exit stubs in " << thread_name;
  }

  Instrumentation* instrumentation = reinterpret_cast<Instrumentation*>(arg);
  UniquePtr<Context> context(Context::Create());
  uintptr_t instrumentation_exit_pc = GetQuickInstrumentationExitPc();
  InstallStackVisitor visitor(thread, context.get(), instrumentation_exit_pc);
  visitor.WalkStack(true);
  CHECK_EQ(visitor.dex_pcs_.size(), thread->GetInstrumentationStack()->size());

  if (instrumentation->ShouldNotifyMethodEnterExitEvents()) {
    // Create method enter events for all methods currently on the thread's stack. We only do this
    // if no debugger is attached to prevent from posting events twice.
    typedef std::deque<InstrumentationStackFrame>::const_reverse_iterator It;
    for (It it = thread->GetInstrumentationStack()->rbegin(),
        end = thread->GetInstrumentationStack()->rend(); it != end; ++it) {
      mirror::Object* this_object = (*it).this_object_;
      mirror::ArtMethod* method = (*it).method_;
      uint32_t dex_pc = visitor.dex_pcs_.back();
      visitor.dex_pcs_.pop_back();
      instrumentation->MethodEnterEvent(thread, this_object, method, dex_pc);
    }
  }
  thread->VerifyStack();
}

// Removes the instrumentation exit pc as the return PC for every quick frame.
static void InstrumentationRestoreStack(Thread* thread, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  struct RestoreStackVisitor : public StackVisitor {
    RestoreStackVisitor(Thread* thread, uintptr_t instrumentation_exit_pc,
                        Instrumentation* instrumentation)
        : StackVisitor(thread, NULL), thread_(thread),
          instrumentation_exit_pc_(instrumentation_exit_pc),
          instrumentation_(instrumentation),
          instrumentation_stack_(thread->GetInstrumentationStack()),
          frames_removed_(0) {}

    virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      if (instrumentation_stack_->size() == 0) {
        return false;  // Stop.
      }
      mirror::ArtMethod* m = GetMethod();
      if (GetCurrentQuickFrame() == NULL) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Ignoring a shadow frame. Frame " << GetFrameId() << " Method=" << PrettyMethod(m);
        }
        return true;  // Ignore shadow frames.
      }
      if (m == NULL) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  Skipping upcall. Frame " << GetFrameId();
        }
        return true;  // Ignore upcalls.
      }
      bool removed_stub = false;
      // TODO: make this search more efficient?
      const size_t frameId = GetFrameId();
      for (const InstrumentationStackFrame& instrumentation_frame : *instrumentation_stack_) {
        if (instrumentation_frame.frame_id_ == frameId) {
          if (kVerboseInstrumentation) {
            LOG(INFO) << "  Removing exit stub in " << DescribeLocation();
          }
          if (instrumentation_frame.interpreter_entry_) {
            CHECK(m == Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs));
          } else {
            CHECK(m == instrumentation_frame.method_) << PrettyMethod(m);
          }
          SetReturnPc(instrumentation_frame.return_pc_);
          if (instrumentation_->ShouldNotifyMethodEnterExitEvents()) {
            // Create the method exit events. As the methods didn't really exit the result is 0.
            // We only do this if no debugger is attached to prevent from posting events twice.
            instrumentation_->MethodExitEvent(thread_, instrumentation_frame.this_object_, m,
                                              GetDexPc(), JValue());
          }
          frames_removed_++;
          removed_stub = true;
          break;
        }
      }
      if (!removed_stub) {
        if (kVerboseInstrumentation) {
          LOG(INFO) << "  No exit stub in " << DescribeLocation();
        }
      }
      return true;  // Continue.
    }
    Thread* const thread_;
    const uintptr_t instrumentation_exit_pc_;
    Instrumentation* const instrumentation_;
    std::deque<instrumentation::InstrumentationStackFrame>* const instrumentation_stack_;
    size_t frames_removed_;
  };
  if (kVerboseInstrumentation) {
    std::string thread_name;
    thread->GetThreadName(thread_name);
    LOG(INFO) << "Removing exit stubs in " << thread_name;
  }
  std::deque<instrumentation::InstrumentationStackFrame>* stack = thread->GetInstrumentationStack();
  if (stack->size() > 0) {
    Instrumentation* instrumentation = reinterpret_cast<Instrumentation*>(arg);
    uintptr_t instrumentation_exit_pc = GetQuickInstrumentationExitPc();
    RestoreStackVisitor visitor(thread, instrumentation_exit_pc, instrumentation);
    visitor.WalkStack(true);
    CHECK_EQ(visitor.frames_removed_, stack->size());
    while (stack->size() > 0) {
      stack->pop_front();
    }
  }
}

void Instrumentation::AddListener(InstrumentationListener* listener, uint32_t events) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());
  if ((events & kMethodEntered) != 0) {
    method_entry_listeners_.push_back(listener);
    have_method_entry_listeners_ = true;
  }
  if ((events & kMethodExited) != 0) {
    method_exit_listeners_.push_back(listener);
    have_method_exit_listeners_ = true;
  }
  if ((events & kMethodUnwind) != 0) {
    method_unwind_listeners_.push_back(listener);
    have_method_unwind_listeners_ = true;
  }
  if ((events & kDexPcMoved) != 0) {
    dex_pc_listeners_.push_back(listener);
    have_dex_pc_listeners_ = true;
  }
  if ((events & kFieldRead) != 0) {
    field_read_listeners_.push_back(listener);
    have_field_read_listeners_ = true;
  }
  if ((events & kFieldWritten) != 0) {
    field_write_listeners_.push_back(listener);
    have_field_write_listeners_ = true;
  }
  if ((events & kExceptionCaught) != 0) {
    exception_caught_listeners_.push_back(listener);
    have_exception_caught_listeners_ = true;
  }
  UpdateInterpreterHandlerTable();
}

void Instrumentation::RemoveListener(InstrumentationListener* listener, uint32_t events) {
  Locks::mutator_lock_->AssertExclusiveHeld(Thread::Current());

  if ((events & kMethodEntered) != 0) {
    bool contains = std::find(method_entry_listeners_.begin(), method_entry_listeners_.end(),
                              listener) != method_entry_listeners_.end();
    if (contains) {
      method_entry_listeners_.remove(listener);
    }
    have_method_entry_listeners_ = method_entry_listeners_.size() > 0;
  }
  if ((events & kMethodExited) != 0) {
    bool contains = std::find(method_exit_listeners_.begin(), method_exit_listeners_.end(),
                              listener) != method_exit_listeners_.end();
    if (contains) {
      method_exit_listeners_.remove(listener);
    }
    have_method_exit_listeners_ = method_exit_listeners_.size() > 0;
  }
  if ((events & kMethodUnwind) != 0) {
    method_unwind_listeners_.remove(listener);
  }
  if ((events & kDexPcMoved) != 0) {
    bool contains = std::find(dex_pc_listeners_.begin(), dex_pc_listeners_.end(),
                              listener) != dex_pc_listeners_.end();
    if (contains) {
      dex_pc_listeners_.remove(listener);
    }
    have_dex_pc_listeners_ = dex_pc_listeners_.size() > 0;
  }
  if ((events & kFieldRead) != 0) {
    bool contains = std::find(field_read_listeners_.begin(), field_read_listeners_.end(),
                              listener) != field_read_listeners_.end();
    if (contains) {
      field_read_listeners_.remove(listener);
    }
    have_field_read_listeners_ = field_read_listeners_.size() > 0;
  }
  if ((events & kFieldWritten) != 0) {
    bool contains = std::find(field_write_listeners_.begin(), field_write_listeners_.end(),
                              listener) != field_write_listeners_.end();
    if (contains) {
      field_write_listeners_.remove(listener);
    }
    have_field_write_listeners_ = field_write_listeners_.size() > 0;
  }
  if ((events & kExceptionCaught) != 0) {
    exception_caught_listeners_.remove(listener);
    have_exception_caught_listeners_ = exception_caught_listeners_.size() > 0;
  }
  UpdateInterpreterHandlerTable();
}

void Instrumentation::ConfigureStubs(bool require_entry_exit_stubs, bool require_interpreter) {
  interpret_only_ = require_interpreter || forced_interpret_only_;
  // Compute what level of instrumentation is required and compare to current.
  int desired_level, current_level;
  if (require_interpreter) {
    desired_level = 2;
  } else if (require_entry_exit_stubs) {
    desired_level = 1;
  } else {
    desired_level = 0;
  }
  if (interpreter_stubs_installed_) {
    current_level = 2;
  } else if (entry_exit_stubs_installed_) {
    current_level = 1;
  } else {
    current_level = 0;
  }
  if (desired_level == current_level) {
    // We're already set.
    return;
  }
  Thread* const self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  Locks::thread_list_lock_->AssertNotHeld(self);
  if (desired_level > 0) {
    if (require_interpreter) {
      interpreter_stubs_installed_ = true;
    } else {
      CHECK(require_entry_exit_stubs);
      entry_exit_stubs_installed_ = true;
    }
    runtime->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, this);
    instrumentation_stubs_installed_ = true;
    MutexLock mu(self, *Locks::thread_list_lock_);
    runtime->GetThreadList()->ForEach(InstrumentationInstallStack, this);
  } else {
    interpreter_stubs_installed_ = false;
    entry_exit_stubs_installed_ = false;
    runtime->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, this);
    // Restore stack only if there is no method currently deoptimized.
    bool empty;
    {
      ReaderMutexLock mu(self, deoptimized_methods_lock_);
      empty = deoptimized_methods_.empty();  // Avoid lock violation.
    }
    if (empty) {
      instrumentation_stubs_installed_ = false;
      MutexLock mu(self, *Locks::thread_list_lock_);
      Runtime::Current()->GetThreadList()->ForEach(InstrumentationRestoreStack, this);
    }
  }
}

static void ResetQuickAllocEntryPointsForThread(Thread* thread, void* arg) {
  thread->ResetQuickAllocEntryPointsForThread();
}

void Instrumentation::SetEntrypointsInstrumented(bool instrumented) {
  Runtime* runtime = Runtime::Current();
  ThreadList* tl = runtime->GetThreadList();
  if (runtime->IsStarted()) {
    tl->SuspendAll();
  }
  {
    MutexLock mu(Thread::Current(), *Locks::runtime_shutdown_lock_);
    SetQuickAllocEntryPointsInstrumented(instrumented);
    ResetQuickAllocEntryPoints();
  }
  if (runtime->IsStarted()) {
    tl->ResumeAll();
  }
}

void Instrumentation::InstrumentQuickAllocEntryPoints() {
  // TODO: the read of quick_alloc_entry_points_instrumentation_counter_ is racey and this code
  //       should be guarded by a lock.
  DCHECK_GE(quick_alloc_entry_points_instrumentation_counter_.Load(), 0);
  const bool enable_instrumentation =
      quick_alloc_entry_points_instrumentation_counter_.FetchAndAdd(1) == 0;
  if (enable_instrumentation) {
    SetEntrypointsInstrumented(true);
  }
}

void Instrumentation::UninstrumentQuickAllocEntryPoints() {
  // TODO: the read of quick_alloc_entry_points_instrumentation_counter_ is racey and this code
  //       should be guarded by a lock.
  DCHECK_GT(quick_alloc_entry_points_instrumentation_counter_.Load(), 0);
  const bool disable_instrumentation =
      quick_alloc_entry_points_instrumentation_counter_.FetchAndSub(1) == 1;
  if (disable_instrumentation) {
    SetEntrypointsInstrumented(false);
  }
}

void Instrumentation::ResetQuickAllocEntryPoints() {
  Runtime* runtime = Runtime::Current();
  if (runtime->IsStarted()) {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    runtime->GetThreadList()->ForEach(ResetQuickAllocEntryPointsForThread, NULL);
  }
}

void Instrumentation::UpdateMethodsCode(mirror::ArtMethod* method, const void* quick_code,
                                        const void* portable_code, bool have_portable_code) const {
  const void* new_portable_code;
  const void* new_quick_code;
  bool new_have_portable_code;
  if (LIKELY(!instrumentation_stubs_installed_)) {
    new_portable_code = portable_code;
    new_quick_code = quick_code;
    new_have_portable_code = have_portable_code;
  } else {
    if ((interpreter_stubs_installed_ || IsDeoptimized(method)) && !method->IsNative()) {
      new_portable_code = GetPortableToInterpreterBridge();
      new_quick_code = GetQuickToInterpreterBridge();
      new_have_portable_code = false;
    } else if (quick_code == GetQuickResolutionTrampoline(Runtime::Current()->GetClassLinker()) ||
        quick_code == GetQuickToInterpreterBridgeTrampoline(Runtime::Current()->GetClassLinker()) ||
        quick_code == GetQuickToInterpreterBridge()) {
      DCHECK((portable_code == GetPortableResolutionTrampoline(Runtime::Current()->GetClassLinker())) ||
             (portable_code == GetPortableToInterpreterBridge()));
      new_portable_code = portable_code;
      new_quick_code = quick_code;
      new_have_portable_code = have_portable_code;
    } else if (entry_exit_stubs_installed_) {
      new_quick_code = GetQuickInstrumentationEntryPoint();
      new_portable_code = GetPortableToInterpreterBridge();
      new_have_portable_code = false;
    } else {
      new_portable_code = portable_code;
      new_quick_code = quick_code;
      new_have_portable_code = have_portable_code;
    }
  }
  UpdateEntrypoints(method, new_quick_code, new_portable_code, new_have_portable_code);
}

void Instrumentation::Deoptimize(mirror::ArtMethod* method) {
  CHECK(!method->IsNative());
  CHECK(!method->IsProxyMethod());
  CHECK(!method->IsAbstract());

  Thread* self = Thread::Current();
  std::pair<std::set<mirror::ArtMethod*>::iterator, bool> pair;
  {
    WriterMutexLock mu(self, deoptimized_methods_lock_);
    pair = deoptimized_methods_.insert(method);
  }
  bool already_deoptimized = !pair.second;
  CHECK(!already_deoptimized) << "Method " << PrettyMethod(method) << " is already deoptimized";

  if (!interpreter_stubs_installed_) {
    UpdateEntrypoints(method, GetQuickToInterpreterBridge(), GetPortableToInterpreterBridge(),
                      false);

    // Install instrumentation exit stub and instrumentation frames. We may already have installed
    // these previously so it will only cover the newly created frames.
    instrumentation_stubs_installed_ = true;
    MutexLock mu(self, *Locks::thread_list_lock_);
    Runtime::Current()->GetThreadList()->ForEach(InstrumentationInstallStack, this);
  }
}

void Instrumentation::Undeoptimize(mirror::ArtMethod* method) {
  CHECK(!method->IsNative());
  CHECK(!method->IsProxyMethod());
  CHECK(!method->IsAbstract());

  Thread* self = Thread::Current();
  bool empty;
  {
    WriterMutexLock mu(self, deoptimized_methods_lock_);
    auto it = deoptimized_methods_.find(method);
    CHECK(it != deoptimized_methods_.end()) << "Method " << PrettyMethod(method)
        << " is not deoptimized";
    deoptimized_methods_.erase(it);
    empty = deoptimized_methods_.empty();
  }

  // Restore code and possibly stack only if we did not deoptimize everything.
  if (!interpreter_stubs_installed_) {
    // Restore its code or resolution trampoline.
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (method->IsStatic() && !method->IsConstructor() &&
        !method->GetDeclaringClass()->IsInitialized()) {
      UpdateEntrypoints(method, GetQuickResolutionTrampoline(class_linker),
                        GetPortableResolutionTrampoline(class_linker), false);
    } else {
      bool have_portable_code = false;
      const void* quick_code = class_linker->GetQuickOatCodeFor(method);
      const void* portable_code = class_linker->GetPortableOatCodeFor(method, &have_portable_code);
      UpdateEntrypoints(method, quick_code, portable_code, have_portable_code);
    }

    // If there is no deoptimized method left, we can restore the stack of each thread.
    if (empty) {
      MutexLock mu(self, *Locks::thread_list_lock_);
      Runtime::Current()->GetThreadList()->ForEach(InstrumentationRestoreStack, this);
      instrumentation_stubs_installed_ = false;
    }
  }
}

bool Instrumentation::IsDeoptimized(mirror::ArtMethod* method) const {
  ReaderMutexLock mu(Thread::Current(), deoptimized_methods_lock_);
  DCHECK(method != nullptr);
  return deoptimized_methods_.find(method) != deoptimized_methods_.end();
}

void Instrumentation::EnableDeoptimization() {
  ReaderMutexLock mu(Thread::Current(), deoptimized_methods_lock_);
  CHECK(deoptimized_methods_.empty());
  CHECK_EQ(deoptimization_enabled_, false);
  deoptimization_enabled_ = true;
}

void Instrumentation::DisableDeoptimization() {
  CHECK_EQ(deoptimization_enabled_, true);
  // If we deoptimized everything, undo it.
  if (interpreter_stubs_installed_) {
    UndeoptimizeEverything();
  }
  // Undeoptimized selected methods.
  while (true) {
    mirror::ArtMethod* method;
    {
      ReaderMutexLock mu(Thread::Current(), deoptimized_methods_lock_);
      if (deoptimized_methods_.empty()) {
        break;
      }
      method = *deoptimized_methods_.begin();
    }
    Undeoptimize(method);
  }
  deoptimization_enabled_ = false;
}

// Indicates if instrumentation should notify method enter/exit events to the listeners.
bool Instrumentation::ShouldNotifyMethodEnterExitEvents() const {
  return !deoptimization_enabled_ && !interpreter_stubs_installed_;
}

void Instrumentation::DeoptimizeEverything() {
  CHECK(!interpreter_stubs_installed_);
  ConfigureStubs(false, true);
}

void Instrumentation::UndeoptimizeEverything() {
  CHECK(interpreter_stubs_installed_);
  ConfigureStubs(false, false);
}

void Instrumentation::EnableMethodTracing() {
  bool require_interpreter = kDeoptimizeForAccurateMethodEntryExitListeners;
  ConfigureStubs(!require_interpreter, require_interpreter);
}

void Instrumentation::DisableMethodTracing() {
  ConfigureStubs(false, false);
}

const void* Instrumentation::GetQuickCodeFor(mirror::ArtMethod* method) const {
  Runtime* runtime = Runtime::Current();
  if (LIKELY(!instrumentation_stubs_installed_)) {
    const void* code = method->GetEntryPointFromQuickCompiledCode();
    DCHECK(code != nullptr);
    if (LIKELY(code != GetQuickResolutionTrampoline(runtime->GetClassLinker())) &&
        LIKELY(code != GetQuickToInterpreterBridgeTrampoline(runtime->GetClassLinker())) &&
        LIKELY(code != GetQuickToInterpreterBridge())) {
      return code;
    }
  }
  return runtime->GetClassLinker()->GetQuickOatCodeFor(method);
}

void Instrumentation::MethodEnterEventImpl(Thread* thread, mirror::Object* this_object,
                                           mirror::ArtMethod* method,
                                           uint32_t dex_pc) const {
  auto it = method_entry_listeners_.begin();
  bool is_end = (it == method_entry_listeners_.end());
  // Implemented this way to prevent problems caused by modification of the list while iterating.
  while (!is_end) {
    InstrumentationListener* cur = *it;
    ++it;
    is_end = (it == method_entry_listeners_.end());
    cur->MethodEntered(thread, this_object, method, dex_pc);
  }
}

void Instrumentation::MethodExitEventImpl(Thread* thread, mirror::Object* this_object,
                                          mirror::ArtMethod* method,
                                          uint32_t dex_pc, const JValue& return_value) const {
  auto it = method_exit_listeners_.begin();
  bool is_end = (it == method_exit_listeners_.end());
  // Implemented this way to prevent problems caused by modification of the list while iterating.
  while (!is_end) {
    InstrumentationListener* cur = *it;
    ++it;
    is_end = (it == method_exit_listeners_.end());
    cur->MethodExited(thread, this_object, method, dex_pc, return_value);
  }
}

void Instrumentation::MethodUnwindEvent(Thread* thread, mirror::Object* this_object,
                                        mirror::ArtMethod* method,
                                        uint32_t dex_pc) const {
  if (have_method_unwind_listeners_) {
    for (InstrumentationListener* listener : method_unwind_listeners_) {
      listener->MethodUnwind(thread, this_object, method, dex_pc);
    }
  }
}

void Instrumentation::DexPcMovedEventImpl(Thread* thread, mirror::Object* this_object,
                                          mirror::ArtMethod* method,
                                          uint32_t dex_pc) const {
  // TODO: STL copy-on-write collection? The copy below is due to the debug listener having an
  // action where it can remove itself as a listener and break the iterator. The copy only works
  // around the problem and in general we may have to move to something like reference counting to
  // ensure listeners are deleted correctly.
  std::list<InstrumentationListener*> copy(dex_pc_listeners_);
  for (InstrumentationListener* listener : copy) {
    listener->DexPcMoved(thread, this_object, method, dex_pc);
  }
}

void Instrumentation::FieldReadEventImpl(Thread* thread, mirror::Object* this_object,
                                         mirror::ArtMethod* method, uint32_t dex_pc,
                                         mirror::ArtField* field) const {
  if (have_field_read_listeners_) {
    // TODO: same comment than DexPcMovedEventImpl.
    std::list<InstrumentationListener*> copy(field_read_listeners_);
    for (InstrumentationListener* listener : copy) {
      listener->FieldRead(thread, this_object, method, dex_pc, field);
    }
  }
}

void Instrumentation::FieldWriteEventImpl(Thread* thread, mirror::Object* this_object,
                                         mirror::ArtMethod* method, uint32_t dex_pc,
                                         mirror::ArtField* field, const JValue& field_value) const {
  if (have_field_write_listeners_) {
    // TODO: same comment than DexPcMovedEventImpl.
    std::list<InstrumentationListener*> copy(field_write_listeners_);
    for (InstrumentationListener* listener : copy) {
      listener->FieldWritten(thread, this_object, method, dex_pc, field, field_value);
    }
  }
}

void Instrumentation::ExceptionCaughtEvent(Thread* thread, const ThrowLocation& throw_location,
                                           mirror::ArtMethod* catch_method,
                                           uint32_t catch_dex_pc,
                                           mirror::Throwable* exception_object) const {
  if (have_exception_caught_listeners_) {
    DCHECK_EQ(thread->GetException(NULL), exception_object);
    thread->ClearException();
    // TODO: The copy below is due to the debug listener having an action where it can remove
    // itself as a listener and break the iterator. The copy only works around the problem.
    std::list<InstrumentationListener*> copy(exception_caught_listeners_);
    for (InstrumentationListener* listener : copy) {
      listener->ExceptionCaught(thread, throw_location, catch_method, catch_dex_pc, exception_object);
    }
    thread->SetException(throw_location, exception_object);
  }
}

static void CheckStackDepth(Thread* self, const InstrumentationStackFrame& instrumentation_frame,
                            int delta)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  size_t frame_id = StackVisitor::ComputeNumFrames(self) + delta;
  if (frame_id != instrumentation_frame.frame_id_) {
    LOG(ERROR) << "Expected frame_id=" << frame_id << " but found "
        << instrumentation_frame.frame_id_;
    StackVisitor::DescribeStack(self);
    CHECK_EQ(frame_id, instrumentation_frame.frame_id_);
  }
}

void Instrumentation::PushInstrumentationStackFrame(Thread* self, mirror::Object* this_object,
                                                    mirror::ArtMethod* method,
                                                    uintptr_t lr, bool interpreter_entry) {
  // We have a callee-save frame meaning this value is guaranteed to never be 0.
  size_t frame_id = StackVisitor::ComputeNumFrames(self);
  std::deque<instrumentation::InstrumentationStackFrame>* stack = self->GetInstrumentationStack();
  if (kVerboseInstrumentation) {
    LOG(INFO) << "Entering " << PrettyMethod(method) << " from PC " << reinterpret_cast<void*>(lr);
  }
  instrumentation::InstrumentationStackFrame instrumentation_frame(this_object, method, lr,
                                                                   frame_id, interpreter_entry);
  stack->push_front(instrumentation_frame);

  MethodEnterEvent(self, this_object, method, 0);
}

uint64_t Instrumentation::PopInstrumentationStackFrame(Thread* self, uintptr_t* return_pc,
                                                       uint64_t gpr_result, uint64_t fpr_result) {
  // Do the pop.
  std::deque<instrumentation::InstrumentationStackFrame>* stack = self->GetInstrumentationStack();
  CHECK_GT(stack->size(), 0U);
  InstrumentationStackFrame instrumentation_frame = stack->front();
  stack->pop_front();

  // Set return PC and check the sanity of the stack.
  *return_pc = instrumentation_frame.return_pc_;
  CheckStackDepth(self, instrumentation_frame, 0);

  mirror::ArtMethod* method = instrumentation_frame.method_;
  char return_shorty = MethodHelper(method).GetShorty()[0];
  JValue return_value;
  if (return_shorty == 'V') {
    return_value.SetJ(0);
  } else if (return_shorty == 'F' || return_shorty == 'D') {
    return_value.SetJ(fpr_result);
  } else {
    return_value.SetJ(gpr_result);
  }
  // TODO: improve the dex pc information here, requires knowledge of current PC as opposed to
  //       return_pc.
  uint32_t dex_pc = DexFile::kDexNoIndex;
  mirror::Object* this_object = instrumentation_frame.this_object_;
  MethodExitEvent(self, this_object, instrumentation_frame.method_, dex_pc, return_value);

  // Deoptimize if the caller needs to continue execution in the interpreter. Do nothing if we get
  // back to an upcall.
  NthCallerVisitor visitor(self, 1, true);
  visitor.WalkStack(true);
  bool deoptimize = (visitor.caller != NULL) &&
                    (interpreter_stubs_installed_ || IsDeoptimized(visitor.caller));
  if (deoptimize && kVerboseInstrumentation) {
    LOG(INFO) << "Deoptimizing into " << PrettyMethod(visitor.caller);
  }
  if (deoptimize) {
    if (kVerboseInstrumentation) {
      LOG(INFO) << "Deoptimizing from " << PrettyMethod(method)
                << " result is " << std::hex << return_value.GetJ();
    }
    self->SetDeoptimizationReturnValue(return_value);
    return static_cast<uint64_t>(GetQuickDeoptimizationEntryPoint()) |
        (static_cast<uint64_t>(*return_pc) << 32);
  } else {
    if (kVerboseInstrumentation) {
      LOG(INFO) << "Returning from " << PrettyMethod(method)
                << " to PC " << reinterpret_cast<void*>(*return_pc);
    }
    return *return_pc;
  }
}

void Instrumentation::PopMethodForUnwind(Thread* self, bool is_deoptimization) const {
  // Do the pop.
  std::deque<instrumentation::InstrumentationStackFrame>* stack = self->GetInstrumentationStack();
  CHECK_GT(stack->size(), 0U);
  InstrumentationStackFrame instrumentation_frame = stack->front();
  // TODO: bring back CheckStackDepth(self, instrumentation_frame, 2);
  stack->pop_front();

  mirror::ArtMethod* method = instrumentation_frame.method_;
  if (is_deoptimization) {
    if (kVerboseInstrumentation) {
      LOG(INFO) << "Popping for deoptimization " << PrettyMethod(method);
    }
  } else {
    if (kVerboseInstrumentation) {
      LOG(INFO) << "Popping for unwind " << PrettyMethod(method);
    }

    // Notify listeners of method unwind.
    // TODO: improve the dex pc information here, requires knowledge of current PC as opposed to
    //       return_pc.
    uint32_t dex_pc = DexFile::kDexNoIndex;
    MethodUnwindEvent(self, instrumentation_frame.this_object_, method, dex_pc);
  }
}

void Instrumentation::VisitRoots(RootCallback* callback, void* arg) {
  WriterMutexLock mu(Thread::Current(), deoptimized_methods_lock_);
  if (deoptimized_methods_.empty()) {
    return;
  }
  std::set<mirror::ArtMethod*> new_deoptimized_methods;
  for (mirror::ArtMethod* method : deoptimized_methods_) {
    DCHECK(method != nullptr);
    callback(reinterpret_cast<mirror::Object**>(&method), arg, 0, kRootVMInternal);
    new_deoptimized_methods.insert(method);
  }
  deoptimized_methods_ = new_deoptimized_methods;
}

std::string InstrumentationStackFrame::Dump() const {
  std::ostringstream os;
  os << "Frame " << frame_id_ << " " << PrettyMethod(method_) << ":"
      << reinterpret_cast<void*>(return_pc_) << " this=" << reinterpret_cast<void*>(this_object_);
  return os.str();
}

}  // namespace instrumentation
}  // namespace art

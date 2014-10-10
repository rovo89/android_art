/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "debugger.h"

#include <sys/uio.h>

#include <set>

#include "arch/context.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "field_helper.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "handle_scope.h"
#include "jdwp/object_registry.h"
#include "method_helper.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "mirror/throwable.h"
#include "quick/inline_method_analyser.h"
#include "reflection.h"
#include "safe_map.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "handle_scope-inl.h"
#include "thread_list.h"
#include "throw_location.h"
#include "utf.h"
#include "verifier/method_verifier-inl.h"
#include "well_known_classes.h"

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif

namespace art {

static const size_t kMaxAllocRecordStackDepth = 16;  // Max 255.
static const size_t kDefaultNumAllocRecords = 64*1024;  // Must be a power of 2. 2BE can hold 64k-1.

// Limit alloc_record_count to the 2BE value that is the limit of the current protocol.
static uint16_t CappedAllocRecordCount(size_t alloc_record_count) {
  if (alloc_record_count > 0xffff) {
    return 0xffff;
  }
  return alloc_record_count;
}

class AllocRecordStackTraceElement {
 public:
  AllocRecordStackTraceElement() : method_(nullptr), dex_pc_(0) {
  }

  int32_t LineNumber() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = Method();
    DCHECK(method != nullptr);
    return method->GetLineNumFromDexPC(DexPc());
  }

  mirror::ArtMethod* Method() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    return soa.DecodeMethod(method_);
  }

  void SetMethod(mirror::ArtMethod* m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    method_ = soa.EncodeMethod(m);
  }

  uint32_t DexPc() const {
    return dex_pc_;
  }

  void SetDexPc(uint32_t pc) {
    dex_pc_ = pc;
  }

 private:
  jmethodID method_;
  uint32_t dex_pc_;
};

jobject Dbg::TypeCache::Add(mirror::Class* t) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  int32_t hash_code = t->IdentityHashCode();
  auto range = objects_.equal_range(hash_code);
  for (auto it = range.first; it != range.second; ++it) {
    if (soa.Decode<mirror::Class*>(it->second) == t) {
      // Found a matching weak global, return it.
      return it->second;
    }
  }
  JNIEnv* env = soa.Env();
  const jobject local_ref = soa.AddLocalReference<jobject>(t);
  const jobject weak_global = env->NewWeakGlobalRef(local_ref);
  env->DeleteLocalRef(local_ref);
  objects_.insert(std::make_pair(hash_code, weak_global));
  return weak_global;
}

void Dbg::TypeCache::Clear() {
  JavaVMExt* vm = Runtime::Current()->GetJavaVM();
  Thread* self = Thread::Current();
  for (const auto& p : objects_) {
    vm->DeleteWeakGlobalRef(self, p.second);
  }
  objects_.clear();
}

class AllocRecord {
 public:
  AllocRecord() : type_(nullptr), byte_count_(0), thin_lock_id_(0) {}

  mirror::Class* Type() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return down_cast<mirror::Class*>(Thread::Current()->DecodeJObject(type_));
  }

  void SetType(mirror::Class* t) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_,
                                                       Locks::alloc_tracker_lock_) {
    type_ = Dbg::type_cache_.Add(t);
  }

  size_t GetDepth() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    size_t depth = 0;
    while (depth < kMaxAllocRecordStackDepth && stack_[depth].Method() != nullptr) {
      ++depth;
    }
    return depth;
  }

  size_t ByteCount() const {
    return byte_count_;
  }

  void SetByteCount(size_t count) {
    byte_count_ = count;
  }

  uint16_t ThinLockId() const {
    return thin_lock_id_;
  }

  void SetThinLockId(uint16_t id) {
    thin_lock_id_ = id;
  }

  AllocRecordStackTraceElement* StackElement(size_t index) {
    DCHECK_LT(index, kMaxAllocRecordStackDepth);
    return &stack_[index];
  }

 private:
  jobject type_;  // This is a weak global.
  size_t byte_count_;
  uint16_t thin_lock_id_;
  AllocRecordStackTraceElement stack_[kMaxAllocRecordStackDepth];  // Unused entries have nullptr method.
};

class Breakpoint {
 public:
  Breakpoint(mirror::ArtMethod* method, uint32_t dex_pc, bool need_full_deoptimization)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
    : method_(nullptr), dex_pc_(dex_pc), need_full_deoptimization_(need_full_deoptimization) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    method_ = soa.EncodeMethod(method);
  }

  Breakpoint(const Breakpoint& other) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
    : method_(nullptr), dex_pc_(other.dex_pc_),
      need_full_deoptimization_(other.need_full_deoptimization_) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    method_ = soa.EncodeMethod(other.Method());
  }

  mirror::ArtMethod* Method() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    return soa.DecodeMethod(method_);
  }

  uint32_t DexPc() const {
    return dex_pc_;
  }

  bool NeedFullDeoptimization() const {
    return need_full_deoptimization_;
  }

 private:
  // The location of this breakpoint.
  jmethodID method_;
  uint32_t dex_pc_;

  // Indicates whether breakpoint needs full deoptimization or selective deoptimization.
  bool need_full_deoptimization_;
};

static std::ostream& operator<<(std::ostream& os, const Breakpoint& rhs)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  os << StringPrintf("Breakpoint[%s @%#x]", PrettyMethod(rhs.Method()).c_str(), rhs.DexPc());
  return os;
}

class DebugInstrumentationListener FINAL : public instrumentation::InstrumentationListener {
 public:
  DebugInstrumentationListener() {}
  virtual ~DebugInstrumentationListener() {}

  void MethodEntered(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                     uint32_t dex_pc)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method->IsNative()) {
      // TODO: post location events is a suspension point and native method entry stubs aren't.
      return;
    }
    Dbg::UpdateDebugger(thread, this_object, method, 0, Dbg::kMethodEntry, nullptr);
  }

  void MethodExited(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                    uint32_t dex_pc, const JValue& return_value)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (method->IsNative()) {
      // TODO: post location events is a suspension point and native method entry stubs aren't.
      return;
    }
    Dbg::UpdateDebugger(thread, this_object, method, dex_pc, Dbg::kMethodExit, &return_value);
  }

  void MethodUnwind(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                    uint32_t dex_pc)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // We're not recorded to listen to this kind of event, so complain.
    LOG(ERROR) << "Unexpected method unwind event in debugger " << PrettyMethod(method)
               << " " << dex_pc;
  }

  void DexPcMoved(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                  uint32_t new_dex_pc)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::UpdateDebugger(thread, this_object, method, new_dex_pc, 0, nullptr);
  }

  void FieldRead(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                 uint32_t dex_pc, mirror::ArtField* field)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::PostFieldAccessEvent(method, dex_pc, this_object, field);
  }

  void FieldWritten(Thread* thread, mirror::Object* this_object, mirror::ArtMethod* method,
                    uint32_t dex_pc, mirror::ArtField* field, const JValue& field_value)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::PostFieldModificationEvent(method, dex_pc, this_object, field, &field_value);
  }

  void ExceptionCaught(Thread* thread, const ThrowLocation& throw_location,
                       mirror::ArtMethod* catch_method, uint32_t catch_dex_pc,
                       mirror::Throwable* exception_object)
      OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Dbg::PostException(throw_location, catch_method, catch_dex_pc, exception_object);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(DebugInstrumentationListener);
} gDebugInstrumentationListener;

// JDWP is allowed unless the Zygote forbids it.
static bool gJdwpAllowed = true;

// Was there a -Xrunjdwp or -agentlib:jdwp= argument on the command line?
static bool gJdwpConfigured = false;

// Broken-down JDWP options. (Only valid if IsJdwpConfigured() is true.)
static JDWP::JdwpOptions gJdwpOptions;

// Runtime JDWP state.
static JDWP::JdwpState* gJdwpState = nullptr;
static bool gDebuggerConnected;  // debugger or DDMS is connected.
static bool gDebuggerActive;     // debugger is making requests.
static bool gDisposed;           // debugger called VirtualMachine.Dispose, so we should drop the connection.

static bool gDdmThreadNotification = false;

// DDMS GC-related settings.
static Dbg::HpifWhen gDdmHpifWhen = Dbg::HPIF_WHEN_NEVER;
static Dbg::HpsgWhen gDdmHpsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmHpsgWhat;
static Dbg::HpsgWhen gDdmNhsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmNhsgWhat;

ObjectRegistry* Dbg::gRegistry = nullptr;

// Recent allocation tracking.
AllocRecord* Dbg::recent_allocation_records_ = nullptr;  // TODO: CircularBuffer<AllocRecord>
size_t Dbg::alloc_record_max_ = 0;
size_t Dbg::alloc_record_head_ = 0;
size_t Dbg::alloc_record_count_ = 0;
Dbg::TypeCache Dbg::type_cache_;

// Deoptimization support.
std::vector<DeoptimizationRequest> Dbg::deoptimization_requests_;
size_t Dbg::full_deoptimization_event_count_ = 0;
size_t Dbg::delayed_full_undeoptimization_count_ = 0;

// Instrumentation event reference counters.
size_t Dbg::dex_pc_change_event_ref_count_ = 0;
size_t Dbg::method_enter_event_ref_count_ = 0;
size_t Dbg::method_exit_event_ref_count_ = 0;
size_t Dbg::field_read_event_ref_count_ = 0;
size_t Dbg::field_write_event_ref_count_ = 0;
size_t Dbg::exception_catch_event_ref_count_ = 0;
uint32_t Dbg::instrumentation_events_ = 0;

// Breakpoints.
static std::vector<Breakpoint> gBreakpoints GUARDED_BY(Locks::breakpoint_lock_);

void DebugInvokeReq::VisitRoots(RootCallback* callback, void* arg, uint32_t tid,
                                RootType root_type) {
  if (receiver != nullptr) {
    callback(&receiver, arg, tid, root_type);
  }
  if (thread != nullptr) {
    callback(&thread, arg, tid, root_type);
  }
  if (klass != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&klass), arg, tid, root_type);
  }
  if (method != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&method), arg, tid, root_type);
  }
}

void DebugInvokeReq::Clear() {
  invoke_needed = false;
  receiver = nullptr;
  thread = nullptr;
  klass = nullptr;
  method = nullptr;
}

void SingleStepControl::VisitRoots(RootCallback* callback, void* arg, uint32_t tid,
                                   RootType root_type) {
  if (method != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&method), arg, tid, root_type);
  }
}

bool SingleStepControl::ContainsDexPc(uint32_t dex_pc) const {
  return dex_pcs.find(dex_pc) == dex_pcs.end();
}

void SingleStepControl::Clear() {
  is_active = false;
  method = nullptr;
  dex_pcs.clear();
}

static bool IsBreakpoint(const mirror::ArtMethod* m, uint32_t dex_pc)
    LOCKS_EXCLUDED(Locks::breakpoint_lock_)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  for (size_t i = 0, e = gBreakpoints.size(); i < e; ++i) {
    if (gBreakpoints[i].DexPc() == dex_pc && gBreakpoints[i].Method() == m) {
      VLOG(jdwp) << "Hit breakpoint #" << i << ": " << gBreakpoints[i];
      return true;
    }
  }
  return false;
}

static bool IsSuspendedForDebugger(ScopedObjectAccessUnchecked& soa, Thread* thread)
    LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_) {
  MutexLock mu(soa.Self(), *Locks::thread_suspend_count_lock_);
  // A thread may be suspended for GC; in this code, we really want to know whether
  // there's a debugger suspension active.
  return thread->IsSuspended() && thread->GetDebugSuspendCount() > 0;
}

static mirror::Array* DecodeNonNullArray(JDWP::RefTypeId id, JDWP::JdwpError* error)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* o = Dbg::GetObjectRegistry()->Get<mirror::Object*>(id, error);
  if (o == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }
  if (!o->IsArrayInstance()) {
    *error = JDWP::ERR_INVALID_ARRAY;
    return nullptr;
  }
  *error = JDWP::ERR_NONE;
  return o->AsArray();
}

static mirror::Class* DecodeClass(JDWP::RefTypeId id, JDWP::JdwpError* error)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* o = Dbg::GetObjectRegistry()->Get<mirror::Object*>(id, error);
  if (o == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }
  if (!o->IsClass()) {
    *error = JDWP::ERR_INVALID_CLASS;
    return nullptr;
  }
  *error = JDWP::ERR_NONE;
  return o->AsClass();
}

static Thread* DecodeThread(ScopedObjectAccessUnchecked& soa, JDWP::ObjectId thread_id,
                            JDWP::JdwpError* error)
    EXCLUSIVE_LOCKS_REQUIRED(Locks::thread_list_lock_)
    LOCKS_EXCLUDED(Locks::thread_suspend_count_lock_)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* thread_peer = Dbg::GetObjectRegistry()->Get<mirror::Object*>(thread_id, error);
  if (thread_peer == nullptr) {
    // This isn't even an object.
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }

  mirror::Class* java_lang_Thread = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread);
  if (!java_lang_Thread->IsAssignableFrom(thread_peer->GetClass())) {
    // This isn't a thread.
    *error = JDWP::ERR_INVALID_THREAD;
    return nullptr;
  }

  Thread* thread = Thread::FromManagedThread(soa, thread_peer);
  // If thread is null then this a java.lang.Thread without a Thread*. Must be a un-started or a
  // zombie.
  *error = (thread == nullptr) ? JDWP::ERR_THREAD_NOT_ALIVE : JDWP::ERR_NONE;
  return thread;
}

static JDWP::JdwpTag BasicTagFromDescriptor(const char* descriptor) {
  // JDWP deliberately uses the descriptor characters' ASCII values for its enum.
  // Note that by "basic" we mean that we don't get more specific than JT_OBJECT.
  return static_cast<JDWP::JdwpTag>(descriptor[0]);
}

static JDWP::JdwpTag BasicTagFromClass(mirror::Class* klass)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string temp;
  const char* descriptor = klass->GetDescriptor(&temp);
  return BasicTagFromDescriptor(descriptor);
}

static JDWP::JdwpTag TagFromClass(const ScopedObjectAccessUnchecked& soa, mirror::Class* c)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(c != nullptr);
  if (c->IsArrayClass()) {
    return JDWP::JT_ARRAY;
  }
  if (c->IsStringClass()) {
    return JDWP::JT_STRING;
  }
  if (c->IsClassClass()) {
    return JDWP::JT_CLASS_OBJECT;
  }
  {
    mirror::Class* thread_class = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread);
    if (thread_class->IsAssignableFrom(c)) {
      return JDWP::JT_THREAD;
    }
  }
  {
    mirror::Class* thread_group_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ThreadGroup);
    if (thread_group_class->IsAssignableFrom(c)) {
      return JDWP::JT_THREAD_GROUP;
    }
  }
  {
    mirror::Class* class_loader_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ClassLoader);
    if (class_loader_class->IsAssignableFrom(c)) {
      return JDWP::JT_CLASS_LOADER;
    }
  }
  return JDWP::JT_OBJECT;
}

/*
 * Objects declared to hold Object might actually hold a more specific
 * type.  The debugger may take a special interest in these (e.g. it
 * wants to display the contents of Strings), so we want to return an
 * appropriate tag.
 *
 * Null objects are tagged JT_OBJECT.
 */
JDWP::JdwpTag Dbg::TagFromObject(const ScopedObjectAccessUnchecked& soa, mirror::Object* o) {
  return (o == nullptr) ? JDWP::JT_OBJECT : TagFromClass(soa, o->GetClass());
}

static bool IsPrimitiveTag(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_BOOLEAN:
  case JDWP::JT_BYTE:
  case JDWP::JT_CHAR:
  case JDWP::JT_FLOAT:
  case JDWP::JT_DOUBLE:
  case JDWP::JT_INT:
  case JDWP::JT_LONG:
  case JDWP::JT_SHORT:
  case JDWP::JT_VOID:
    return true;
  default:
    return false;
  }
}

/*
 * Handle one of the JDWP name/value pairs.
 *
 * JDWP options are:
 *  help: if specified, show help message and bail
 *  transport: may be dt_socket or dt_shmem
 *  address: for dt_socket, "host:port", or just "port" when listening
 *  server: if "y", wait for debugger to attach; if "n", attach to debugger
 *  timeout: how long to wait for debugger to connect / listen
 *
 * Useful with server=n (these aren't supported yet):
 *  onthrow=<exception-name>: connect to debugger when exception thrown
 *  onuncaught=y|n: connect to debugger when uncaught exception thrown
 *  launch=<command-line>: launch the debugger itself
 *
 * The "transport" option is required, as is "address" if server=n.
 */
static bool ParseJdwpOption(const std::string& name, const std::string& value) {
  if (name == "transport") {
    if (value == "dt_socket") {
      gJdwpOptions.transport = JDWP::kJdwpTransportSocket;
    } else if (value == "dt_android_adb") {
      gJdwpOptions.transport = JDWP::kJdwpTransportAndroidAdb;
    } else {
      LOG(ERROR) << "JDWP transport not supported: " << value;
      return false;
    }
  } else if (name == "server") {
    if (value == "n") {
      gJdwpOptions.server = false;
    } else if (value == "y") {
      gJdwpOptions.server = true;
    } else {
      LOG(ERROR) << "JDWP option 'server' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "suspend") {
    if (value == "n") {
      gJdwpOptions.suspend = false;
    } else if (value == "y") {
      gJdwpOptions.suspend = true;
    } else {
      LOG(ERROR) << "JDWP option 'suspend' must be 'y' or 'n'";
      return false;
    }
  } else if (name == "address") {
    /* this is either <port> or <host>:<port> */
    std::string port_string;
    gJdwpOptions.host.clear();
    std::string::size_type colon = value.find(':');
    if (colon != std::string::npos) {
      gJdwpOptions.host = value.substr(0, colon);
      port_string = value.substr(colon + 1);
    } else {
      port_string = value;
    }
    if (port_string.empty()) {
      LOG(ERROR) << "JDWP address missing port: " << value;
      return false;
    }
    char* end;
    uint64_t port = strtoul(port_string.c_str(), &end, 10);
    if (*end != '\0' || port > 0xffff) {
      LOG(ERROR) << "JDWP address has junk in port field: " << value;
      return false;
    }
    gJdwpOptions.port = port;
  } else if (name == "launch" || name == "onthrow" || name == "oncaught" || name == "timeout") {
    /* valid but unsupported */
    LOG(INFO) << "Ignoring JDWP option '" << name << "'='" << value << "'";
  } else {
    LOG(INFO) << "Ignoring unrecognized JDWP option '" << name << "'='" << value << "'";
  }

  return true;
}

/*
 * Parse the latter half of a -Xrunjdwp/-agentlib:jdwp= string, e.g.:
 * "transport=dt_socket,address=8000,server=y,suspend=n"
 */
bool Dbg::ParseJdwpOptions(const std::string& options) {
  VLOG(jdwp) << "ParseJdwpOptions: " << options;

  std::vector<std::string> pairs;
  Split(options, ',', pairs);

  for (size_t i = 0; i < pairs.size(); ++i) {
    std::string::size_type equals = pairs[i].find('=');
    if (equals == std::string::npos) {
      LOG(ERROR) << "Can't parse JDWP option '" << pairs[i] << "' in '" << options << "'";
      return false;
    }
    ParseJdwpOption(pairs[i].substr(0, equals), pairs[i].substr(equals + 1));
  }

  if (gJdwpOptions.transport == JDWP::kJdwpTransportUnknown) {
    LOG(ERROR) << "Must specify JDWP transport: " << options;
  }
  if (!gJdwpOptions.server && (gJdwpOptions.host.empty() || gJdwpOptions.port == 0)) {
    LOG(ERROR) << "Must specify JDWP host and port when server=n: " << options;
    return false;
  }

  gJdwpConfigured = true;
  return true;
}

void Dbg::StartJdwp() {
  if (!gJdwpAllowed || !IsJdwpConfigured()) {
    // No JDWP for you!
    return;
  }

  CHECK(gRegistry == nullptr);
  gRegistry = new ObjectRegistry;

  // Init JDWP if the debugger is enabled. This may connect out to a
  // debugger, passively listen for a debugger, or block waiting for a
  // debugger.
  gJdwpState = JDWP::JdwpState::Create(&gJdwpOptions);
  if (gJdwpState == nullptr) {
    // We probably failed because some other process has the port already, which means that
    // if we don't abort the user is likely to think they're talking to us when they're actually
    // talking to that other process.
    LOG(FATAL) << "Debugger thread failed to initialize";
  }

  // If a debugger has already attached, send the "welcome" message.
  // This may cause us to suspend all threads.
  if (gJdwpState->IsActive()) {
    ScopedObjectAccess soa(Thread::Current());
    if (!gJdwpState->PostVMStart()) {
      LOG(WARNING) << "Failed to post 'start' message to debugger";
    }
  }
}

void Dbg::StopJdwp() {
  // Post VM_DEATH event before the JDWP connection is closed (either by the JDWP thread or the
  // destruction of gJdwpState).
  if (gJdwpState != nullptr && gJdwpState->IsActive()) {
    gJdwpState->PostVMDeath();
  }
  // Prevent the JDWP thread from processing JDWP incoming packets after we close the connection.
  Disposed();
  delete gJdwpState;
  gJdwpState = nullptr;
  delete gRegistry;
  gRegistry = nullptr;
}

void Dbg::GcDidFinish() {
  if (gDdmHpifWhen != HPIF_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Sending heap info to DDM";
    DdmSendHeapInfo(gDdmHpifWhen);
  }
  if (gDdmHpsgWhen != HPSG_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Dumping heap to DDM";
    DdmSendHeapSegments(false);
  }
  if (gDdmNhsgWhen != HPSG_WHEN_NEVER) {
    ScopedObjectAccess soa(Thread::Current());
    VLOG(jdwp) << "Dumping native heap to DDM";
    DdmSendHeapSegments(true);
  }
}

void Dbg::SetJdwpAllowed(bool allowed) {
  gJdwpAllowed = allowed;
}

DebugInvokeReq* Dbg::GetInvokeReq() {
  return Thread::Current()->GetInvokeReq();
}

Thread* Dbg::GetDebugThread() {
  return (gJdwpState != nullptr) ? gJdwpState->GetDebugThread() : nullptr;
}

void Dbg::ClearWaitForEventThread() {
  gJdwpState->ClearWaitForEventThread();
}

void Dbg::Connected() {
  CHECK(!gDebuggerConnected);
  VLOG(jdwp) << "JDWP has attached";
  gDebuggerConnected = true;
  gDisposed = false;
}

void Dbg::Disposed() {
  gDisposed = true;
}

bool Dbg::IsDisposed() {
  return gDisposed;
}

void Dbg::GoActive() {
  // Enable all debugging features, including scans for breakpoints.
  // This is a no-op if we're already active.
  // Only called from the JDWP handler thread.
  if (gDebuggerActive) {
    return;
  }

  {
    // TODO: dalvik only warned if there were breakpoints left over. clear in Dbg::Disconnected?
    ReaderMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
    CHECK_EQ(gBreakpoints.size(), 0U);
  }

  {
    MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
    CHECK_EQ(deoptimization_requests_.size(), 0U);
    CHECK_EQ(full_deoptimization_event_count_, 0U);
    CHECK_EQ(delayed_full_undeoptimization_count_, 0U);
    CHECK_EQ(dex_pc_change_event_ref_count_, 0U);
    CHECK_EQ(method_enter_event_ref_count_, 0U);
    CHECK_EQ(method_exit_event_ref_count_, 0U);
    CHECK_EQ(field_read_event_ref_count_, 0U);
    CHECK_EQ(field_write_event_ref_count_, 0U);
    CHECK_EQ(exception_catch_event_ref_count_, 0U);
  }

  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  Thread* self = Thread::Current();
  ThreadState old_state = self->SetStateUnsafe(kRunnable);
  CHECK_NE(old_state, kRunnable);
  runtime->GetInstrumentation()->EnableDeoptimization();
  instrumentation_events_ = 0;
  gDebuggerActive = true;
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
  runtime->GetThreadList()->ResumeAll();

  LOG(INFO) << "Debugger is active";
}

void Dbg::Disconnected() {
  CHECK(gDebuggerConnected);

  LOG(INFO) << "Debugger is no longer active";

  // Suspend all threads and exclusively acquire the mutator lock. Set the state of the thread
  // to kRunnable to avoid scoped object access transitions. Remove the debugger as a listener
  // and clear the object registry.
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  Thread* self = Thread::Current();
  ThreadState old_state = self->SetStateUnsafe(kRunnable);

  // Debugger may not be active at this point.
  if (gDebuggerActive) {
    {
      // Since we're going to disable deoptimization, we clear the deoptimization requests queue.
      // This prevents us from having any pending deoptimization request when the debugger attaches
      // to us again while no event has been requested yet.
      MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
      deoptimization_requests_.clear();
      full_deoptimization_event_count_ = 0U;
      delayed_full_undeoptimization_count_ = 0U;
    }
    if (instrumentation_events_ != 0) {
      runtime->GetInstrumentation()->RemoveListener(&gDebugInstrumentationListener,
                                                    instrumentation_events_);
      instrumentation_events_ = 0;
    }
    runtime->GetInstrumentation()->DisableDeoptimization();
    gDebuggerActive = false;
  }
  gRegistry->Clear();
  gDebuggerConnected = false;
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
  runtime->GetThreadList()->ResumeAll();
}

bool Dbg::IsDebuggerActive() {
  return gDebuggerActive;
}

bool Dbg::IsJdwpConfigured() {
  return gJdwpConfigured;
}

int64_t Dbg::LastDebuggerActivity() {
  return gJdwpState->LastDebuggerActivity();
}

void Dbg::UndoDebuggerSuspensions() {
  Runtime::Current()->GetThreadList()->UndoDebuggerSuspensions();
}

std::string Dbg::GetClassName(JDWP::RefTypeId class_id) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(class_id, &error);
  if (o == nullptr) {
    if (error == JDWP::ERR_NONE) {
      return "NULL";
    } else {
      return StringPrintf("invalid object %p", reinterpret_cast<void*>(class_id));
    }
  }
  if (!o->IsClass()) {
    return StringPrintf("non-class %p", o);  // This is only used for debugging output anyway.
  }
  return GetClassName(o->AsClass());
}

std::string Dbg::GetClassName(mirror::Class* klass) {
  if (klass == nullptr) {
    return "NULL";
  }
  std::string temp;
  return DescriptorToName(klass->GetDescriptor(&temp));
}

JDWP::JdwpError Dbg::GetClassObject(JDWP::RefTypeId id, JDWP::ObjectId* class_object_id) {
  JDWP::JdwpError status;
  mirror::Class* c = DecodeClass(id, &status);
  if (c == nullptr) {
    *class_object_id = 0;
    return status;
  }
  *class_object_id = gRegistry->Add(c);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSuperclass(JDWP::RefTypeId id, JDWP::RefTypeId* superclass_id) {
  JDWP::JdwpError status;
  mirror::Class* c = DecodeClass(id, &status);
  if (c == nullptr) {
    *superclass_id = 0;
    return status;
  }
  if (c->IsInterface()) {
    // http://code.google.com/p/android/issues/detail?id=20856
    *superclass_id = 0;
  } else {
    *superclass_id = gRegistry->Add(c->GetSuperClass());
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetClassLoader(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  expandBufAddObjectId(pReply, gRegistry->Add(o->GetClass()->GetClassLoader()));
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetModifiers(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(id, &error);
  if (c == nullptr) {
    return error;
  }

  uint32_t access_flags = c->GetAccessFlags() & kAccJavaFlagsMask;

  // Set ACC_SUPER. Dex files don't contain this flag but only classes are supposed to have it set,
  // not interfaces.
  // Class.getModifiers doesn't return it, but JDWP does, so we set it here.
  if ((access_flags & kAccInterface) == 0) {
    access_flags |= kAccSuper;
  }

  expandBufAdd4BE(pReply, access_flags);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetMonitorInfo(JDWP::ObjectId object_id, JDWP::ExpandBuf* reply) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }

  // Ensure all threads are suspended while we read objects' lock words.
  Thread* self = Thread::Current();
  CHECK_EQ(self->GetState(), kRunnable);
  self->TransitionFromRunnableToSuspended(kSuspended);
  Runtime::Current()->GetThreadList()->SuspendAll();

  MonitorInfo monitor_info(o);

  Runtime::Current()->GetThreadList()->ResumeAll();
  self->TransitionFromSuspendedToRunnable();

  if (monitor_info.owner_ != nullptr) {
    expandBufAddObjectId(reply, gRegistry->Add(monitor_info.owner_->GetPeer()));
  } else {
    expandBufAddObjectId(reply, gRegistry->Add(nullptr));
  }
  expandBufAdd4BE(reply, monitor_info.entry_count_);
  expandBufAdd4BE(reply, monitor_info.waiters_.size());
  for (size_t i = 0; i < monitor_info.waiters_.size(); ++i) {
    expandBufAddObjectId(reply, gRegistry->Add(monitor_info.waiters_[i]->GetPeer()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetOwnedMonitors(JDWP::ObjectId thread_id,
                                      std::vector<JDWP::ObjectId>* monitors,
                                      std::vector<uint32_t>* stack_depths) {
  struct OwnedMonitorVisitor : public StackVisitor {
    OwnedMonitorVisitor(Thread* thread, Context* context,
                        std::vector<JDWP::ObjectId>* monitor_vector,
                        std::vector<uint32_t>* stack_depth_vector)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), current_stack_depth(0),
        monitors(monitor_vector), stack_depths(stack_depth_vector) {}

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
      if (!GetMethod()->IsRuntimeMethod()) {
        Monitor::VisitLocks(this, AppendOwnedMonitors, this);
        ++current_stack_depth;
      }
      return true;
    }

    static void AppendOwnedMonitors(mirror::Object* owned_monitor, void* arg)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      OwnedMonitorVisitor* visitor = reinterpret_cast<OwnedMonitorVisitor*>(arg);
      visitor->monitors->push_back(gRegistry->Add(owned_monitor));
      visitor->stack_depths->push_back(visitor->current_stack_depth);
    }

    size_t current_stack_depth;
    std::vector<JDWP::ObjectId>* const monitors;
    std::vector<uint32_t>* const stack_depths;
  };

  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    thread = DecodeThread(soa, thread_id, &error);
    if (thread == nullptr) {
      return error;
    }
    if (!IsSuspendedForDebugger(soa, thread)) {
      return JDWP::ERR_THREAD_NOT_SUSPENDED;
    }
  }
  std::unique_ptr<Context> context(Context::Create());
  OwnedMonitorVisitor visitor(thread, context.get(), monitors, stack_depths);
  visitor.WalkStack();
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetContendedMonitor(JDWP::ObjectId thread_id,
                                         JDWP::ObjectId* contended_monitor) {
  mirror::Object* contended_monitor_obj;
  ScopedObjectAccessUnchecked soa(Thread::Current());
  *contended_monitor = 0;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    Thread* thread = DecodeThread(soa, thread_id, &error);
    if (thread == nullptr) {
      return error;
    }
    if (!IsSuspendedForDebugger(soa, thread)) {
      return JDWP::ERR_THREAD_NOT_SUSPENDED;
    }
    contended_monitor_obj = Monitor::GetContendedMonitor(thread);
  }
  // Add() requires the thread_list_lock_ not held to avoid the lock
  // level violation.
  *contended_monitor = gRegistry->Add(contended_monitor_obj);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetInstanceCounts(const std::vector<JDWP::RefTypeId>& class_ids,
                                       std::vector<uint64_t>* counts) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(false);
  std::vector<mirror::Class*> classes;
  counts->clear();
  for (size_t i = 0; i < class_ids.size(); ++i) {
    JDWP::JdwpError error;
    mirror::Class* c = DecodeClass(class_ids[i], &error);
    if (c == nullptr) {
      return error;
    }
    classes.push_back(c);
    counts->push_back(0);
  }
  heap->CountInstances(classes, false, &(*counts)[0]);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetInstances(JDWP::RefTypeId class_id, int32_t max_count,
                                  std::vector<JDWP::ObjectId>* instances) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  // We only want reachable instances, so do a GC.
  heap->CollectGarbage(false);
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  std::vector<mirror::Object*> raw_instances;
  Runtime::Current()->GetHeap()->GetInstances(c, max_count, raw_instances);
  for (size_t i = 0; i < raw_instances.size(); ++i) {
    instances->push_back(gRegistry->Add(raw_instances[i]));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetReferringObjects(JDWP::ObjectId object_id, int32_t max_count,
                                         std::vector<JDWP::ObjectId>* referring_objects) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  heap->CollectGarbage(false);
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  std::vector<mirror::Object*> raw_instances;
  heap->GetReferringObjects(o, max_count, raw_instances);
  for (size_t i = 0; i < raw_instances.size(); ++i) {
    referring_objects->push_back(gRegistry->Add(raw_instances[i]));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::DisableCollection(JDWP::ObjectId object_id) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  gRegistry->DisableCollection(object_id);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::EnableCollection(JDWP::ObjectId object_id) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  // Unlike DisableCollection, JDWP specs do not state an invalid object causes an error. The RI
  // also ignores these cases and never return an error. However it's not obvious why this command
  // should behave differently from DisableCollection and IsCollected commands. So let's be more
  // strict and return an error if this happens.
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  gRegistry->EnableCollection(object_id);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::IsCollected(JDWP::ObjectId object_id, bool* is_collected) {
  *is_collected = true;
  if (object_id == 0) {
    // Null object id is invalid.
    return JDWP::ERR_INVALID_OBJECT;
  }
  // JDWP specs state an INVALID_OBJECT error is returned if the object ID is not valid. However
  // the RI seems to ignore this and assume object has been collected.
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o != nullptr) {
    *is_collected = gRegistry->IsCollected(object_id);
  }
  return JDWP::ERR_NONE;
}

void Dbg::DisposeObject(JDWP::ObjectId object_id, uint32_t reference_count) {
  gRegistry->DisposeObject(object_id, reference_count);
}

JDWP::JdwpTypeTag Dbg::GetTypeTag(mirror::Class* klass) {
  DCHECK(klass != nullptr);
  if (klass->IsArrayClass()) {
    return JDWP::TT_ARRAY;
  } else if (klass->IsInterface()) {
    return JDWP::TT_INTERFACE;
  } else {
    return JDWP::TT_CLASS;
  }
}

JDWP::JdwpError Dbg::GetReflectedType(JDWP::RefTypeId class_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  JDWP::JdwpTypeTag type_tag = GetTypeTag(c);
  expandBufAdd1(pReply, type_tag);
  expandBufAddRefTypeId(pReply, class_id);
  return JDWP::ERR_NONE;
}

void Dbg::GetClassList(std::vector<JDWP::RefTypeId>* classes) {
  // Get the complete list of reference classes (i.e. all classes except
  // the primitive types).
  // Returns a newly-allocated buffer full of RefTypeId values.
  struct ClassListCreator {
    explicit ClassListCreator(std::vector<JDWP::RefTypeId>* classes) : classes(classes) {
    }

    static bool Visit(mirror::Class* c, void* arg) {
      return reinterpret_cast<ClassListCreator*>(arg)->Visit(c);
    }

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    bool Visit(mirror::Class* c) NO_THREAD_SAFETY_ANALYSIS {
      if (!c->IsPrimitive()) {
        classes->push_back(gRegistry->AddRefType(c));
      }
      return true;
    }

    std::vector<JDWP::RefTypeId>* const classes;
  };

  ClassListCreator clc(classes);
  Runtime::Current()->GetClassLinker()->VisitClassesWithoutClassesLock(ClassListCreator::Visit,
                                                                       &clc);
}

JDWP::JdwpError Dbg::GetClassInfo(JDWP::RefTypeId class_id, JDWP::JdwpTypeTag* pTypeTag,
                                  uint32_t* pStatus, std::string* pDescriptor) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  if (c->IsArrayClass()) {
    *pStatus = JDWP::CS_VERIFIED | JDWP::CS_PREPARED;
    *pTypeTag = JDWP::TT_ARRAY;
  } else {
    if (c->IsErroneous()) {
      *pStatus = JDWP::CS_ERROR;
    } else {
      *pStatus = JDWP::CS_VERIFIED | JDWP::CS_PREPARED | JDWP::CS_INITIALIZED;
    }
    *pTypeTag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
  }

  if (pDescriptor != nullptr) {
    std::string temp;
    *pDescriptor = c->GetDescriptor(&temp);
  }
  return JDWP::ERR_NONE;
}

void Dbg::FindLoadedClassBySignature(const char* descriptor, std::vector<JDWP::RefTypeId>* ids) {
  std::vector<mirror::Class*> classes;
  Runtime::Current()->GetClassLinker()->LookupClasses(descriptor, classes);
  ids->clear();
  for (size_t i = 0; i < classes.size(); ++i) {
    ids->push_back(gRegistry->Add(classes[i]));
  }
}

JDWP::JdwpError Dbg::GetReferenceType(JDWP::ObjectId object_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (o == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }

  JDWP::JdwpTypeTag type_tag = GetTypeTag(o->GetClass());
  JDWP::RefTypeId type_id = gRegistry->AddRefType(o->GetClass());

  expandBufAdd1(pReply, type_tag);
  expandBufAddRefTypeId(pReply, type_id);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSignature(JDWP::RefTypeId class_id, std::string* signature) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  std::string temp;
  *signature = c->GetDescriptor(&temp);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetSourceFile(JDWP::RefTypeId class_id, std::string* result) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }
  const char* source_file = c->GetSourceFile();
  if (source_file == nullptr) {
    return JDWP::ERR_ABSENT_INFORMATION;
  }
  *result = source_file;
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetObjectTag(JDWP::ObjectId object_id, uint8_t* tag) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  mirror::Object* o = gRegistry->Get<mirror::Object*>(object_id, &error);
  if (error != JDWP::ERR_NONE) {
    *tag = JDWP::JT_VOID;
    return error;
  }
  *tag = TagFromObject(soa, o);
  return JDWP::ERR_NONE;
}

size_t Dbg::GetTagWidth(JDWP::JdwpTag tag) {
  switch (tag) {
  case JDWP::JT_VOID:
    return 0;
  case JDWP::JT_BYTE:
  case JDWP::JT_BOOLEAN:
    return 1;
  case JDWP::JT_CHAR:
  case JDWP::JT_SHORT:
    return 2;
  case JDWP::JT_FLOAT:
  case JDWP::JT_INT:
    return 4;
  case JDWP::JT_ARRAY:
  case JDWP::JT_OBJECT:
  case JDWP::JT_STRING:
  case JDWP::JT_THREAD:
  case JDWP::JT_THREAD_GROUP:
  case JDWP::JT_CLASS_LOADER:
  case JDWP::JT_CLASS_OBJECT:
    return sizeof(JDWP::ObjectId);
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    return 8;
  default:
    LOG(FATAL) << "Unknown tag " << tag;
    return -1;
  }
}

JDWP::JdwpError Dbg::GetArrayLength(JDWP::ObjectId array_id, int32_t* length) {
  JDWP::JdwpError error;
  mirror::Array* a = DecodeNonNullArray(array_id, &error);
  if (a == nullptr) {
    return error;
  }
  *length = a->GetLength();
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputArray(JDWP::ObjectId array_id, int offset, int count, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Array* a = DecodeNonNullArray(array_id, &error);
  if (a == nullptr) {
    return error;
  }

  if (offset < 0 || count < 0 || offset > a->GetLength() || a->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return JDWP::ERR_INVALID_LENGTH;
  }
  JDWP::JdwpTag element_tag = BasicTagFromClass(a->GetClass()->GetComponentType());
  expandBufAdd1(pReply, element_tag);
  expandBufAdd4BE(pReply, count);

  if (IsPrimitiveTag(element_tag)) {
    size_t width = GetTagWidth(element_tag);
    uint8_t* dst = expandBufAddSpace(pReply, count * width);
    if (width == 8) {
      const uint64_t* src8 = reinterpret_cast<uint64_t*>(a->GetRawData(sizeof(uint64_t), 0));
      for (int i = 0; i < count; ++i) JDWP::Write8BE(&dst, src8[offset + i]);
    } else if (width == 4) {
      const uint32_t* src4 = reinterpret_cast<uint32_t*>(a->GetRawData(sizeof(uint32_t), 0));
      for (int i = 0; i < count; ++i) JDWP::Write4BE(&dst, src4[offset + i]);
    } else if (width == 2) {
      const uint16_t* src2 = reinterpret_cast<uint16_t*>(a->GetRawData(sizeof(uint16_t), 0));
      for (int i = 0; i < count; ++i) JDWP::Write2BE(&dst, src2[offset + i]);
    } else {
      const uint8_t* src = reinterpret_cast<uint8_t*>(a->GetRawData(sizeof(uint8_t), 0));
      memcpy(dst, &src[offset * width], count * width);
    }
  } else {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    mirror::ObjectArray<mirror::Object>* oa = a->AsObjectArray<mirror::Object>();
    for (int i = 0; i < count; ++i) {
      mirror::Object* element = oa->Get(offset + i);
      JDWP::JdwpTag specific_tag = (element != nullptr) ? TagFromObject(soa, element)
                                                        : element_tag;
      expandBufAdd1(pReply, specific_tag);
      expandBufAddObjectId(pReply, gRegistry->Add(element));
    }
  }

  return JDWP::ERR_NONE;
}

template <typename T>
static void CopyArrayData(mirror::Array* a, JDWP::Request* src, int offset, int count)
    NO_THREAD_SAFETY_ANALYSIS {
  // TODO: fix when annotalysis correctly handles non-member functions.
  DCHECK(a->GetClass()->IsPrimitiveArray());

  T* dst = reinterpret_cast<T*>(a->GetRawData(sizeof(T), offset));
  for (int i = 0; i < count; ++i) {
    *dst++ = src->ReadValue(sizeof(T));
  }
}

JDWP::JdwpError Dbg::SetArrayElements(JDWP::ObjectId array_id, int offset, int count,
                                      JDWP::Request* request) {
  JDWP::JdwpError error;
  mirror::Array* dst = DecodeNonNullArray(array_id, &error);
  if (dst == nullptr) {
    return error;
  }

  if (offset < 0 || count < 0 || offset > dst->GetLength() || dst->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return JDWP::ERR_INVALID_LENGTH;
  }
  JDWP::JdwpTag element_tag = BasicTagFromClass(dst->GetClass()->GetComponentType());

  if (IsPrimitiveTag(element_tag)) {
    size_t width = GetTagWidth(element_tag);
    if (width == 8) {
      CopyArrayData<uint64_t>(dst, request, offset, count);
    } else if (width == 4) {
      CopyArrayData<uint32_t>(dst, request, offset, count);
    } else if (width == 2) {
      CopyArrayData<uint16_t>(dst, request, offset, count);
    } else {
      CopyArrayData<uint8_t>(dst, request, offset, count);
    }
  } else {
    mirror::ObjectArray<mirror::Object>* oa = dst->AsObjectArray<mirror::Object>();
    for (int i = 0; i < count; ++i) {
      JDWP::ObjectId id = request->ReadObjectId();
      JDWP::JdwpError error;
      mirror::Object* o = gRegistry->Get<mirror::Object*>(id, &error);
      if (error != JDWP::ERR_NONE) {
        return error;
      }
      oa->Set<false>(offset + i, o);
    }
  }

  return JDWP::ERR_NONE;
}

JDWP::ObjectId Dbg::CreateString(const std::string& str) {
  return gRegistry->Add(mirror::String::AllocFromModifiedUtf8(Thread::Current(), str.c_str()));
}

JDWP::JdwpError Dbg::CreateObject(JDWP::RefTypeId class_id, JDWP::ObjectId* new_object) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    *new_object = 0;
    return error;
  }
  *new_object = gRegistry->Add(c->AllocObject(Thread::Current()));
  return JDWP::ERR_NONE;
}

/*
 * Used by Eclipse's "Display" view to evaluate "new byte[5]" to get "(byte[]) [0, 0, 0, 0, 0]".
 */
JDWP::JdwpError Dbg::CreateArrayObject(JDWP::RefTypeId array_class_id, uint32_t length,
                                       JDWP::ObjectId* new_array) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(array_class_id, &error);
  if (c == nullptr) {
    *new_array = 0;
    return error;
  }
  *new_array = gRegistry->Add(mirror::Array::Alloc<true>(Thread::Current(), c, length,
                                                         c->GetComponentSizeShift(),
                                                         Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  return JDWP::ERR_NONE;
}

JDWP::FieldId Dbg::ToFieldId(const mirror::ArtField* f) {
  CHECK(!kMovingFields);
  return static_cast<JDWP::FieldId>(reinterpret_cast<uintptr_t>(f));
}

static JDWP::MethodId ToMethodId(const mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(!kMovingMethods);
  return static_cast<JDWP::MethodId>(reinterpret_cast<uintptr_t>(m));
}

static mirror::ArtField* FromFieldId(JDWP::FieldId fid)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(!kMovingFields);
  return reinterpret_cast<mirror::ArtField*>(static_cast<uintptr_t>(fid));
}

static mirror::ArtMethod* FromMethodId(JDWP::MethodId mid)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(!kMovingMethods);
  return reinterpret_cast<mirror::ArtMethod*>(static_cast<uintptr_t>(mid));
}

bool Dbg::MatchThread(JDWP::ObjectId expected_thread_id, Thread* event_thread) {
  CHECK(event_thread != nullptr);
  JDWP::JdwpError error;
  mirror::Object* expected_thread_peer = gRegistry->Get<mirror::Object*>(expected_thread_id,
                                                                         &error);
  return expected_thread_peer == event_thread->GetPeer();
}

bool Dbg::MatchLocation(const JDWP::JdwpLocation& expected_location,
                        const JDWP::EventLocation& event_location) {
  if (expected_location.dex_pc != event_location.dex_pc) {
    return false;
  }
  mirror::ArtMethod* m = FromMethodId(expected_location.method_id);
  return m == event_location.method;
}

bool Dbg::MatchType(mirror::Class* event_class, JDWP::RefTypeId class_id) {
  if (event_class == nullptr) {
    return false;
  }
  JDWP::JdwpError error;
  mirror::Class* expected_class = DecodeClass(class_id, &error);
  CHECK(expected_class != nullptr);
  return expected_class->IsAssignableFrom(event_class);
}

bool Dbg::MatchField(JDWP::RefTypeId expected_type_id, JDWP::FieldId expected_field_id,
                     mirror::ArtField* event_field) {
  mirror::ArtField* expected_field = FromFieldId(expected_field_id);
  if (expected_field != event_field) {
    return false;
  }
  return Dbg::MatchType(event_field->GetDeclaringClass(), expected_type_id);
}

bool Dbg::MatchInstance(JDWP::ObjectId expected_instance_id, mirror::Object* event_instance) {
  JDWP::JdwpError error;
  mirror::Object* modifier_instance = gRegistry->Get<mirror::Object*>(expected_instance_id, &error);
  return modifier_instance == event_instance;
}

void Dbg::SetJdwpLocation(JDWP::JdwpLocation* location, mirror::ArtMethod* m, uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (m == nullptr) {
    memset(location, 0, sizeof(*location));
  } else {
    mirror::Class* c = m->GetDeclaringClass();
    location->type_tag = GetTypeTag(c);
    location->class_id = gRegistry->AddRefType(c);
    location->method_id = ToMethodId(m);
    location->dex_pc = (m->IsNative() || m->IsProxyMethod()) ? static_cast<uint64_t>(-1) : dex_pc;
  }
}

std::string Dbg::GetMethodName(JDWP::MethodId method_id) {
  mirror::ArtMethod* m = FromMethodId(method_id);
  if (m == nullptr) {
    return "NULL";
  }
  return m->GetName();
}

std::string Dbg::GetFieldName(JDWP::FieldId field_id) {
  mirror::ArtField* f = FromFieldId(field_id);
  if (f == nullptr) {
    return "NULL";
  }
  return f->GetName();
}

/*
 * Augment the access flags for synthetic methods and fields by setting
 * the (as described by the spec) "0xf0000000 bit".  Also, strip out any
 * flags not specified by the Java programming language.
 */
static uint32_t MangleAccessFlags(uint32_t accessFlags) {
  accessFlags &= kAccJavaFlagsMask;
  if ((accessFlags & kAccSynthetic) != 0) {
    accessFlags |= 0xf0000000;
  }
  return accessFlags;
}

/*
 * Circularly shifts registers so that arguments come first. Debuggers
 * expect slots to begin with arguments, but dex code places them at
 * the end.
 */
static uint16_t MangleSlot(uint16_t slot, mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  if (code_item == nullptr) {
    // We should not get here for a method without code (native, proxy or abstract). Log it and
    // return the slot as is since all registers are arguments.
    LOG(WARNING) << "Trying to mangle slot for method without code " << PrettyMethod(m);
    return slot;
  }
  uint16_t ins_size = code_item->ins_size_;
  uint16_t locals_size = code_item->registers_size_ - ins_size;
  if (slot >= locals_size) {
    return slot - locals_size;
  } else {
    return slot + ins_size;
  }
}

/*
 * Circularly shifts registers so that arguments come last. Reverts
 * slots to dex style argument placement.
 */
static uint16_t DemangleSlot(uint16_t slot, mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  if (code_item == nullptr) {
    // We should not get here for a method without code (native, proxy or abstract). Log it and
    // return the slot as is since all registers are arguments.
    LOG(WARNING) << "Trying to demangle slot for method without code " << PrettyMethod(m);
    return slot;
  }
  uint16_t ins_size = code_item->ins_size_;
  uint16_t locals_size = code_item->registers_size_ - ins_size;
  if (slot < ins_size) {
    return slot + locals_size;
  } else {
    return slot - ins_size;
  }
}

JDWP::JdwpError Dbg::OutputDeclaredFields(JDWP::RefTypeId class_id, bool with_generic, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  size_t instance_field_count = c->NumInstanceFields();
  size_t static_field_count = c->NumStaticFields();

  expandBufAdd4BE(pReply, instance_field_count + static_field_count);

  for (size_t i = 0; i < instance_field_count + static_field_count; ++i) {
    mirror::ArtField* f = (i < instance_field_count) ? c->GetInstanceField(i) : c->GetStaticField(i - instance_field_count);
    expandBufAddFieldId(pReply, ToFieldId(f));
    expandBufAddUtf8String(pReply, f->GetName());
    expandBufAddUtf8String(pReply, f->GetTypeDescriptor());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(f->GetAccessFlags()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputDeclaredMethods(JDWP::RefTypeId class_id, bool with_generic,
                                           JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(class_id, &error);
  if (c == nullptr) {
    return error;
  }

  size_t direct_method_count = c->NumDirectMethods();
  size_t virtual_method_count = c->NumVirtualMethods();

  expandBufAdd4BE(pReply, direct_method_count + virtual_method_count);

  for (size_t i = 0; i < direct_method_count + virtual_method_count; ++i) {
    mirror::ArtMethod* m = (i < direct_method_count) ? c->GetDirectMethod(i) : c->GetVirtualMethod(i - direct_method_count);
    expandBufAddMethodId(pReply, ToMethodId(m));
    expandBufAddUtf8String(pReply, m->GetName());
    expandBufAddUtf8String(pReply, m->GetSignature().ToString());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(m->GetAccessFlags()));
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::OutputDeclaredInterfaces(JDWP::RefTypeId class_id, JDWP::ExpandBuf* pReply) {
  JDWP::JdwpError error;
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> c(hs.NewHandle(DecodeClass(class_id, &error)));
  if (c.Get() == nullptr) {
    return error;
  }
  size_t interface_count = c->NumDirectInterfaces();
  expandBufAdd4BE(pReply, interface_count);
  for (size_t i = 0; i < interface_count; ++i) {
    expandBufAddRefTypeId(pReply,
                          gRegistry->AddRefType(mirror::Class::GetDirectInterface(self, c, i)));
  }
  return JDWP::ERR_NONE;
}

void Dbg::OutputLineTable(JDWP::RefTypeId, JDWP::MethodId method_id, JDWP::ExpandBuf* pReply) {
  struct DebugCallbackContext {
    int numItems;
    JDWP::ExpandBuf* pReply;

    static bool Callback(void* context, uint32_t address, uint32_t line_number) {
      DebugCallbackContext* pContext = reinterpret_cast<DebugCallbackContext*>(context);
      expandBufAdd8BE(pContext->pReply, address);
      expandBufAdd4BE(pContext->pReply, line_number);
      pContext->numItems++;
      return false;
    }
  };
  mirror::ArtMethod* m = FromMethodId(method_id);
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  uint64_t start, end;
  if (code_item == nullptr) {
    DCHECK(m->IsNative() || m->IsProxyMethod());
    start = -1;
    end = -1;
  } else {
    start = 0;
    // Return the index of the last instruction
    end = code_item->insns_size_in_code_units_ - 1;
  }

  expandBufAdd8BE(pReply, start);
  expandBufAdd8BE(pReply, end);

  // Add numLines later
  size_t numLinesOffset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  DebugCallbackContext context;
  context.numItems = 0;
  context.pReply = pReply;

  if (code_item != nullptr) {
    m->GetDexFile()->DecodeDebugInfo(code_item, m->IsStatic(), m->GetDexMethodIndex(),
                                     DebugCallbackContext::Callback, nullptr, &context);
  }

  JDWP::Set4BE(expandBufGetBuffer(pReply) + numLinesOffset, context.numItems);
}

void Dbg::OutputVariableTable(JDWP::RefTypeId, JDWP::MethodId method_id, bool with_generic,
                              JDWP::ExpandBuf* pReply) {
  struct DebugCallbackContext {
    mirror::ArtMethod* method;
    JDWP::ExpandBuf* pReply;
    size_t variable_count;
    bool with_generic;

    static void Callback(void* context, uint16_t slot, uint32_t startAddress, uint32_t endAddress,
                         const char* name, const char* descriptor, const char* signature)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      DebugCallbackContext* pContext = reinterpret_cast<DebugCallbackContext*>(context);

      VLOG(jdwp) << StringPrintf("    %2zd: %d(%d) '%s' '%s' '%s' actual slot=%d mangled slot=%d",
                                 pContext->variable_count, startAddress, endAddress - startAddress,
                                 name, descriptor, signature, slot,
                                 MangleSlot(slot, pContext->method));

      slot = MangleSlot(slot, pContext->method);

      expandBufAdd8BE(pContext->pReply, startAddress);
      expandBufAddUtf8String(pContext->pReply, name);
      expandBufAddUtf8String(pContext->pReply, descriptor);
      if (pContext->with_generic) {
        expandBufAddUtf8String(pContext->pReply, signature);
      }
      expandBufAdd4BE(pContext->pReply, endAddress - startAddress);
      expandBufAdd4BE(pContext->pReply, slot);

      ++pContext->variable_count;
    }
  };
  mirror::ArtMethod* m = FromMethodId(method_id);

  // arg_count considers doubles and longs to take 2 units.
  // variable_count considers everything to take 1 unit.
  std::string shorty(m->GetShorty());
  expandBufAdd4BE(pReply, mirror::ArtMethod::NumArgRegisters(shorty));

  // We don't know the total number of variables yet, so leave a blank and update it later.
  size_t variable_count_offset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  DebugCallbackContext context;
  context.method = m;
  context.pReply = pReply;
  context.variable_count = 0;
  context.with_generic = with_generic;

  const DexFile::CodeItem* code_item = m->GetCodeItem();
  if (code_item != nullptr) {
    m->GetDexFile()->DecodeDebugInfo(
        code_item, m->IsStatic(), m->GetDexMethodIndex(), nullptr, DebugCallbackContext::Callback,
        &context);
  }

  JDWP::Set4BE(expandBufGetBuffer(pReply) + variable_count_offset, context.variable_count);
}

void Dbg::OutputMethodReturnValue(JDWP::MethodId method_id, const JValue* return_value,
                                  JDWP::ExpandBuf* pReply) {
  mirror::ArtMethod* m = FromMethodId(method_id);
  JDWP::JdwpTag tag = BasicTagFromDescriptor(m->GetShorty());
  OutputJValue(tag, return_value, pReply);
}

void Dbg::OutputFieldValue(JDWP::FieldId field_id, const JValue* field_value,
                           JDWP::ExpandBuf* pReply) {
  mirror::ArtField* f = FromFieldId(field_id);
  JDWP::JdwpTag tag = BasicTagFromDescriptor(f->GetTypeDescriptor());
  OutputJValue(tag, field_value, pReply);
}

JDWP::JdwpError Dbg::GetBytecodes(JDWP::RefTypeId, JDWP::MethodId method_id,
                                  std::vector<uint8_t>* bytecodes) {
  mirror::ArtMethod* m = FromMethodId(method_id);
  if (m == nullptr) {
    return JDWP::ERR_INVALID_METHODID;
  }
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  size_t byte_count = code_item->insns_size_in_code_units_ * 2;
  const uint8_t* begin = reinterpret_cast<const uint8_t*>(code_item->insns_);
  const uint8_t* end = begin + byte_count;
  for (const uint8_t* p = begin; p != end; ++p) {
    bytecodes->push_back(*p);
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpTag Dbg::GetFieldBasicTag(JDWP::FieldId field_id) {
  return BasicTagFromDescriptor(FromFieldId(field_id)->GetTypeDescriptor());
}

JDWP::JdwpTag Dbg::GetStaticFieldBasicTag(JDWP::FieldId field_id) {
  return BasicTagFromDescriptor(FromFieldId(field_id)->GetTypeDescriptor());
}

static JDWP::JdwpError GetFieldValueImpl(JDWP::RefTypeId ref_type_id, JDWP::ObjectId object_id,
                                         JDWP::FieldId field_id, JDWP::ExpandBuf* pReply,
                                         bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JDWP::JdwpError error;
  mirror::Class* c = DecodeClass(ref_type_id, &error);
  if (ref_type_id != 0 && c == nullptr) {
    return error;
  }

  mirror::Object* o = Dbg::GetObjectRegistry()->Get<mirror::Object*>(object_id, &error);
  if ((!is_static && o == nullptr) || error != JDWP::ERR_NONE) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  mirror::ArtField* f = FromFieldId(field_id);

  mirror::Class* receiver_class = c;
  if (receiver_class == nullptr && o != nullptr) {
    receiver_class = o->GetClass();
  }
  // TODO: should we give up now if receiver_class is nullptr?
  if (receiver_class != nullptr && !f->GetDeclaringClass()->IsAssignableFrom(receiver_class)) {
    LOG(INFO) << "ERR_INVALID_FIELDID: " << PrettyField(f) << " " << PrettyClass(receiver_class);
    return JDWP::ERR_INVALID_FIELDID;
  }

  // The RI only enforces the static/non-static mismatch in one direction.
  // TODO: should we change the tests and check both?
  if (is_static) {
    if (!f->IsStatic()) {
      return JDWP::ERR_INVALID_FIELDID;
    }
  } else {
    if (f->IsStatic()) {
      LOG(WARNING) << "Ignoring non-nullptr receiver for ObjectReference.SetValues on static field "
          << PrettyField(f);
    }
  }
  if (f->IsStatic()) {
    o = f->GetDeclaringClass();
  }

  JDWP::JdwpTag tag = BasicTagFromDescriptor(f->GetTypeDescriptor());
  JValue field_value;
  if (tag == JDWP::JT_VOID) {
    LOG(FATAL) << "Unknown tag: " << tag;
  } else if (!IsPrimitiveTag(tag)) {
    field_value.SetL(f->GetObject(o));
  } else if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
    field_value.SetJ(f->Get64(o));
  } else {
    field_value.SetI(f->Get32(o));
  }
  Dbg::OutputJValue(tag, &field_value, pReply);

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetFieldValue(JDWP::ObjectId object_id, JDWP::FieldId field_id,
                                   JDWP::ExpandBuf* pReply) {
  return GetFieldValueImpl(0, object_id, field_id, pReply, false);
}

JDWP::JdwpError Dbg::GetStaticFieldValue(JDWP::RefTypeId ref_type_id, JDWP::FieldId field_id,
                                         JDWP::ExpandBuf* pReply) {
  return GetFieldValueImpl(ref_type_id, 0, field_id, pReply, true);
}

static JDWP::JdwpError SetFieldValueImpl(JDWP::ObjectId object_id, JDWP::FieldId field_id,
                                         uint64_t value, int width, bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JDWP::JdwpError error;
  mirror::Object* o = Dbg::GetObjectRegistry()->Get<mirror::Object*>(object_id, &error);
  if ((!is_static && o == nullptr) || error != JDWP::ERR_NONE) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  mirror::ArtField* f = FromFieldId(field_id);

  // The RI only enforces the static/non-static mismatch in one direction.
  // TODO: should we change the tests and check both?
  if (is_static) {
    if (!f->IsStatic()) {
      return JDWP::ERR_INVALID_FIELDID;
    }
  } else {
    if (f->IsStatic()) {
      LOG(WARNING) << "Ignoring non-nullptr receiver for ObjectReference.SetValues on static field " << PrettyField(f);
    }
  }
  if (f->IsStatic()) {
    o = f->GetDeclaringClass();
  }

  JDWP::JdwpTag tag = BasicTagFromDescriptor(f->GetTypeDescriptor());

  if (IsPrimitiveTag(tag)) {
    if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      CHECK_EQ(width, 8);
      // Debugging can't use transactional mode (runtime only).
      f->Set64<false>(o, value);
    } else {
      CHECK_LE(width, 4);
      // Debugging can't use transactional mode (runtime only).
      f->Set32<false>(o, value);
    }
  } else {
    mirror::Object* v = Dbg::GetObjectRegistry()->Get<mirror::Object*>(value, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }
    if (v != nullptr) {
      mirror::Class* field_type;
      {
        StackHandleScope<3> hs(Thread::Current());
        HandleWrapper<mirror::Object> h_v(hs.NewHandleWrapper(&v));
        HandleWrapper<mirror::ArtField> h_f(hs.NewHandleWrapper(&f));
        HandleWrapper<mirror::Object> h_o(hs.NewHandleWrapper(&o));
        field_type = FieldHelper(h_f).GetType();
      }
      if (!field_type->IsAssignableFrom(v->GetClass())) {
        return JDWP::ERR_INVALID_OBJECT;
      }
    }
    // Debugging can't use transactional mode (runtime only).
    f->SetObject<false>(o, v);
  }

  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::SetFieldValue(JDWP::ObjectId object_id, JDWP::FieldId field_id, uint64_t value,
                                   int width) {
  return SetFieldValueImpl(object_id, field_id, value, width, false);
}

JDWP::JdwpError Dbg::SetStaticFieldValue(JDWP::FieldId field_id, uint64_t value, int width) {
  return SetFieldValueImpl(0, field_id, value, width, true);
}

JDWP::JdwpError Dbg::StringToUtf8(JDWP::ObjectId string_id, std::string* str) {
  JDWP::JdwpError error;
  mirror::Object* obj = gRegistry->Get<mirror::Object*>(string_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (obj == nullptr) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    mirror::Class* java_lang_String = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_String);
    if (!java_lang_String->IsAssignableFrom(obj->GetClass())) {
      // This isn't a string.
      return JDWP::ERR_INVALID_STRING;
    }
  }
  *str = obj->AsString()->ToModifiedUtf8();
  return JDWP::ERR_NONE;
}

void Dbg::OutputJValue(JDWP::JdwpTag tag, const JValue* return_value, JDWP::ExpandBuf* pReply) {
  if (IsPrimitiveTag(tag)) {
    expandBufAdd1(pReply, tag);
    if (tag == JDWP::JT_BOOLEAN || tag == JDWP::JT_BYTE) {
      expandBufAdd1(pReply, return_value->GetI());
    } else if (tag == JDWP::JT_CHAR || tag == JDWP::JT_SHORT) {
      expandBufAdd2BE(pReply, return_value->GetI());
    } else if (tag == JDWP::JT_FLOAT || tag == JDWP::JT_INT) {
      expandBufAdd4BE(pReply, return_value->GetI());
    } else if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      expandBufAdd8BE(pReply, return_value->GetJ());
    } else {
      CHECK_EQ(tag, JDWP::JT_VOID);
    }
  } else {
    ScopedObjectAccessUnchecked soa(Thread::Current());
    mirror::Object* value = return_value->GetL();
    expandBufAdd1(pReply, TagFromObject(soa, value));
    expandBufAddObjectId(pReply, gRegistry->Add(value));
  }
}

JDWP::JdwpError Dbg::GetThreadName(JDWP::ObjectId thread_id, std::string* name) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  UNUSED(thread);
  if (error != JDWP::ERR_NONE && error != JDWP::ERR_THREAD_NOT_ALIVE) {
    return error;
  }

  // We still need to report the zombie threads' names, so we can't just call Thread::GetThreadName.
  mirror::Object* thread_object = gRegistry->Get<mirror::Object*>(thread_id, &error);
  CHECK(thread_object != nullptr) << error;
  mirror::ArtField* java_lang_Thread_name_field =
      soa.DecodeField(WellKnownClasses::java_lang_Thread_name);
  mirror::String* s =
      reinterpret_cast<mirror::String*>(java_lang_Thread_name_field->GetObject(thread_object));
  if (s != nullptr) {
    *name = s->ToModifiedUtf8();
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadGroup(JDWP::ObjectId thread_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  mirror::Object* thread_object = gRegistry->Get<mirror::Object*>(thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return JDWP::ERR_INVALID_OBJECT;
  }
  ScopedAssertNoThreadSuspension ants(soa.Self(), "Debugger: GetThreadGroup");
  // Okay, so it's an object, but is it actually a thread?
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    Thread* thread = DecodeThread(soa, thread_id, &error);
    UNUSED(thread);
  }
  if (error == JDWP::ERR_THREAD_NOT_ALIVE) {
    // Zombie threads are in the null group.
    expandBufAddObjectId(pReply, JDWP::ObjectId(0));
    error = JDWP::ERR_NONE;
  } else if (error == JDWP::ERR_NONE) {
    mirror::Class* c = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread);
    CHECK(c != nullptr);
    mirror::ArtField* f = c->FindInstanceField("group", "Ljava/lang/ThreadGroup;");
    CHECK(f != nullptr);
    mirror::Object* group = f->GetObject(thread_object);
    CHECK(group != nullptr);
    JDWP::ObjectId thread_group_id = gRegistry->Add(group);
    expandBufAddObjectId(pReply, thread_group_id);
  }
  return error;
}

static mirror::Object* DecodeThreadGroup(ScopedObjectAccessUnchecked& soa,
                                         JDWP::ObjectId thread_group_id, JDWP::JdwpError* error)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* thread_group = Dbg::GetObjectRegistry()->Get<mirror::Object*>(thread_group_id,
                                                                                error);
  if (*error != JDWP::ERR_NONE) {
    return nullptr;
  }
  if (thread_group == nullptr) {
    *error = JDWP::ERR_INVALID_OBJECT;
    return nullptr;
  }
  mirror::Class* c = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ThreadGroup);
  CHECK(c != nullptr);
  if (!c->IsAssignableFrom(thread_group->GetClass())) {
    // This is not a java.lang.ThreadGroup.
    *error = JDWP::ERR_INVALID_THREAD_GROUP;
    return nullptr;
  }
  *error = JDWP::ERR_NONE;
  return thread_group;
}

JDWP::JdwpError Dbg::GetThreadGroupName(JDWP::ObjectId thread_group_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  mirror::Object* thread_group = DecodeThreadGroup(soa, thread_group_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  ScopedAssertNoThreadSuspension ants(soa.Self(), "Debugger: GetThreadGroupName");
  mirror::Class* c = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ThreadGroup);
  mirror::ArtField* f = c->FindInstanceField("name", "Ljava/lang/String;");
  CHECK(f != nullptr);
  mirror::String* s = reinterpret_cast<mirror::String*>(f->GetObject(thread_group));

  std::string thread_group_name(s->ToModifiedUtf8());
  expandBufAddUtf8String(pReply, thread_group_name);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadGroupParent(JDWP::ObjectId thread_group_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  mirror::Object* thread_group = DecodeThreadGroup(soa, thread_group_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  mirror::Object* parent;
  {
    ScopedAssertNoThreadSuspension ants(soa.Self(), "Debugger: GetThreadGroupParent");
    mirror::Class* c = soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ThreadGroup);
    CHECK(c != nullptr);
    mirror::ArtField* f = c->FindInstanceField("parent", "Ljava/lang/ThreadGroup;");
    CHECK(f != nullptr);
    parent = f->GetObject(thread_group);
  }
  JDWP::ObjectId parent_group_id = gRegistry->Add(parent);
  expandBufAddObjectId(pReply, parent_group_id);
  return JDWP::ERR_NONE;
}

static void GetChildThreadGroups(ScopedObjectAccessUnchecked& soa, mirror::Object* thread_group,
                                 std::vector<JDWP::ObjectId>* child_thread_group_ids)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(thread_group != nullptr);

  // Get the ArrayList<ThreadGroup> "groups" out of this thread group...
  mirror::ArtField* groups_field = thread_group->GetClass()->FindInstanceField("groups", "Ljava/util/List;");
  mirror::Object* groups_array_list = groups_field->GetObject(thread_group);

  // Get the array and size out of the ArrayList<ThreadGroup>...
  mirror::ArtField* array_field = groups_array_list->GetClass()->FindInstanceField("array", "[Ljava/lang/Object;");
  mirror::ArtField* size_field = groups_array_list->GetClass()->FindInstanceField("size", "I");
  mirror::ObjectArray<mirror::Object>* groups_array =
      array_field->GetObject(groups_array_list)->AsObjectArray<mirror::Object>();
  const int32_t size = size_field->GetInt(groups_array_list);

  // Copy the first 'size' elements out of the array into the result.
  ObjectRegistry* registry = Dbg::GetObjectRegistry();
  for (int32_t i = 0; i < size; ++i) {
    child_thread_group_ids->push_back(registry->Add(groups_array->Get(i)));
  }
}

JDWP::JdwpError Dbg::GetThreadGroupChildren(JDWP::ObjectId thread_group_id,
                                            JDWP::ExpandBuf* pReply) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  mirror::Object* thread_group = DecodeThreadGroup(soa, thread_group_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }

  // Add child threads.
  {
    std::vector<JDWP::ObjectId> child_thread_ids;
    GetThreads(thread_group, &child_thread_ids);
    expandBufAdd4BE(pReply, child_thread_ids.size());
    for (JDWP::ObjectId child_thread_id : child_thread_ids) {
      expandBufAddObjectId(pReply, child_thread_id);
    }
  }

  // Add child thread groups.
  {
    std::vector<JDWP::ObjectId> child_thread_groups_ids;
    GetChildThreadGroups(soa, thread_group, &child_thread_groups_ids);
    expandBufAdd4BE(pReply, child_thread_groups_ids.size());
    for (JDWP::ObjectId child_thread_group_id : child_thread_groups_ids) {
      expandBufAddObjectId(pReply, child_thread_group_id);
    }
  }

  return JDWP::ERR_NONE;
}

JDWP::ObjectId Dbg::GetSystemThreadGroupId() {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  mirror::ArtField* f = soa.DecodeField(WellKnownClasses::java_lang_ThreadGroup_systemThreadGroup);
  mirror::Object* group = f->GetObject(f->GetDeclaringClass());
  return gRegistry->Add(group);
}

JDWP::JdwpThreadStatus Dbg::ToJdwpThreadStatus(ThreadState state) {
  switch (state) {
    case kBlocked:
      return JDWP::TS_MONITOR;
    case kNative:
    case kRunnable:
    case kSuspended:
      return JDWP::TS_RUNNING;
    case kSleeping:
      return JDWP::TS_SLEEPING;
    case kStarting:
    case kTerminated:
      return JDWP::TS_ZOMBIE;
    case kTimedWaiting:
    case kWaitingForCheckPointsToRun:
    case kWaitingForDebuggerSend:
    case kWaitingForDebuggerSuspension:
    case kWaitingForDebuggerToAttach:
    case kWaitingForDeoptimization:
    case kWaitingForGcToComplete:
    case kWaitingForJniOnLoad:
    case kWaitingForMethodTracingStart:
    case kWaitingForSignalCatcherOutput:
    case kWaitingInMainDebuggerLoop:
    case kWaitingInMainSignalCatcherLoop:
    case kWaitingPerformingGc:
    case kWaiting:
      return JDWP::TS_WAIT;
      // Don't add a 'default' here so the compiler can spot incompatible enum changes.
  }
  LOG(FATAL) << "Unknown thread state: " << state;
  return JDWP::TS_ZOMBIE;
}

JDWP::JdwpError Dbg::GetThreadStatus(JDWP::ObjectId thread_id, JDWP::JdwpThreadStatus* pThreadStatus,
                                     JDWP::JdwpSuspendStatus* pSuspendStatus) {
  ScopedObjectAccess soa(Thread::Current());

  *pSuspendStatus = JDWP::SUSPEND_STATUS_NOT_SUSPENDED;

  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    if (error == JDWP::ERR_THREAD_NOT_ALIVE) {
      *pThreadStatus = JDWP::TS_ZOMBIE;
      return JDWP::ERR_NONE;
    }
    return error;
  }

  if (IsSuspendedForDebugger(soa, thread)) {
    *pSuspendStatus = JDWP::SUSPEND_STATUS_SUSPENDED;
  }

  *pThreadStatus = ToJdwpThreadStatus(thread->GetState());
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadDebugSuspendCount(JDWP::ObjectId thread_id, JDWP::ExpandBuf* pReply) {
  ScopedObjectAccess soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
  expandBufAdd4BE(pReply, thread->GetDebugSuspendCount());
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::Interrupt(JDWP::ObjectId thread_id) {
  ScopedObjectAccess soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  thread->Interrupt(soa.Self());
  return JDWP::ERR_NONE;
}

static bool IsInDesiredThreadGroup(ScopedObjectAccessUnchecked& soa,
                                   mirror::Object* desired_thread_group, mirror::Object* peer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Do we want threads from all thread groups?
  if (desired_thread_group == nullptr) {
    return true;
  }
  mirror::ArtField* thread_group_field = soa.DecodeField(WellKnownClasses::java_lang_Thread_group);
  DCHECK(thread_group_field != nullptr);
  mirror::Object* group = thread_group_field->GetObject(peer);
  return (group == desired_thread_group);
}

void Dbg::GetThreads(mirror::Object* thread_group, std::vector<JDWP::ObjectId>* thread_ids) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  std::list<Thread*> all_threads_list;
  {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    all_threads_list = Runtime::Current()->GetThreadList()->GetList();
  }
  for (Thread* t : all_threads_list) {
    if (t == Dbg::GetDebugThread()) {
      // Skip the JDWP thread. Some debuggers get bent out of shape when they can't suspend and
      // query all threads, so it's easier if we just don't tell them about this thread.
      continue;
    }
    if (t->IsStillStarting()) {
      // This thread is being started (and has been registered in the thread list). However, it is
      // not completely started yet so we must ignore it.
      continue;
    }
    mirror::Object* peer = t->GetPeer();
    if (peer == nullptr) {
      // peer might be NULL if the thread is still starting up. We can't tell the debugger about
      // this thread yet.
      // TODO: if we identified threads to the debugger by their Thread*
      // rather than their peer's mirror::Object*, we could fix this.
      // Doing so might help us report ZOMBIE threads too.
      continue;
    }
    if (IsInDesiredThreadGroup(soa, thread_group, peer)) {
      thread_ids->push_back(gRegistry->Add(peer));
    }
  }
}

static int GetStackDepth(Thread* thread) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  struct CountStackDepthVisitor : public StackVisitor {
    explicit CountStackDepthVisitor(Thread* thread)
        : StackVisitor(thread, nullptr), depth(0) {}

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
      if (!GetMethod()->IsRuntimeMethod()) {
        ++depth;
      }
      return true;
    }
    size_t depth;
  };

  CountStackDepthVisitor visitor(thread);
  visitor.WalkStack();
  return visitor.depth;
}

JDWP::JdwpError Dbg::GetThreadFrameCount(JDWP::ObjectId thread_id, size_t* result) {
  ScopedObjectAccess soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  *result = 0;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }
  *result = GetStackDepth(thread);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetThreadFrames(JDWP::ObjectId thread_id, size_t start_frame,
                                     size_t frame_count, JDWP::ExpandBuf* buf) {
  class GetFrameVisitor : public StackVisitor {
   public:
    GetFrameVisitor(Thread* thread, size_t start_frame, size_t frame_count, JDWP::ExpandBuf* buf)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        : StackVisitor(thread, nullptr), depth_(0),
          start_frame_(start_frame), frame_count_(frame_count), buf_(buf) {
      expandBufAdd4BE(buf_, frame_count_);
    }

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    virtual bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
      if (GetMethod()->IsRuntimeMethod()) {
        return true;  // The debugger can't do anything useful with a frame that has no Method*.
      }
      if (depth_ >= start_frame_ + frame_count_) {
        return false;
      }
      if (depth_ >= start_frame_) {
        JDWP::FrameId frame_id(GetFrameId());
        JDWP::JdwpLocation location;
        SetJdwpLocation(&location, GetMethod(), GetDexPc());
        VLOG(jdwp) << StringPrintf("    Frame %3zd: id=%3" PRIu64 " ", depth_, frame_id) << location;
        expandBufAdd8BE(buf_, frame_id);
        expandBufAddLocation(buf_, location);
      }
      ++depth_;
      return true;
    }

   private:
    size_t depth_;
    const size_t start_frame_;
    const size_t frame_count_;
    JDWP::ExpandBuf* buf_;
  };

  ScopedObjectAccessUnchecked soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error != JDWP::ERR_NONE) {
    return error;
  }
  if (!IsSuspendedForDebugger(soa, thread)) {
    return JDWP::ERR_THREAD_NOT_SUSPENDED;
  }
  GetFrameVisitor visitor(thread, start_frame, frame_count, buf);
  visitor.WalkStack();
  return JDWP::ERR_NONE;
}

JDWP::ObjectId Dbg::GetThreadSelfId() {
  return GetThreadId(Thread::Current());
}

JDWP::ObjectId Dbg::GetThreadId(Thread* thread) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  return gRegistry->Add(thread->GetPeer());
}

void Dbg::SuspendVM() {
  Runtime::Current()->GetThreadList()->SuspendAllForDebugger();
}

void Dbg::ResumeVM() {
  Runtime::Current()->GetThreadList()->UndoDebuggerSuspensions();
}

JDWP::JdwpError Dbg::SuspendThread(JDWP::ObjectId thread_id, bool request_suspension) {
  Thread* self = Thread::Current();
  ScopedLocalRef<jobject> peer(self->GetJniEnv(), nullptr);
  {
    ScopedObjectAccess soa(self);
    JDWP::JdwpError error;
    peer.reset(soa.AddLocalReference<jobject>(gRegistry->Get<mirror::Object*>(thread_id, &error)));
  }
  if (peer.get() == nullptr) {
    return JDWP::ERR_THREAD_NOT_ALIVE;
  }
  // Suspend thread to build stack trace. Take suspend thread lock to avoid races with threads
  // trying to suspend this one.
  MutexLock mu(self, *Locks::thread_list_suspend_thread_lock_);
  bool timed_out;
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  Thread* thread = thread_list->SuspendThreadByPeer(peer.get(), request_suspension, true,
                                                    &timed_out);
  if (thread != nullptr) {
    return JDWP::ERR_NONE;
  } else if (timed_out) {
    return JDWP::ERR_INTERNAL;
  } else {
    return JDWP::ERR_THREAD_NOT_ALIVE;
  }
}

void Dbg::ResumeThread(JDWP::ObjectId thread_id) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  JDWP::JdwpError error;
  mirror::Object* peer = gRegistry->Get<mirror::Object*>(thread_id, &error);
  CHECK(peer != nullptr) << error;
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    thread = Thread::FromManagedThread(soa, peer);
  }
  if (thread == nullptr) {
    LOG(WARNING) << "No such thread for resume: " << peer;
    return;
  }
  bool needs_resume;
  {
    MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
    needs_resume = thread->GetSuspendCount() > 0;
  }
  if (needs_resume) {
    Runtime::Current()->GetThreadList()->Resume(thread, true);
  }
}

void Dbg::SuspendSelf() {
  Runtime::Current()->GetThreadList()->SuspendSelfForDebugger();
}

struct GetThisVisitor : public StackVisitor {
  GetThisVisitor(Thread* thread, Context* context, JDWP::FrameId frame_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), this_object(nullptr), frame_id(frame_id) {}

  // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
  // annotalysis.
  virtual bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
    if (frame_id != GetFrameId()) {
      return true;  // continue
    } else {
      this_object = GetThisObject();
      return false;
    }
  }

  mirror::Object* this_object;
  JDWP::FrameId frame_id;
};

JDWP::JdwpError Dbg::GetThisObject(JDWP::ObjectId thread_id, JDWP::FrameId frame_id,
                                   JDWP::ObjectId* result) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    thread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
    if (!IsSuspendedForDebugger(soa, thread)) {
      return JDWP::ERR_THREAD_NOT_SUSPENDED;
    }
  }
  std::unique_ptr<Context> context(Context::Create());
  GetThisVisitor visitor(thread, context.get(), frame_id);
  visitor.WalkStack();
  *result = gRegistry->Add(visitor.this_object);
  return JDWP::ERR_NONE;
}

// Walks the stack until we find the frame with the given FrameId.
class FindFrameVisitor FINAL : public StackVisitor {
 public:
  FindFrameVisitor(Thread* thread, Context* context, JDWP::FrameId frame_id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, context), frame_id_(frame_id), error_(JDWP::ERR_INVALID_FRAMEID) {}

  // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
  // annotalysis.
  bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
    if (GetFrameId() != frame_id_) {
      return true;  // Not our frame, carry on.
    }
    mirror::ArtMethod* m = GetMethod();
    if (m->IsNative()) {
      // We can't read/write local value from/into native method.
      error_ = JDWP::ERR_OPAQUE_FRAME;
    } else {
      // We found our frame.
      error_ = JDWP::ERR_NONE;
    }
    return false;
  }

  JDWP::JdwpError GetError() const {
    return error_;
  }

 private:
  const JDWP::FrameId frame_id_;
  JDWP::JdwpError error_;
};

JDWP::JdwpError Dbg::GetLocalValues(JDWP::Request* request, JDWP::ExpandBuf* pReply) {
  JDWP::ObjectId thread_id = request->ReadThreadId();
  JDWP::FrameId frame_id = request->ReadFrameId();

  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    thread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  // Find the frame with the given frame_id.
  std::unique_ptr<Context> context(Context::Create());
  FindFrameVisitor visitor(thread, context.get(), frame_id);
  visitor.WalkStack();
  if (visitor.GetError() != JDWP::ERR_NONE) {
    return visitor.GetError();
  }

  // Read the values from visitor's context.
  int32_t slot_count = request->ReadSigned32("slot count");
  expandBufAdd4BE(pReply, slot_count);     /* "int values" */
  for (int32_t i = 0; i < slot_count; ++i) {
    uint32_t slot = request->ReadUnsigned32("slot");
    JDWP::JdwpTag reqSigByte = request->ReadTag();

    VLOG(jdwp) << "    --> slot " << slot << " " << reqSigByte;

    size_t width = Dbg::GetTagWidth(reqSigByte);
    uint8_t* ptr = expandBufAddSpace(pReply, width+1);
    JDWP::JdwpError error = Dbg::GetLocalValue(visitor, soa, slot, reqSigByte, ptr, width);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::GetLocalValue(const StackVisitor& visitor, ScopedObjectAccessUnchecked& soa,
                                   int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t width) {
  mirror::ArtMethod* m = visitor.GetMethod();
  uint16_t reg = DemangleSlot(slot, m);
  // TODO: check that the tag is compatible with the actual type of the slot!
  // TODO: check slot is valid for this method or return INVALID_SLOT error.
  constexpr JDWP::JdwpError kFailureErrorCode = JDWP::ERR_ABSENT_INFORMATION;
  switch (tag) {
    case JDWP::JT_BOOLEAN: {
      CHECK_EQ(width, 1U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get boolean local " << reg << " = " << intVal;
        JDWP::Set1(buf + 1, intVal != 0);
      } else {
        VLOG(jdwp) << "failed to get boolean local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_BYTE: {
      CHECK_EQ(width, 1U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get byte local " << reg << " = " << intVal;
        JDWP::Set1(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get byte local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_SHORT:
    case JDWP::JT_CHAR: {
      CHECK_EQ(width, 2U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get short/char local " << reg << " = " << intVal;
        JDWP::Set2BE(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get short/char local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_INT: {
      CHECK_EQ(width, 4U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kIntVReg, &intVal)) {
        VLOG(jdwp) << "get int local " << reg << " = " << intVal;
        JDWP::Set4BE(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get int local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_FLOAT: {
      CHECK_EQ(width, 4U);
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kFloatVReg, &intVal)) {
        VLOG(jdwp) << "get float local " << reg << " = " << intVal;
        JDWP::Set4BE(buf + 1, intVal);
      } else {
        VLOG(jdwp) << "failed to get float local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_ARRAY:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP: {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      uint32_t intVal;
      if (visitor.GetVReg(m, reg, kReferenceVReg, &intVal)) {
        mirror::Object* o = reinterpret_cast<mirror::Object*>(intVal);
        VLOG(jdwp) << "get " << tag << " object local " << reg << " = " << o;
        if (!Runtime::Current()->GetHeap()->IsValidObjectAddress(o)) {
          LOG(FATAL) << "Register " << reg << " expected to hold " << tag << " object: " << o;
        }
        tag = TagFromObject(soa, o);
        JDWP::SetObjectId(buf + 1, gRegistry->Add(o));
      } else {
        VLOG(jdwp) << "failed to get " << tag << " object local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_DOUBLE: {
      CHECK_EQ(width, 8U);
      uint64_t longVal;
      if (visitor.GetVRegPair(m, reg, kDoubleLoVReg, kDoubleHiVReg, &longVal)) {
        VLOG(jdwp) << "get double local " << reg << " = " << longVal;
        JDWP::Set8BE(buf + 1, longVal);
      } else {
        VLOG(jdwp) << "failed to get double local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_LONG: {
      CHECK_EQ(width, 8U);
      uint64_t longVal;
      if (visitor.GetVRegPair(m, reg, kLongLoVReg, kLongHiVReg, &longVal)) {
        VLOG(jdwp) << "get long local " << reg << " = " << longVal;
        JDWP::Set8BE(buf + 1, longVal);
      } else {
        VLOG(jdwp) << "failed to get long local " << reg;
        return kFailureErrorCode;
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown tag " << tag;
      break;
  }

  // Prepend tag, which may have been updated.
  JDWP::Set1(buf, tag);
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::SetLocalValues(JDWP::Request* request) {
  JDWP::ObjectId thread_id = request->ReadThreadId();
  JDWP::FrameId frame_id = request->ReadFrameId();

  ScopedObjectAccessUnchecked soa(Thread::Current());
  Thread* thread;
  {
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    thread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  // Find the frame with the given frame_id.
  std::unique_ptr<Context> context(Context::Create());
  FindFrameVisitor visitor(thread, context.get(), frame_id);
  visitor.WalkStack();
  if (visitor.GetError() != JDWP::ERR_NONE) {
    return visitor.GetError();
  }

  // Writes the values into visitor's context.
  int32_t slot_count = request->ReadSigned32("slot count");
  for (int32_t i = 0; i < slot_count; ++i) {
    uint32_t slot = request->ReadUnsigned32("slot");
    JDWP::JdwpTag sigByte = request->ReadTag();
    size_t width = Dbg::GetTagWidth(sigByte);
    uint64_t value = request->ReadValue(width);

    VLOG(jdwp) << "    --> slot " << slot << " " << sigByte << " " << value;
    JDWP::JdwpError error = Dbg::SetLocalValue(visitor, slot, sigByte, value, width);
    if (error != JDWP::ERR_NONE) {
      return error;
    }
  }
  return JDWP::ERR_NONE;
}

JDWP::JdwpError Dbg::SetLocalValue(StackVisitor& visitor, int slot, JDWP::JdwpTag tag,
                                   uint64_t value, size_t width) {
  mirror::ArtMethod* m = visitor.GetMethod();
  uint16_t reg = DemangleSlot(slot, m);
  // TODO: check that the tag is compatible with the actual type of the slot!
  // TODO: check slot is valid for this method or return INVALID_SLOT error.
  constexpr JDWP::JdwpError kFailureErrorCode = JDWP::ERR_ABSENT_INFORMATION;
  switch (tag) {
    case JDWP::JT_BOOLEAN:
    case JDWP::JT_BYTE:
      CHECK_EQ(width, 1U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kIntVReg)) {
        VLOG(jdwp) << "failed to set boolean/byte local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_SHORT:
    case JDWP::JT_CHAR:
      CHECK_EQ(width, 2U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kIntVReg)) {
        VLOG(jdwp) << "failed to set short/char local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_INT:
      CHECK_EQ(width, 4U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kIntVReg)) {
        VLOG(jdwp) << "failed to set int local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_FLOAT:
      CHECK_EQ(width, 4U);
      if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(value), kFloatVReg)) {
        VLOG(jdwp) << "failed to set float local " << reg << " = "
                   << static_cast<uint32_t>(value);
        return kFailureErrorCode;
      }
      break;
    case JDWP::JT_ARRAY:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP: {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      JDWP::JdwpError error;
      mirror::Object* o = gRegistry->Get<mirror::Object*>(static_cast<JDWP::ObjectId>(value),
                                                          &error);
      if (error != JDWP::ERR_NONE) {
        VLOG(jdwp) << tag << " object " << o << " is an invalid object";
        return JDWP::ERR_INVALID_OBJECT;
      } else if (!visitor.SetVReg(m, reg, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o)),
                          kReferenceVReg)) {
        VLOG(jdwp) << "failed to set " << tag << " object local " << reg << " = " << o;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_DOUBLE: {
      CHECK_EQ(width, 8U);
      if (!visitor.SetVRegPair(m, reg, value, kDoubleLoVReg, kDoubleHiVReg)) {
        VLOG(jdwp) << "failed to set double local " << reg << " = " << value;
        return kFailureErrorCode;
      }
      break;
    }
    case JDWP::JT_LONG: {
      CHECK_EQ(width, 8U);
      if (!visitor.SetVRegPair(m, reg, value, kLongLoVReg, kLongHiVReg)) {
        VLOG(jdwp) << "failed to set double local " << reg << " = " << value;
        return kFailureErrorCode;
      }
      break;
    }
    default:
      LOG(FATAL) << "Unknown tag " << tag;
      break;
  }
  return JDWP::ERR_NONE;
}

static void SetEventLocation(JDWP::EventLocation* location, mirror::ArtMethod* m, uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(location != nullptr);
  if (m == nullptr) {
    memset(location, 0, sizeof(*location));
  } else {
    location->method = m;
    location->dex_pc = (m->IsNative() || m->IsProxyMethod()) ? static_cast<uint32_t>(-1) : dex_pc;
  }
}

void Dbg::PostLocationEvent(mirror::ArtMethod* m, int dex_pc, mirror::Object* this_object,
                            int event_flags, const JValue* return_value) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK_EQ(m->IsStatic(), this_object == nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);

  gJdwpState->PostLocationEvent(&location, this_object, event_flags, return_value);
}

void Dbg::PostFieldAccessEvent(mirror::ArtMethod* m, int dex_pc,
                               mirror::Object* this_object, mirror::ArtField* f) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK(f != nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);

  gJdwpState->PostFieldEvent(&location, f, this_object, nullptr, false);
}

void Dbg::PostFieldModificationEvent(mirror::ArtMethod* m, int dex_pc,
                                     mirror::Object* this_object, mirror::ArtField* f,
                                     const JValue* field_value) {
  if (!IsDebuggerActive()) {
    return;
  }
  DCHECK(m != nullptr);
  DCHECK(f != nullptr);
  DCHECK(field_value != nullptr);
  JDWP::EventLocation location;
  SetEventLocation(&location, m, dex_pc);

  gJdwpState->PostFieldEvent(&location, f, this_object, field_value, true);
}

void Dbg::PostException(const ThrowLocation& throw_location,
                        mirror::ArtMethod* catch_method,
                        uint32_t catch_dex_pc, mirror::Throwable* exception_object) {
  if (!IsDebuggerActive()) {
    return;
  }
  JDWP::EventLocation exception_throw_location;
  SetEventLocation(&exception_throw_location, throw_location.GetMethod(), throw_location.GetDexPc());
  JDWP::EventLocation exception_catch_location;
  SetEventLocation(&exception_catch_location, catch_method, catch_dex_pc);

  gJdwpState->PostException(&exception_throw_location, exception_object, &exception_catch_location,
                            throw_location.GetThis());
}

void Dbg::PostClassPrepare(mirror::Class* c) {
  if (!IsDebuggerActive()) {
    return;
  }
  gJdwpState->PostClassPrepare(c);
}

void Dbg::UpdateDebugger(Thread* thread, mirror::Object* this_object,
                         mirror::ArtMethod* m, uint32_t dex_pc,
                         int event_flags, const JValue* return_value) {
  if (!IsDebuggerActive() || dex_pc == static_cast<uint32_t>(-2) /* fake method exit */) {
    return;
  }

  if (IsBreakpoint(m, dex_pc)) {
    event_flags |= kBreakpoint;
  }

  // If the debugger is single-stepping one of our threads, check to
  // see if we're that thread and we've reached a step point.
  const SingleStepControl* single_step_control = thread->GetSingleStepControl();
  DCHECK(single_step_control != nullptr);
  if (single_step_control->is_active) {
    CHECK(!m->IsNative());
    if (single_step_control->step_depth == JDWP::SD_INTO) {
      // Step into method calls.  We break when the line number
      // or method pointer changes.  If we're in SS_MIN mode, we
      // always stop.
      if (single_step_control->method != m) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new method";
      } else if (single_step_control->step_size == JDWP::SS_MIN) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new instruction";
      } else if (single_step_control->ContainsDexPc(dex_pc)) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS new line";
      }
    } else if (single_step_control->step_depth == JDWP::SD_OVER) {
      // Step over method calls.  We break when the line number is
      // different and the frame depth is <= the original frame
      // depth.  (We can't just compare on the method, because we
      // might get unrolled past it by an exception, and it's tricky
      // to identify recursion.)

      int stack_depth = GetStackDepth(thread);

      if (stack_depth < single_step_control->stack_depth) {
        // Popped up one or more frames, always trigger.
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      } else if (stack_depth == single_step_control->stack_depth) {
        // Same depth, see if we moved.
        if (single_step_control->step_size == JDWP::SS_MIN) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new instruction";
        } else if (single_step_control->ContainsDexPc(dex_pc)) {
          event_flags |= kSingleStep;
          VLOG(jdwp) << "SS new line";
        }
      }
    } else {
      CHECK_EQ(single_step_control->step_depth, JDWP::SD_OUT);
      // Return from the current method.  We break when the frame
      // depth pops up.

      // This differs from the "method exit" break in that it stops
      // with the PC at the next instruction in the returned-to
      // function, rather than the end of the returning function.

      int stack_depth = GetStackDepth(thread);
      if (stack_depth < single_step_control->stack_depth) {
        event_flags |= kSingleStep;
        VLOG(jdwp) << "SS method pop";
      }
    }
  }

  // If there's something interesting going on, see if it matches one
  // of the debugger filters.
  if (event_flags != 0) {
    Dbg::PostLocationEvent(m, dex_pc, this_object, event_flags, return_value);
  }
}

size_t* Dbg::GetReferenceCounterForEvent(uint32_t instrumentation_event) {
  switch (instrumentation_event) {
    case instrumentation::Instrumentation::kMethodEntered:
      return &method_enter_event_ref_count_;
    case instrumentation::Instrumentation::kMethodExited:
      return &method_exit_event_ref_count_;
    case instrumentation::Instrumentation::kDexPcMoved:
      return &dex_pc_change_event_ref_count_;
    case instrumentation::Instrumentation::kFieldRead:
      return &field_read_event_ref_count_;
    case instrumentation::Instrumentation::kFieldWritten:
      return &field_write_event_ref_count_;
    case instrumentation::Instrumentation::kExceptionCaught:
      return &exception_catch_event_ref_count_;
    default:
      return nullptr;
  }
}

// Process request while all mutator threads are suspended.
void Dbg::ProcessDeoptimizationRequest(const DeoptimizationRequest& request) {
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  switch (request.GetKind()) {
    case DeoptimizationRequest::kNothing:
      LOG(WARNING) << "Ignoring empty deoptimization request.";
      break;
    case DeoptimizationRequest::kRegisterForEvent:
      VLOG(jdwp) << StringPrintf("Add debugger as listener for instrumentation event 0x%x",
                                 request.InstrumentationEvent());
      instrumentation->AddListener(&gDebugInstrumentationListener, request.InstrumentationEvent());
      instrumentation_events_ |= request.InstrumentationEvent();
      break;
    case DeoptimizationRequest::kUnregisterForEvent:
      VLOG(jdwp) << StringPrintf("Remove debugger as listener for instrumentation event 0x%x",
                                 request.InstrumentationEvent());
      instrumentation->RemoveListener(&gDebugInstrumentationListener,
                                      request.InstrumentationEvent());
      instrumentation_events_ &= ~request.InstrumentationEvent();
      break;
    case DeoptimizationRequest::kFullDeoptimization:
      VLOG(jdwp) << "Deoptimize the world ...";
      instrumentation->DeoptimizeEverything();
      VLOG(jdwp) << "Deoptimize the world DONE";
      break;
    case DeoptimizationRequest::kFullUndeoptimization:
      VLOG(jdwp) << "Undeoptimize the world ...";
      instrumentation->UndeoptimizeEverything();
      VLOG(jdwp) << "Undeoptimize the world DONE";
      break;
    case DeoptimizationRequest::kSelectiveDeoptimization:
      VLOG(jdwp) << "Deoptimize method " << PrettyMethod(request.Method()) << " ...";
      instrumentation->Deoptimize(request.Method());
      VLOG(jdwp) << "Deoptimize method " << PrettyMethod(request.Method()) << " DONE";
      break;
    case DeoptimizationRequest::kSelectiveUndeoptimization:
      VLOG(jdwp) << "Undeoptimize method " << PrettyMethod(request.Method()) << " ...";
      instrumentation->Undeoptimize(request.Method());
      VLOG(jdwp) << "Undeoptimize method " << PrettyMethod(request.Method()) << " DONE";
      break;
    default:
      LOG(FATAL) << "Unsupported deoptimization request kind " << request.GetKind();
      break;
  }
}

void Dbg::DelayFullUndeoptimization() {
  MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
  ++delayed_full_undeoptimization_count_;
  DCHECK_LE(delayed_full_undeoptimization_count_, full_deoptimization_event_count_);
}

void Dbg::ProcessDelayedFullUndeoptimizations() {
  // TODO: avoid taking the lock twice (once here and once in ManageDeoptimization).
  {
    MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
    while (delayed_full_undeoptimization_count_ > 0) {
      DeoptimizationRequest req;
      req.SetKind(DeoptimizationRequest::kFullUndeoptimization);
      req.SetMethod(nullptr);
      RequestDeoptimizationLocked(req);
      --delayed_full_undeoptimization_count_;
    }
  }
  ManageDeoptimization();
}

void Dbg::RequestDeoptimization(const DeoptimizationRequest& req) {
  if (req.GetKind() == DeoptimizationRequest::kNothing) {
    // Nothing to do.
    return;
  }
  MutexLock mu(Thread::Current(), *Locks::deoptimization_lock_);
  RequestDeoptimizationLocked(req);
}

void Dbg::RequestDeoptimizationLocked(const DeoptimizationRequest& req) {
  switch (req.GetKind()) {
    case DeoptimizationRequest::kRegisterForEvent: {
      DCHECK_NE(req.InstrumentationEvent(), 0u);
      size_t* counter = GetReferenceCounterForEvent(req.InstrumentationEvent());
      CHECK(counter != nullptr) << StringPrintf("No counter for instrumentation event 0x%x",
                                                req.InstrumentationEvent());
      if (*counter == 0) {
        VLOG(jdwp) << StringPrintf("Queue request #%zd to start listening to instrumentation event 0x%x",
                                   deoptimization_requests_.size(), req.InstrumentationEvent());
        deoptimization_requests_.push_back(req);
      }
      *counter = *counter + 1;
      break;
    }
    case DeoptimizationRequest::kUnregisterForEvent: {
      DCHECK_NE(req.InstrumentationEvent(), 0u);
      size_t* counter = GetReferenceCounterForEvent(req.InstrumentationEvent());
      CHECK(counter != nullptr) << StringPrintf("No counter for instrumentation event 0x%x",
                                                req.InstrumentationEvent());
      *counter = *counter - 1;
      if (*counter == 0) {
        VLOG(jdwp) << StringPrintf("Queue request #%zd to stop listening to instrumentation event 0x%x",
                                   deoptimization_requests_.size(), req.InstrumentationEvent());
        deoptimization_requests_.push_back(req);
      }
      break;
    }
    case DeoptimizationRequest::kFullDeoptimization: {
      DCHECK(req.Method() == nullptr);
      if (full_deoptimization_event_count_ == 0) {
        VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                   << " for full deoptimization";
        deoptimization_requests_.push_back(req);
      }
      ++full_deoptimization_event_count_;
      break;
    }
    case DeoptimizationRequest::kFullUndeoptimization: {
      DCHECK(req.Method() == nullptr);
      DCHECK_GT(full_deoptimization_event_count_, 0U);
      --full_deoptimization_event_count_;
      if (full_deoptimization_event_count_ == 0) {
        VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                   << " for full undeoptimization";
        deoptimization_requests_.push_back(req);
      }
      break;
    }
    case DeoptimizationRequest::kSelectiveDeoptimization: {
      DCHECK(req.Method() != nullptr);
      VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                 << " for deoptimization of " << PrettyMethod(req.Method());
      deoptimization_requests_.push_back(req);
      break;
    }
    case DeoptimizationRequest::kSelectiveUndeoptimization: {
      DCHECK(req.Method() != nullptr);
      VLOG(jdwp) << "Queue request #" << deoptimization_requests_.size()
                 << " for undeoptimization of " << PrettyMethod(req.Method());
      deoptimization_requests_.push_back(req);
      break;
    }
    default: {
      LOG(FATAL) << "Unknown deoptimization request kind " << req.GetKind();
      break;
    }
  }
}

void Dbg::ManageDeoptimization() {
  Thread* const self = Thread::Current();
  {
    // Avoid suspend/resume if there is no pending request.
    MutexLock mu(self, *Locks::deoptimization_lock_);
    if (deoptimization_requests_.empty()) {
      return;
    }
  }
  CHECK_EQ(self->GetState(), kRunnable);
  self->TransitionFromRunnableToSuspended(kWaitingForDeoptimization);
  // We need to suspend mutator threads first.
  Runtime* const runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  const ThreadState old_state = self->SetStateUnsafe(kRunnable);
  {
    MutexLock mu(self, *Locks::deoptimization_lock_);
    size_t req_index = 0;
    for (DeoptimizationRequest& request : deoptimization_requests_) {
      VLOG(jdwp) << "Process deoptimization request #" << req_index++;
      ProcessDeoptimizationRequest(request);
    }
    deoptimization_requests_.clear();
  }
  CHECK_EQ(self->SetStateUnsafe(old_state), kRunnable);
  runtime->GetThreadList()->ResumeAll();
  self->TransitionFromSuspendedToRunnable();
}

static bool IsMethodPossiblyInlined(Thread* self, mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile::CodeItem* code_item = m->GetCodeItem();
  if (code_item == nullptr) {
    // TODO We should not be asked to watch location in a native or abstract method so the code item
    // should never be null. We could just check we never encounter this case.
    return false;
  }
  // Note: method verifier may cause thread suspension.
  self->AssertThreadSuspensionIsAllowable();
  StackHandleScope<3> hs(self);
  mirror::Class* declaring_class = m->GetDeclaringClass();
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
  Handle<mirror::ArtMethod> method(hs.NewHandle(m));
  verifier::MethodVerifier verifier(self, dex_cache->GetDexFile(), dex_cache, class_loader,
                                    &m->GetClassDef(), code_item, m->GetDexMethodIndex(), method,
                                    m->GetAccessFlags(), false, true, false);
  // Note: we don't need to verify the method.
  return InlineMethodAnalyser::AnalyseMethodCode(&verifier, nullptr);
}

static const Breakpoint* FindFirstBreakpointForMethod(mirror::ArtMethod* m)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::breakpoint_lock_) {
  for (Breakpoint& breakpoint : gBreakpoints) {
    if (breakpoint.Method() == m) {
      return &breakpoint;
    }
  }
  return nullptr;
}

// Sanity checks all existing breakpoints on the same method.
static void SanityCheckExistingBreakpoints(mirror::ArtMethod* m, bool need_full_deoptimization)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::breakpoint_lock_) {
  for (const Breakpoint& breakpoint : gBreakpoints) {
    CHECK_EQ(need_full_deoptimization, breakpoint.NeedFullDeoptimization());
  }
  if (need_full_deoptimization) {
    // We should have deoptimized everything but not "selectively" deoptimized this method.
    CHECK(Runtime::Current()->GetInstrumentation()->AreAllMethodsDeoptimized());
    CHECK(!Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
  } else {
    // We should have "selectively" deoptimized this method.
    // Note: while we have not deoptimized everything for this method, we may have done it for
    // another event.
    CHECK(Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
  }
}

// Installs a breakpoint at the specified location. Also indicates through the deoptimization
// request if we need to deoptimize.
void Dbg::WatchLocation(const JDWP::JdwpLocation* location, DeoptimizationRequest* req) {
  Thread* const self = Thread::Current();
  mirror::ArtMethod* m = FromMethodId(location->method_id);
  DCHECK(m != nullptr) << "No method for method id " << location->method_id;

  const Breakpoint* existing_breakpoint;
  {
    ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
    existing_breakpoint = FindFirstBreakpointForMethod(m);
  }
  bool need_full_deoptimization;
  if (existing_breakpoint == nullptr) {
    // There is no breakpoint on this method yet: we need to deoptimize. If this method may be
    // inlined, we deoptimize everything; otherwise we deoptimize only this method.
    // Note: IsMethodPossiblyInlined goes into the method verifier and may cause thread suspension.
    // Therefore we must not hold any lock when we call it.
    need_full_deoptimization = IsMethodPossiblyInlined(self, m);
    if (need_full_deoptimization) {
      req->SetKind(DeoptimizationRequest::kFullDeoptimization);
      req->SetMethod(nullptr);
    } else {
      req->SetKind(DeoptimizationRequest::kSelectiveDeoptimization);
      req->SetMethod(m);
    }
  } else {
    // There is at least one breakpoint for this method: we don't need to deoptimize.
    req->SetKind(DeoptimizationRequest::kNothing);
    req->SetMethod(nullptr);

    need_full_deoptimization = existing_breakpoint->NeedFullDeoptimization();
    if (kIsDebugBuild) {
      ReaderMutexLock mu(self, *Locks::breakpoint_lock_);
      SanityCheckExistingBreakpoints(m, need_full_deoptimization);
    }
  }

  {
    WriterMutexLock mu(self, *Locks::breakpoint_lock_);
    gBreakpoints.push_back(Breakpoint(m, location->dex_pc, need_full_deoptimization));
    VLOG(jdwp) << "Set breakpoint #" << (gBreakpoints.size() - 1) << ": "
               << gBreakpoints[gBreakpoints.size() - 1];
  }
}

// Uninstalls a breakpoint at the specified location. Also indicates through the deoptimization
// request if we need to undeoptimize.
void Dbg::UnwatchLocation(const JDWP::JdwpLocation* location, DeoptimizationRequest* req) {
  WriterMutexLock mu(Thread::Current(), *Locks::breakpoint_lock_);
  mirror::ArtMethod* m = FromMethodId(location->method_id);
  DCHECK(m != nullptr) << "No method for method id " << location->method_id;
  bool need_full_deoptimization = false;
  for (size_t i = 0, e = gBreakpoints.size(); i < e; ++i) {
    if (gBreakpoints[i].DexPc() == location->dex_pc && gBreakpoints[i].Method() == m) {
      VLOG(jdwp) << "Removed breakpoint #" << i << ": " << gBreakpoints[i];
      need_full_deoptimization = gBreakpoints[i].NeedFullDeoptimization();
      DCHECK_NE(need_full_deoptimization, Runtime::Current()->GetInstrumentation()->IsDeoptimized(m));
      gBreakpoints.erase(gBreakpoints.begin() + i);
      break;
    }
  }
  const Breakpoint* const existing_breakpoint = FindFirstBreakpointForMethod(m);
  if (existing_breakpoint == nullptr) {
    // There is no more breakpoint on this method: we need to undeoptimize.
    if (need_full_deoptimization) {
      // This method required full deoptimization: we need to undeoptimize everything.
      req->SetKind(DeoptimizationRequest::kFullUndeoptimization);
      req->SetMethod(nullptr);
    } else {
      // This method required selective deoptimization: we need to undeoptimize only that method.
      req->SetKind(DeoptimizationRequest::kSelectiveUndeoptimization);
      req->SetMethod(m);
    }
  } else {
    // There is at least one breakpoint for this method: we don't need to undeoptimize.
    req->SetKind(DeoptimizationRequest::kNothing);
    req->SetMethod(nullptr);
    if (kIsDebugBuild) {
      SanityCheckExistingBreakpoints(m, need_full_deoptimization);
    }
  }
}

// Scoped utility class to suspend a thread so that we may do tasks such as walk its stack. Doesn't
// cause suspension if the thread is the current thread.
class ScopedThreadSuspension {
 public:
  ScopedThreadSuspension(Thread* self, JDWP::ObjectId thread_id)
      LOCKS_EXCLUDED(Locks::thread_list_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
      thread_(nullptr),
      error_(JDWP::ERR_NONE),
      self_suspend_(false),
      other_suspend_(false) {
    ScopedObjectAccessUnchecked soa(self);
    {
      MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
      thread_ = DecodeThread(soa, thread_id, &error_);
    }
    if (error_ == JDWP::ERR_NONE) {
      if (thread_ == soa.Self()) {
        self_suspend_ = true;
      } else {
        soa.Self()->TransitionFromRunnableToSuspended(kWaitingForDebuggerSuspension);
        jobject thread_peer = Dbg::GetObjectRegistry()->GetJObject(thread_id);
        bool timed_out;
        Thread* suspended_thread;
        {
          // Take suspend thread lock to avoid races with threads trying to suspend this one.
          MutexLock mu(soa.Self(), *Locks::thread_list_suspend_thread_lock_);
          ThreadList* thread_list = Runtime::Current()->GetThreadList();
          suspended_thread = thread_list->SuspendThreadByPeer(thread_peer, true, true, &timed_out);
        }
        CHECK_EQ(soa.Self()->TransitionFromSuspendedToRunnable(), kWaitingForDebuggerSuspension);
        if (suspended_thread == nullptr) {
          // Thread terminated from under us while suspending.
          error_ = JDWP::ERR_INVALID_THREAD;
        } else {
          CHECK_EQ(suspended_thread, thread_);
          other_suspend_ = true;
        }
      }
    }
  }

  Thread* GetThread() const {
    return thread_;
  }

  JDWP::JdwpError GetError() const {
    return error_;
  }

  ~ScopedThreadSuspension() {
    if (other_suspend_) {
      Runtime::Current()->GetThreadList()->Resume(thread_, true);
    }
  }

 private:
  Thread* thread_;
  JDWP::JdwpError error_;
  bool self_suspend_;
  bool other_suspend_;
};

JDWP::JdwpError Dbg::ConfigureStep(JDWP::ObjectId thread_id, JDWP::JdwpStepSize step_size,
                                   JDWP::JdwpStepDepth step_depth) {
  Thread* self = Thread::Current();
  ScopedThreadSuspension sts(self, thread_id);
  if (sts.GetError() != JDWP::ERR_NONE) {
    return sts.GetError();
  }

  //
  // Work out what Method* we're in, the current line number, and how deep the stack currently
  // is for step-out.
  //

  struct SingleStepStackVisitor : public StackVisitor {
    explicit SingleStepStackVisitor(Thread* thread, SingleStepControl* single_step_control,
                                    int32_t* line_number)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
        : StackVisitor(thread, nullptr), single_step_control_(single_step_control),
          line_number_(line_number) {
      DCHECK_EQ(single_step_control_, thread->GetSingleStepControl());
      single_step_control_->method = nullptr;
      single_step_control_->stack_depth = 0;
    }

    // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
    // annotalysis.
    bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
      mirror::ArtMethod* m = GetMethod();
      if (!m->IsRuntimeMethod()) {
        ++single_step_control_->stack_depth;
        if (single_step_control_->method == nullptr) {
          mirror::DexCache* dex_cache = m->GetDeclaringClass()->GetDexCache();
          single_step_control_->method = m;
          *line_number_ = -1;
          if (dex_cache != nullptr) {
            const DexFile& dex_file = *dex_cache->GetDexFile();
            *line_number_ = dex_file.GetLineNumFromPC(m, GetDexPc());
          }
        }
      }
      return true;
    }

    SingleStepControl* const single_step_control_;
    int32_t* const line_number_;
  };

  Thread* const thread = sts.GetThread();
  SingleStepControl* const single_step_control = thread->GetSingleStepControl();
  DCHECK(single_step_control != nullptr);
  int32_t line_number = -1;
  SingleStepStackVisitor visitor(thread, single_step_control, &line_number);
  visitor.WalkStack();

  //
  // Find the dex_pc values that correspond to the current line, for line-based single-stepping.
  //

  struct DebugCallbackContext {
    explicit DebugCallbackContext(SingleStepControl* single_step_control, int32_t line_number,
                                  const DexFile::CodeItem* code_item)
      : single_step_control_(single_step_control), line_number_(line_number), code_item_(code_item),
        last_pc_valid(false), last_pc(0) {
    }

    static bool Callback(void* raw_context, uint32_t address, uint32_t line_number) {
      DebugCallbackContext* context = reinterpret_cast<DebugCallbackContext*>(raw_context);
      if (static_cast<int32_t>(line_number) == context->line_number_) {
        if (!context->last_pc_valid) {
          // Everything from this address until the next line change is ours.
          context->last_pc = address;
          context->last_pc_valid = true;
        }
        // Otherwise, if we're already in a valid range for this line,
        // just keep going (shouldn't really happen)...
      } else if (context->last_pc_valid) {  // and the line number is new
        // Add everything from the last entry up until here to the set
        for (uint32_t dex_pc = context->last_pc; dex_pc < address; ++dex_pc) {
          context->single_step_control_->dex_pcs.insert(dex_pc);
        }
        context->last_pc_valid = false;
      }
      return false;  // There may be multiple entries for any given line.
    }

    ~DebugCallbackContext() {
      // If the line number was the last in the position table...
      if (last_pc_valid) {
        size_t end = code_item_->insns_size_in_code_units_;
        for (uint32_t dex_pc = last_pc; dex_pc < end; ++dex_pc) {
          single_step_control_->dex_pcs.insert(dex_pc);
        }
      }
    }

    SingleStepControl* const single_step_control_;
    const int32_t line_number_;
    const DexFile::CodeItem* const code_item_;
    bool last_pc_valid;
    uint32_t last_pc;
  };
  single_step_control->dex_pcs.clear();
  mirror::ArtMethod* m = single_step_control->method;
  if (!m->IsNative()) {
    const DexFile::CodeItem* const code_item = m->GetCodeItem();
    DebugCallbackContext context(single_step_control, line_number, code_item);
    m->GetDexFile()->DecodeDebugInfo(code_item, m->IsStatic(), m->GetDexMethodIndex(),
                                     DebugCallbackContext::Callback, nullptr, &context);
  }

  //
  // Everything else...
  //

  single_step_control->step_size = step_size;
  single_step_control->step_depth = step_depth;
  single_step_control->is_active = true;

  if (VLOG_IS_ON(jdwp)) {
    VLOG(jdwp) << "Single-step thread: " << *thread;
    VLOG(jdwp) << "Single-step step size: " << single_step_control->step_size;
    VLOG(jdwp) << "Single-step step depth: " << single_step_control->step_depth;
    VLOG(jdwp) << "Single-step current method: " << PrettyMethod(single_step_control->method);
    VLOG(jdwp) << "Single-step current line: " << line_number;
    VLOG(jdwp) << "Single-step current stack depth: " << single_step_control->stack_depth;
    VLOG(jdwp) << "Single-step dex_pc values:";
    for (uint32_t dex_pc : single_step_control->dex_pcs) {
      VLOG(jdwp) << StringPrintf(" %#x", dex_pc);
    }
  }

  return JDWP::ERR_NONE;
}

void Dbg::UnconfigureStep(JDWP::ObjectId thread_id) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
  JDWP::JdwpError error;
  Thread* thread = DecodeThread(soa, thread_id, &error);
  if (error == JDWP::ERR_NONE) {
    SingleStepControl* single_step_control = thread->GetSingleStepControl();
    DCHECK(single_step_control != nullptr);
    single_step_control->Clear();
  }
}

static char JdwpTagToShortyChar(JDWP::JdwpTag tag) {
  switch (tag) {
    default:
      LOG(FATAL) << "unknown JDWP tag: " << PrintableChar(tag);
      UNREACHABLE();

    // Primitives.
    case JDWP::JT_BYTE:    return 'B';
    case JDWP::JT_CHAR:    return 'C';
    case JDWP::JT_FLOAT:   return 'F';
    case JDWP::JT_DOUBLE:  return 'D';
    case JDWP::JT_INT:     return 'I';
    case JDWP::JT_LONG:    return 'J';
    case JDWP::JT_SHORT:   return 'S';
    case JDWP::JT_VOID:    return 'V';
    case JDWP::JT_BOOLEAN: return 'Z';

    // Reference types.
    case JDWP::JT_ARRAY:
    case JDWP::JT_OBJECT:
    case JDWP::JT_STRING:
    case JDWP::JT_THREAD:
    case JDWP::JT_THREAD_GROUP:
    case JDWP::JT_CLASS_LOADER:
    case JDWP::JT_CLASS_OBJECT:
      return 'L';
  }
}

JDWP::JdwpError Dbg::InvokeMethod(JDWP::ObjectId thread_id, JDWP::ObjectId object_id,
                                  JDWP::RefTypeId class_id, JDWP::MethodId method_id,
                                  uint32_t arg_count, uint64_t* arg_values,
                                  JDWP::JdwpTag* arg_types, uint32_t options,
                                  JDWP::JdwpTag* pResultTag, uint64_t* pResultValue,
                                  JDWP::ObjectId* pExceptionId) {
  ThreadList* thread_list = Runtime::Current()->GetThreadList();

  Thread* targetThread = nullptr;
  DebugInvokeReq* req = nullptr;
  Thread* self = Thread::Current();
  {
    ScopedObjectAccessUnchecked soa(self);
    MutexLock mu(soa.Self(), *Locks::thread_list_lock_);
    JDWP::JdwpError error;
    targetThread = DecodeThread(soa, thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      LOG(ERROR) << "InvokeMethod request for invalid thread id " << thread_id;
      return error;
    }
    req = targetThread->GetInvokeReq();
    if (!req->ready) {
      LOG(ERROR) << "InvokeMethod request for thread not stopped by event: " << *targetThread;
      return JDWP::ERR_INVALID_THREAD;
    }

    /*
     * We currently have a bug where we don't successfully resume the
     * target thread if the suspend count is too deep.  We're expected to
     * require one "resume" for each "suspend", but when asked to execute
     * a method we have to resume fully and then re-suspend it back to the
     * same level.  (The easiest way to cause this is to type "suspend"
     * multiple times in jdb.)
     *
     * It's unclear what this means when the event specifies "resume all"
     * and some threads are suspended more deeply than others.  This is
     * a rare problem, so for now we just prevent it from hanging forever
     * by rejecting the method invocation request.  Without this, we will
     * be stuck waiting on a suspended thread.
     */
    int suspend_count;
    {
      MutexLock mu2(soa.Self(), *Locks::thread_suspend_count_lock_);
      suspend_count = targetThread->GetSuspendCount();
    }
    if (suspend_count > 1) {
      LOG(ERROR) << *targetThread << " suspend count too deep for method invocation: " << suspend_count;
      return JDWP::ERR_THREAD_SUSPENDED;  // Probably not expected here.
    }

    mirror::Object* receiver = gRegistry->Get<mirror::Object*>(object_id, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }

    mirror::Object* thread = gRegistry->Get<mirror::Object*>(thread_id, &error);
    if (error != JDWP::ERR_NONE) {
      return JDWP::ERR_INVALID_OBJECT;
    }
    // TODO: check that 'thread' is actually a java.lang.Thread!

    mirror::Class* c = DecodeClass(class_id, &error);
    if (c == nullptr) {
      return error;
    }

    mirror::ArtMethod* m = FromMethodId(method_id);
    if (m->IsStatic() != (receiver == nullptr)) {
      return JDWP::ERR_INVALID_METHODID;
    }
    if (m->IsStatic()) {
      if (m->GetDeclaringClass() != c) {
        return JDWP::ERR_INVALID_METHODID;
      }
    } else {
      if (!m->GetDeclaringClass()->IsAssignableFrom(c)) {
        return JDWP::ERR_INVALID_METHODID;
      }
    }

    // Check the argument list matches the method.
    uint32_t shorty_len = 0;
    const char* shorty = m->GetShorty(&shorty_len);
    if (shorty_len - 1 != arg_count) {
      return JDWP::ERR_ILLEGAL_ARGUMENT;
    }

    {
      StackHandleScope<3> hs(soa.Self());
      MethodHelper mh(hs.NewHandle(m));
      HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&receiver));
      HandleWrapper<mirror::Class> h_klass(hs.NewHandleWrapper(&c));
      const DexFile::TypeList* types = m->GetParameterTypeList();
      for (size_t i = 0; i < arg_count; ++i) {
        if (shorty[i + 1] != JdwpTagToShortyChar(arg_types[i])) {
          return JDWP::ERR_ILLEGAL_ARGUMENT;
        }

        if (shorty[i + 1] == 'L') {
          // Did we really get an argument of an appropriate reference type?
          mirror::Class* parameter_type = mh.GetClassFromTypeIdx(types->GetTypeItem(i).type_idx_);
          mirror::Object* argument = gRegistry->Get<mirror::Object*>(arg_values[i], &error);
          if (error != JDWP::ERR_NONE) {
            return JDWP::ERR_INVALID_OBJECT;
          }
          if (argument != nullptr && !argument->InstanceOf(parameter_type)) {
            return JDWP::ERR_ILLEGAL_ARGUMENT;
          }

          // Turn the on-the-wire ObjectId into a jobject.
          jvalue& v = reinterpret_cast<jvalue&>(arg_values[i]);
          v.l = gRegistry->GetJObject(arg_values[i]);
        }
      }
      // Update in case it moved.
      m = mh.GetMethod();
    }

    req->receiver = receiver;
    req->thread = thread;
    req->klass = c;
    req->method = m;
    req->arg_count = arg_count;
    req->arg_values = arg_values;
    req->options = options;
    req->invoke_needed = true;
  }

  // The fact that we've released the thread list lock is a bit risky --- if the thread goes
  // away we're sitting high and dry -- but we must release this before the ResumeAllThreads
  // call, and it's unwise to hold it during WaitForSuspend.

  {
    /*
     * We change our (JDWP thread) status, which should be THREAD_RUNNING,
     * so we can suspend for a GC if the invoke request causes us to
     * run out of memory.  It's also a good idea to change it before locking
     * the invokeReq mutex, although that should never be held for long.
     */
    self->TransitionFromRunnableToSuspended(kWaitingForDebuggerSend);

    VLOG(jdwp) << "    Transferring control to event thread";
    {
      MutexLock mu(self, req->lock);

      if ((options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
        VLOG(jdwp) << "      Resuming all threads";
        thread_list->UndoDebuggerSuspensions();
      } else {
        VLOG(jdwp) << "      Resuming event thread only";
        thread_list->Resume(targetThread, true);
      }

      // Wait for the request to finish executing.
      while (req->invoke_needed) {
        req->cond.Wait(self);
      }
    }
    VLOG(jdwp) << "    Control has returned from event thread";

    /* wait for thread to re-suspend itself */
    SuspendThread(thread_id, false /* request_suspension */);
    self->TransitionFromSuspendedToRunnable();
  }

  /*
   * Suspend the threads.  We waited for the target thread to suspend
   * itself, so all we need to do is suspend the others.
   *
   * The suspendAllThreads() call will double-suspend the event thread,
   * so we want to resume the target thread once to keep the books straight.
   */
  if ((options & JDWP::INVOKE_SINGLE_THREADED) == 0) {
    self->TransitionFromRunnableToSuspended(kWaitingForDebuggerSuspension);
    VLOG(jdwp) << "      Suspending all threads";
    thread_list->SuspendAllForDebugger();
    self->TransitionFromSuspendedToRunnable();
    VLOG(jdwp) << "      Resuming event thread to balance the count";
    thread_list->Resume(targetThread, true);
  }

  // Copy the result.
  *pResultTag = req->result_tag;
  if (IsPrimitiveTag(req->result_tag)) {
    *pResultValue = req->result_value.GetJ();
  } else {
    *pResultValue = gRegistry->Add(req->result_value.GetL());
  }
  *pExceptionId = req->exception;
  return req->error;
}

void Dbg::ExecuteMethod(DebugInvokeReq* pReq) {
  ScopedObjectAccess soa(Thread::Current());

  // We can be called while an exception is pending. We need
  // to preserve that across the method invocation.
  StackHandleScope<4> hs(soa.Self());
  auto old_throw_this_object = hs.NewHandle<mirror::Object>(nullptr);
  auto old_throw_method = hs.NewHandle<mirror::ArtMethod>(nullptr);
  auto old_exception = hs.NewHandle<mirror::Throwable>(nullptr);
  uint32_t old_throw_dex_pc;
  bool old_exception_report_flag;
  {
    ThrowLocation old_throw_location;
    mirror::Throwable* old_exception_obj = soa.Self()->GetException(&old_throw_location);
    old_throw_this_object.Assign(old_throw_location.GetThis());
    old_throw_method.Assign(old_throw_location.GetMethod());
    old_exception.Assign(old_exception_obj);
    old_throw_dex_pc = old_throw_location.GetDexPc();
    old_exception_report_flag = soa.Self()->IsExceptionReportedToInstrumentation();
    soa.Self()->ClearException();
  }

  // Translate the method through the vtable, unless the debugger wants to suppress it.
  MutableHandle<mirror::ArtMethod> m(hs.NewHandle(pReq->method));
  if ((pReq->options & JDWP::INVOKE_NONVIRTUAL) == 0 && pReq->receiver != nullptr) {
    mirror::ArtMethod* actual_method = pReq->klass->FindVirtualMethodForVirtualOrInterface(m.Get());
    if (actual_method != m.Get()) {
      VLOG(jdwp) << "ExecuteMethod translated " << PrettyMethod(m.Get()) << " to " << PrettyMethod(actual_method);
      m.Assign(actual_method);
    }
  }
  VLOG(jdwp) << "ExecuteMethod " << PrettyMethod(m.Get())
             << " receiver=" << pReq->receiver
             << " arg_count=" << pReq->arg_count;
  CHECK(m.Get() != nullptr);

  CHECK_EQ(sizeof(jvalue), sizeof(uint64_t));

  pReq->result_value = InvokeWithJValues(soa, pReq->receiver, soa.EncodeMethod(m.Get()),
                                         reinterpret_cast<jvalue*>(pReq->arg_values));

  mirror::Throwable* exception = soa.Self()->GetException(nullptr);
  soa.Self()->ClearException();
  pReq->exception = gRegistry->Add(exception);
  pReq->result_tag = BasicTagFromDescriptor(m.Get()->GetShorty());
  if (pReq->exception != 0) {
    VLOG(jdwp) << "  JDWP invocation returning with exception=" << exception
        << " " << exception->Dump();
    pReq->result_value.SetJ(0);
  } else if (pReq->result_tag == JDWP::JT_OBJECT) {
    /* if no exception thrown, examine object result more closely */
    JDWP::JdwpTag new_tag = TagFromObject(soa, pReq->result_value.GetL());
    if (new_tag != pReq->result_tag) {
      VLOG(jdwp) << "  JDWP promoted result from " << pReq->result_tag << " to " << new_tag;
      pReq->result_tag = new_tag;
    }

    /*
     * Register the object.  We don't actually need an ObjectId yet,
     * but we do need to be sure that the GC won't move or discard the
     * object when we switch out of RUNNING.  The ObjectId conversion
     * will add the object to the "do not touch" list.
     *
     * We can't use the "tracked allocation" mechanism here because
     * the object is going to be handed off to a different thread.
     */
    gRegistry->Add(pReq->result_value.GetL());
  }

  if (old_exception.Get() != nullptr) {
    ThrowLocation gc_safe_throw_location(old_throw_this_object.Get(), old_throw_method.Get(),
                                         old_throw_dex_pc);
    soa.Self()->SetException(gc_safe_throw_location, old_exception.Get());
    soa.Self()->SetExceptionReportedToInstrumentation(old_exception_report_flag);
  }
}

/*
 * "request" contains a full JDWP packet, possibly with multiple chunks.  We
 * need to process each, accumulate the replies, and ship the whole thing
 * back.
 *
 * Returns "true" if we have a reply.  The reply buffer is newly allocated,
 * and includes the chunk type/length, followed by the data.
 *
 * OLD-TODO: we currently assume that the request and reply include a single
 * chunk.  If this becomes inconvenient we will need to adapt.
 */
bool Dbg::DdmHandlePacket(JDWP::Request* request, uint8_t** pReplyBuf, int* pReplyLen) {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  uint32_t type = request->ReadUnsigned32("type");
  uint32_t length = request->ReadUnsigned32("length");

  // Create a byte[] corresponding to 'request'.
  size_t request_length = request->size();
  ScopedLocalRef<jbyteArray> dataArray(env, env->NewByteArray(request_length));
  if (dataArray.get() == nullptr) {
    LOG(WARNING) << "byte[] allocation failed: " << request_length;
    env->ExceptionClear();
    return false;
  }
  env->SetByteArrayRegion(dataArray.get(), 0, request_length,
                          reinterpret_cast<const jbyte*>(request->data()));
  request->Skip(request_length);

  // Run through and find all chunks.  [Currently just find the first.]
  ScopedByteArrayRO contents(env, dataArray.get());
  if (length != request_length) {
    LOG(WARNING) << StringPrintf("bad chunk found (len=%u pktLen=%zd)", length, request_length);
    return false;
  }

  // Call "private static Chunk dispatch(int type, byte[] data, int offset, int length)".
  ScopedLocalRef<jobject> chunk(env, env->CallStaticObjectMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                                                                 WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_dispatch,
                                                                 type, dataArray.get(), 0, length));
  if (env->ExceptionCheck()) {
    LOG(INFO) << StringPrintf("Exception thrown by dispatcher for 0x%08x", type);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
  }

  if (chunk.get() == nullptr) {
    return false;
  }

  /*
   * Pull the pieces out of the chunk.  We copy the results into a
   * newly-allocated buffer that the caller can free.  We don't want to
   * continue using the Chunk object because nothing has a reference to it.
   *
   * We could avoid this by returning type/data/offset/length and having
   * the caller be aware of the object lifetime issues, but that
   * integrates the JDWP code more tightly into the rest of the runtime, and doesn't work
   * if we have responses for multiple chunks.
   *
   * So we're pretty much stuck with copying data around multiple times.
   */
  ScopedLocalRef<jbyteArray> replyData(env, reinterpret_cast<jbyteArray>(env->GetObjectField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_data)));
  jint offset = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_offset);
  length = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_length);
  type = env->GetIntField(chunk.get(), WellKnownClasses::org_apache_harmony_dalvik_ddmc_Chunk_type);

  VLOG(jdwp) << StringPrintf("DDM reply: type=0x%08x data=%p offset=%d length=%d", type, replyData.get(), offset, length);
  if (length == 0 || replyData.get() == nullptr) {
    return false;
  }

  const int kChunkHdrLen = 8;
  uint8_t* reply = new uint8_t[length + kChunkHdrLen];
  if (reply == nullptr) {
    LOG(WARNING) << "malloc failed: " << (length + kChunkHdrLen);
    return false;
  }
  JDWP::Set4BE(reply + 0, type);
  JDWP::Set4BE(reply + 4, length);
  env->GetByteArrayRegion(replyData.get(), offset, length, reinterpret_cast<jbyte*>(reply + kChunkHdrLen));

  *pReplyBuf = reply;
  *pReplyLen = length + kChunkHdrLen;

  VLOG(jdwp) << StringPrintf("dvmHandleDdm returning type=%.4s %p len=%d", reinterpret_cast<char*>(reply), reply, length);
  return true;
}

void Dbg::DdmBroadcast(bool connect) {
  VLOG(jdwp) << "Broadcasting DDM " << (connect ? "connect" : "disconnect") << "...";

  Thread* self = Thread::Current();
  if (self->GetState() != kRunnable) {
    LOG(ERROR) << "DDM broadcast in thread state " << self->GetState();
    /* try anyway? */
  }

  JNIEnv* env = self->GetJniEnv();
  jint event = connect ? 1 /*DdmServer.CONNECTED*/ : 2 /*DdmServer.DISCONNECTED*/;
  env->CallStaticVoidMethod(WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer,
                            WellKnownClasses::org_apache_harmony_dalvik_ddmc_DdmServer_broadcast,
                            event);
  if (env->ExceptionCheck()) {
    LOG(ERROR) << "DdmServer.broadcast " << event << " failed";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
}

void Dbg::DdmConnected() {
  Dbg::DdmBroadcast(true);
}

void Dbg::DdmDisconnected() {
  Dbg::DdmBroadcast(false);
  gDdmThreadNotification = false;
}

/*
 * Send a notification when a thread starts, stops, or changes its name.
 *
 * Because we broadcast the full set of threads when the notifications are
 * first enabled, it's possible for "thread" to be actively executing.
 */
void Dbg::DdmSendThreadNotification(Thread* t, uint32_t type) {
  if (!gDdmThreadNotification) {
    return;
  }

  if (type == CHUNK_TYPE("THDE")) {
    uint8_t buf[4];
    JDWP::Set4BE(&buf[0], t->GetThreadId());
    Dbg::DdmSendChunk(CHUNK_TYPE("THDE"), 4, buf);
  } else {
    CHECK(type == CHUNK_TYPE("THCR") || type == CHUNK_TYPE("THNM")) << type;
    ScopedObjectAccessUnchecked soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::String> name(hs.NewHandle(t->GetThreadName(soa)));
    size_t char_count = (name.Get() != nullptr) ? name->GetLength() : 0;
    const jchar* chars = (name.Get() != nullptr) ? name->GetCharArray()->GetData() : nullptr;

    std::vector<uint8_t> bytes;
    JDWP::Append4BE(bytes, t->GetThreadId());
    JDWP::AppendUtf16BE(bytes, chars, char_count);
    CHECK_EQ(bytes.size(), char_count*2 + sizeof(uint32_t)*2);
    Dbg::DdmSendChunk(type, bytes);
  }
}

void Dbg::DdmSetThreadNotification(bool enable) {
  // Enable/disable thread notifications.
  gDdmThreadNotification = enable;
  if (enable) {
    // Suspend the VM then post thread start notifications for all threads. Threads attaching will
    // see a suspension in progress and block until that ends. They then post their own start
    // notification.
    SuspendVM();
    std::list<Thread*> threads;
    Thread* self = Thread::Current();
    {
      MutexLock mu(self, *Locks::thread_list_lock_);
      threads = Runtime::Current()->GetThreadList()->GetList();
    }
    {
      ScopedObjectAccess soa(self);
      for (Thread* thread : threads) {
        Dbg::DdmSendThreadNotification(thread, CHUNK_TYPE("THCR"));
      }
    }
    ResumeVM();
  }
}

void Dbg::PostThreadStartOrStop(Thread* t, uint32_t type) {
  if (IsDebuggerActive()) {
    gJdwpState->PostThreadChange(t, type == CHUNK_TYPE("THCR"));
  }
  Dbg::DdmSendThreadNotification(t, type);
}

void Dbg::PostThreadStart(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THCR"));
}

void Dbg::PostThreadDeath(Thread* t) {
  Dbg::PostThreadStartOrStop(t, CHUNK_TYPE("THDE"));
}

void Dbg::DdmSendChunk(uint32_t type, size_t byte_count, const uint8_t* buf) {
  CHECK(buf != nullptr);
  iovec vec[1];
  vec[0].iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(buf));
  vec[0].iov_len = byte_count;
  Dbg::DdmSendChunkV(type, vec, 1);
}

void Dbg::DdmSendChunk(uint32_t type, const std::vector<uint8_t>& bytes) {
  DdmSendChunk(type, bytes.size(), &bytes[0]);
}

void Dbg::DdmSendChunkV(uint32_t type, const iovec* iov, int iov_count) {
  if (gJdwpState == nullptr) {
    VLOG(jdwp) << "Debugger thread not active, ignoring DDM send: " << type;
  } else {
    gJdwpState->DdmSendChunkV(type, iov, iov_count);
  }
}

int Dbg::DdmHandleHpifChunk(HpifWhen when) {
  if (when == HPIF_WHEN_NOW) {
    DdmSendHeapInfo(when);
    return true;
  }

  if (when != HPIF_WHEN_NEVER && when != HPIF_WHEN_NEXT_GC && when != HPIF_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpifWhen value: " << static_cast<int>(when);
    return false;
  }

  gDdmHpifWhen = when;
  return true;
}

bool Dbg::DdmHandleHpsgNhsgChunk(Dbg::HpsgWhen when, Dbg::HpsgWhat what, bool native) {
  if (when != HPSG_WHEN_NEVER && when != HPSG_WHEN_EVERY_GC) {
    LOG(ERROR) << "invalid HpsgWhen value: " << static_cast<int>(when);
    return false;
  }

  if (what != HPSG_WHAT_MERGED_OBJECTS && what != HPSG_WHAT_DISTINCT_OBJECTS) {
    LOG(ERROR) << "invalid HpsgWhat value: " << static_cast<int>(what);
    return false;
  }

  if (native) {
    gDdmNhsgWhen = when;
    gDdmNhsgWhat = what;
  } else {
    gDdmHpsgWhen = when;
    gDdmHpsgWhat = what;
  }
  return true;
}

void Dbg::DdmSendHeapInfo(HpifWhen reason) {
  // If there's a one-shot 'when', reset it.
  if (reason == gDdmHpifWhen) {
    if (gDdmHpifWhen == HPIF_WHEN_NEXT_GC) {
      gDdmHpifWhen = HPIF_WHEN_NEVER;
    }
  }

  /*
   * Chunk HPIF (client --> server)
   *
   * Heap Info. General information about the heap,
   * suitable for a summary display.
   *
   *   [u4]: number of heaps
   *
   *   For each heap:
   *     [u4]: heap ID
   *     [u8]: timestamp in ms since Unix epoch
   *     [u1]: capture reason (same as 'when' value from server)
   *     [u4]: max heap size in bytes (-Xmx)
   *     [u4]: current heap size in bytes
   *     [u4]: current number of bytes allocated
   *     [u4]: current number of objects allocated
   */
  uint8_t heap_count = 1;
  gc::Heap* heap = Runtime::Current()->GetHeap();
  std::vector<uint8_t> bytes;
  JDWP::Append4BE(bytes, heap_count);
  JDWP::Append4BE(bytes, 1);  // Heap id (bogus; we only have one heap).
  JDWP::Append8BE(bytes, MilliTime());
  JDWP::Append1BE(bytes, reason);
  JDWP::Append4BE(bytes, heap->GetMaxMemory());  // Max allowed heap size in bytes.
  JDWP::Append4BE(bytes, heap->GetTotalMemory());  // Current heap size in bytes.
  JDWP::Append4BE(bytes, heap->GetBytesAllocated());
  JDWP::Append4BE(bytes, heap->GetObjectsAllocated());
  CHECK_EQ(bytes.size(), 4U + (heap_count * (4 + 8 + 1 + 4 + 4 + 4 + 4)));
  Dbg::DdmSendChunk(CHUNK_TYPE("HPIF"), bytes);
}

enum HpsgSolidity {
  SOLIDITY_FREE = 0,
  SOLIDITY_HARD = 1,
  SOLIDITY_SOFT = 2,
  SOLIDITY_WEAK = 3,
  SOLIDITY_PHANTOM = 4,
  SOLIDITY_FINALIZABLE = 5,
  SOLIDITY_SWEEP = 6,
};

enum HpsgKind {
  KIND_OBJECT = 0,
  KIND_CLASS_OBJECT = 1,
  KIND_ARRAY_1 = 2,
  KIND_ARRAY_2 = 3,
  KIND_ARRAY_4 = 4,
  KIND_ARRAY_8 = 5,
  KIND_UNKNOWN = 6,
  KIND_NATIVE = 7,
};

#define HPSG_PARTIAL (1<<7)
#define HPSG_STATE(solidity, kind) ((uint8_t)((((kind) & 0x7) << 3) | ((solidity) & 0x7)))

class HeapChunkContext {
 public:
  // Maximum chunk size.  Obtain this from the formula:
  // (((maximum_heap_size / ALLOCATION_UNIT_SIZE) + 255) / 256) * 2
  HeapChunkContext(bool merge, bool native)
      : buf_(16384 - 16),
        type_(0),
        merge_(merge),
        chunk_overhead_(0) {
    Reset();
    if (native) {
      type_ = CHUNK_TYPE("NHSG");
    } else {
      type_ = merge ? CHUNK_TYPE("HPSG") : CHUNK_TYPE("HPSO");
    }
  }

  ~HeapChunkContext() {
    if (p_ > &buf_[0]) {
      Flush();
    }
  }

  void SetChunkOverhead(size_t chunk_overhead) {
    chunk_overhead_ = chunk_overhead;
  }

  void ResetStartOfNextChunk() {
    startOfNextMemoryChunk_ = nullptr;
  }

  void EnsureHeader(const void* chunk_ptr) {
    if (!needHeader_) {
      return;
    }

    // Start a new HPSx chunk.
    JDWP::Write4BE(&p_, 1);  // Heap id (bogus; we only have one heap).
    JDWP::Write1BE(&p_, 8);  // Size of allocation unit, in bytes.

    JDWP::Write4BE(&p_, reinterpret_cast<uintptr_t>(chunk_ptr));  // virtual address of segment start.
    JDWP::Write4BE(&p_, 0);  // offset of this piece (relative to the virtual address).
    // [u4]: length of piece, in allocation units
    // We won't know this until we're done, so save the offset and stuff in a dummy value.
    pieceLenField_ = p_;
    JDWP::Write4BE(&p_, 0x55555555);
    needHeader_ = false;
  }

  void Flush() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (pieceLenField_ == nullptr) {
      // Flush immediately post Reset (maybe back-to-back Flush). Ignore.
      CHECK(needHeader_);
      return;
    }
    // Patch the "length of piece" field.
    CHECK_LE(&buf_[0], pieceLenField_);
    CHECK_LE(pieceLenField_, p_);
    JDWP::Set4BE(pieceLenField_, totalAllocationUnits_);

    Dbg::DdmSendChunk(type_, p_ - &buf_[0], &buf_[0]);
    Reset();
  }

  static void HeapChunkCallback(void* start, void* end, size_t used_bytes, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
    reinterpret_cast<HeapChunkContext*>(arg)->HeapChunkCallback(start, end, used_bytes);
  }

 private:
  enum { ALLOCATION_UNIT_SIZE = 8 };

  void Reset() {
    p_ = &buf_[0];
    ResetStartOfNextChunk();
    totalAllocationUnits_ = 0;
    needHeader_ = true;
    pieceLenField_ = nullptr;
  }

  void HeapChunkCallback(void* start, void* /*end*/, size_t used_bytes)
      SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                            Locks::mutator_lock_) {
    // Note: heap call backs cannot manipulate the heap upon which they are crawling, care is taken
    // in the following code not to allocate memory, by ensuring buf_ is of the correct size
    if (used_bytes == 0) {
        if (start == nullptr) {
            // Reset for start of new heap.
            startOfNextMemoryChunk_ = nullptr;
            Flush();
        }
        // Only process in use memory so that free region information
        // also includes dlmalloc book keeping.
        return;
    }

    /* If we're looking at the native heap, we'll just return
     * (SOLIDITY_HARD, KIND_NATIVE) for all allocated chunks
     */
    bool native = type_ == CHUNK_TYPE("NHSG");

    // TODO: I'm not sure using start of next chunk works well with multiple spaces. We shouldn't
    // count gaps inbetween spaces as free memory.
    if (startOfNextMemoryChunk_ != nullptr) {
        // Transmit any pending free memory. Native free memory of
        // over kMaxFreeLen could be because of the use of mmaps, so
        // don't report. If not free memory then start a new segment.
        bool flush = true;
        if (start > startOfNextMemoryChunk_) {
            const size_t kMaxFreeLen = 2 * kPageSize;
            void* freeStart = startOfNextMemoryChunk_;
            void* freeEnd = start;
            size_t freeLen = reinterpret_cast<char*>(freeEnd) - reinterpret_cast<char*>(freeStart);
            if (!native || freeLen < kMaxFreeLen) {
                AppendChunk(HPSG_STATE(SOLIDITY_FREE, 0), freeStart, freeLen);
                flush = false;
            }
        }
        if (flush) {
            startOfNextMemoryChunk_ = nullptr;
            Flush();
        }
    }
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(start);

    // Determine the type of this chunk.
    // OLD-TODO: if context.merge, see if this chunk is different from the last chunk.
    // If it's the same, we should combine them.
    uint8_t state = ExamineObject(obj, native);
    AppendChunk(state, start, used_bytes + chunk_overhead_);
    startOfNextMemoryChunk_ = reinterpret_cast<char*>(start) + used_bytes + chunk_overhead_;
  }

  void AppendChunk(uint8_t state, void* ptr, size_t length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // Make sure there's enough room left in the buffer.
    // We need to use two bytes for every fractional 256 allocation units used by the chunk plus
    // 17 bytes for any header.
    size_t needed = (((length/ALLOCATION_UNIT_SIZE + 255) / 256) * 2) + 17;
    size_t bytesLeft = buf_.size() - (size_t)(p_ - &buf_[0]);
    if (bytesLeft < needed) {
      Flush();
    }

    bytesLeft = buf_.size() - (size_t)(p_ - &buf_[0]);
    if (bytesLeft < needed) {
      LOG(WARNING) << "Chunk is too big to transmit (chunk_len=" << length << ", "
          << needed << " bytes)";
      return;
    }
    EnsureHeader(ptr);
    // Write out the chunk description.
    length /= ALLOCATION_UNIT_SIZE;   // Convert to allocation units.
    totalAllocationUnits_ += length;
    while (length > 256) {
      *p_++ = state | HPSG_PARTIAL;
      *p_++ = 255;     // length - 1
      length -= 256;
    }
    *p_++ = state;
    *p_++ = length - 1;
  }

  uint8_t ExamineObject(mirror::Object* o, bool is_native_heap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_, Locks::heap_bitmap_lock_) {
    if (o == nullptr) {
      return HPSG_STATE(SOLIDITY_FREE, 0);
    }

    // It's an allocated chunk. Figure out what it is.

    // If we're looking at the native heap, we'll just return
    // (SOLIDITY_HARD, KIND_NATIVE) for all allocated chunks.
    if (is_native_heap) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
    }

    if (!Runtime::Current()->GetHeap()->IsLiveObjectLocked(o)) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
    }

    mirror::Class* c = o->GetClass();
    if (c == nullptr) {
      // The object was probably just created but hasn't been initialized yet.
      return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
    }

    if (!Runtime::Current()->GetHeap()->IsValidObjectAddress(c)) {
      LOG(ERROR) << "Invalid class for managed heap object: " << o << " " << c;
      return HPSG_STATE(SOLIDITY_HARD, KIND_UNKNOWN);
    }

    if (c->IsClassClass()) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_CLASS_OBJECT);
    }

    if (c->IsArrayClass()) {
      if (o->IsObjectArray()) {
        return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      }
      switch (c->GetComponentSize()) {
      case 1: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_1);
      case 2: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_2);
      case 4: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_4);
      case 8: return HPSG_STATE(SOLIDITY_HARD, KIND_ARRAY_8);
      }
    }

    return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
  }

  std::vector<uint8_t> buf_;
  uint8_t* p_;
  uint8_t* pieceLenField_;
  void* startOfNextMemoryChunk_;
  size_t totalAllocationUnits_;
  uint32_t type_;
  bool merge_;
  bool needHeader_;
  size_t chunk_overhead_;

  DISALLOW_COPY_AND_ASSIGN(HeapChunkContext);
};

static void BumpPointerSpaceCallback(mirror::Object* obj, void* arg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) EXCLUSIVE_LOCKS_REQUIRED(Locks::heap_bitmap_lock_) {
  const size_t size = RoundUp(obj->SizeOf(), kObjectAlignment);
  HeapChunkContext::HeapChunkCallback(
      obj, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(obj) + size), size, arg);
}

void Dbg::DdmSendHeapSegments(bool native) {
  Dbg::HpsgWhen when;
  Dbg::HpsgWhat what;
  if (!native) {
    when = gDdmHpsgWhen;
    what = gDdmHpsgWhat;
  } else {
    when = gDdmNhsgWhen;
    what = gDdmNhsgWhat;
  }
  if (when == HPSG_WHEN_NEVER) {
    return;
  }

  // Figure out what kind of chunks we'll be sending.
  CHECK(what == HPSG_WHAT_MERGED_OBJECTS || what == HPSG_WHAT_DISTINCT_OBJECTS) << static_cast<int>(what);

  // First, send a heap start chunk.
  uint8_t heap_id[4];
  JDWP::Set4BE(&heap_id[0], 1);  // Heap id (bogus; we only have one heap).
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHST") : CHUNK_TYPE("HPST"), sizeof(heap_id), heap_id);

  Thread* self = Thread::Current();

  Locks::mutator_lock_->AssertSharedHeld(self);

  // Send a series of heap segment chunks.
  HeapChunkContext context((what == HPSG_WHAT_MERGED_OBJECTS), native);
  if (native) {
#ifdef USE_DLMALLOC
    dlmalloc_inspect_all(HeapChunkContext::HeapChunkCallback, &context);
#else
    UNIMPLEMENTED(WARNING) << "Native heap inspection is only supported with dlmalloc";
#endif
  } else {
    gc::Heap* heap = Runtime::Current()->GetHeap();
    for (const auto& space : heap->GetContinuousSpaces()) {
      if (space->IsDlMallocSpace()) {
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        // dlmalloc's chunk header is 2 * sizeof(size_t), but if the previous chunk is in use for an
        // allocation then the first sizeof(size_t) may belong to it.
        context.SetChunkOverhead(sizeof(size_t));
        space->AsDlMallocSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
      } else if (space->IsRosAllocSpace()) {
        context.SetChunkOverhead(0);
        // Need to acquire the mutator lock before the heap bitmap lock with exclusive access since
        // RosAlloc's internal logic doesn't know to release and reacquire the heap bitmap lock.
        self->TransitionFromRunnableToSuspended(kSuspended);
        ThreadList* tl = Runtime::Current()->GetThreadList();
        tl->SuspendAll();
        {
          ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
          space->AsRosAllocSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
        }
        tl->ResumeAll();
        self->TransitionFromSuspendedToRunnable();
      } else if (space->IsBumpPointerSpace()) {
        ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
        context.SetChunkOverhead(0);
        space->AsBumpPointerSpace()->Walk(BumpPointerSpaceCallback, &context);
      } else {
        UNIMPLEMENTED(WARNING) << "Not counting objects in space " << *space;
      }
      context.ResetStartOfNextChunk();
    }
    ReaderMutexLock mu(self, *Locks::heap_bitmap_lock_);
    // Walk the large objects, these are not in the AllocSpace.
    context.SetChunkOverhead(0);
    heap->GetLargeObjectsSpace()->Walk(HeapChunkContext::HeapChunkCallback, &context);
  }

  // Finally, send a heap end chunk.
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHEN") : CHUNK_TYPE("HPEN"), sizeof(heap_id), heap_id);
}

static size_t GetAllocTrackerMax() {
#ifdef HAVE_ANDROID_OS
  // Check whether there's a system property overriding the number of records.
  const char* propertyName = "dalvik.vm.allocTrackerMax";
  char allocRecordMaxString[PROPERTY_VALUE_MAX];
  if (property_get(propertyName, allocRecordMaxString, "") > 0) {
    char* end;
    size_t value = strtoul(allocRecordMaxString, &end, 10);
    if (*end != '\0') {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << allocRecordMaxString
                 << "' --- invalid";
      return kDefaultNumAllocRecords;
    }
    if (!IsPowerOfTwo(value)) {
      LOG(ERROR) << "Ignoring  " << propertyName << " '" << allocRecordMaxString
                 << "' --- not power of two";
      return kDefaultNumAllocRecords;
    }
    return value;
  }
#endif
  return kDefaultNumAllocRecords;
}

void Dbg::SetAllocTrackingEnabled(bool enable) {
  Thread* self = Thread::Current();
  if (enable) {
    {
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (recent_allocation_records_ != nullptr) {
        return;  // Already enabled, bail.
      }
      alloc_record_max_ = GetAllocTrackerMax();
      LOG(INFO) << "Enabling alloc tracker (" << alloc_record_max_ << " entries of "
                << kMaxAllocRecordStackDepth << " frames, taking "
                << PrettySize(sizeof(AllocRecord) * alloc_record_max_) << ")";
      DCHECK_EQ(alloc_record_head_, 0U);
      DCHECK_EQ(alloc_record_count_, 0U);
      recent_allocation_records_ = new AllocRecord[alloc_record_max_];
      CHECK(recent_allocation_records_ != nullptr);
    }
    Runtime::Current()->GetInstrumentation()->InstrumentQuickAllocEntryPoints();
  } else {
    {
      ScopedObjectAccess soa(self);  // For type_cache_.Clear();
      MutexLock mu(self, *Locks::alloc_tracker_lock_);
      if (recent_allocation_records_ == nullptr) {
        return;  // Already disabled, bail.
      }
      LOG(INFO) << "Disabling alloc tracker";
      delete[] recent_allocation_records_;
      recent_allocation_records_ = nullptr;
      alloc_record_head_ = 0;
      alloc_record_count_ = 0;
      type_cache_.Clear();
    }
    // If an allocation comes in before we uninstrument, we will safely drop it on the floor.
    Runtime::Current()->GetInstrumentation()->UninstrumentQuickAllocEntryPoints();
  }
}

struct AllocRecordStackVisitor : public StackVisitor {
  AllocRecordStackVisitor(Thread* thread, AllocRecord* record)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : StackVisitor(thread, nullptr), record(record), depth(0) {}

  // TODO: Enable annotalysis. We know lock is held in constructor, but abstraction confuses
  // annotalysis.
  bool VisitFrame() NO_THREAD_SAFETY_ANALYSIS {
    if (depth >= kMaxAllocRecordStackDepth) {
      return false;
    }
    mirror::ArtMethod* m = GetMethod();
    if (!m->IsRuntimeMethod()) {
      record->StackElement(depth)->SetMethod(m);
      record->StackElement(depth)->SetDexPc(GetDexPc());
      ++depth;
    }
    return true;
  }

  ~AllocRecordStackVisitor() {
    // Clear out any unused stack trace elements.
    for (; depth < kMaxAllocRecordStackDepth; ++depth) {
      record->StackElement(depth)->SetMethod(nullptr);
      record->StackElement(depth)->SetDexPc(0);
    }
  }

  AllocRecord* record;
  size_t depth;
};

void Dbg::RecordAllocation(Thread* self, mirror::Class* type, size_t byte_count) {
  MutexLock mu(self, *Locks::alloc_tracker_lock_);
  if (recent_allocation_records_ == nullptr) {
    // In the process of shutting down recording, bail.
    return;
  }

  // Advance and clip.
  if (++alloc_record_head_ == alloc_record_max_) {
    alloc_record_head_ = 0;
  }

  // Fill in the basics.
  AllocRecord* record = &recent_allocation_records_[alloc_record_head_];
  record->SetType(type);
  record->SetByteCount(byte_count);
  record->SetThinLockId(self->GetThreadId());

  // Fill in the stack trace.
  AllocRecordStackVisitor visitor(self, record);
  visitor.WalkStack();

  if (alloc_record_count_ < alloc_record_max_) {
    ++alloc_record_count_;
  }
}

// Returns the index of the head element.
//
// We point at the most-recently-written record, so if alloc_record_count_ is 1
// we want to use the current element.  Take "head+1" and subtract count
// from it.
//
// We need to handle underflow in our circular buffer, so we add
// alloc_record_max_ and then mask it back down.
size_t Dbg::HeadIndex() {
  return (Dbg::alloc_record_head_ + 1 + Dbg::alloc_record_max_ - Dbg::alloc_record_count_) &
      (Dbg::alloc_record_max_ - 1);
}

void Dbg::DumpRecentAllocations() {
  ScopedObjectAccess soa(Thread::Current());
  MutexLock mu(soa.Self(), *Locks::alloc_tracker_lock_);
  if (recent_allocation_records_ == nullptr) {
    LOG(INFO) << "Not recording tracked allocations";
    return;
  }

  // "i" is the head of the list.  We want to start at the end of the
  // list and move forward to the tail.
  size_t i = HeadIndex();
  const uint16_t capped_count = CappedAllocRecordCount(Dbg::alloc_record_count_);
  uint16_t count = capped_count;

  LOG(INFO) << "Tracked allocations, (head=" << alloc_record_head_ << " count=" << count << ")";
  while (count--) {
    AllocRecord* record = &recent_allocation_records_[i];

    LOG(INFO) << StringPrintf(" Thread %-2d %6zd bytes ", record->ThinLockId(), record->ByteCount())
              << PrettyClass(record->Type());

    for (size_t stack_frame = 0; stack_frame < kMaxAllocRecordStackDepth; ++stack_frame) {
      AllocRecordStackTraceElement* stack_element = record->StackElement(stack_frame);
      mirror::ArtMethod* m = stack_element->Method();
      if (m == nullptr) {
        break;
      }
      LOG(INFO) << "    " << PrettyMethod(m) << " line " << stack_element->LineNumber();
    }

    // pause periodically to help logcat catch up
    if ((count % 5) == 0) {
      usleep(40000);
    }

    i = (i + 1) & (alloc_record_max_ - 1);
  }
}

class StringTable {
 public:
  StringTable() {
  }

  void Add(const std::string& str) {
    table_.insert(str);
  }

  void Add(const char* str) {
    table_.insert(str);
  }

  size_t IndexOf(const char* s) const {
    auto it = table_.find(s);
    if (it == table_.end()) {
      LOG(FATAL) << "IndexOf(\"" << s << "\") failed";
    }
    return std::distance(table_.begin(), it);
  }

  size_t Size() const {
    return table_.size();
  }

  void WriteTo(std::vector<uint8_t>& bytes) const {
    for (const std::string& str : table_) {
      const char* s = str.c_str();
      size_t s_len = CountModifiedUtf8Chars(s);
      std::unique_ptr<uint16_t> s_utf16(new uint16_t[s_len]);
      ConvertModifiedUtf8ToUtf16(s_utf16.get(), s);
      JDWP::AppendUtf16BE(bytes, s_utf16.get(), s_len);
    }
  }

 private:
  std::set<std::string> table_;
  DISALLOW_COPY_AND_ASSIGN(StringTable);
};

static const char* GetMethodSourceFile(mirror::ArtMethod* method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(method != nullptr);
  const char* source_file = method->GetDeclaringClassSourceFile();
  return (source_file != nullptr) ? source_file : "";
}

/*
 * The data we send to DDMS contains everything we have recorded.
 *
 * Message header (all values big-endian):
 * (1b) message header len (to allow future expansion); includes itself
 * (1b) entry header len
 * (1b) stack frame len
 * (2b) number of entries
 * (4b) offset to string table from start of message
 * (2b) number of class name strings
 * (2b) number of method name strings
 * (2b) number of source file name strings
 * For each entry:
 *   (4b) total allocation size
 *   (2b) thread id
 *   (2b) allocated object's class name index
 *   (1b) stack depth
 *   For each stack frame:
 *     (2b) method's class name
 *     (2b) method name
 *     (2b) method source file
 *     (2b) line number, clipped to 32767; -2 if native; -1 if no source
 * (xb) class name strings
 * (xb) method name strings
 * (xb) source file strings
 *
 * As with other DDM traffic, strings are sent as a 4-byte length
 * followed by UTF-16 data.
 *
 * We send up 16-bit unsigned indexes into string tables.  In theory there
 * can be (kMaxAllocRecordStackDepth * alloc_record_max_) unique strings in
 * each table, but in practice there should be far fewer.
 *
 * The chief reason for using a string table here is to keep the size of
 * the DDMS message to a minimum.  This is partly to make the protocol
 * efficient, but also because we have to form the whole thing up all at
 * once in a memory buffer.
 *
 * We use separate string tables for class names, method names, and source
 * files to keep the indexes small.  There will generally be no overlap
 * between the contents of these tables.
 */
jbyteArray Dbg::GetRecentAllocations() {
  if (false) {
    DumpRecentAllocations();
  }

  Thread* self = Thread::Current();
  std::vector<uint8_t> bytes;
  {
    MutexLock mu(self, *Locks::alloc_tracker_lock_);
    //
    // Part 1: generate string tables.
    //
    StringTable class_names;
    StringTable method_names;
    StringTable filenames;

    const uint16_t capped_count = CappedAllocRecordCount(Dbg::alloc_record_count_);
    uint16_t count = capped_count;
    size_t idx = HeadIndex();
    while (count--) {
      AllocRecord* record = &recent_allocation_records_[idx];
      std::string temp;
      class_names.Add(record->Type()->GetDescriptor(&temp));
      for (size_t i = 0; i < kMaxAllocRecordStackDepth; i++) {
        mirror::ArtMethod* m = record->StackElement(i)->Method();
        if (m != nullptr) {
          class_names.Add(m->GetDeclaringClassDescriptor());
          method_names.Add(m->GetName());
          filenames.Add(GetMethodSourceFile(m));
        }
      }

      idx = (idx + 1) & (alloc_record_max_ - 1);
    }

    LOG(INFO) << "allocation records: " << capped_count;

    //
    // Part 2: Generate the output and store it in the buffer.
    //

    // (1b) message header len (to allow future expansion); includes itself
    // (1b) entry header len
    // (1b) stack frame len
    const int kMessageHeaderLen = 15;
    const int kEntryHeaderLen = 9;
    const int kStackFrameLen = 8;
    JDWP::Append1BE(bytes, kMessageHeaderLen);
    JDWP::Append1BE(bytes, kEntryHeaderLen);
    JDWP::Append1BE(bytes, kStackFrameLen);

    // (2b) number of entries
    // (4b) offset to string table from start of message
    // (2b) number of class name strings
    // (2b) number of method name strings
    // (2b) number of source file name strings
    JDWP::Append2BE(bytes, capped_count);
    size_t string_table_offset = bytes.size();
    JDWP::Append4BE(bytes, 0);  // We'll patch this later...
    JDWP::Append2BE(bytes, class_names.Size());
    JDWP::Append2BE(bytes, method_names.Size());
    JDWP::Append2BE(bytes, filenames.Size());

    idx = HeadIndex();
    std::string temp;
    for (count = capped_count; count != 0; --count) {
      // For each entry:
      // (4b) total allocation size
      // (2b) thread id
      // (2b) allocated object's class name index
      // (1b) stack depth
      AllocRecord* record = &recent_allocation_records_[idx];
      size_t stack_depth = record->GetDepth();
      size_t allocated_object_class_name_index =
          class_names.IndexOf(record->Type()->GetDescriptor(&temp));
      JDWP::Append4BE(bytes, record->ByteCount());
      JDWP::Append2BE(bytes, record->ThinLockId());
      JDWP::Append2BE(bytes, allocated_object_class_name_index);
      JDWP::Append1BE(bytes, stack_depth);

      for (size_t stack_frame = 0; stack_frame < stack_depth; ++stack_frame) {
        // For each stack frame:
        // (2b) method's class name
        // (2b) method name
        // (2b) method source file
        // (2b) line number, clipped to 32767; -2 if native; -1 if no source
        mirror::ArtMethod* m = record->StackElement(stack_frame)->Method();
        size_t class_name_index = class_names.IndexOf(m->GetDeclaringClassDescriptor());
        size_t method_name_index = method_names.IndexOf(m->GetName());
        size_t file_name_index = filenames.IndexOf(GetMethodSourceFile(m));
        JDWP::Append2BE(bytes, class_name_index);
        JDWP::Append2BE(bytes, method_name_index);
        JDWP::Append2BE(bytes, file_name_index);
        JDWP::Append2BE(bytes, record->StackElement(stack_frame)->LineNumber());
      }
      idx = (idx + 1) & (alloc_record_max_ - 1);
    }

    // (xb) class name strings
    // (xb) method name strings
    // (xb) source file strings
    JDWP::Set4BE(&bytes[string_table_offset], bytes.size());
    class_names.WriteTo(bytes);
    method_names.WriteTo(bytes);
    filenames.WriteTo(bytes);
  }
  JNIEnv* env = self->GetJniEnv();
  jbyteArray result = env->NewByteArray(bytes.size());
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  }
  return result;
}

mirror::ArtMethod* DeoptimizationRequest::Method() const {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  return soa.DecodeMethod(method_);
}

void DeoptimizationRequest::SetMethod(mirror::ArtMethod* m) {
  ScopedObjectAccessUnchecked soa(Thread::Current());
  method_ = soa.EncodeMethod(m);
}

}  // namespace art

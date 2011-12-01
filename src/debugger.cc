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

#include "class_linker.h"
#include "class_loader.h"
#include "context.h"
#include "object_utils.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "stack_indirect_reference_table.h"
#include "thread_list.h"

extern "C" void dlmalloc_walk_heap(void(*)(const void*, size_t, const void*, size_t, void*), void*);
#ifndef HAVE_ANDROID_OS
void dlmalloc_walk_heap(void(*)(const void*, size_t, const void*, size_t, void*), void*) {
  // No-op for glibc.
}
#endif

namespace art {

static const size_t kMaxAllocRecordStackDepth = 16; // Max 255.
static const size_t kNumAllocRecords = 512; // Must be power of 2.

class ObjectRegistry {
 public:
  ObjectRegistry() : lock_("ObjectRegistry lock") {
  }

  JDWP::ObjectId Add(Object* o) {
    if (o == NULL) {
      return 0;
    }
    JDWP::ObjectId id = static_cast<JDWP::ObjectId>(reinterpret_cast<uintptr_t>(o));
    MutexLock mu(lock_);
    map_[id] = o;
    return id;
  }

  void Clear() {
    MutexLock mu(lock_);
    LOG(DEBUG) << "Debugger has detached; object registry had " << map_.size() << " entries";
    map_.clear();
  }

  bool Contains(JDWP::ObjectId id) {
    MutexLock mu(lock_);
    return map_.find(id) != map_.end();
  }

  template<typename T> T Get(JDWP::ObjectId id) {
    MutexLock mu(lock_);
    typedef std::map<JDWP::ObjectId, Object*>::iterator It; // C++0x auto
    It it = map_.find(id);
    return (it != map_.end()) ? reinterpret_cast<T>(it->second) : NULL;
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) {
    MutexLock mu(lock_);
    typedef std::map<JDWP::ObjectId, Object*>::iterator It; // C++0x auto
    for (It it = map_.begin(); it != map_.end(); ++it) {
      visitor(it->second, arg);
    }
  }

 private:
  Mutex lock_;
  std::map<JDWP::ObjectId, Object*> map_;
};

struct AllocRecordStackTraceElement {
  Method* method;
  uintptr_t raw_pc;

  int32_t LineNumber() const {
    return MethodHelper(method).GetLineNumFromNativePC(raw_pc);
  }
};

struct AllocRecord {
  Class* type;
  size_t byte_count;
  uint16_t thin_lock_id;
  AllocRecordStackTraceElement stack[kMaxAllocRecordStackDepth]; // Unused entries have NULL method.

  size_t GetDepth() {
    size_t depth = 0;
    while (depth < kMaxAllocRecordStackDepth && stack[depth].method != NULL) {
      ++depth;
    }
    return depth;
  }
};

// JDWP is allowed unless the Zygote forbids it.
static bool gJdwpAllowed = true;

// Was there a -Xrunjdwp or -agent argument on the command-line?
static bool gJdwpConfigured = false;

// Broken-down JDWP options. (Only valid if gJdwpConfigured is true.)
static JDWP::JdwpOptions gJdwpOptions;

// Runtime JDWP state.
static JDWP::JdwpState* gJdwpState = NULL;
static bool gDebuggerConnected;  // debugger or DDMS is connected.
static bool gDebuggerActive;     // debugger is making requests.

static bool gDdmThreadNotification = false;

// DDMS GC-related settings.
static Dbg::HpifWhen gDdmHpifWhen = Dbg::HPIF_WHEN_NEVER;
static Dbg::HpsgWhen gDdmHpsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmHpsgWhat;
static Dbg::HpsgWhen gDdmNhsgWhen = Dbg::HPSG_WHEN_NEVER;
static Dbg::HpsgWhat gDdmNhsgWhat;

static ObjectRegistry* gRegistry = NULL;

// Recent allocation tracking.
static Mutex gAllocTrackerLock("AllocTracker lock");
AllocRecord* Dbg::recent_allocation_records_ = NULL; // TODO: CircularBuffer<AllocRecord>
static size_t gAllocRecordHead = 0;
static size_t gAllocRecordCount = 0;

static JDWP::JdwpTag BasicTagFromDescriptor(const char* descriptor) {
  // JDWP deliberately uses the descriptor characters' ASCII values for its enum.
  // Note that by "basic" we mean that we don't get more specific than JT_OBJECT.
  return static_cast<JDWP::JdwpTag>(descriptor[0]);
}

static JDWP::JdwpTag TagFromClass(Class* c) {
  CHECK(c != NULL);
  if (c->IsArrayClass()) {
    return JDWP::JT_ARRAY;
  }

  if (c->IsStringClass()) {
    return JDWP::JT_STRING;
  } else if (c->IsClassClass()) {
    return JDWP::JT_CLASS_OBJECT;
#if 0 // TODO
  } else if (dvmInstanceof(clazz, gDvm.classJavaLangThread)) {
    return JDWP::JT_THREAD;
  } else if (dvmInstanceof(clazz, gDvm.classJavaLangThreadGroup)) {
    return JDWP::JT_THREAD_GROUP;
  } else if (dvmInstanceof(clazz, gDvm.classJavaLangClassLoader)) {
    return JDWP::JT_CLASS_LOADER;
#endif
  } else {
    return JDWP::JT_OBJECT;
  }
}

/*
 * Objects declared to hold Object might actually hold a more specific
 * type.  The debugger may take a special interest in these (e.g. it
 * wants to display the contents of Strings), so we want to return an
 * appropriate tag.
 *
 * Null objects are tagged JT_OBJECT.
 */
static JDWP::JdwpTag TagFromObject(const Object* o) {
  return (o == NULL) ? JDWP::JT_OBJECT : TagFromClass(o->GetClass());
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
    long port = strtol(port_string.c_str(), &end, 10);
    if (*end != '\0') {
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
  LOG(VERBOSE) << "ParseJdwpOptions: " << options;

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
  if (!gJdwpAllowed || !gJdwpConfigured) {
    // No JDWP for you!
    return;
  }

  CHECK(gRegistry == NULL);
  gRegistry = new ObjectRegistry;

  // Init JDWP if the debugger is enabled. This may connect out to a
  // debugger, passively listen for a debugger, or block waiting for a
  // debugger.
  gJdwpState = JDWP::JdwpState::Create(&gJdwpOptions);
  if (gJdwpState == NULL) {
    // We probably failed because some other process has the port already, which means that
    // if we don't abort the user is likely to think they're talking to us when they're actually
    // talking to that other process.
    LOG(FATAL) << "debugger thread failed to initialize";
  }

  // If a debugger has already attached, send the "welcome" message.
  // This may cause us to suspend all threads.
  if (gJdwpState->IsActive()) {
    //ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
    if (!gJdwpState->PostVMStart()) {
      LOG(WARNING) << "failed to post 'start' message to debugger";
    }
  }
}

void Dbg::StopJdwp() {
  delete gJdwpState;
  delete gRegistry;
  gRegistry = NULL;
}

void Dbg::GcDidFinish() {
  if (gDdmHpifWhen != HPIF_WHEN_NEVER) {
    LOG(DEBUG) << "Sending VM heap info to DDM";
    DdmSendHeapInfo(gDdmHpifWhen);
  }
  if (gDdmHpsgWhen != HPSG_WHEN_NEVER) {
    LOG(DEBUG) << "Dumping VM heap to DDM";
    DdmSendHeapSegments(false);
  }
  if (gDdmNhsgWhen != HPSG_WHEN_NEVER) {
    LOG(DEBUG) << "Dumping native heap to DDM";
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
  return (gJdwpState != NULL) ? gJdwpState->GetDebugThread() : NULL;
}

void Dbg::ClearWaitForEventThread() {
  gJdwpState->ClearWaitForEventThread();
}

void Dbg::Connected() {
  CHECK(!gDebuggerConnected);
  LOG(VERBOSE) << "JDWP has attached";
  gDebuggerConnected = true;
}

void Dbg::GoActive() {
  // Enable all debugging features, including scans for breakpoints.
  // This is a no-op if we're already active.
  // Only called from the JDWP handler thread.
  if (gDebuggerActive) {
    return;
  }

  LOG(INFO) << "Debugger is active";

  // TODO: CHECK we don't have any outstanding breakpoints.

  gDebuggerActive = true;

  //dvmEnableAllSubMode(kSubModeDebuggerActive);
}

void Dbg::Disconnected() {
  CHECK(gDebuggerConnected);

  gDebuggerActive = false;

  //dvmDisableAllSubMode(kSubModeDebuggerActive);

  gRegistry->Clear();
  gDebuggerConnected = false;
}

bool Dbg::IsDebuggerConnected() {
  return gDebuggerActive;
}

bool Dbg::IsDebuggingEnabled() {
  return gJdwpConfigured;
}

int64_t Dbg::LastDebuggerActivity() {
  return gJdwpState->LastDebuggerActivity();
}

int Dbg::ThreadRunning() {
  return static_cast<int>(Thread::Current()->SetState(Thread::kRunnable));
}

int Dbg::ThreadWaiting() {
  return static_cast<int>(Thread::Current()->SetState(Thread::kVmWait));
}

int Dbg::ThreadContinuing(int new_state) {
  return static_cast<int>(Thread::Current()->SetState(static_cast<Thread::State>(new_state)));
}

void Dbg::UndoDebuggerSuspensions() {
  Runtime::Current()->GetThreadList()->UndoDebuggerSuspensions();
}

void Dbg::Exit(int status) {
  exit(status); // This is all dalvik did.
}

void Dbg::VisitRoots(Heap::RootVisitor* visitor, void* arg) {
  if (gRegistry != NULL) {
    gRegistry->VisitRoots(visitor, arg);
  }
}

std::string Dbg::GetClassDescriptor(JDWP::RefTypeId classId) {
  Class* c = gRegistry->Get<Class*>(classId);
  return ClassHelper(c).GetDescriptor();
}

JDWP::ObjectId Dbg::GetClassObject(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::RefTypeId Dbg::GetSuperclass(JDWP::RefTypeId id) {
  Class* c = gRegistry->Get<Class*>(id);
  return gRegistry->Add(c->GetSuperClass());
}

JDWP::ObjectId Dbg::GetClassLoader(JDWP::RefTypeId id) {
  Object* o = gRegistry->Get<Object*>(id);
  return gRegistry->Add(o->GetClass()->GetClassLoader());
}

uint32_t Dbg::GetAccessFlags(JDWP::RefTypeId id) {
  Class* c = gRegistry->Get<Class*>(id);
  return c->GetAccessFlags() & kAccJavaFlagsMask;
}

bool Dbg::IsInterface(JDWP::RefTypeId classId) {
  Class* c = gRegistry->Get<Class*>(classId);
  return c->IsInterface();
}

void Dbg::GetClassList(uint32_t* pClassCount, JDWP::RefTypeId** pClasses) {
  // Get the complete list of reference classes (i.e. all classes except
  // the primitive types).
  // Returns a newly-allocated buffer full of RefTypeId values.
  struct ClassListCreator {
    static bool Visit(Class* c, void* arg) {
      return reinterpret_cast<ClassListCreator*>(arg)->Visit(c);
    }

    bool Visit(Class* c) {
      if (!c->IsPrimitive()) {
        classes.push_back(static_cast<JDWP::RefTypeId>(gRegistry->Add(c)));
      }
      return true;
    }

    std::vector<JDWP::RefTypeId> classes;
  };

  ClassListCreator clc;
  Runtime::Current()->GetClassLinker()->VisitClasses(ClassListCreator::Visit, &clc);
  *pClassCount = clc.classes.size();
  *pClasses = new JDWP::RefTypeId[clc.classes.size()];
  for (size_t i = 0; i < clc.classes.size(); ++i) {
    (*pClasses)[i] = clc.classes[i];
  }
}

void Dbg::GetVisibleClassList(JDWP::ObjectId classLoaderId, uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetClassInfo(JDWP::RefTypeId classId, JDWP::JdwpTypeTag* pTypeTag, uint32_t* pStatus, std::string* pDescriptor) {
  Class* c = gRegistry->Get<Class*>(classId);
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

  if (pDescriptor != NULL) {
    *pDescriptor = ClassHelper(c).GetDescriptor();
  }
}

void Dbg::FindLoadedClassBySignature(const char* descriptor, std::vector<JDWP::RefTypeId>& ids) {
  std::vector<Class*> classes;
  Runtime::Current()->GetClassLinker()->LookupClasses(descriptor, classes);
  ids.clear();
  for (size_t i = 0; i < classes.size(); ++i) {
    ids.push_back(gRegistry->Add(classes[i]));
  }
}

void Dbg::GetObjectType(JDWP::ObjectId objectId, JDWP::JdwpTypeTag* pRefTypeTag, JDWP::RefTypeId* pRefTypeId) {
  Object* o = gRegistry->Get<Object*>(objectId);
  if (o->GetClass()->IsArrayClass()) {
    *pRefTypeTag = JDWP::TT_ARRAY;
  } else if (o->GetClass()->IsInterface()) {
    *pRefTypeTag = JDWP::TT_INTERFACE;
  } else {
    *pRefTypeTag = JDWP::TT_CLASS;
  }
  *pRefTypeId = gRegistry->Add(o->GetClass());
}

uint8_t Dbg::GetClassObjectType(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

std::string Dbg::GetSignature(JDWP::RefTypeId refTypeId) {
  Class* c = gRegistry->Get<Class*>(refTypeId);
  CHECK(c != NULL);
  return ClassHelper(c).GetDescriptor();
}

bool Dbg::GetSourceFile(JDWP::RefTypeId refTypeId, std::string& result) {
  Class* c = gRegistry->Get<Class*>(refTypeId);
  CHECK(c != NULL);
  result = ClassHelper(c).GetSourceFile();
  return result == NULL;
}

uint8_t Dbg::GetObjectTag(JDWP::ObjectId objectId) {
  Object* o = gRegistry->Get<Object*>(objectId);
  return TagFromObject(o);
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
    LOG(FATAL) << "unknown tag " << tag;
    return -1;
  }
}

int Dbg::GetArrayLength(JDWP::ObjectId arrayId) {
  Object* o = gRegistry->Get<Object*>(arrayId);
  Array* a = o->AsArray();
  return a->GetLength();
}

uint8_t Dbg::GetArrayElementTag(JDWP::ObjectId arrayId) {
  Object* o = gRegistry->Get<Object*>(arrayId);
  Array* a = o->AsArray();
  std::string descriptor(ClassHelper(a->GetClass()).GetDescriptor());
  JDWP::JdwpTag tag = BasicTagFromDescriptor(descriptor.c_str() + 1);
  if (!IsPrimitiveTag(tag)) {
    tag = TagFromClass(a->GetClass()->GetComponentType());
  }
  return tag;
}

bool Dbg::OutputArray(JDWP::ObjectId arrayId, int offset, int count, JDWP::ExpandBuf* pReply) {
  Object* o = gRegistry->Get<Object*>(arrayId);
  Array* a = o->AsArray();

  if (offset < 0 || count < 0 || offset > a->GetLength() || a->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return false;
  }
  std::string descriptor(ClassHelper(a->GetClass()).GetDescriptor());
  JDWP::JdwpTag tag = BasicTagFromDescriptor(descriptor.c_str() + 1);

  if (IsPrimitiveTag(tag)) {
    size_t width = GetTagWidth(tag);
    const uint8_t* src = reinterpret_cast<uint8_t*>(a->GetRawData());
    uint8_t* dst = expandBufAddSpace(pReply, count * width);
    if (width == 8) {
      const uint64_t* src8 = reinterpret_cast<const uint64_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write8BE(&dst, src8[offset + i]);
    } else if (width == 4) {
      const uint32_t* src4 = reinterpret_cast<const uint32_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write4BE(&dst, src4[offset + i]);
    } else if (width == 2) {
      const uint16_t* src2 = reinterpret_cast<const uint16_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write2BE(&dst, src2[offset + i]);
    } else {
      memcpy(dst, &src[offset * width], count * width);
    }
  } else {
    ObjectArray<Object>* oa = a->AsObjectArray<Object>();
    for (int i = 0; i < count; ++i) {
      Object* element = oa->Get(offset + i);
      JDWP::JdwpTag specific_tag = (element != NULL) ? TagFromObject(element) : tag;
      expandBufAdd1(pReply, specific_tag);
      expandBufAddObjectId(pReply, gRegistry->Add(element));
    }
  }

  return true;
}

bool Dbg::SetArrayElements(JDWP::ObjectId arrayId, int offset, int count, const uint8_t* src) {
  Object* o = gRegistry->Get<Object*>(arrayId);
  Array* a = o->AsArray();

  if (offset < 0 || count < 0 || offset > a->GetLength() || a->GetLength() - offset < count) {
    LOG(WARNING) << __FUNCTION__ << " access out of bounds: offset=" << offset << "; count=" << count;
    return false;
  }
  std::string descriptor(ClassHelper(a->GetClass()).GetDescriptor());
  JDWP::JdwpTag tag = BasicTagFromDescriptor(descriptor.c_str() + 1);

  if (IsPrimitiveTag(tag)) {
    size_t width = GetTagWidth(tag);
    uint8_t* dst = &(reinterpret_cast<uint8_t*>(a->GetRawData())[offset * width]);
    if (width == 8) {
      for (int i = 0; i < count; ++i) {
        // Handle potentially non-aligned memory access one byte at a time for ARM's benefit.
        uint64_t value;
        for (size_t j = 0; j < sizeof(uint64_t); ++j) reinterpret_cast<uint8_t*>(&value)[j] = src[j];
        src += sizeof(uint64_t);
        JDWP::Write8BE(&dst, value);
      }
    } else if (width == 4) {
      const uint32_t* src4 = reinterpret_cast<const uint32_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write4BE(&dst, src4[i]);
    } else if (width == 2) {
      const uint16_t* src2 = reinterpret_cast<const uint16_t*>(src);
      for (int i = 0; i < count; ++i) JDWP::Write2BE(&dst, src2[i]);
    } else {
      memcpy(&dst[offset * width], src, count * width);
    }
  } else {
    ObjectArray<Object>* oa = a->AsObjectArray<Object>();
    for (int i = 0; i < count; ++i) {
      JDWP::ObjectId id = JDWP::ReadObjectId(&src);
      oa->Set(offset + i, gRegistry->Get<Object*>(id));
    }
  }

  return true;
}

JDWP::ObjectId Dbg::CreateString(const char* str) {
  return gRegistry->Add(String::AllocFromModifiedUtf8(str));
}

JDWP::ObjectId Dbg::CreateObject(JDWP::RefTypeId classId) {
  Class* c = gRegistry->Get<Class*>(classId);
  return gRegistry->Add(c->AllocObject());
}

JDWP::ObjectId Dbg::CreateArrayObject(JDWP::RefTypeId arrayTypeId, uint32_t length) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

JDWP::FieldId ToFieldId(Field* f) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return static_cast<JDWP::FieldId>(reinterpret_cast<uintptr_t>(f));
#endif
}

JDWP::MethodId ToMethodId(Method* m) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return static_cast<JDWP::MethodId>(reinterpret_cast<uintptr_t>(m));
#endif
}

Field* FromFieldId(JDWP::FieldId fid) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return reinterpret_cast<Field*>(static_cast<uintptr_t>(fid));
#endif
}

Method* FromMethodId(JDWP::MethodId mid) {
#ifdef MOVING_GARBAGE_COLLECTOR
  UNIMPLEMENTED(FATAL);
#else
  return reinterpret_cast<Method*>(static_cast<uintptr_t>(mid));
#endif
}

std::string Dbg::GetMethodName(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId) {
  Method* m = FromMethodId(methodId);
  return MethodHelper(m).GetName();
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

static const uint16_t kEclipseWorkaroundSlot = 1000;

/*
 * Eclipse appears to expect that the "this" reference is in slot zero.
 * If it's not, the "variables" display will show two copies of "this",
 * possibly because it gets "this" from SF.ThisObject and then displays
 * all locals with nonzero slot numbers.
 *
 * So, we remap the item in slot 0 to 1000, and remap "this" to zero.  On
 * SF.GetValues / SF.SetValues we map them back.
 *
 * TODO: jdb uses the value to determine whether a variable is a local or an argument,
 * by checking whether it's less than the number of arguments. To make that work, we'd
 * have to "mangle" all the arguments to come first, not just the implicit argument 'this'.
 */
static uint16_t MangleSlot(uint16_t slot, const char* name) {
  uint16_t newSlot = slot;
  if (strcmp(name, "this") == 0) {
    newSlot = 0;
  } else if (slot == 0) {
    newSlot = kEclipseWorkaroundSlot;
  }
  return newSlot;
}

static uint16_t DemangleSlot(uint16_t slot, Frame& f) {
  if (slot == kEclipseWorkaroundSlot) {
    return 0;
  } else if (slot == 0) {
    const DexFile::CodeItem* code_item = MethodHelper(f.GetMethod()).GetCodeItem();
    return code_item->registers_size_ - code_item->ins_size_;
  }
  return slot;
}

void Dbg::OutputDeclaredFields(JDWP::RefTypeId refTypeId, bool with_generic, JDWP::ExpandBuf* pReply) {
  Class* c = gRegistry->Get<Class*>(refTypeId);
  CHECK(c != NULL);

  size_t instance_field_count = c->NumInstanceFields();
  size_t static_field_count = c->NumStaticFields();

  expandBufAdd4BE(pReply, instance_field_count + static_field_count);

  for (size_t i = 0; i < instance_field_count + static_field_count; ++i) {
    Field* f = (i < instance_field_count) ? c->GetInstanceField(i) : c->GetStaticField(i - instance_field_count);
    FieldHelper fh(f);
    expandBufAddFieldId(pReply, ToFieldId(f));
    expandBufAddUtf8String(pReply, fh.GetName());
    expandBufAddUtf8String(pReply, fh.GetTypeDescriptor());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(f->GetAccessFlags()));
  }
}

void Dbg::OutputDeclaredMethods(JDWP::RefTypeId refTypeId, bool with_generic, JDWP::ExpandBuf* pReply) {
  Class* c = gRegistry->Get<Class*>(refTypeId);
  CHECK(c != NULL);

  size_t direct_method_count = c->NumDirectMethods();
  size_t virtual_method_count = c->NumVirtualMethods();

  expandBufAdd4BE(pReply, direct_method_count + virtual_method_count);

  for (size_t i = 0; i < direct_method_count + virtual_method_count; ++i) {
    Method* m = (i < direct_method_count) ? c->GetDirectMethod(i) : c->GetVirtualMethod(i - direct_method_count);
    MethodHelper mh(m);
    expandBufAddMethodId(pReply, ToMethodId(m));
    expandBufAddUtf8String(pReply, mh.GetName());
    expandBufAddUtf8String(pReply, mh.GetSignature().c_str());
    if (with_generic) {
      static const char genericSignature[1] = "";
      expandBufAddUtf8String(pReply, genericSignature);
    }
    expandBufAdd4BE(pReply, MangleAccessFlags(m->GetAccessFlags()));
  }
}

void Dbg::OutputDeclaredInterfaces(JDWP::RefTypeId refTypeId, JDWP::ExpandBuf* pReply) {
  Class* c = gRegistry->Get<Class*>(refTypeId);
  CHECK(c != NULL);
  ClassHelper kh(c);
  size_t interface_count = kh.NumInterfaces();
  expandBufAdd4BE(pReply, interface_count);
  for (size_t i = 0; i < interface_count; ++i) {
    expandBufAddRefTypeId(pReply, gRegistry->Add(kh.GetInterface(i)));
  }
}

void Dbg::OutputLineTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId, JDWP::ExpandBuf* pReply) {
  struct DebugCallbackContext {
    int numItems;
    JDWP::ExpandBuf* pReply;

    static bool Callback(void* context, uint32_t address, uint32_t lineNum) {
      DebugCallbackContext* pContext = reinterpret_cast<DebugCallbackContext*>(context);
      expandBufAdd8BE(pContext->pReply, address);
      expandBufAdd4BE(pContext->pReply, lineNum);
      pContext->numItems++;
      return true;
    }
  };

  Method* m = FromMethodId(methodId);
  MethodHelper mh(m);
  uint64_t start, end;
  if (m->IsNative()) {
    start = -1;
    end = -1;
  } else {
    start = 0;
    // TODO: what are the units supposed to be? *2?
    end = mh.GetCodeItem()->insns_size_in_code_units_;
  }

  expandBufAdd8BE(pReply, start);
  expandBufAdd8BE(pReply, end);

  // Add numLines later
  size_t numLinesOffset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  DebugCallbackContext context;
  context.numItems = 0;
  context.pReply = pReply;

  mh.GetDexFile().DecodeDebugInfo(mh.GetCodeItem(), m->IsStatic(), m->GetDexMethodIndex(),
                                  DebugCallbackContext::Callback, NULL, &context);

  JDWP::Set4BE(expandBufGetBuffer(pReply) + numLinesOffset, context.numItems);
}

void Dbg::OutputVariableTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId, bool with_generic, JDWP::ExpandBuf* pReply) {
  struct DebugCallbackContext {
    JDWP::ExpandBuf* pReply;
    size_t variable_count;
    bool with_generic;

    static void Callback(void* context, uint16_t slot, uint32_t startAddress, uint32_t endAddress, const char* name, const char* descriptor, const char* signature) {
      DebugCallbackContext* pContext = reinterpret_cast<DebugCallbackContext*>(context);

      LOG(VERBOSE) << StringPrintf("    %2d: %d(%d) '%s' '%s' '%s' slot=%d", pContext->variable_count, startAddress, endAddress - startAddress, name, descriptor, signature, slot);

      slot = MangleSlot(slot, name);

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

  Method* m = FromMethodId(methodId);
  MethodHelper mh(m);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();

  // arg_count considers doubles and longs to take 2 units.
  // variable_count considers everything to take 1 unit.
  std::string shorty(mh.GetShorty());
  expandBufAdd4BE(pReply, m->NumArgRegisters(shorty));

  // We don't know the total number of variables yet, so leave a blank and update it later.
  size_t variable_count_offset = expandBufGetLength(pReply);
  expandBufAdd4BE(pReply, 0);

  DebugCallbackContext context;
  context.pReply = pReply;
  context.variable_count = 0;
  context.with_generic = with_generic;

  mh.GetDexFile().DecodeDebugInfo(code_item, m->IsStatic(), m->GetDexMethodIndex(), NULL,
                                  DebugCallbackContext::Callback, &context);

  JDWP::Set4BE(expandBufGetBuffer(pReply) + variable_count_offset, context.variable_count);
}

JDWP::JdwpTag Dbg::GetFieldBasicTag(JDWP::FieldId fieldId) {
  return BasicTagFromDescriptor(FieldHelper(FromFieldId(fieldId)).GetTypeDescriptor());
}

JDWP::JdwpTag Dbg::GetStaticFieldBasicTag(JDWP::FieldId fieldId) {
  return BasicTagFromDescriptor(FieldHelper(FromFieldId(fieldId)).GetTypeDescriptor());
}

void Dbg::GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  Object* o = gRegistry->Get<Object*>(objectId);
  Field* f = FromFieldId(fieldId);

  JDWP::JdwpTag tag = BasicTagFromDescriptor(FieldHelper(f).GetTypeDescriptor());

  if (IsPrimitiveTag(tag)) {
    expandBufAdd1(pReply, tag);
    if (tag == JDWP::JT_BOOLEAN || tag == JDWP::JT_BYTE) {
      expandBufAdd1(pReply, f->Get32(o));
    } else if (tag == JDWP::JT_CHAR || tag == JDWP::JT_SHORT) {
      expandBufAdd2BE(pReply, f->Get32(o));
    } else if (tag == JDWP::JT_FLOAT || tag == JDWP::JT_INT) {
      expandBufAdd4BE(pReply, f->Get32(o));
    } else if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      expandBufAdd8BE(pReply, f->Get64(o));
    } else {
      LOG(FATAL) << "unknown tag: " << tag;
    }
  } else {
    Object* value = f->GetObject(o);
    expandBufAdd1(pReply, TagFromObject(value));
    expandBufAddObjectId(pReply, gRegistry->Add(value));
  }
}

void Dbg::SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width) {
  Object* o = gRegistry->Get<Object*>(objectId);
  Field* f = FromFieldId(fieldId);

  JDWP::JdwpTag tag = BasicTagFromDescriptor(FieldHelper(f).GetTypeDescriptor());

  if (IsPrimitiveTag(tag)) {
    if (tag == JDWP::JT_DOUBLE || tag == JDWP::JT_LONG) {
      f->Set64(o, value);
    } else {
      f->Set32(o, value);
    }
  } else {
    f->SetObject(o, gRegistry->Get<Object*>(value));
  }
}

void Dbg::GetStaticFieldValue(JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  GetFieldValue(0, fieldId, pReply);
}

void Dbg::SetStaticFieldValue(JDWP::FieldId fieldId, uint64_t value, int width) {
  SetFieldValue(0, fieldId, value, width);
}

std::string Dbg::StringToUtf8(JDWP::ObjectId strId) {
  String* s = gRegistry->Get<String*>(strId);
  return s->ToModifiedUtf8();
}

Thread* DecodeThread(JDWP::ObjectId threadId) {
  Object* thread_peer = gRegistry->Get<Object*>(threadId);
  CHECK(thread_peer != NULL);
  return Thread::FromManagedThread(thread_peer);
}

bool Dbg::GetThreadName(JDWP::ObjectId threadId, std::string& name) {
  ScopedThreadListLock thread_list_lock;
  Thread* thread = DecodeThread(threadId);
  if (thread == NULL) {
    return false;
  }
  StringAppendF(&name, "<%d> %s", thread->GetThinLockId(), thread->GetName()->ToModifiedUtf8().c_str());
  return true;
}

JDWP::ObjectId Dbg::GetThreadGroup(JDWP::ObjectId threadId) {
  Object* thread = gRegistry->Get<Object*>(threadId);
  CHECK(thread != NULL);

  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/Thread;");
  CHECK(c != NULL);
  Field* f = c->FindInstanceField("group", "Ljava/lang/ThreadGroup;");
  CHECK(f != NULL);
  Object* group = f->GetObject(thread);
  CHECK(group != NULL);
  return gRegistry->Add(group);
}

std::string Dbg::GetThreadGroupName(JDWP::ObjectId threadGroupId) {
  Object* thread_group = gRegistry->Get<Object*>(threadGroupId);
  CHECK(thread_group != NULL);

  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/ThreadGroup;");
  CHECK(c != NULL);
  Field* f = c->FindInstanceField("name", "Ljava/lang/String;");
  CHECK(f != NULL);
  String* s = reinterpret_cast<String*>(f->GetObject(thread_group));
  return s->ToModifiedUtf8();
}

JDWP::ObjectId Dbg::GetThreadGroupParent(JDWP::ObjectId threadGroupId) {
  Object* thread_group = gRegistry->Get<Object*>(threadGroupId);
  CHECK(thread_group != NULL);

  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/ThreadGroup;");
  CHECK(c != NULL);
  Field* f = c->FindInstanceField("parent", "Ljava/lang/ThreadGroup;");
  CHECK(f != NULL);
  Object* parent = f->GetObject(thread_group);
  return gRegistry->Add(parent);
}

static Object* GetStaticThreadGroup(const char* field_name) {
  Class* c = Runtime::Current()->GetClassLinker()->FindSystemClass("Ljava/lang/ThreadGroup;");
  CHECK(c != NULL);
  Field* f = c->FindStaticField(field_name, "Ljava/lang/ThreadGroup;");
  CHECK(f != NULL);
  Object* group = f->GetObject(NULL);
  CHECK(group != NULL);
  return group;
}

JDWP::ObjectId Dbg::GetSystemThreadGroupId() {
  return gRegistry->Add(GetStaticThreadGroup("mSystem"));
}

JDWP::ObjectId Dbg::GetMainThreadGroupId() {
  return gRegistry->Add(GetStaticThreadGroup("mMain"));
}

bool Dbg::GetThreadStatus(JDWP::ObjectId threadId, uint32_t* pThreadStatus, uint32_t* pSuspendStatus) {
  ScopedThreadListLock thread_list_lock;

  Thread* thread = DecodeThread(threadId);
  if (thread == NULL) {
    return false;
  }

  switch (thread->GetState()) {
  case Thread::kTerminated:   *pThreadStatus = JDWP::TS_ZOMBIE;   break;
  case Thread::kRunnable:     *pThreadStatus = JDWP::TS_RUNNING;  break;
  case Thread::kTimedWaiting: *pThreadStatus = JDWP::TS_SLEEPING; break;
  case Thread::kBlocked:      *pThreadStatus = JDWP::TS_MONITOR;  break;
  case Thread::kWaiting:      *pThreadStatus = JDWP::TS_WAIT;     break;
  case Thread::kInitializing: *pThreadStatus = JDWP::TS_ZOMBIE;   break;
  case Thread::kStarting:     *pThreadStatus = JDWP::TS_ZOMBIE;   break;
  case Thread::kNative:       *pThreadStatus = JDWP::TS_RUNNING;  break;
  case Thread::kVmWait:       *pThreadStatus = JDWP::TS_WAIT;     break;
  case Thread::kSuspended:    *pThreadStatus = JDWP::TS_RUNNING;  break;
  default:
    LOG(FATAL) << "unknown thread state " << thread->GetState();
  }

  *pSuspendStatus = (thread->IsSuspended() ? JDWP::SUSPEND_STATUS_SUSPENDED : 0);

  return true;
}

uint32_t Dbg::GetThreadSuspendCount(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::ThreadExists(JDWP::ObjectId threadId) {
  return DecodeThread(threadId) != NULL;
}

bool Dbg::IsSuspended(JDWP::ObjectId threadId) {
  return DecodeThread(threadId)->IsSuspended();
}

//void Dbg::WaitForSuspend(JDWP::ObjectId threadId);

void Dbg::GetThreadGroupThreadsImpl(Object* thread_group, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  struct ThreadListVisitor {
    static void Visit(Thread* t, void* arg) {
      reinterpret_cast<ThreadListVisitor*>(arg)->Visit(t);
    }

    void Visit(Thread* t) {
      if (t == Dbg::GetDebugThread()) {
        // Skip the JDWP thread. Some debuggers get bent out of shape when they can't suspend and
        // query all threads, so it's easier if we just don't tell them about this thread.
        return;
      }
      if (thread_group == NULL || t->GetThreadGroup() == thread_group) {
        threads.push_back(gRegistry->Add(t->GetPeer()));
      }
    }

    Object* thread_group;
    std::vector<JDWP::ObjectId> threads;
  };

  ThreadListVisitor tlv;
  tlv.thread_group = thread_group;

  {
    ScopedThreadListLock thread_list_lock;
    Runtime::Current()->GetThreadList()->ForEach(ThreadListVisitor::Visit, &tlv);
  }

  *pThreadCount = tlv.threads.size();
  if (*pThreadCount == 0) {
    *ppThreadIds = NULL;
  } else {
    *ppThreadIds = new JDWP::ObjectId[*pThreadCount];
    for (size_t i = 0; i < *pThreadCount; ++i) {
      (*ppThreadIds)[i] = tlv.threads[i];
    }
  }
}

void Dbg::GetThreadGroupThreads(JDWP::ObjectId threadGroupId, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  GetThreadGroupThreadsImpl(gRegistry->Get<Object*>(threadGroupId), ppThreadIds, pThreadCount);
}

void Dbg::GetAllThreads(JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  GetThreadGroupThreadsImpl(NULL, ppThreadIds, pThreadCount);
}

int Dbg::GetThreadFrameCount(JDWP::ObjectId threadId) {
  ScopedThreadListLock thread_list_lock;
  struct CountStackDepthVisitor : public Thread::StackVisitor {
    CountStackDepthVisitor() : depth(0) {}
    virtual void VisitFrame(const Frame& f, uintptr_t) {
      // TODO: we'll need to skip callee-save frames too.
      if (f.HasMethod()) {
        ++depth;
      }
    }
    size_t depth;
  };
  CountStackDepthVisitor visitor;
  DecodeThread(threadId)->WalkStack(&visitor);
  return visitor.depth;
}

bool Dbg::GetThreadFrame(JDWP::ObjectId threadId, int desired_frame_number, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc) {
  ScopedThreadListLock thread_list_lock;
  struct GetFrameVisitor : public Thread::StackVisitor {
    GetFrameVisitor(int desired_frame_number, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc)
        : found(false) ,depth(0), desired_frame_number(desired_frame_number), pFrameId(pFrameId), pLoc(pLoc) {
    }
    virtual void VisitFrame(const Frame& f, uintptr_t pc) {
      // TODO: we'll need to skip callee-save frames too.
      if (!f.HasMethod()) {
        return; // The debugger can't do anything useful with a frame that has no Method*.
      }

      if (depth == desired_frame_number) {
        *pFrameId = reinterpret_cast<JDWP::FrameId>(f.GetSP());

        Method* m = f.GetMethod();
        Class* c = m->GetDeclaringClass();

        pLoc->typeTag = c->IsInterface() ? JDWP::TT_INTERFACE : JDWP::TT_CLASS;
        pLoc->classId = gRegistry->Add(c);
        pLoc->methodId = ToMethodId(m);
        pLoc->idx = m->IsNative() ? -1 : m->ToDexPC(pc);

        found = true;
      }
      ++depth;
    }
    bool found;
    int depth;
    int desired_frame_number;
    JDWP::FrameId* pFrameId;
    JDWP::JdwpLocation* pLoc;
  };
  GetFrameVisitor visitor(desired_frame_number, pFrameId, pLoc);
  visitor.desired_frame_number = desired_frame_number;
  DecodeThread(threadId)->WalkStack(&visitor);
  return visitor.found;
}

JDWP::ObjectId Dbg::GetThreadSelfId() {
  return gRegistry->Add(Thread::Current()->GetPeer());
}

void Dbg::SuspendVM() {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable); // TODO: do we really want to change back? should the JDWP thread be Runnable usually?
  Runtime::Current()->GetThreadList()->SuspendAll(true);
}

void Dbg::ResumeVM() {
  Runtime::Current()->GetThreadList()->ResumeAll(true);
}

void Dbg::SuspendThread(JDWP::ObjectId threadId) {
  Object* peer = gRegistry->Get<Object*>(threadId);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(peer);
  if (thread == NULL) {
    LOG(WARNING) << "No such thread for suspend: " << peer;
    return;
  }
  Runtime::Current()->GetThreadList()->Suspend(thread, true);
}

void Dbg::ResumeThread(JDWP::ObjectId threadId) {
  Object* peer = gRegistry->Get<Object*>(threadId);
  ScopedThreadListLock thread_list_lock;
  Thread* thread = Thread::FromManagedThread(peer);
  if (thread == NULL) {
    LOG(WARNING) << "No such thread for resume: " << peer;
    return;
  }
  Runtime::Current()->GetThreadList()->Resume(thread, true);
}

void Dbg::SuspendSelf() {
  Runtime::Current()->GetThreadList()->SuspendSelfForDebugger();
}

bool Dbg::GetThisObject(JDWP::ObjectId threadId, JDWP::FrameId frameId, JDWP::ObjectId* pThisId) {
  Method** sp = reinterpret_cast<Method**>(frameId);
  Frame f;
  f.SetSP(sp);
  uint16_t reg = DemangleSlot(0, f);
  Method* m = f.GetMethod();

  Object* o = NULL;
  if (!m->IsNative() && !m->IsStatic()) {
    o = reinterpret_cast<Object*>(f.GetVReg(m, reg));
  }
  *pThisId = gRegistry->Add(o);
  return true;
}

void Dbg::GetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t width) {
  Method** sp = reinterpret_cast<Method**>(frameId);
  Frame f;
  f.SetSP(sp);
  uint16_t reg = DemangleSlot(slot, f);
  Method* m = f.GetMethod();

  const VmapTable vmap_table(m->GetVmapTableRaw());
  uint32_t vmap_offset;
  if (vmap_table.IsInContext(reg, vmap_offset)) {
    UNIMPLEMENTED(FATAL) << "don't know how to pull locals from callee save frames: " << vmap_offset;
  }

  switch (tag) {
  case JDWP::JT_BOOLEAN:
    {
      CHECK_EQ(width, 1U);
      uint32_t intVal = f.GetVReg(m, reg);
      LOG(VERBOSE) << "get boolean local " << reg << " = " << intVal;
      JDWP::Set1(buf+1, intVal != 0);
    }
    break;
  case JDWP::JT_BYTE:
    {
      CHECK_EQ(width, 1U);
      uint32_t intVal = f.GetVReg(m, reg);
      LOG(VERBOSE) << "get byte local " << reg << " = " << intVal;
      JDWP::Set1(buf+1, intVal);
    }
    break;
  case JDWP::JT_SHORT:
  case JDWP::JT_CHAR:
    {
      CHECK_EQ(width, 2U);
      uint32_t intVal = f.GetVReg(m, reg);
      LOG(VERBOSE) << "get short/char local " << reg << " = " << intVal;
      JDWP::Set2BE(buf+1, intVal);
    }
    break;
  case JDWP::JT_INT:
  case JDWP::JT_FLOAT:
    {
      CHECK_EQ(width, 4U);
      uint32_t intVal = f.GetVReg(m, reg);
      LOG(VERBOSE) << "get int/float local " << reg << " = " << intVal;
      JDWP::Set4BE(buf+1, intVal);
    }
    break;
  case JDWP::JT_ARRAY:
    {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      Object* o = reinterpret_cast<Object*>(f.GetVReg(m, reg));
      LOG(VERBOSE) << "get array local " << reg << " = " << o;
      if (o != NULL && !Heap::IsHeapAddress(o)) {
        LOG(FATAL) << "reg " << reg << " expected to hold array: " << o;
      }
      JDWP::SetObjectId(buf+1, gRegistry->Add(o));
    }
    break;
  case JDWP::JT_OBJECT:
    {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      Object* o = reinterpret_cast<Object*>(f.GetVReg(m, reg));
      LOG(VERBOSE) << "get object local " << reg << " = " << o;
      if (o != NULL && !Heap::IsHeapAddress(o)) {
        LOG(FATAL) << "reg " << reg << " expected to hold object: " << o;
      }
      tag = TagFromObject(o);
      JDWP::SetObjectId(buf+1, gRegistry->Add(o));
    }
    break;
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    {
      CHECK_EQ(width, 8U);
      uint32_t lo = f.GetVReg(m, reg);
      uint64_t hi = f.GetVReg(m, reg + 1);
      uint64_t longVal = (hi << 32) | lo;
      LOG(VERBOSE) << "get double/long local " << hi << ":" << lo << " = " << longVal;
      JDWP::Set8BE(buf+1, longVal);
    }
    break;
  default:
    LOG(FATAL) << "unknown tag " << tag;
    break;
  }

  // Prepend tag, which may have been updated.
  JDWP::Set1(buf, tag);
}

void Dbg::SetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint64_t value, size_t width) {
  Method** sp = reinterpret_cast<Method**>(frameId);
  Frame f;
  f.SetSP(sp);
  uint16_t reg = DemangleSlot(slot, f);
  Method* m = f.GetMethod();

  const VmapTable vmap_table(m->GetVmapTableRaw());
  uint32_t vmap_offset;
  if (vmap_table.IsInContext(reg, vmap_offset)) {
    UNIMPLEMENTED(FATAL) << "don't know how to pull locals from callee save frames: " << vmap_offset;
  }

  switch (tag) {
  case JDWP::JT_BOOLEAN:
  case JDWP::JT_BYTE:
    CHECK_EQ(width, 1U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    break;
  case JDWP::JT_SHORT:
  case JDWP::JT_CHAR:
    CHECK_EQ(width, 2U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    break;
  case JDWP::JT_INT:
  case JDWP::JT_FLOAT:
    CHECK_EQ(width, 4U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    break;
  case JDWP::JT_ARRAY:
  case JDWP::JT_OBJECT:
  case JDWP::JT_STRING:
    {
      CHECK_EQ(width, sizeof(JDWP::ObjectId));
      Object* o = gRegistry->Get<Object*>(static_cast<JDWP::ObjectId>(value));
      f.SetVReg(m, reg, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(o)));
    }
    break;
  case JDWP::JT_DOUBLE:
  case JDWP::JT_LONG:
    CHECK_EQ(width, 8U);
    f.SetVReg(m, reg, static_cast<uint32_t>(value));
    f.SetVReg(m, reg + 1, static_cast<uint32_t>(value >> 32));
    break;
  default:
    LOG(FATAL) << "unknown tag " << tag;
    break;
  }
}

void Dbg::PostLocationEvent(const Method* method, int pcOffset, Object* thisPtr, int eventFlags) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::PostException(void* throwFp, int throwRelPc, void* catchFp, int catchRelPc, Object* exception) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::PostClassPrepare(Class* c) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::WatchLocation(const JDWP::JdwpLocation* pLoc) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::UnwatchLocation(const JDWP::JdwpLocation* pLoc) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::ConfigureStep(JDWP::ObjectId threadId, JDWP::JdwpStepSize size, JDWP::JdwpStepDepth depth) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::UnconfigureStep(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
}

JDWP::JdwpError Dbg::InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId, JDWP::RefTypeId classId, JDWP::MethodId methodId, uint32_t numArgs, uint64_t* argArray, uint32_t options, JDWP::JdwpTag* pResultTag, uint64_t* pResultValue, JDWP::ObjectId* pExceptObj) {
  UNIMPLEMENTED(FATAL);
  return JDWP::ERR_NONE;
}

void Dbg::ExecuteMethod(DebugInvokeReq* pReq) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::RegisterObjectId(JDWP::ObjectId id) {
  UNIMPLEMENTED(FATAL);
}

/*
 * "buf" contains a full JDWP packet, possibly with multiple chunks.  We
 * need to process each, accumulate the replies, and ship the whole thing
 * back.
 *
 * Returns "true" if we have a reply.  The reply buffer is newly allocated,
 * and includes the chunk type/length, followed by the data.
 *
 * TODO: we currently assume that the request and reply include a single
 * chunk.  If this becomes inconvenient we will need to adapt.
 */
bool Dbg::DdmHandlePacket(const uint8_t* buf, int dataLen, uint8_t** pReplyBuf, int* pReplyLen) {
  CHECK_GE(dataLen, 0);

  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  static jclass Chunk_class = env->FindClass("org/apache/harmony/dalvik/ddmc/Chunk");
  static jclass DdmServer_class = env->FindClass("org/apache/harmony/dalvik/ddmc/DdmServer");
  static jmethodID dispatch_mid = env->GetStaticMethodID(DdmServer_class, "dispatch",
      "(I[BII)Lorg/apache/harmony/dalvik/ddmc/Chunk;");
  static jfieldID data_fid = env->GetFieldID(Chunk_class, "data", "[B");
  static jfieldID length_fid = env->GetFieldID(Chunk_class, "length", "I");
  static jfieldID offset_fid = env->GetFieldID(Chunk_class, "offset", "I");
  static jfieldID type_fid = env->GetFieldID(Chunk_class, "type", "I");

  // Create a byte[] corresponding to 'buf'.
  ScopedLocalRef<jbyteArray> dataArray(env, env->NewByteArray(dataLen));
  if (dataArray.get() == NULL) {
    LOG(WARNING) << "byte[] allocation failed: " << dataLen;
    env->ExceptionClear();
    return false;
  }
  env->SetByteArrayRegion(dataArray.get(), 0, dataLen, reinterpret_cast<const jbyte*>(buf));

  const int kChunkHdrLen = 8;

  // Run through and find all chunks.  [Currently just find the first.]
  ScopedByteArrayRO contents(env, dataArray.get());
  jint type = JDWP::Get4BE(reinterpret_cast<const uint8_t*>(&contents[0]));
  jint length = JDWP::Get4BE(reinterpret_cast<const uint8_t*>(&contents[4]));
  jint offset = kChunkHdrLen;
  if (offset + length > dataLen) {
    LOG(WARNING) << StringPrintf("bad chunk found (len=%u pktLen=%d)", length, dataLen);
    return false;
  }

  // Call "private static Chunk dispatch(int type, byte[] data, int offset, int length)".
  ScopedLocalRef<jobject> chunk(env, env->CallStaticObjectMethod(DdmServer_class, dispatch_mid, type, dataArray.get(), offset, length));
  if (env->ExceptionCheck()) {
    LOG(INFO) << StringPrintf("Exception thrown by dispatcher for 0x%08x", type);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
  }

  if (chunk.get() == NULL) {
    return false;
  }

  /*
   * Pull the pieces out of the chunk.  We copy the results into a
   * newly-allocated buffer that the caller can free.  We don't want to
   * continue using the Chunk object because nothing has a reference to it.
   *
   * We could avoid this by returning type/data/offset/length and having
   * the caller be aware of the object lifetime issues, but that
   * integrates the JDWP code more tightly into the VM, and doesn't work
   * if we have responses for multiple chunks.
   *
   * So we're pretty much stuck with copying data around multiple times.
   */
  ScopedLocalRef<jbyteArray> replyData(env, reinterpret_cast<jbyteArray>(env->GetObjectField(chunk.get(), data_fid)));
  length = env->GetIntField(chunk.get(), length_fid);
  offset = env->GetIntField(chunk.get(), offset_fid);
  type = env->GetIntField(chunk.get(), type_fid);

  LOG(VERBOSE) << StringPrintf("DDM reply: type=0x%08x data=%p offset=%d length=%d", type, replyData.get(), offset, length);
  if (length == 0 || replyData.get() == NULL) {
    return false;
  }

  jsize replyLength = env->GetArrayLength(replyData.get());
  if (offset + length > replyLength) {
    LOG(WARNING) << StringPrintf("chunk off=%d len=%d exceeds reply array len %d", offset, length, replyLength);
    return false;
  }

  uint8_t* reply = new uint8_t[length + kChunkHdrLen];
  if (reply == NULL) {
    LOG(WARNING) << "malloc failed: " << (length + kChunkHdrLen);
    return false;
  }
  JDWP::Set4BE(reply + 0, type);
  JDWP::Set4BE(reply + 4, length);
  env->GetByteArrayRegion(replyData.get(), offset, length, reinterpret_cast<jbyte*>(reply + kChunkHdrLen));

  *pReplyBuf = reply;
  *pReplyLen = length + kChunkHdrLen;

  LOG(VERBOSE) << StringPrintf("dvmHandleDdm returning type=%.4s buf=%p len=%d", (char*) reply, reply, length);
  return true;
}

void Dbg::DdmBroadcast(bool connect) {
  LOG(VERBOSE) << "Broadcasting DDM " << (connect ? "connect" : "disconnect") << "...";

  Thread* self = Thread::Current();
  if (self->GetState() != Thread::kRunnable) {
    LOG(ERROR) << "DDM broadcast in thread state " << self->GetState();
    /* try anyway? */
  }

  JNIEnv* env = self->GetJniEnv();
  static jclass DdmServer_class = env->FindClass("org/apache/harmony/dalvik/ddmc/DdmServer");
  static jmethodID broadcast_mid = env->GetStaticMethodID(DdmServer_class, "broadcast", "(I)V");
  jint event = connect ? 1 /*DdmServer.CONNECTED*/ : 2 /*DdmServer.DISCONNECTED*/;
  env->CallStaticVoidMethod(DdmServer_class, broadcast_mid, event);
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
    JDWP::Set4BE(&buf[0], t->GetThinLockId());
    Dbg::DdmSendChunk(CHUNK_TYPE("THDE"), 4, buf);
  } else {
    CHECK(type == CHUNK_TYPE("THCR") || type == CHUNK_TYPE("THNM")) << type;
    SirtRef<String> name(t->GetName());
    size_t char_count = (name.get() != NULL) ? name->GetLength() : 0;
    const jchar* chars = name->GetCharArray()->GetData();

    std::vector<uint8_t> bytes;
    JDWP::Append4BE(bytes, t->GetThinLockId());
    JDWP::AppendUtf16BE(bytes, chars, char_count);
    CHECK_EQ(bytes.size(), char_count*2 + sizeof(uint32_t)*2);
    Dbg::DdmSendChunk(type, bytes);
  }
}

static void DdmSendThreadStartCallback(Thread* t, void*) {
  Dbg::DdmSendThreadNotification(t, CHUNK_TYPE("THCR"));
}

void Dbg::DdmSetThreadNotification(bool enable) {
  // We lock the thread list to avoid sending duplicate events or missing
  // a thread change. We should be okay holding this lock while sending
  // the messages out. (We have to hold it while accessing a live thread.)
  ScopedThreadListLock thread_list_lock;

  gDdmThreadNotification = enable;
  if (enable) {
    Runtime::Current()->GetThreadList()->ForEach(DdmSendThreadStartCallback, NULL);
  }
}

void Dbg::PostThreadStartOrStop(Thread* t, uint32_t type) {
  if (gDebuggerActive) {
    JDWP::ObjectId id = gRegistry->Add(t->GetPeer());
    gJdwpState->PostThreadChange(id, type == CHUNK_TYPE("THCR"));
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
  CHECK(buf != NULL);
  iovec vec[1];
  vec[0].iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(buf));
  vec[0].iov_len = byte_count;
  Dbg::DdmSendChunkV(type, vec, 1);
}

void Dbg::DdmSendChunk(uint32_t type, const std::vector<uint8_t>& bytes) {
  DdmSendChunk(type, bytes.size(), &bytes[0]);
}

void Dbg::DdmSendChunkV(uint32_t type, const struct iovec* iov, int iov_count) {
  if (gJdwpState == NULL) {
    LOG(VERBOSE) << "Debugger thread not active, ignoring DDM send: " << type;
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
  std::vector<uint8_t> bytes;
  JDWP::Append4BE(bytes, heap_count);
  JDWP::Append4BE(bytes, 1); // Heap id (bogus; we only have one heap).
  JDWP::Append8BE(bytes, MilliTime());
  JDWP::Append1BE(bytes, reason);
  JDWP::Append4BE(bytes, Heap::GetMaxMemory()); // Max allowed heap size in bytes.
  JDWP::Append4BE(bytes, Heap::GetTotalMemory()); // Current heap size in bytes.
  JDWP::Append4BE(bytes, Heap::GetBytesAllocated());
  JDWP::Append4BE(bytes, Heap::GetObjectsAllocated());
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

struct HeapChunkContext {
  std::vector<uint8_t> buf;
  uint8_t* p;
  uint8_t* pieceLenField;
  size_t totalAllocationUnits;
  uint32_t type;
  bool merge;
  bool needHeader;

  // Maximum chunk size.  Obtain this from the formula:
  // (((maximum_heap_size / ALLOCATION_UNIT_SIZE) + 255) / 256) * 2
  HeapChunkContext(bool merge, bool native)
      : buf(16384 - 16),
        type(0),
        merge(merge) {
    Reset();
    if (native) {
      type = CHUNK_TYPE("NHSG");
    } else {
      type = merge ? CHUNK_TYPE("HPSG") : CHUNK_TYPE("HPSO");
    }
  }

  ~HeapChunkContext() {
    if (p > &buf[0]) {
      Flush();
    }
  }

  void EnsureHeader(const void* chunk_ptr) {
    if (!needHeader) {
      return;
    }

    // Start a new HPSx chunk.
    JDWP::Write4BE(&p, 1); // Heap id (bogus; we only have one heap).
    JDWP::Write1BE(&p, 8); // Size of allocation unit, in bytes.

    JDWP::Write4BE(&p, reinterpret_cast<uintptr_t>(chunk_ptr)); // virtual address of segment start.
    JDWP::Write4BE(&p, 0); // offset of this piece (relative to the virtual address).
    // [u4]: length of piece, in allocation units
    // We won't know this until we're done, so save the offset and stuff in a dummy value.
    pieceLenField = p;
    JDWP::Write4BE(&p, 0x55555555);
    needHeader = false;
  }

  void Flush() {
    // Patch the "length of piece" field.
    CHECK_LE(&buf[0], pieceLenField);
    CHECK_LE(pieceLenField, p);
    JDWP::Set4BE(pieceLenField, totalAllocationUnits);

    Dbg::DdmSendChunk(type, p - &buf[0], &buf[0]);
    Reset();
  }

  static void HeapChunkCallback(const void* chunk_ptr, size_t chunk_len, const void* user_ptr, size_t user_len, void* arg) {
    reinterpret_cast<HeapChunkContext*>(arg)->HeapChunkCallback(chunk_ptr, chunk_len, user_ptr, user_len);
  }

 private:
  enum { ALLOCATION_UNIT_SIZE = 8 };

  void Reset() {
    p = &buf[0];
    totalAllocationUnits = 0;
    needHeader = true;
    pieceLenField = NULL;
  }

  void HeapChunkCallback(const void* chunk_ptr, size_t chunk_len, const void* user_ptr, size_t user_len) {
    CHECK_EQ((chunk_len & (ALLOCATION_UNIT_SIZE-1)), 0U);

    /* Make sure there's enough room left in the buffer.
     * We need to use two bytes for every fractional 256
     * allocation units used by the chunk.
     */
    {
      size_t needed = (((chunk_len/ALLOCATION_UNIT_SIZE + 255) / 256) * 2);
      size_t bytesLeft = buf.size() - (size_t)(p - &buf[0]);
      if (bytesLeft < needed) {
        Flush();
      }

      bytesLeft = buf.size() - (size_t)(p - &buf[0]);
      if (bytesLeft < needed) {
        LOG(WARNING) << "chunk is too big to transmit (chunk_len=" << chunk_len << ", " << needed << " bytes)";
        return;
      }
    }

    // OLD-TODO: notice when there's a gap and start a new heap, or at least a new range.
    EnsureHeader(chunk_ptr);

    // Determine the type of this chunk.
    // OLD-TODO: if context.merge, see if this chunk is different from the last chunk.
    // If it's the same, we should combine them.
    uint8_t state = ExamineObject(reinterpret_cast<const Object*>(user_ptr), (type == CHUNK_TYPE("NHSG")));

    // Write out the chunk description.
    chunk_len /= ALLOCATION_UNIT_SIZE;   // convert to allocation units
    totalAllocationUnits += chunk_len;
    while (chunk_len > 256) {
      *p++ = state | HPSG_PARTIAL;
      *p++ = 255;     // length - 1
      chunk_len -= 256;
    }
    *p++ = state;
    *p++ = chunk_len - 1;
  }

  uint8_t ExamineObject(const Object* o, bool is_native_heap) {
    if (o == NULL) {
      return HPSG_STATE(SOLIDITY_FREE, 0);
    }

    // It's an allocated chunk. Figure out what it is.

    // If we're looking at the native heap, we'll just return
    // (SOLIDITY_HARD, KIND_NATIVE) for all allocated chunks.
    if (is_native_heap || !Heap::IsLiveObjectLocked(o)) {
      return HPSG_STATE(SOLIDITY_HARD, KIND_NATIVE);
    }

    Class* c = o->GetClass();
    if (c == NULL) {
      // The object was probably just created but hasn't been initialized yet.
      return HPSG_STATE(SOLIDITY_HARD, KIND_OBJECT);
    }

    if (!Heap::IsHeapAddress(c)) {
      LOG(WARNING) << "invalid class for managed heap object: " << o << " " << c;
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

  DISALLOW_COPY_AND_ASSIGN(HeapChunkContext);
};

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
  JDWP::Set4BE(&heap_id[0], 1); // Heap id (bogus; we only have one heap).
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHST") : CHUNK_TYPE("HPST"), sizeof(heap_id), heap_id);

  // Send a series of heap segment chunks.
  HeapChunkContext context((what == HPSG_WHAT_MERGED_OBJECTS), native);
  if (native) {
    dlmalloc_walk_heap(HeapChunkContext::HeapChunkCallback, &context);
  } else {
    Heap::WalkHeap(HeapChunkContext::HeapChunkCallback, &context);
  }

  // Finally, send a heap end chunk.
  Dbg::DdmSendChunk(native ? CHUNK_TYPE("NHEN") : CHUNK_TYPE("HPEN"), sizeof(heap_id), heap_id);
}

void Dbg::SetAllocTrackingEnabled(bool enabled) {
  MutexLock mu(gAllocTrackerLock);
  if (enabled) {
    if (recent_allocation_records_ == NULL) {
      LOG(INFO) << "Enabling alloc tracker (" << kNumAllocRecords << " entries, "
                << kMaxAllocRecordStackDepth << " frames --> "
                << (sizeof(AllocRecord) * kNumAllocRecords) << " bytes)";
      gAllocRecordHead = gAllocRecordCount = 0;
      recent_allocation_records_ = new AllocRecord[kNumAllocRecords];
      CHECK(recent_allocation_records_ != NULL);
    }
  } else {
    delete[] recent_allocation_records_;
    recent_allocation_records_ = NULL;
  }
}

struct AllocRecordStackVisitor : public Thread::StackVisitor {
  AllocRecordStackVisitor(AllocRecord* record) : record(record), depth(0) {
  }

  virtual void VisitFrame(const Frame& f, uintptr_t pc) {
    if (depth >= kMaxAllocRecordStackDepth) {
      return;
    }
    Method* m = f.GetMethod();
    if (m == NULL || m->IsCalleeSaveMethod()) {
      return;
    }
    record->stack[depth].method = m;
    record->stack[depth].raw_pc = pc;
    ++depth;
  }

  ~AllocRecordStackVisitor() {
    // Clear out any unused stack trace elements.
    for (; depth < kMaxAllocRecordStackDepth; ++depth) {
      record->stack[depth].method = NULL;
      record->stack[depth].raw_pc = 0;
    }
  }

  AllocRecord* record;
  size_t depth;
};

void Dbg::RecordAllocation(Class* type, size_t byte_count) {
  Thread* self = Thread::Current();
  CHECK(self != NULL);

  MutexLock mu(gAllocTrackerLock);
  if (recent_allocation_records_ == NULL) {
    return;
  }

  // Advance and clip.
  if (++gAllocRecordHead == kNumAllocRecords) {
    gAllocRecordHead = 0;
  }

  // Fill in the basics.
  AllocRecord* record = &recent_allocation_records_[gAllocRecordHead];
  record->type = type;
  record->byte_count = byte_count;
  record->thin_lock_id = self->GetThinLockId();

  // Fill in the stack trace.
  AllocRecordStackVisitor visitor(record);
  self->WalkStack(&visitor);

  if (gAllocRecordCount < kNumAllocRecords) {
    ++gAllocRecordCount;
  }
}

/*
 * Return the index of the head element.
 *
 * We point at the most-recently-written record, so if allocRecordCount is 1
 * we want to use the current element.  Take "head+1" and subtract count
 * from it.
 *
 * We need to handle underflow in our circular buffer, so we add
 * kNumAllocRecords and then mask it back down.
 */
inline static int headIndex() {
  return (gAllocRecordHead+1 + kNumAllocRecords - gAllocRecordCount) & (kNumAllocRecords-1);
}

void Dbg::DumpRecentAllocations() {
  MutexLock mu(gAllocTrackerLock);
  if (recent_allocation_records_ == NULL) {
    LOG(INFO) << "Not recording tracked allocations";
    return;
  }

  // "i" is the head of the list.  We want to start at the end of the
  // list and move forward to the tail.
  size_t i = headIndex();
  size_t count = gAllocRecordCount;

  LOG(INFO) << "Tracked allocations, (head=" << gAllocRecordHead << " count=" << count << ")";
  while (count--) {
    AllocRecord* record = &recent_allocation_records_[i];

    LOG(INFO) << StringPrintf(" T=%-2d %6d ", record->thin_lock_id, record->byte_count)
              << PrettyClass(record->type);

    for (size_t stack_frame = 0; stack_frame < kMaxAllocRecordStackDepth; ++stack_frame) {
      const Method* m = record->stack[stack_frame].method;
      if (m == NULL) {
        break;
      }
      LOG(INFO) << "    " << PrettyMethod(m) << " line " << record->stack[stack_frame].LineNumber();
    }

    // pause periodically to help logcat catch up
    if ((count % 5) == 0) {
      usleep(40000);
    }

    i = (i + 1) & (kNumAllocRecords-1);
  }
}

class StringTable {
 public:
  StringTable() {
  }

  void Add(const char* s) {
    table_.insert(s);
  }

  size_t IndexOf(const char* s) {
    return std::distance(table_.begin(), table_.find(s));
  }

  size_t Size() {
    return table_.size();
  }

  void WriteTo(std::vector<uint8_t>& bytes) {
    typedef std::set<const char*>::const_iterator It; // TODO: C++0x auto
    for (It it = table_.begin(); it != table_.end(); ++it) {
      const char* s = *it;
      size_t s_len = CountModifiedUtf8Chars(s);
      UniquePtr<uint16_t> s_utf16(new uint16_t[s_len]);
      ConvertModifiedUtf8ToUtf16(s_utf16.get(), s);
      JDWP::AppendUtf16BE(bytes, s_utf16.get(), s_len);
    }
  }

 private:
  std::set<const char*> table_;
  DISALLOW_COPY_AND_ASSIGN(StringTable);
};

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
 *   (2b) threadId
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
 * can be (kMaxAllocRecordStackDepth * kNumAllocRecords) unique strings in
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

  MutexLock mu(gAllocTrackerLock);

  /*
   * Part 1: generate string tables.
   */
  StringTable class_names;
  StringTable method_names;
  StringTable filenames;

  int count = gAllocRecordCount;
  int idx = headIndex();
  while (count--) {
    AllocRecord* record = &recent_allocation_records_[idx];

    class_names.Add(ClassHelper(record->type).GetDescriptor().c_str());

    MethodHelper mh;
    for (size_t i = 0; i < kMaxAllocRecordStackDepth; i++) {
      Method* m = record->stack[i].method;
      mh.ChangeMethod(m);
      if (m != NULL) {
        class_names.Add(mh.GetDeclaringClassDescriptor());
        method_names.Add(mh.GetName());
        filenames.Add(mh.GetDeclaringClassSourceFile());
      }
    }

    idx = (idx + 1) & (kNumAllocRecords-1);
  }

  LOG(INFO) << "allocation records: " << gAllocRecordCount;

  /*
   * Part 2: allocate a buffer and generate the output.
   */
  std::vector<uint8_t> bytes;

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
  JDWP::Append2BE(bytes, gAllocRecordCount);
  size_t string_table_offset = bytes.size();
  JDWP::Append4BE(bytes, 0); // We'll patch this later...
  JDWP::Append2BE(bytes, class_names.Size());
  JDWP::Append2BE(bytes, method_names.Size());
  JDWP::Append2BE(bytes, filenames.Size());

  count = gAllocRecordCount;
  idx = headIndex();
  ClassHelper kh;
  while (count--) {
    // For each entry:
    // (4b) total allocation size
    // (2b) thread id
    // (2b) allocated object's class name index
    // (1b) stack depth
    AllocRecord* record = &recent_allocation_records_[idx];
    size_t stack_depth = record->GetDepth();
    JDWP::Append4BE(bytes, record->byte_count);
    JDWP::Append2BE(bytes, record->thin_lock_id);
    kh.ChangeClass(record->type);
    JDWP::Append2BE(bytes, class_names.IndexOf(kh.GetDescriptor().c_str()));
    JDWP::Append1BE(bytes, stack_depth);

    MethodHelper mh;
    for (size_t stack_frame = 0; stack_frame < stack_depth; ++stack_frame) {
      // For each stack frame:
      // (2b) method's class name
      // (2b) method name
      // (2b) method source file
      // (2b) line number, clipped to 32767; -2 if native; -1 if no source
      mh.ChangeMethod(record->stack[stack_frame].method);
      JDWP::Append2BE(bytes, class_names.IndexOf(mh.GetDeclaringClassDescriptor()));
      JDWP::Append2BE(bytes, method_names.IndexOf(mh.GetName()));
      JDWP::Append2BE(bytes, filenames.IndexOf(mh.GetDeclaringClassSourceFile()));
      JDWP::Append2BE(bytes, record->stack[stack_frame].LineNumber());
    }

    idx = (idx + 1) & (kNumAllocRecords-1);
  }

  // (xb) class name strings
  // (xb) method name strings
  // (xb) source file strings
  JDWP::Set4BE(&bytes[string_table_offset], bytes.size());
  class_names.WriteTo(bytes);
  method_names.WriteTo(bytes);
  filenames.WriteTo(bytes);

  JNIEnv* env = Thread::Current()->GetJniEnv();
  jbyteArray result = env->NewByteArray(bytes.size());
  if (result != NULL) {
    env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  }
  return result;
}

}  // namespace art

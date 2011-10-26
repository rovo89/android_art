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

#include "ScopedPrimitiveArray.h"
#include "stack_indirect_reference_table.h"
#include "thread_list.h"

namespace art {

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

  bool Contains(JDWP::ObjectId id) {
    MutexLock mu(lock_);
    return map_.find(id) != map_.end();
  }

 private:
  Mutex lock_;
  std::map<JDWP::ObjectId, Object*> map_;
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

static ObjectRegistry* gRegistry = NULL;

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
    LOG(WARNING) << "debugger thread failed to initialize";
    return;
  }

  // If a debugger has already attached, send the "welcome" message.
  // This may cause us to suspend all threads.
  if (gJdwpState->IsActive()) {
    //ScopedThreadStateChange(Thread::Current(), Thread::kRunnable);
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

void Dbg::Active() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::Disconnected() {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::IsDebuggerConnected() {
  return gDebuggerActive;
}

bool Dbg::IsDebuggingEnabled() {
  return gJdwpConfigured;
}

int64_t Dbg::LastDebuggerActivity() {
  UNIMPLEMENTED(WARNING);
  return -1;
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
  UNIMPLEMENTED(FATAL);
}

void Dbg::Exit(int status) {
  UNIMPLEMENTED(FATAL);
}

const char* Dbg::GetClassDescriptor(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

JDWP::ObjectId Dbg::GetClassObject(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::RefTypeId Dbg::GetSuperclass(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::GetClassLoader(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

uint32_t Dbg::GetAccessFlags(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::IsInterface(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::GetClassList(uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetVisibleClassList(JDWP::ObjectId classLoaderId, uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetClassInfo(JDWP::RefTypeId classId, uint8_t* pTypeTag, uint32_t* pStatus, const char** pSignature) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::FindLoadedClassBySignature(const char* classDescriptor, JDWP::RefTypeId* pRefTypeId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::GetObjectType(JDWP::ObjectId objectId, uint8_t* pRefTypeTag, JDWP::RefTypeId* pRefTypeId) {
  UNIMPLEMENTED(FATAL);
}

uint8_t Dbg::GetClassObjectType(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

const char* Dbg::GetSignature(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

const char* Dbg::GetSourceFile(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

const char* Dbg::GetObjectTypeName(JDWP::ObjectId objectId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

uint8_t Dbg::GetObjectTag(JDWP::ObjectId objectId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

int Dbg::GetTagWidth(int tag) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

int Dbg::GetArrayLength(JDWP::ObjectId arrayId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

uint8_t Dbg::GetArrayElementTag(JDWP::ObjectId arrayId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::OutputArray(JDWP::ObjectId arrayId, int firstIndex, int count, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
  return false;
}

bool Dbg::SetArrayElements(JDWP::ObjectId arrayId, int firstIndex, int count, const uint8_t* buf) {
  UNIMPLEMENTED(FATAL);
  return false;
}

JDWP::ObjectId Dbg::CreateString(const char* str) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::CreateObject(JDWP::RefTypeId classId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::CreateArrayObject(JDWP::RefTypeId arrayTypeId, uint32_t length) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

const char* Dbg::GetMethodName(JDWP::RefTypeId refTypeId, JDWP::MethodId id) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void Dbg::OutputAllFields(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputAllMethods(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputAllInterfaces(JDWP::RefTypeId refTypeId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputLineTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputVariableTable(JDWP::RefTypeId refTypeId, JDWP::MethodId id, bool withGeneric, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

uint8_t Dbg::GetFieldBasicTag(JDWP::ObjectId objId, JDWP::FieldId fieldId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

uint8_t Dbg::GetStaticFieldBasicTag(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void Dbg::GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, uint64_t rawValue, int width) {
  UNIMPLEMENTED(FATAL);
}

char* Dbg::StringToUtf8(JDWP::ObjectId strId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

char* Dbg::GetThreadName(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

JDWP::ObjectId Dbg::GetThreadGroup(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

char* Dbg::GetThreadGroupName(JDWP::ObjectId threadGroupId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

JDWP::ObjectId Dbg::GetThreadGroupParent(JDWP::ObjectId threadGroupId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::GetSystemThreadGroupId() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::GetMainThreadGroupId() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::GetThreadStatus(JDWP::ObjectId threadId, uint32_t* threadStatus, uint32_t* suspendStatus) {
  UNIMPLEMENTED(FATAL);
  return false;
}

uint32_t Dbg::GetThreadSuspendCount(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::ThreadExists(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

bool Dbg::IsSuspended(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

//void Dbg::WaitForSuspend(JDWP::ObjectId threadId);

void Dbg::GetThreadGroupThreads(JDWP::ObjectId threadGroupId, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetAllThreads(JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  UNIMPLEMENTED(FATAL);
}

int Dbg::GetThreadFrameCount(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::GetThreadFrame(JDWP::ObjectId threadId, int num, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc) {
  UNIMPLEMENTED(FATAL);
  return false;
}

JDWP::ObjectId Dbg::GetThreadSelfId() {
  return gRegistry->Add(Thread::Current()->GetPeer());
}

void Dbg::SuspendVM() {
  Runtime::Current()->GetThreadList()->SuspendAll(true);
}

void Dbg::ResumeVM() {
  Runtime::Current()->GetThreadList()->ResumeAll(true);
}

void Dbg::SuspendThread(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::ResumeThread(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SuspendSelf() {
  Runtime::Current()->GetThreadList()->SuspendSelfForDebugger();
}

bool Dbg::GetThisObject(JDWP::ObjectId threadId, JDWP::FrameId frameId, JDWP::ObjectId* pThisId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::GetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, uint8_t tag, uint8_t* buf, int expectedLen) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, uint8_t tag, uint64_t value, int width) {
  UNIMPLEMENTED(FATAL);
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

JDWP::JdwpError Dbg::InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId, JDWP::RefTypeId classId, JDWP::MethodId methodId, uint32_t numArgs, uint64_t* argArray, uint32_t options, uint8_t* pResultTag, uint64_t* pResultValue, JDWP::ObjectId* pExceptObj) {
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
  jbyteArray dataArray = env->NewByteArray(dataLen);
  if (dataArray == NULL) {
    LOG(WARNING) << "byte[] allocation failed: " << dataLen;
    env->ExceptionClear();
    return false;
  }
  env->SetByteArrayRegion(dataArray, 0, dataLen, reinterpret_cast<const jbyte*>(buf));

  const int kChunkHdrLen = 8;

  // Run through and find all chunks.  [Currently just find the first.]
  ScopedByteArrayRO contents(env, dataArray);
  jint type = JDWP::get4BE(reinterpret_cast<const uint8_t*>(&contents[0]));
  jint length = JDWP::get4BE(reinterpret_cast<const uint8_t*>(&contents[4]));
  jint offset = kChunkHdrLen;
  if (offset + length > dataLen) {
    LOG(WARNING) << StringPrintf("bad chunk found (len=%u pktLen=%d)", length, dataLen);
    return false;
  }

  // Call "private static Chunk dispatch(int type, byte[] data, int offset, int length)".
  jobject chunk = env->CallStaticObjectMethod(DdmServer_class, dispatch_mid, type, dataArray, offset, length);
  if (env->ExceptionCheck()) {
    LOG(INFO) << StringPrintf("Exception thrown by dispatcher for 0x%08x", type);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return false;
  }

  if (chunk == NULL) {
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
  jbyteArray replyData = reinterpret_cast<jbyteArray>(env->GetObjectField(chunk, data_fid));
  length = env->GetIntField(chunk, length_fid);
  offset = env->GetIntField(chunk, offset_fid);
  type = env->GetIntField(chunk, type_fid);

  LOG(VERBOSE) << StringPrintf("DDM reply: type=0x%08x data=%p offset=%d length=%d", type, replyData, offset, length);
  if (length == 0 || replyData == NULL) {
    return false;
  }

  jsize replyLength = env->GetArrayLength(replyData);
  if (offset + length > replyLength) {
    LOG(WARNING) << StringPrintf("chunk off=%d len=%d exceeds reply array len %d", offset, length, replyLength);
    return false;
  }

  uint8_t* reply = new uint8_t[length + kChunkHdrLen];
  if (reply == NULL) {
    LOG(WARNING) << "malloc failed: " << (length + kChunkHdrLen);
    return false;
  }
  JDWP::set4BE(reply + 0, type);
  JDWP::set4BE(reply + 4, length);
  env->GetByteArrayRegion(replyData, offset, length, reinterpret_cast<jbyte*>(reply + kChunkHdrLen));

  *pReplyBuf = reply;
  *pReplyLen = length + kChunkHdrLen;

  LOG(VERBOSE) << StringPrintf("dvmHandleDdm returning type=%.4s buf=%p len=%d", (char*) reply, reply, length);
  return true;
}

void DdmBroadcast(bool connect) {
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
  DdmBroadcast(true);
}

void Dbg::DdmDisconnected() {
  DdmBroadcast(false);
  gDdmThreadNotification = false;
}

/*
 * Send a notification when a thread starts or stops.
 *
 * Because we broadcast the full set of threads when the notifications are
 * first enabled, it's possible for "thread" to be actively executing.
 */
void DdmSendThreadNotification(Thread* t, bool started) {
  if (!gDdmThreadNotification) {
    return;
  }

  if (started) {
    SirtRef<String> name(t->GetName());
    size_t char_count = (name.get() != NULL) ? name->GetLength() : 0;

    size_t byte_count = char_count*2 + sizeof(uint32_t)*2;
    std::vector<uint8_t> buf(byte_count);
    JDWP::set4BE(&buf[0], t->GetThinLockId());
    JDWP::set4BE(&buf[4], char_count);
    if (char_count > 0) {
      // Copy the UTF-16 string, transforming to big-endian.
      const jchar* src = name->GetCharArray()->GetData();
      jchar* dst = reinterpret_cast<jchar*>(&buf[8]);
      while (char_count--) {
        JDWP::set2BE(reinterpret_cast<uint8_t*>(dst++), *src++);
      }
    }
    Dbg::DdmSendChunk(CHUNK_TYPE("THCR"), buf.size(), &buf[0]);
  } else {
    uint8_t buf[4];
    JDWP::set4BE(&buf[0], t->GetThinLockId());
    Dbg::DdmSendChunk(CHUNK_TYPE("THDE"), 4, buf);
  }
}

void DdmSendThreadStartCallback(Thread* t) {
  DdmSendThreadNotification(t, true);
}

void Dbg::DdmSetThreadNotification(bool enable) {
  // We lock the thread list to avoid sending duplicate events or missing
  // a thread change. We should be okay holding this lock while sending
  // the messages out. (We have to hold it while accessing a live thread.)
  ThreadListLock lock;

  gDdmThreadNotification = enable;
  if (enable) {
    Runtime::Current()->GetThreadList()->ForEach(DdmSendThreadStartCallback);
  }
}

void PostThreadStartOrStop(Thread* t, bool is_start) {
  if (gDebuggerActive) {
    JDWP::ObjectId id = gRegistry->Add(t->GetPeer());
    gJdwpState->PostThreadChange(id, is_start);
  }
  DdmSendThreadNotification(t, is_start);
}

void Dbg::PostThreadStart(Thread* t) {
  PostThreadStartOrStop(t, true);
}

void Dbg::PostThreadDeath(Thread* t) {
  PostThreadStartOrStop(t, false);
}

void Dbg::DdmSendChunk(int type, size_t byte_count, const uint8_t* buf) {
  CHECK(buf != NULL);
  iovec vec[1];
  vec[0].iov_base = reinterpret_cast<void*>(const_cast<uint8_t*>(buf));
  vec[0].iov_len = byte_count;
  Dbg::DdmSendChunkV(type, vec, 1);
}

void Dbg::DdmSendChunkV(int type, const struct iovec* iov, int iovcnt) {
  if (gJdwpState == NULL) {
    LOG(VERBOSE) << "Debugger thread not active, ignoring DDM send: " << type;
  } else {
    gJdwpState->DdmSendChunkV(type, iov, iovcnt);
  }
}

}  // namespace art

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

/*
 * Dalvik-specific side of debugger support.  (The JDWP code is intended to
 * be relatively generic.)
 */
#ifndef ART_DEBUGGER_H_
#define ART_DEBUGGER_H_

#include <pthread.h>

#include <string>

#include "jdwp/jdwp.h"
#include "object.h"

namespace art {

struct AllocRecord;
struct Thread;

/*
 * Invoke-during-breakpoint support.
 */
struct DebugInvokeReq {
  DebugInvokeReq() : lock_("a DebugInvokeReq lock"), cond_("a DebugInvokeReq condition variable") {
  }

  /* boolean; only set when we're in the tail end of an event handler */
  bool ready;

  /* boolean; set if the JDWP thread wants this thread to do work */
  bool invoke_needed;

  /* request */
  Object* obj;        /* not used for ClassType.InvokeMethod */
  Object* thread;
  Class* class_;
  Method* method;
  uint32_t num_args;
  uint64_t* arg_array;   /* will be NULL if numArgs==0 */
  uint32_t options;

  /* result */
  JDWP::JdwpError error;
  uint8_t result_tag;
  JValue result_value;
  JDWP::ObjectId exception;

  /* condition variable to wait on while the method executes */
  Mutex lock_;
  ConditionVariable cond_;
};

class Dbg {
public:
  static bool ParseJdwpOptions(const std::string& options);
  static void SetJdwpAllowed(bool allowed);

  static void StartJdwp();
  static void StopJdwp();

  // Invoked by the GC in case we need to keep DDMS informed.
  static void GcDidFinish();

  // Return the DebugInvokeReq for the current thread.
  static DebugInvokeReq* GetInvokeReq();

  static Thread* GetDebugThread();
  static void ClearWaitForEventThread();

  /*
   * Enable/disable breakpoints and step modes.  Used to provide a heads-up
   * when the debugger attaches.
   */
  static void Connected();
  static void GoActive();
  static void Disconnected();

  /*
   * Returns "true" if a debugger is connected.  Returns "false" if it's
   * just DDM.
   */
  static bool IsDebuggerConnected();

  static bool IsDebuggingEnabled();

  /*
   * Time, in milliseconds, since the last debugger activity.  Does not
   * include DDMS activity.  Returns -1 if there has been no activity.
   * Returns 0 if we're in the middle of handling a debugger request.
   */
  static int64_t LastDebuggerActivity();

  /*
   * Block/allow GC depending on what we're doing.  These return the old
   * status, which can be fed to ThreadContinuing() to restore the previous
   * mode.
   */
  static int ThreadRunning();
  static int ThreadWaiting();
  static int ThreadContinuing(int status);

  static void UndoDebuggerSuspensions();

  // The debugger wants the VM to exit.
  static void Exit(int status);

  static void VisitRoots(Heap::RootVisitor* visitor, void* arg);

  /*
   * Class, Object, Array
   */
  static std::string GetClassDescriptor(JDWP::RefTypeId id);
  static JDWP::ObjectId GetClassObject(JDWP::RefTypeId id);
  static JDWP::RefTypeId GetSuperclass(JDWP::RefTypeId id);
  static JDWP::ObjectId GetClassLoader(JDWP::RefTypeId id);
  static uint32_t GetAccessFlags(JDWP::RefTypeId id);
  static bool IsInterface(JDWP::RefTypeId id);
  static void GetClassList(uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf);
  static void GetVisibleClassList(JDWP::ObjectId classLoaderId, uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf);
  static void GetClassInfo(JDWP::RefTypeId classId, uint8_t* pTypeTag, uint32_t* pStatus, std::string* pDescriptor);
  static bool FindLoadedClassBySignature(const char* classDescriptor, JDWP::RefTypeId* pRefTypeId);
  static void GetObjectType(JDWP::ObjectId objectId, uint8_t* pRefTypeTag, JDWP::RefTypeId* pRefTypeId);
  static uint8_t GetClassObjectType(JDWP::RefTypeId refTypeId);
  static std::string GetSignature(JDWP::RefTypeId refTypeId);
  static bool GetSourceFile(JDWP::RefTypeId refTypeId, std::string& source_file);
  static const char* GetObjectTypeName(JDWP::ObjectId objectId);
  static uint8_t GetObjectTag(JDWP::ObjectId objectId);
  static size_t GetTagWidth(JDWP::JdwpTag tag);

  static int GetArrayLength(JDWP::ObjectId arrayId);
  static uint8_t GetArrayElementTag(JDWP::ObjectId arrayId);
  static bool OutputArray(JDWP::ObjectId arrayId, int firstIndex, int count, JDWP::ExpandBuf* pReply);
  static bool SetArrayElements(JDWP::ObjectId arrayId, int firstIndex, int count, const uint8_t* buf);

  static JDWP::ObjectId CreateString(const char* str);
  static JDWP::ObjectId CreateObject(JDWP::RefTypeId classId);
  static JDWP::ObjectId CreateArrayObject(JDWP::RefTypeId arrayTypeId, uint32_t length);

  static bool MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId);

  /*
   * Method and Field
   */
  static std::string GetMethodName(JDWP::RefTypeId refTypeId, JDWP::MethodId id);
  static void OutputDeclaredFields(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply);
  static void OutputDeclaredMethods(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply);
  static void OutputDeclaredInterfaces(JDWP::RefTypeId refTypeId, JDWP::ExpandBuf* pReply);
  static void OutputLineTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId, JDWP::ExpandBuf* pReply);
  static void OutputVariableTable(JDWP::RefTypeId refTypeId, JDWP::MethodId id, bool withGeneric, JDWP::ExpandBuf* pReply);

  static JDWP::JdwpTag GetFieldBasicTag(JDWP::FieldId fieldId);
  static JDWP::JdwpTag GetStaticFieldBasicTag(JDWP::FieldId fieldId);
  static void GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply);
  static void SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width);
  static void GetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply);
  static void SetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, uint64_t rawValue, int width);

  static std::string StringToUtf8(JDWP::ObjectId strId);

  /*
   * Thread, ThreadGroup, Frame
   */
  static bool GetThreadName(JDWP::ObjectId threadId, std::string& name);
  static JDWP::ObjectId GetThreadGroup(JDWP::ObjectId threadId);
  static std::string GetThreadGroupName(JDWP::ObjectId threadGroupId);
  static JDWP::ObjectId GetThreadGroupParent(JDWP::ObjectId threadGroupId);
  static JDWP::ObjectId GetSystemThreadGroupId();
  static JDWP::ObjectId GetMainThreadGroupId();

  static bool GetThreadStatus(JDWP::ObjectId threadId, uint32_t* threadStatus, uint32_t* suspendStatus);
  static uint32_t GetThreadSuspendCount(JDWP::ObjectId threadId);
  static bool ThreadExists(JDWP::ObjectId threadId);
  static bool IsSuspended(JDWP::ObjectId threadId);
  //static void WaitForSuspend(JDWP::ObjectId threadId);
  static void GetThreadGroupThreads(JDWP::ObjectId threadGroupId, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount);
  static void GetAllThreads(JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount);
  static int GetThreadFrameCount(JDWP::ObjectId threadId);
  static bool GetThreadFrame(JDWP::ObjectId threadId, int num, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc);

  static JDWP::ObjectId GetThreadSelfId();
  static void SuspendVM();
  static void ResumeVM();
  static void SuspendThread(JDWP::ObjectId threadId);
  static void ResumeThread(JDWP::ObjectId threadId);
  static void SuspendSelf();

  static bool GetThisObject(JDWP::ObjectId threadId, JDWP::FrameId frameId, JDWP::ObjectId* pThisId);
  static void GetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t expectedLen);
  static void SetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint64_t value, size_t width);

  /*
   * Debugger notification
   */
  enum {
    kBreakPoint     = 0x01,
    kSingleStep     = 0x02,
    kMethodEntry    = 0x04,
    kMethodExit     = 0x08,
  };
  static void PostLocationEvent(const Method* method, int pcOffset, Object* thisPtr, int eventFlags);
  static void PostException(void* throwFp, int throwRelPc, void* catchFp, int catchRelPc, Object* exception);
  static void PostThreadStart(Thread* t);
  static void PostThreadDeath(Thread* t);
  static void PostClassPrepare(Class* c);

  static bool WatchLocation(const JDWP::JdwpLocation* pLoc);
  static void UnwatchLocation(const JDWP::JdwpLocation* pLoc);
  static bool ConfigureStep(JDWP::ObjectId threadId, JDWP::JdwpStepSize size, JDWP::JdwpStepDepth depth);
  static void UnconfigureStep(JDWP::ObjectId threadId);

  static JDWP::JdwpError InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId, JDWP::RefTypeId classId, JDWP::MethodId methodId, uint32_t numArgs, uint64_t* argArray, uint32_t options, JDWP::JdwpTag* pResultTag, uint64_t* pResultValue, JDWP::ObjectId* pExceptObj);
  static void ExecuteMethod(DebugInvokeReq* pReq);

  /* perform "late registration" of an object ID */
  static void RegisterObjectId(JDWP::ObjectId id);

  /*
   * DDM support.
   */
  static void DdmSendThreadNotification(Thread* t, uint32_t type);
  static void DdmSetThreadNotification(bool enable);
  static bool DdmHandlePacket(const uint8_t* buf, int dataLen, uint8_t** pReplyBuf, int* pReplyLen);
  static void DdmConnected();
  static void DdmDisconnected();
  static void DdmSendChunk(uint32_t type, const std::vector<uint8_t>& bytes);
  static void DdmSendChunk(uint32_t type, size_t len, const uint8_t* buf);
  static void DdmSendChunkV(uint32_t type, const struct iovec* iov, int iovcnt);

  /*
   * Recent allocation tracking support.
   */
  static void RecordAllocation(Class* type, size_t byte_count);
  static void SetAllocTrackingEnabled(bool enabled);
  static inline bool IsAllocTrackingEnabled() { return recent_allocation_records_ != NULL; }
  static jbyteArray GetRecentAllocations();
  static void DumpRecentAllocations();

  enum HpifWhen {
    HPIF_WHEN_NEVER = 0,
    HPIF_WHEN_NOW = 1,
    HPIF_WHEN_NEXT_GC = 2,
    HPIF_WHEN_EVERY_GC = 3
  };
  static int DdmHandleHpifChunk(HpifWhen when);

  enum HpsgWhen {
    HPSG_WHEN_NEVER = 0,
    HPSG_WHEN_EVERY_GC = 1,
  };
  enum HpsgWhat {
    HPSG_WHAT_MERGED_OBJECTS = 0,
    HPSG_WHAT_DISTINCT_OBJECTS = 1,
  };
  static bool DdmHandleHpsgNhsgChunk(HpsgWhen when, HpsgWhat what, bool native);

  static void DdmSendHeapInfo(HpifWhen reason);
  static void DdmSendHeapSegments(bool native);

 private:
  static void DdmBroadcast(bool);
  static void GetThreadGroupThreadsImpl(Object*, JDWP::ObjectId**, uint32_t*);
  static void PostThreadStartOrStop(Thread*, uint32_t);

  static AllocRecord* recent_allocation_records_;
};

#define CHUNK_TYPE(_name) \
    static_cast<uint32_t>((_name)[0] << 24 | (_name)[1] << 16 | (_name)[2] << 8 | (_name)[3])

}  // namespace art

#endif  // ART_DEBUGGER_H_

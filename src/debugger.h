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
  DebugInvokeReq()
      : invoke_needed_(false),
        lock_("a DebugInvokeReq lock"),
        cond_("a DebugInvokeReq condition variable") {
  }

  /* boolean; only set when we're in the tail end of an event handler */
  bool ready;

  /* boolean; set if the JDWP thread wants this thread to do work */
  bool invoke_needed_;

  /* request */
  Object* receiver_;      /* not used for ClassType.InvokeMethod */
  Object* thread_;
  Class* class_;
  Method* method_;
  uint32_t arg_count_;
  uint64_t* arg_values_;   /* will be NULL if arg_count_ == 0 */
  uint32_t options_;

  /* result */
  JDWP::JdwpError error;
  JDWP::JdwpTag result_tag;
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
  static void Disposed();

  /*
   * Returns "true" if a debugger is connected.  Returns "false" if it's
   * just DDM.
   */
  static bool IsDebuggerConnected();

  static bool IsDebuggingEnabled();

  static bool IsDisposed();

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
  static std::string GetClassName(JDWP::RefTypeId id);
  static JDWP::JdwpError GetClassObject(JDWP::RefTypeId id, JDWP::ObjectId& classObjectId);
  static JDWP::JdwpError GetSuperclass(JDWP::RefTypeId id, JDWP::RefTypeId& superclassId);
  static JDWP::JdwpError GetClassLoader(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError GetModifiers(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError GetReflectedType(JDWP::RefTypeId classId, JDWP::ExpandBuf* pReply);
  static void GetClassList(std::vector<JDWP::RefTypeId>& classes);
  static JDWP::JdwpError GetClassInfo(JDWP::RefTypeId classId, JDWP::JdwpTypeTag* pTypeTag, uint32_t* pStatus, std::string* pDescriptor);
  static void FindLoadedClassBySignature(const char* descriptor, std::vector<JDWP::RefTypeId>& ids);
  static JDWP::JdwpError GetReferenceType(JDWP::ObjectId objectId, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError GetSignature(JDWP::RefTypeId refTypeId, std::string& signature);
  static JDWP::JdwpError GetSourceFile(JDWP::RefTypeId refTypeId, std::string& source_file);
  static uint8_t GetObjectTag(JDWP::ObjectId objectId);
  static size_t GetTagWidth(JDWP::JdwpTag tag);

  static JDWP::JdwpError GetArrayLength(JDWP::ObjectId arrayId, int& length);
  static JDWP::JdwpError OutputArray(JDWP::ObjectId arrayId, int firstIndex, int count, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError SetArrayElements(JDWP::ObjectId arrayId, int firstIndex, int count, const uint8_t* buf);

  static JDWP::ObjectId CreateString(const std::string& str);
  static JDWP::JdwpError CreateObject(JDWP::RefTypeId classId, JDWP::ObjectId& new_object);
  static JDWP::JdwpError CreateArrayObject(JDWP::RefTypeId arrayTypeId, uint32_t length, JDWP::ObjectId& new_array);

  static bool MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId);

  /*
   * Method and Field
   */
  static std::string GetMethodName(JDWP::RefTypeId refTypeId, JDWP::MethodId id);
  static JDWP::JdwpError OutputDeclaredFields(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError OutputDeclaredMethods(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError OutputDeclaredInterfaces(JDWP::RefTypeId refTypeId, JDWP::ExpandBuf* pReply);
  static void OutputLineTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId, JDWP::ExpandBuf* pReply);
  static void OutputVariableTable(JDWP::RefTypeId refTypeId, JDWP::MethodId id, bool withGeneric, JDWP::ExpandBuf* pReply);

  static JDWP::JdwpTag GetFieldBasicTag(JDWP::FieldId fieldId);
  static JDWP::JdwpTag GetStaticFieldBasicTag(JDWP::FieldId fieldId);
  static JDWP::JdwpError GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width);
  static JDWP::JdwpError GetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError SetStaticFieldValue(JDWP::FieldId fieldId, uint64_t value, int width);

  static std::string StringToUtf8(JDWP::ObjectId strId);

  /*
   * Thread, ThreadGroup, Frame
   */
  static bool GetThreadName(JDWP::ObjectId threadId, std::string& name);
  static JDWP::JdwpError GetThreadGroup(JDWP::ObjectId threadId, JDWP::ExpandBuf* pReply);
  static std::string GetThreadGroupName(JDWP::ObjectId threadGroupId);
  static JDWP::ObjectId GetThreadGroupParent(JDWP::ObjectId threadGroupId);
  static JDWP::ObjectId GetSystemThreadGroupId();
  static JDWP::ObjectId GetMainThreadGroupId();

  static bool GetThreadStatus(JDWP::ObjectId threadId, JDWP::JdwpThreadStatus* pThreadStatus, JDWP::JdwpSuspendStatus* pSuspendStatus);
  static JDWP::JdwpError GetThreadSuspendCount(JDWP::ObjectId threadId, JDWP::ExpandBuf* pReply);
  static bool ThreadExists(JDWP::ObjectId threadId);
  static bool IsSuspended(JDWP::ObjectId threadId);
  //static void WaitForSuspend(JDWP::ObjectId threadId);
  static void GetThreadGroupThreads(JDWP::ObjectId threadGroupId, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount);
  static void GetAllThreads(JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount);
  static int GetThreadFrameCount(JDWP::ObjectId threadId);
  static void GetThreadFrame(JDWP::ObjectId threadId, int num, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc);

  static JDWP::ObjectId GetThreadSelfId();
  static void SuspendVM();
  static void ResumeVM();
  static void SuspendThread(JDWP::ObjectId threadId);
  static void ResumeThread(JDWP::ObjectId threadId);
  static void SuspendSelf();

  static void GetThisObject(JDWP::FrameId frameId, JDWP::ObjectId* pThisId);
  static void GetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint8_t* buf, size_t expectedLen);
  static void SetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, JDWP::JdwpTag tag, uint64_t value, size_t width);

  /*
   * Debugger notification
   */
  enum {
    kBreakpoint     = 0x01,
    kSingleStep     = 0x02,
    kMethodEntry    = 0x04,
    kMethodExit     = 0x08,
  };
  static void PostLocationEvent(const Method* method, int pcOffset, Object* thisPtr, int eventFlags);
  static void PostException(Method** sp, Method* throwMethod, uintptr_t throwNativePc, Method* catchMethod, uintptr_t catchNativePc, Object* exception);
  static void PostThreadStart(Thread* t);
  static void PostThreadDeath(Thread* t);
  static void PostClassPrepare(Class* c);

  static void UpdateDebugger(int32_t dex_pc, Thread* self, Method** sp);

  static void WatchLocation(const JDWP::JdwpLocation* pLoc);
  static void UnwatchLocation(const JDWP::JdwpLocation* pLoc);
  static JDWP::JdwpError ConfigureStep(JDWP::ObjectId threadId, JDWP::JdwpStepSize size, JDWP::JdwpStepDepth depth);
  static void UnconfigureStep(JDWP::ObjectId threadId);

  static JDWP::JdwpError InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId, JDWP::RefTypeId classId, JDWP::MethodId methodId, uint32_t arg_count, uint64_t* arg_values, JDWP::JdwpTag* arg_types, uint32_t options, JDWP::JdwpTag* pResultTag, uint64_t* pResultValue, JDWP::ObjectId* pExceptObj);
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
  static void DdmSendChunkV(uint32_t type, const struct iovec* iov, int iov_count);

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

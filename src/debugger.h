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
class Thread;

/*
 * Invoke-during-breakpoint support.
 */
struct DebugInvokeReq {
  DebugInvokeReq()
      : ready(false), invoke_needed_(false),
        receiver_(NULL), thread_(NULL), class_(NULL), method_(NULL),
        arg_count_(0), arg_values_(NULL), options_(0), error(JDWP::ERR_NONE),
        result_tag(JDWP::JT_VOID), exception(0),
        lock_("a DebugInvokeReq lock", kBreakpointInvokeLock),
        cond_("a DebugInvokeReq condition variable", lock_) {
  }

  /* boolean; only set when we're in the tail end of an event handler */
  bool ready;

  /* boolean; set if the JDWP thread wants this thread to do work */
  bool invoke_needed_;

  /* request */
  Object* receiver_;      /* not used for ClassType.InvokeMethod */
  Object* thread_;
  Class* class_;
  AbstractMethod* method_;
  uint32_t arg_count_;
  uint64_t* arg_values_;   /* will be NULL if arg_count_ == 0 */
  uint32_t options_;

  /* result */
  JDWP::JdwpError error;
  JDWP::JdwpTag result_tag;
  JValue result_value;
  JDWP::ObjectId exception;

  /* condition variable to wait on while the method executes */
  Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ConditionVariable cond_ GUARDED_BY(lock_);
};

class Dbg {
 public:
  static bool ParseJdwpOptions(const std::string& options);
  static void SetJdwpAllowed(bool allowed);

  static void StartJdwp();
  static void StopJdwp();

  // Invoked by the GC in case we need to keep DDMS informed.
  static void GcDidFinish() LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Return the DebugInvokeReq for the current thread.
  static DebugInvokeReq* GetInvokeReq();

  static Thread* GetDebugThread();
  static void ClearWaitForEventThread();

  /*
   * Enable/disable breakpoints and step modes.  Used to provide a heads-up
   * when the debugger attaches.
   */
  static void Connected();
  static void GoActive() LOCKS_EXCLUDED(Locks::breakpoint_lock_);
  static void Disconnected();
  static void Disposed();

  // Returns true if we're actually debugging with a real debugger, false if it's
  // just DDMS (or nothing at all).
  static bool IsDebuggerActive();

  // Returns true if we had -Xrunjdwp or -agentlib:jdwp= on the command line.
  static bool IsJdwpConfigured();

  static bool IsDisposed();

  /*
   * Time, in milliseconds, since the last debugger activity.  Does not
   * include DDMS activity.  Returns -1 if there has been no activity.
   * Returns 0 if we're in the middle of handling a debugger request.
   */
  static int64_t LastDebuggerActivity();

  static void UndoDebuggerSuspensions();

  static void Exit(int status);

  static void VisitRoots(Heap::RootVisitor* visitor, void* arg);

  /*
   * Class, Object, Array
   */
  static std::string GetClassName(JDWP::RefTypeId id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetClassObject(JDWP::RefTypeId id, JDWP::ObjectId& classObjectId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetSuperclass(JDWP::RefTypeId id, JDWP::RefTypeId& superclassId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetClassLoader(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetModifiers(JDWP::RefTypeId id, JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetReflectedType(JDWP::RefTypeId classId, JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void GetClassList(std::vector<JDWP::RefTypeId>& classes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetClassInfo(JDWP::RefTypeId classId, JDWP::JdwpTypeTag* pTypeTag,
                                      uint32_t* pStatus, std::string* pDescriptor)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void FindLoadedClassBySignature(const char* descriptor, std::vector<JDWP::RefTypeId>& ids)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetReferenceType(JDWP::ObjectId objectId, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError GetSignature(JDWP::RefTypeId refTypeId, std::string& signature)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetSourceFile(JDWP::RefTypeId refTypeId, std::string& source_file)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetObjectTag(JDWP::ObjectId objectId, uint8_t& tag)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static size_t GetTagWidth(JDWP::JdwpTag tag);

  static JDWP::JdwpError GetArrayLength(JDWP::ObjectId arrayId, int& length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError OutputArray(JDWP::ObjectId arrayId, int firstIndex, int count,
                                     JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError SetArrayElements(JDWP::ObjectId arrayId, int firstIndex, int count,
                                          const uint8_t* buf)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static JDWP::ObjectId CreateString(const std::string& str)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError CreateObject(JDWP::RefTypeId classId, JDWP::ObjectId& new_object)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError CreateArrayObject(JDWP::RefTypeId arrayTypeId, uint32_t length,
                                           JDWP::ObjectId& new_array)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static bool MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /*
   * Method and Field
   */
  static std::string GetMethodName(JDWP::RefTypeId refTypeId, JDWP::MethodId id)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError OutputDeclaredFields(JDWP::RefTypeId refTypeId, bool withGeneric,
                                              JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError OutputDeclaredMethods(JDWP::RefTypeId refTypeId, bool withGeneric,
                                               JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError OutputDeclaredInterfaces(JDWP::RefTypeId refTypeId,
                                                  JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void OutputLineTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId,
                              JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void OutputVariableTable(JDWP::RefTypeId refTypeId, JDWP::MethodId id, bool withGeneric,
                                  JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static JDWP::JdwpTag GetFieldBasicTag(JDWP::FieldId fieldId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpTag GetStaticFieldBasicTag(JDWP::FieldId fieldId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);;
  static JDWP::JdwpError GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId,
                                       JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId,
                                       uint64_t value, int width)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError GetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId,
                                             JDWP::ExpandBuf* pReply)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError SetStaticFieldValue(JDWP::FieldId fieldId, uint64_t value, int width)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static std::string StringToUtf8(JDWP::ObjectId strId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /*
   * Thread, ThreadGroup, Frame
   */
  static JDWP::JdwpError GetThreadName(JDWP::ObjectId threadId, std::string& name)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      LOCKS_EXCLUDED(Locks::thread_list_lock_);
  static JDWP::JdwpError GetThreadGroup(JDWP::ObjectId threadId, JDWP::ExpandBuf* pReply);
  static std::string GetThreadGroupName(JDWP::ObjectId threadGroupId);
  static JDWP::ObjectId GetThreadGroupParent(JDWP::ObjectId threadGroupId)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::ObjectId GetSystemThreadGroupId()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::ObjectId GetMainThreadGroupId();

  static JDWP::JdwpError GetThreadStatus(JDWP::ObjectId threadId, JDWP::JdwpThreadStatus* pThreadStatus, JDWP::JdwpSuspendStatus* pSuspendStatus);
  static JDWP::JdwpError GetThreadDebugSuspendCount(JDWP::ObjectId threadId, JDWP::ExpandBuf* pReply);
  static JDWP::JdwpError IsSuspended(JDWP::ObjectId threadId, bool& result);
  //static void WaitForSuspend(JDWP::ObjectId threadId);

  // Fills 'thread_ids' with the threads in the given thread group. If thread_group_id == 0,
  // returns all threads.
  static void GetThreads(JDWP::ObjectId thread_group_id, std::vector<JDWP::ObjectId>& thread_ids)
      LOCKS_EXCLUDED(Locks::thread_list_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void GetChildThreadGroups(JDWP::ObjectId thread_group_id, std::vector<JDWP::ObjectId>& child_thread_group_ids);

  static JDWP::JdwpError GetThreadFrameCount(JDWP::ObjectId threadId, size_t& result);
  static JDWP::JdwpError GetThreadFrames(JDWP::ObjectId thread_id, size_t start_frame,
                                         size_t frame_count, JDWP::ExpandBuf* buf)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static JDWP::ObjectId GetThreadSelfId()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void SuspendVM()
      LOCKS_EXCLUDED(Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_);
  static void ResumeVM();
  static JDWP::JdwpError SuspendThread(JDWP::ObjectId threadId, bool request_suspension = true)
      LOCKS_EXCLUDED(Locks::mutator_lock_,
                     Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_);

  static void ResumeThread(JDWP::ObjectId threadId)
      LOCKS_EXCLUDED(Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void SuspendSelf();

  static JDWP::JdwpError GetThisObject(JDWP::ObjectId thread_id, JDWP::FrameId frame_id,
                                       JDWP::ObjectId* result)
      LOCKS_EXCLUDED(Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void GetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot,
                            JDWP::JdwpTag tag, uint8_t* buf, size_t expectedLen)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void SetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot,
                            JDWP::JdwpTag tag, uint64_t value, size_t width)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /*
   * Debugger notification
   */
  enum {
    kBreakpoint     = 0x01,
    kSingleStep     = 0x02,
    kMethodEntry    = 0x04,
    kMethodExit     = 0x08,
  };
  static void PostLocationEvent(const AbstractMethod* method, int pcOffset, Object* thisPtr, int eventFlags)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void PostException(Thread* thread, JDWP::FrameId throw_frame_id, AbstractMethod* throw_method,
                            uint32_t throw_dex_pc, AbstractMethod* catch_method, uint32_t catch_dex_pc,
                            Throwable* exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void PostThreadStart(Thread* t)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void PostThreadDeath(Thread* t)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void PostClassPrepare(Class* c)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void UpdateDebugger(int32_t dex_pc, Thread* self)
      LOCKS_EXCLUDED(Locks::breakpoint_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void WatchLocation(const JDWP::JdwpLocation* pLoc)
      LOCKS_EXCLUDED(Locks::breakpoint_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void UnwatchLocation(const JDWP::JdwpLocation* pLoc)
      LOCKS_EXCLUDED(Locks::breakpoint_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static JDWP::JdwpError ConfigureStep(JDWP::ObjectId threadId, JDWP::JdwpStepSize size,
                                       JDWP::JdwpStepDepth depth)
      LOCKS_EXCLUDED(Locks::breakpoint_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void UnconfigureStep(JDWP::ObjectId threadId) LOCKS_EXCLUDED(Locks::breakpoint_lock_);

  static JDWP::JdwpError InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId,
                                      JDWP::RefTypeId classId, JDWP::MethodId methodId,
                                      uint32_t arg_count, uint64_t* arg_values,
                                      JDWP::JdwpTag* arg_types, uint32_t options,
                                      JDWP::JdwpTag* pResultTag, uint64_t* pResultValue,
                                      JDWP::ObjectId* pExceptObj)
      LOCKS_EXCLUDED(Locks::thread_list_lock_,
                     Locks::thread_suspend_count_lock_)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void ExecuteMethod(DebugInvokeReq* pReq);

  /* perform "late registration" of an object ID */
  static void RegisterObjectId(JDWP::ObjectId id);

  /*
   * DDM support.
   */
  static void DdmSendThreadNotification(Thread* t, uint32_t type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DdmSetThreadNotification(bool enable);
  static bool DdmHandlePacket(const uint8_t* buf, int dataLen, uint8_t** pReplyBuf, int* pReplyLen);
  static void DdmConnected() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DdmDisconnected() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DdmSendChunk(uint32_t type, const std::vector<uint8_t>& bytes)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DdmSendChunk(uint32_t type, size_t len, const uint8_t* buf)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DdmSendChunkV(uint32_t type, const iovec* iov, int iov_count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /*
   * Recent allocation tracking support.
   */
  static void RecordAllocation(Class* type, size_t byte_count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void SetAllocTrackingEnabled(bool enabled);
  static inline bool IsAllocTrackingEnabled() { return recent_allocation_records_ != NULL; }
  static jbyteArray GetRecentAllocations()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DumpRecentAllocations();

  enum HpifWhen {
    HPIF_WHEN_NEVER = 0,
    HPIF_WHEN_NOW = 1,
    HPIF_WHEN_NEXT_GC = 2,
    HPIF_WHEN_EVERY_GC = 3
  };
  static int DdmHandleHpifChunk(HpifWhen when)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  enum HpsgWhen {
    HPSG_WHEN_NEVER = 0,
    HPSG_WHEN_EVERY_GC = 1,
  };
  enum HpsgWhat {
    HPSG_WHAT_MERGED_OBJECTS = 0,
    HPSG_WHAT_DISTINCT_OBJECTS = 1,
  };
  static bool DdmHandleHpsgNhsgChunk(HpsgWhen when, HpsgWhat what, bool native);

  static void DdmSendHeapInfo(HpifWhen reason)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void DdmSendHeapSegments(bool native)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  static void DdmBroadcast(bool) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static void PostThreadStartOrStop(Thread*, uint32_t)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static AllocRecord* recent_allocation_records_;
};

#define CHUNK_TYPE(_name) \
    static_cast<uint32_t>((_name)[0] << 24 | (_name)[1] << 16 | (_name)[2] << 8 | (_name)[3])

}  // namespace art

#endif  // ART_DEBUGGER_H_

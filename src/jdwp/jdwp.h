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

#ifndef ART_JDWP_JDWP_H_
#define ART_JDWP_JDWP_H_

#include "jdwp/jdwp_bits.h"
#include "jdwp/jdwp_constants.h"
#include "jdwp/jdwp_expand_buf.h"
#include "../mutex.h" // TODO: fix our include path!

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct iovec;

namespace art {

struct Method;
struct Thread;

namespace JDWP {

/*
 * Fundamental types.
 *
 * ObjectId and RefTypeId must be the same size.
 */
typedef uint32_t FieldId;     /* static or instance field */
typedef uint32_t MethodId;    /* any kind of method, including constructors */
typedef uint64_t ObjectId;    /* any object (threadID, stringID, arrayID, etc) */
typedef uint64_t RefTypeId;   /* like ObjectID, but unique for Class objects */
typedef uint64_t FrameId;     /* short-lived stack frame ID */

/*
 * Match these with the type sizes.  This way we don't have to pass
 * a value and a length.
 */
static inline FieldId ReadFieldId(const uint8_t** pBuf) { return Read4BE(pBuf); }
static inline MethodId ReadMethodId(const uint8_t** pBuf) { return Read4BE(pBuf); }
static inline ObjectId ReadObjectId(const uint8_t** pBuf) { return Read8BE(pBuf); }
static inline RefTypeId ReadRefTypeId(const uint8_t** pBuf) { return Read8BE(pBuf); }
static inline FrameId ReadFrameId(const uint8_t** pBuf) { return Read8BE(pBuf); }
static inline JdwpTag ReadTag(const uint8_t** pBuf) { return static_cast<JdwpTag>(Read1(pBuf)); }
static inline JdwpTypeTag ReadTypeTag(const uint8_t** pBuf) { return static_cast<JdwpTypeTag>(Read1(pBuf)); }
static inline void SetFieldId(uint8_t* buf, FieldId val) { return Set4BE(buf, val); }
static inline void SetMethodId(uint8_t* buf, MethodId val) { return Set4BE(buf, val); }
static inline void SetObjectId(uint8_t* buf, ObjectId val) { return Set8BE(buf, val); }
static inline void SetRefTypeId(uint8_t* buf, RefTypeId val) { return Set8BE(buf, val); }
static inline void SetFrameId(uint8_t* buf, FrameId val) { return Set8BE(buf, val); }
static inline void expandBufAddFieldId(ExpandBuf* pReply, FieldId id) { expandBufAdd4BE(pReply, id); }
static inline void expandBufAddMethodId(ExpandBuf* pReply, MethodId id) { expandBufAdd4BE(pReply, id); }
static inline void expandBufAddObjectId(ExpandBuf* pReply, ObjectId id) { expandBufAdd8BE(pReply, id); }
static inline void expandBufAddRefTypeId(ExpandBuf* pReply, RefTypeId id) { expandBufAdd8BE(pReply, id); }
static inline void expandBufAddFrameId(ExpandBuf* pReply, FrameId id) { expandBufAdd8BE(pReply, id); }

/*
 * Holds a JDWP "location".
 */
struct JdwpLocation {
  JdwpTypeTag typeTag;
  RefTypeId classId;
  MethodId methodId;
  uint64_t idx; // A Dex PC.
};
std::ostream& operator<<(std::ostream& os, const JdwpLocation& rhs);

/*
 * How we talk to the debugger.
 */
enum JdwpTransportType {
  kJdwpTransportUnknown = 0,
  kJdwpTransportSocket,       /* transport=dt_socket */
  kJdwpTransportAndroidAdb,   /* transport=dt_android_adb */
};
std::ostream& operator<<(std::ostream& os, const JdwpTransportType& rhs);

struct JdwpOptions {
  JdwpTransportType transport;
  bool server;
  bool suspend;
  std::string host;
  uint16_t port;
};

struct JdwpEvent;
struct JdwpNetState;
struct JdwpReqHeader;
struct JdwpTransport;
struct ModBasket;

/*
 * State for JDWP functions.
 */
struct JdwpState {
  /*
   * Perform one-time initialization.
   *
   * Among other things, this binds to a port to listen for a connection from
   * the debugger.
   *
   * Returns a newly-allocated JdwpState struct on success, or NULL on failure.
   */
  static JdwpState* Create(const JdwpOptions* options);

  ~JdwpState();

  /*
   * Returns "true" if a debugger or DDM is connected.
   */
  bool IsActive();

  /**
   * Returns the Thread* for the JDWP daemon thread.
   */
  Thread* GetDebugThread();

  /*
   * Get time, in milliseconds, since the last debugger activity.
   */
  int64_t LastDebuggerActivity();

  /*
   * When we hit a debugger event that requires suspension, it's important
   * that we wait for the thread to suspend itself before processing any
   * additional requests.  (Otherwise, if the debugger immediately sends a
   * "resume thread" command, the resume might arrive before the thread has
   * suspended itself.)
   *
   * The thread should call the "set" function before sending the event to
   * the debugger.  The main JDWP handler loop calls "get" before processing
   * an event, and will wait for thread suspension if it's set.  Once the
   * thread has suspended itself, the JDWP handler calls "clear" and
   * continues processing the current event.  This works in the suspend-all
   * case because the event thread doesn't suspend itself until everything
   * else has suspended.
   *
   * It's possible that multiple threads could encounter thread-suspending
   * events at the same time, so we grab a mutex in the "set" call, and
   * release it in the "clear" call.
   */
  //ObjectId GetWaitForEventThread();
  void SetWaitForEventThread(ObjectId threadId);
  void ClearWaitForEventThread();

  /*
   * These notify the debug code that something interesting has happened.  This
   * could be a thread starting or ending, an exception, or an opportunity
   * for a breakpoint.  These calls do not mean that an event the debugger
   * is interested has happened, just that something has happened that the
   * debugger *might* be interested in.
   *
   * The item of interest may trigger multiple events, some or all of which
   * are grouped together in a single response.
   *
   * The event may cause the current thread or all threads (except the
   * JDWP support thread) to be suspended.
   */

  /*
   * The VM has finished initializing.  Only called when the debugger is
   * connected at the time initialization completes.
   */
  bool PostVMStart();

  /*
   * A location of interest has been reached.  This is used for breakpoints,
   * single-stepping, and method entry/exit.  (JDWP requires that these four
   * events are grouped together in a single response.)
   *
   * In some cases "*pLoc" will just have a method and class name, e.g. when
   * issuing a MethodEntry on a native method.
   *
   * "eventFlags" indicates the types of events that have occurred.
   */
  bool PostLocationEvent(const JdwpLocation* pLoc, ObjectId thisPtr, int eventFlags);

  /*
   * An exception has been thrown.
   *
   * Pass in a zeroed-out "*pCatchLoc" if the exception wasn't caught.
   */
  bool PostException(const JdwpLocation* pThrowLoc, ObjectId excepId, RefTypeId excepClassId, const JdwpLocation* pCatchLoc, ObjectId thisPtr);

  /*
   * A thread has started or stopped.
   */
  bool PostThreadChange(ObjectId threadId, bool start);

  /*
   * Class has been prepared.
   */
  bool PostClassPrepare(JdwpTypeTag tag, RefTypeId refTypeId, const std::string& signature, int status);

  /*
   * The VM is about to stop.
   */
  bool PostVMDeath();

  /*
   * Send up a chunk of DDM data.
   */
  void DdmSendChunkV(uint32_t type, const iovec* iov, int iov_count);

  /*
   * Process a request from the debugger.
   *
   * "buf" points past the header, to the content of the message.  "dataLen"
   * can therefore be zero.
   */
  void ProcessRequest(const JdwpReqHeader* pHeader, const uint8_t* buf, int dataLen, ExpandBuf* pReply);

  /*
   * Send an event, formatted into "pReq", to the debugger.
   *
   * (Messages are sent asynchronously, and do not receive a reply.)
   */
  bool SendRequest(ExpandBuf* pReq);

  void ResetState();

  /* atomic ops to get next serial number */
  uint32_t NextRequestSerial();
  uint32_t NextEventSerial();

  void Run();

  /*
   * Register an event by adding it to the event list.
   *
   * "*pEvent" must be storage allocated with jdwpEventAlloc().  The caller
   * may discard its pointer after calling this.
   */
  JdwpError RegisterEvent(JdwpEvent* pEvent);

  /*
   * Unregister an event, given the requestId.
   */
  void UnregisterEventById(uint32_t requestId);

  /*
   * Unregister all events.
   */
  void UnregisterAll();

 private:
  explicit JdwpState(const JdwpOptions* options);
  bool InvokeInProgress();
  bool IsConnected();
  void SuspendByPolicy(JdwpSuspendPolicy suspendPolicy);
  void CleanupMatchList(JdwpEvent** matchList, int matchCount);
  void EventFinish(ExpandBuf* pReq);
  void FindMatchingEvents(JdwpEventKind eventKind, ModBasket* basket, JdwpEvent** matchList, int* pMatchCount);
  void UnregisterEvent(JdwpEvent* pEvent);

public: // TODO: fix privacy
  const JdwpOptions* options_;
private:

  /* wait for creation of the JDWP thread */
  Mutex thread_start_lock_;
  ConditionVariable thread_start_cond_;

  volatile int32_t debug_thread_started_;
  pthread_t pthread_;
  Thread* thread_;
public: // TODO: fix privacy
  ObjectId debugThreadId;
private:
  bool run;

  const JdwpTransport* transport;
public: // TODO: fix privacy
  JdwpNetState* netState;
private:

  /* for wait-for-debugger */
  Mutex attach_lock_;
  ConditionVariable attach_cond_;

  /* time of last debugger activity, in milliseconds */
  int64_t lastActivityWhen;

  /* global counters and a mutex to protect them */
  uint32_t requestSerial;
  uint32_t eventSerial;
  Mutex serial_lock_;

  /*
   * Events requested by the debugger (breakpoints, class prep, etc).
   */
public: // TODO: fix privacy
  int numEvents;      /* #of elements in eventList */
  JdwpEvent* eventList;      /* linked list of events */
  Mutex event_lock_;      /* guards numEvents/eventList */
private:

  /*
   * Synchronize suspension of event thread (to avoid receiving "resume"
   * events before the thread has finished suspending itself).
   */
  Mutex event_thread_lock_;
  ConditionVariable event_thread_cond_;
  ObjectId eventThreadId;

  /*
   * DDM support.
   */
public: // TODO: fix privacy
  bool ddmActive;
private:
};

}  // namespace JDWP

}  // namespace art

#endif  // ART_JDWP_JDWP_H_

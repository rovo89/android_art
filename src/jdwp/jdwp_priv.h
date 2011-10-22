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
 * JDWP internal interfaces.
 */
#ifndef ART_JDWP_JDWPPRIV_H_
#define ART_JDWP_JDWPPRIV_H_

#define LOG_TAG "jdwp"

#include "debugger.h"
#include "jdwp/jdwp.h"
#include "jdwp/jdwp_event.h"
#include "../mutex.h" // TODO: fix our include path!

#include <pthread.h>
#include <sys/uio.h>

/*
 * JDWP constants.
 */
#define kJDWPHeaderLen  11
#define kJDWPFlagReply  0x80

/* DDM support */
#define kJDWPDdmCmdSet  199     /* 0xc7, or 'G'+128 */
#define kJDWPDdmCmd     1

namespace art {

namespace JDWP {

/*
 * Transport-specific network status.
 */
struct JdwpNetState;
struct JdwpState;

/*
 * Transport functions.
 */
struct JdwpTransport {
  bool (*startup)(JdwpState* state, const JdwpStartupParams* pParams);
  bool (*accept)(JdwpState* state);
  bool (*establish)(JdwpState* state);
  void (*close)(JdwpState* state);
  void (*shutdown)(JdwpState* state);
  void (*free)(JdwpState* state);
  bool (*isConnected)(JdwpState* state);
  bool (*awaitingHandshake)(JdwpState* state);
  bool (*processIncoming)(JdwpState* state);
  bool (*sendRequest)(JdwpState* state, ExpandBuf* pReq);
  bool (*sendBufferedRequest)(JdwpState* state, const iovec* iov, int iovcnt);
};

const JdwpTransport* SocketTransport();
const JdwpTransport* AndroidAdbTransport();


/*
 * State for JDWP functions.
 */
struct JdwpState {
  JdwpState();

  JdwpStartupParams params;

  /* wait for creation of the JDWP thread */
  Mutex thread_start_lock_;
  ConditionVariable thread_start_cond_;

  volatile int32_t debug_thread_started_;
  pthread_t debugThreadHandle;
  ObjectId debugThreadId;
  bool run;

  const JdwpTransport* transport;
  JdwpNetState* netState;

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
  int numEvents;      /* #of elements in eventList */
  JdwpEvent* eventList;      /* linked list of events */
  Mutex event_lock_;      /* guards numEvents/eventList */

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
  bool ddmActive;
};

/*
 * Base class for JdwpNetState
 */
class JdwpNetStateBase {
public:
  int clientSock;     /* active connection to debugger */

  JdwpNetStateBase();
  ssize_t writePacket(ExpandBuf* pReply);
  ssize_t writeBufferedPacket(const iovec* iov, int iovcnt);

private:
  Mutex socket_lock_;
};


/* reset all session-specific data */
void ResetState(JdwpState* state);

/* atomic ops to get next serial number */
uint32_t NextRequestSerial(JdwpState* state);
uint32_t NextEventSerial(JdwpState* state);

/* get current time, in msec */
int64_t GetNowMsec();

}  // namespace JDWP

}  // namespace art

#endif  // ART_JDWP_JDWPPRIV_H_

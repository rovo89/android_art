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
 * JDWP initialization.
 */

#include "atomic.h"
#include "debugger.h"
#include "jdwp/jdwp_priv.h"
#include "logging.h"
#include "scoped_thread_state_change.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

namespace art {

namespace JDWP {

static void* StartJdwpThread(void* arg);

/*
 * JdwpNetStateBase class implementation
 */
JdwpNetStateBase::JdwpNetStateBase() : socket_lock_("JdwpNetStateBase lock") {
  clientSock = -1;
}

/*
 * Write a packet. Grabs a mutex to assure atomicity.
 */
ssize_t JdwpNetStateBase::writePacket(ExpandBuf* pReply) {
  MutexLock mu(socket_lock_);
  return write(clientSock, expandBufGetBuffer(pReply), expandBufGetLength(pReply));
}

/*
 * Write a buffered packet. Grabs a mutex to assure atomicity.
 */
ssize_t JdwpNetStateBase::writeBufferedPacket(const iovec* iov, int iov_count) {
  MutexLock mu(socket_lock_);
  return writev(clientSock, iov, iov_count);
}

bool JdwpState::IsConnected() {
  return (*transport_->isConnected)(this);
}

bool JdwpState::SendRequest(ExpandBuf* pReq) {
  return (*transport_->sendRequest)(this, pReq);
}

/*
 * Get the next "request" serial number.  We use this when sending
 * packets to the debugger.
 */
uint32_t JdwpState::NextRequestSerial() {
  MutexLock mu(serial_lock_);
  return request_serial_++;
}

/*
 * Get the next "event" serial number.  We use this in the response to
 * message type EventRequest.Set.
 */
uint32_t JdwpState::NextEventSerial() {
  MutexLock mu(serial_lock_);
  return event_serial_++;
}

JdwpState::JdwpState(const JdwpOptions* options)
    : options_(options),
      thread_start_lock_("JDWP thread start lock"),
      thread_start_cond_("JDWP thread start condition variable"),
      pthread_(0),
      thread_(NULL),
      debug_thread_started_(false),
      debug_thread_id_(0),
      run(false),
      transport_(NULL),
      netState(NULL),
      attach_lock_("JDWP attach lock"),
      attach_cond_("JDWP attach condition variable"),
      last_activity_time_ms_(0),
      serial_lock_("JDWP serial lock", kJdwpSerialLock),
      request_serial_(0x10000000),
      event_serial_(0x20000000),
      event_list_lock_("JDWP event list lock"),
      event_list_(NULL),
      event_list_size_(0),
      event_thread_lock_("JDWP event thread lock"),
      event_thread_cond_("JDWP event thread condition variable"),
      event_thread_id_(0),
      ddm_is_active_(false) {
}

/*
 * Initialize JDWP.
 *
 * Does not return until JDWP thread is running, but may return before
 * the thread is accepting network connections.
 */
JdwpState* JdwpState::Create(const JdwpOptions* options) {
  Locks::mutator_lock_->AssertNotHeld();
  UniquePtr<JdwpState> state(new JdwpState(options));
  switch (options->transport) {
  case kJdwpTransportSocket:
    // LOGD("prepping for JDWP over TCP");
    state->transport_ = SocketTransport();
    break;
#ifdef HAVE_ANDROID_OS
  case kJdwpTransportAndroidAdb:
    // LOGD("prepping for JDWP over ADB");
    state->transport_ = AndroidAdbTransport();
    break;
#endif
  default:
    LOG(FATAL) << "Unknown transport: " << options->transport;
  }

  if (!(*state->transport_->startup)(state.get(), options)) {
    return NULL;
  }

  /*
   * Grab a mutex or two before starting the thread.  This ensures they
   * won't signal the cond var before we're waiting.
   */
  {
    MutexLock thread_start_locker(state->thread_start_lock_);
    const bool should_suspend = options->suspend;
    if (!should_suspend) {
      /*
       * We have bound to a port, or are trying to connect outbound to a
       * debugger.  Create the JDWP thread and let it continue the mission.
       */
      CHECK_PTHREAD_CALL(pthread_create, (&state->pthread_, NULL, StartJdwpThread, state.get()), "JDWP thread");

      /*
       * Wait until the thread finishes basic initialization.
       * TODO: cond vars should be waited upon in a loop
       */
      state->thread_start_cond_.Wait(state->thread_start_lock_);
    } else {
      {
        MutexLock attach_locker(state->attach_lock_);
        /*
         * We have bound to a port, or are trying to connect outbound to a
         * debugger.  Create the JDWP thread and let it continue the mission.
         */
        CHECK_PTHREAD_CALL(pthread_create, (&state->pthread_, NULL, StartJdwpThread, state.get()), "JDWP thread");

        /*
         * Wait until the thread finishes basic initialization.
         * TODO: cond vars should be waited upon in a loop
         */
        state->thread_start_cond_.Wait(state->thread_start_lock_);

        /*
         * For suspend=y, wait for the debugger to connect to us or for us to
         * connect to the debugger.
         *
         * The JDWP thread will signal us when it connects successfully or
         * times out (for timeout=xxx), so we have to check to see what happened
         * when we wake up.
         */
        {
          ScopedThreadStateChange tsc(Thread::Current(), kWaitingForDebuggerToAttach);
          state->attach_cond_.Wait(state->attach_lock_);
        }
      }
      if (!state->IsActive()) {
        LOG(ERROR) << "JDWP connection failed";
        return NULL;
      }

      LOG(INFO) << "JDWP connected";

      /*
       * Ordinarily we would pause briefly to allow the debugger to set
       * breakpoints and so on, but for "suspend=y" the VM init code will
       * pause the VM when it sends the VM_START message.
       */
    }
  }

  return state.release();
}

/*
 * Reset all session-related state.  There should not be an active connection
 * to the client at this point.  The rest of the VM still thinks there is
 * a debugger attached.
 *
 * This includes freeing up the debugger event list.
 */
void JdwpState::ResetState() {
  /* could reset the serial numbers, but no need to */

  UnregisterAll();
  {
    MutexLock mu(event_list_lock_);
    CHECK(event_list_ == NULL);
  }

  /*
   * Should not have one of these in progress.  If the debugger went away
   * mid-request, though, we could see this.
   */
  if (event_thread_id_ != 0) {
    LOG(WARNING) << "Resetting state while event in progress";
    DCHECK(false);
  }
}

/*
 * Tell the JDWP thread to shut down.  Frees "state".
 */
JdwpState::~JdwpState() {
  if (transport_ != NULL) {
    if (IsConnected()) {
      PostVMDeath();
    }

    /*
     * Close down the network to inspire the thread to halt.
     */
    VLOG(jdwp) << "JDWP shutting down net...";
    (*transport_->shutdown)(this);

    if (debug_thread_started_) {
      run = false;
      void* threadReturn;
      if (pthread_join(pthread_, &threadReturn) != 0) {
        LOG(WARNING) << "JDWP thread join failed";
      }
    }

    VLOG(jdwp) << "JDWP freeing netstate...";
    (*transport_->free)(this);
    netState = NULL;
  }
  CHECK(netState == NULL);

  ResetState();
}

/*
 * Are we talking to a debugger?
 */
bool JdwpState::IsActive() {
  return IsConnected();
}

/*
 * Entry point for JDWP thread.  The thread was created through the VM
 * mechanisms, so there is a java/lang/Thread associated with us.
 */
static void* StartJdwpThread(void* arg) {
  JdwpState* state = reinterpret_cast<JdwpState*>(arg);
  CHECK(state != NULL);

  state->Run();
  return NULL;
}

void JdwpState::Run() {
  Runtime* runtime = Runtime::Current();
  runtime->AttachCurrentThread("JDWP", true, runtime->GetSystemThreadGroup());

  VLOG(jdwp) << "JDWP: thread running";

  /*
   * Finish initializing, then notify the creating thread that
   * we're running.
   */
  thread_ = Thread::Current();
  run = true;

  thread_start_lock_.Lock();
  debug_thread_started_ = true;
  thread_start_cond_.Broadcast();
  thread_start_lock_.Unlock();

  /* set the thread state to kWaitingInMainDebuggerLoop so GCs don't wait for us */
  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    CHECK_EQ(thread_->GetState(), kNative);
    thread_->SetState(kWaitingInMainDebuggerLoop);
  }

  /*
   * Loop forever if we're in server mode, processing connections.  In
   * non-server mode, we bail out of the thread when the debugger drops
   * us.
   *
   * We broadcast a notification when a debugger attaches, after we
   * successfully process the handshake.
   */
  while (run) {
    if (options_->server) {
      /*
       * Block forever, waiting for a connection.  To support the
       * "timeout=xxx" option we'll need to tweak this.
       */
      if (!(*transport_->accept)(this)) {
        break;
      }
    } else {
      /*
       * If we're not acting as a server, we need to connect out to the
       * debugger.  To support the "timeout=xxx" option we need to
       * have a timeout if the handshake reply isn't received in a
       * reasonable amount of time.
       */
      if (!(*transport_->establish)(this, options_)) {
        /* wake anybody who was waiting for us to succeed */
        MutexLock mu(attach_lock_);
        attach_cond_.Broadcast();
        break;
      }
    }

    /* prep debug code to handle the new connection */
    Dbg::Connected();

    /* process requests until the debugger drops */
    bool first = true;
    while (!Dbg::IsDisposed()) {
      {
        // sanity check -- shouldn't happen?
        MutexLock mu(*Locks::thread_suspend_count_lock_);
        CHECK_EQ(thread_->GetState(), kWaitingInMainDebuggerLoop);
      }

      if (!(*transport_->processIncoming)(this)) {
        /* blocking read */
        break;
      }

      if (first && !(*transport_->awaitingHandshake)(this)) {
        /* handshake worked, tell the interpreter that we're active */
        first = false;

        /* set thread ID; requires object registry to be active */
        {
          ScopedObjectAccess soa(thread_);
          debug_thread_id_ = Dbg::GetThreadSelfId();
        }

        /* wake anybody who's waiting for us */
        MutexLock mu(attach_lock_);
        attach_cond_.Broadcast();
      }
    }

    (*transport_->close)(this);

    if (ddm_is_active_) {
      ddm_is_active_ = false;

      /* broadcast the disconnect; must be in RUNNING state */
      thread_->TransitionFromSuspendedToRunnable();
      Dbg::DdmDisconnected();
      thread_->TransitionFromRunnableToSuspended(kWaitingInMainDebuggerLoop);
    }

    /* release session state, e.g. remove breakpoint instructions */
    {
      ScopedObjectAccess soa(thread_);
      ResetState();
    }
    /* tell the interpreter that the debugger is no longer around */
    Dbg::Disconnected();

    /* if we had threads suspended, resume them now */
    Dbg::UndoDebuggerSuspensions();

    /* if we connected out, this was a one-shot deal */
    if (!options_->server) {
      run = false;
    }
  }

  /* back to native, for thread shutdown */
  {
    MutexLock mu(*Locks::thread_suspend_count_lock_);
    CHECK_EQ(thread_->GetState(), kWaitingInMainDebuggerLoop);
    thread_->SetState(kNative);
  }

  VLOG(jdwp) << "JDWP: thread detaching and exiting...";
  runtime->DetachCurrentThread();
}

void JdwpState::NotifyDdmsActive() {
  if (!ddm_is_active_) {
    ddm_is_active_ = true;
    Dbg::DdmConnected();
  }
}

Thread* JdwpState::GetDebugThread() {
  return thread_;
}

/*
 * Support routines for waitForDebugger().
 *
 * We can't have a trivial "waitForDebugger" function that returns the
 * instant the debugger connects, because we run the risk of executing code
 * before the debugger has had a chance to configure breakpoints or issue
 * suspend calls.  It would be nice to just sit in the suspended state, but
 * most debuggers don't expect any threads to be suspended when they attach.
 *
 * There's no JDWP event we can post to tell the debugger, "we've stopped,
 * and we like it that way".  We could send a fake breakpoint, which should
 * cause the debugger to immediately send a resume, but the debugger might
 * send the resume immediately or might throw an exception of its own upon
 * receiving a breakpoint event that it didn't ask for.
 *
 * What we really want is a "wait until the debugger is done configuring
 * stuff" event.  We can approximate this with a "wait until the debugger
 * has been idle for a brief period".
 */

/*
 * Return the time, in milliseconds, since the last debugger activity.
 *
 * Returns -1 if no debugger is attached, or 0 if we're in the middle of
 * processing a debugger request.
 */
int64_t JdwpState::LastDebuggerActivity() {
  if (!Dbg::IsDebuggerActive()) {
    LOG(DEBUG) << "no active debugger";
    return -1;
  }

  int64_t last = QuasiAtomic::Read64(&last_activity_time_ms_);

  /* initializing or in the middle of something? */
  if (last == 0) {
    VLOG(jdwp) << "+++ last=busy";
    return 0;
  }

  /* now get the current time */
  int64_t now = MilliTime();
  CHECK_GE(now, last);

  VLOG(jdwp) << "+++ debugger interval=" << (now - last);
  return now - last;
}

std::ostream& operator<<(std::ostream& os, const JdwpLocation& rhs) {
  os << "JdwpLocation["
     << Dbg::GetClassName(rhs.class_id) << "." << Dbg::GetMethodName(rhs.class_id, rhs.method_id)
     << "@" << StringPrintf("%#llx", rhs.dex_pc) << " " << rhs.type_tag << "]";
  return os;
}

bool operator==(const JdwpLocation& lhs, const JdwpLocation& rhs) {
  return lhs.dex_pc == rhs.dex_pc && lhs.method_id == rhs.method_id &&
      lhs.class_id == rhs.class_id && lhs.type_tag == rhs.type_tag;
}

bool operator!=(const JdwpLocation& lhs, const JdwpLocation& rhs) {
  return !(lhs == rhs);
}

}  // namespace JDWP

}  // namespace art

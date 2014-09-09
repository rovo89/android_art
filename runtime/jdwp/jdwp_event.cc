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

#include "jdwp/jdwp_event.h"

#include <stddef.h>     /* for offsetof() */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/logging.h"
#include "base/stringprintf.h"
#include "debugger.h"
#include "jdwp/jdwp_constants.h"
#include "jdwp/jdwp_expand_buf.h"
#include "jdwp/jdwp_priv.h"
#include "jdwp/object_registry.h"
#include "mirror/art_field-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"

/*
General notes:

The event add/remove stuff usually happens from the debugger thread,
in response to requests from the debugger, but can also happen as the
result of an event in an arbitrary thread (e.g. an event with a "count"
mod expires).  It's important to keep the event list locked when processing
events.

Event posting can happen from any thread.  The JDWP thread will not usually
post anything but VM start/death, but if a JDWP request causes a class
to be loaded, the ClassPrepare event will come from the JDWP thread.


We can have serialization issues when we post an event to the debugger.
For example, a thread could send an "I hit a breakpoint and am suspending
myself" message to the debugger.  Before it manages to suspend itself, the
debugger's response ("not interested, resume thread") arrives and is
processed.  We try to resume a thread that hasn't yet suspended.

This means that, after posting an event to the debugger, we need to wait
for the event thread to suspend itself (and, potentially, all other threads)
before processing any additional requests from the debugger.  While doing
so we need to be aware that multiple threads may be hitting breakpoints
or other events simultaneously, so we either need to wait for all of them
or serialize the events with each other.

The current mechanism works like this:
  Event thread:
   - If I'm going to suspend, grab the "I am posting an event" token.  Wait
     for it if it's not currently available.
   - Post the event to the debugger.
   - If appropriate, suspend others and then myself.  As part of suspending
     myself, release the "I am posting" token.
  JDWP thread:
   - When an event arrives, see if somebody is posting an event.  If so,
     sleep until we can acquire the "I am posting an event" token.  Release
     it immediately and continue processing -- the event we have already
     received should not interfere with other events that haven't yet
     been posted.

Some care must be taken to avoid deadlock:

 - thread A and thread B exit near-simultaneously, and post thread-death
   events with a "suspend all" clause
 - thread A gets the event token, thread B sits and waits for it
 - thread A wants to suspend all other threads, but thread B is waiting
   for the token and can't be suspended

So we need to mark thread B in such a way that thread A doesn't wait for it.

If we just bracket the "grab event token" call with a change to VMWAIT
before sleeping, the switch back to RUNNING state when we get the token
will cause thread B to suspend (remember, thread A's global suspend is
still in force, even after it releases the token).  Suspending while
holding the event token is very bad, because it prevents the JDWP thread
from processing incoming messages.

We need to change to VMWAIT state at the *start* of posting an event,
and stay there until we either finish posting the event or decide to
put ourselves to sleep.  That way we don't interfere with anyone else and
don't allow anyone else to interfere with us.
*/


#define kJdwpEventCommandSet    64
#define kJdwpCompositeCommand   100

namespace art {

namespace JDWP {

/*
 * Stuff to compare against when deciding if a mod matches.  Only the
 * values for mods valid for the event being evaluated will be filled in.
 * The rest will be zeroed.
 */
struct ModBasket {
  ModBasket() : pLoc(nullptr), thread(nullptr), locationClass(nullptr), exceptionClass(nullptr),
                caught(false), field(nullptr), thisPtr(nullptr) { }

  const EventLocation*  pLoc;             /* LocationOnly */
  std::string           className;        /* ClassMatch/ClassExclude */
  Thread*               thread;           /* ThreadOnly */
  mirror::Class*        locationClass;    /* ClassOnly */
  mirror::Class*        exceptionClass;   /* ExceptionOnly */
  bool                  caught;           /* ExceptionOnly */
  mirror::ArtField*     field;            /* FieldOnly */
  mirror::Object*       thisPtr;          /* InstanceOnly */
  /* nothing for StepOnly -- handled differently */
};

static bool NeedsFullDeoptimization(JdwpEventKind eventKind) {
  switch (eventKind) {
      case EK_METHOD_ENTRY:
      case EK_METHOD_EXIT:
      case EK_METHOD_EXIT_WITH_RETURN_VALUE:
      case EK_SINGLE_STEP:
      case EK_FIELD_ACCESS:
      case EK_FIELD_MODIFICATION:
        return true;
      default:
        return false;
    }
}

uint32_t GetInstrumentationEventFor(JdwpEventKind eventKind) {
  switch (eventKind) {
    case EK_BREAKPOINT:
    case EK_SINGLE_STEP:
      return instrumentation::Instrumentation::kDexPcMoved;
    case EK_EXCEPTION:
    case EK_EXCEPTION_CATCH:
      return instrumentation::Instrumentation::kExceptionCaught;
    case EK_METHOD_ENTRY:
      return instrumentation::Instrumentation::kMethodEntered;
    case EK_METHOD_EXIT:
    case EK_METHOD_EXIT_WITH_RETURN_VALUE:
      return instrumentation::Instrumentation::kMethodExited;
    case EK_FIELD_ACCESS:
      return instrumentation::Instrumentation::kFieldRead;
    case EK_FIELD_MODIFICATION:
      return instrumentation::Instrumentation::kFieldWritten;
    default:
      return 0;
  }
}

/*
 * Add an event to the list.  Ordering is not important.
 *
 * If something prevents the event from being registered, e.g. it's a
 * single-step request on a thread that doesn't exist, the event will
 * not be added to the list, and an appropriate error will be returned.
 */
JdwpError JdwpState::RegisterEvent(JdwpEvent* pEvent) {
  CHECK(pEvent != NULL);
  CHECK(pEvent->prev == NULL);
  CHECK(pEvent->next == NULL);

  {
    /*
     * If one or more "break"-type mods are used, register them with
     * the interpreter.
     */
    DeoptimizationRequest req;
    for (int i = 0; i < pEvent->modCount; i++) {
      const JdwpEventMod* pMod = &pEvent->mods[i];
      if (pMod->modKind == MK_LOCATION_ONLY) {
        /* should only be for Breakpoint, Step, and Exception */
        Dbg::WatchLocation(&pMod->locationOnly.loc, &req);
      } else if (pMod->modKind == MK_STEP) {
        /* should only be for EK_SINGLE_STEP; should only be one */
        JdwpStepSize size = static_cast<JdwpStepSize>(pMod->step.size);
        JdwpStepDepth depth = static_cast<JdwpStepDepth>(pMod->step.depth);
        JdwpError status = Dbg::ConfigureStep(pMod->step.threadId, size, depth);
        if (status != ERR_NONE) {
          return status;
        }
      }
    }
    if (NeedsFullDeoptimization(pEvent->eventKind)) {
      CHECK_EQ(req.GetKind(), DeoptimizationRequest::kNothing);
      CHECK(req.Method() == nullptr);
      req.SetKind(DeoptimizationRequest::kFullDeoptimization);
    }
    Dbg::RequestDeoptimization(req);
  }
  uint32_t instrumentation_event = GetInstrumentationEventFor(pEvent->eventKind);
  if (instrumentation_event != 0) {
    DeoptimizationRequest req;
    req.SetKind(DeoptimizationRequest::kRegisterForEvent);
    req.SetInstrumentationEvent(instrumentation_event);
    Dbg::RequestDeoptimization(req);
  }

  {
    /*
     * Add to list.
     */
    MutexLock mu(Thread::Current(), event_list_lock_);
    if (event_list_ != NULL) {
      pEvent->next = event_list_;
      event_list_->prev = pEvent;
    }
    event_list_ = pEvent;
    ++event_list_size_;
  }

  Dbg::ManageDeoptimization();

  return ERR_NONE;
}

/*
 * Remove an event from the list.  This will also remove the event from
 * any optimization tables, e.g. breakpoints.
 *
 * Does not free the JdwpEvent.
 *
 * Grab the eventLock before calling here.
 */
void JdwpState::UnregisterEvent(JdwpEvent* pEvent) {
  if (pEvent->prev == NULL) {
    /* head of the list */
    CHECK(event_list_ == pEvent);

    event_list_ = pEvent->next;
  } else {
    pEvent->prev->next = pEvent->next;
  }

  if (pEvent->next != NULL) {
    pEvent->next->prev = pEvent->prev;
    pEvent->next = NULL;
  }
  pEvent->prev = NULL;

  {
    /*
     * Unhook us from the interpreter, if necessary.
     */
    DeoptimizationRequest req;
    for (int i = 0; i < pEvent->modCount; i++) {
      JdwpEventMod* pMod = &pEvent->mods[i];
      if (pMod->modKind == MK_LOCATION_ONLY) {
        /* should only be for Breakpoint, Step, and Exception */
        Dbg::UnwatchLocation(&pMod->locationOnly.loc, &req);
      }
      if (pMod->modKind == MK_STEP) {
        /* should only be for EK_SINGLE_STEP; should only be one */
        Dbg::UnconfigureStep(pMod->step.threadId);
      }
    }
    if (pEvent->eventKind == EK_SINGLE_STEP) {
      // Special case for single-steps where we want to avoid the slow pattern deoptimize/undeoptimize
      // loop between each single-step. In a IDE, this would happens each time the user click on the
      // "single-step" button. Here we delay the full undeoptimization to the next resume
      // (VM.Resume or ThreadReference.Resume) or the end of the debugging session (VM.Dispose or
      // runtime shutdown).
      // Therefore, in a singles-stepping sequence, only the first single-step will trigger a full
      // deoptimization and only the last single-step will trigger a full undeoptimization.
      Dbg::DelayFullUndeoptimization();
    } else if (NeedsFullDeoptimization(pEvent->eventKind)) {
      CHECK_EQ(req.GetKind(), DeoptimizationRequest::kNothing);
      CHECK(req.Method() == nullptr);
      req.SetKind(DeoptimizationRequest::kFullUndeoptimization);
    }
    Dbg::RequestDeoptimization(req);
  }
  uint32_t instrumentation_event = GetInstrumentationEventFor(pEvent->eventKind);
  if (instrumentation_event != 0) {
    DeoptimizationRequest req;
    req.SetKind(DeoptimizationRequest::kUnregisterForEvent);
    req.SetInstrumentationEvent(instrumentation_event);
    Dbg::RequestDeoptimization(req);
  }

  --event_list_size_;
  CHECK(event_list_size_ != 0 || event_list_ == NULL);
}

/*
 * Remove the event with the given ID from the list.
 *
 * Failure to find the event isn't really an error, but it is a little
 * weird.  (It looks like Eclipse will try to be extra careful and will
 * explicitly remove one-off single-step events.)
 */
void JdwpState::UnregisterEventById(uint32_t requestId) {
  bool found = false;
  {
    MutexLock mu(Thread::Current(), event_list_lock_);

    for (JdwpEvent* pEvent = event_list_; pEvent != nullptr; pEvent = pEvent->next) {
      if (pEvent->requestId == requestId) {
        found = true;
        UnregisterEvent(pEvent);
        EventFree(pEvent);
        break;      /* there can be only one with a given ID */
      }
    }
  }

  if (found) {
    Dbg::ManageDeoptimization();
  } else {
    LOG(WARNING) << StringPrintf("Odd: no match when removing event reqId=0x%04x", requestId);
  }
}

/*
 * Remove all entries from the event list.
 */
void JdwpState::UnregisterAll() {
  MutexLock mu(Thread::Current(), event_list_lock_);

  JdwpEvent* pEvent = event_list_;
  while (pEvent != NULL) {
    JdwpEvent* pNextEvent = pEvent->next;

    UnregisterEvent(pEvent);
    EventFree(pEvent);
    pEvent = pNextEvent;
  }

  event_list_ = NULL;
}

/*
 * Allocate a JdwpEvent struct with enough space to hold the specified
 * number of mod records.
 */
JdwpEvent* EventAlloc(int numMods) {
  JdwpEvent* newEvent;
  int allocSize = offsetof(JdwpEvent, mods) + numMods * sizeof(newEvent->mods[0]);
  newEvent = reinterpret_cast<JdwpEvent*>(malloc(allocSize));
  memset(newEvent, 0, allocSize);
  return newEvent;
}

/*
 * Free a JdwpEvent.
 *
 * Do not call this until the event has been removed from the list.
 */
void EventFree(JdwpEvent* pEvent) {
  if (pEvent == NULL) {
    return;
  }

  /* make sure it was removed from the list */
  CHECK(pEvent->prev == NULL);
  CHECK(pEvent->next == NULL);
  /* want to check state->event_list_ != pEvent */

  /*
   * Free any hairy bits in the mods.
   */
  for (int i = 0; i < pEvent->modCount; i++) {
    if (pEvent->mods[i].modKind == MK_CLASS_MATCH) {
      free(pEvent->mods[i].classMatch.classPattern);
      pEvent->mods[i].classMatch.classPattern = NULL;
    }
    if (pEvent->mods[i].modKind == MK_CLASS_EXCLUDE) {
      free(pEvent->mods[i].classExclude.classPattern);
      pEvent->mods[i].classExclude.classPattern = NULL;
    }
  }

  free(pEvent);
}

/*
 * Allocate storage for matching events.  To keep things simple we
 * use an array with enough storage for the entire list.
 *
 * The state->eventLock should be held before calling.
 */
static JdwpEvent** AllocMatchList(size_t event_count) {
  return new JdwpEvent*[event_count];
}

/*
 * Run through the list and remove any entries with an expired "count" mod
 * from the event list, then free the match list.
 */
void JdwpState::CleanupMatchList(JdwpEvent** match_list, size_t match_count) {
  JdwpEvent** ppEvent = match_list;

  while (match_count--) {
    JdwpEvent* pEvent = *ppEvent;

    for (int i = 0; i < pEvent->modCount; i++) {
      if (pEvent->mods[i].modKind == MK_COUNT && pEvent->mods[i].count.count == 0) {
        VLOG(jdwp) << StringPrintf("##### Removing expired event (requestId=%#" PRIx32 ")",
                                   pEvent->requestId);
        UnregisterEvent(pEvent);
        EventFree(pEvent);
        break;
      }
    }

    ppEvent++;
  }

  delete[] match_list;
}

/*
 * Match a string against a "restricted regular expression", which is just
 * a string that may start or end with '*' (e.g. "*.Foo" or "java.*").
 *
 * ("Restricted name globbing" might have been a better term.)
 */
static bool PatternMatch(const char* pattern, const std::string& target) {
  size_t patLen = strlen(pattern);
  if (pattern[0] == '*') {
    patLen--;
    if (target.size() < patLen) {
      return false;
    }
    return strcmp(pattern+1, target.c_str() + (target.size()-patLen)) == 0;
  } else if (pattern[patLen-1] == '*') {
    return strncmp(pattern, target.c_str(), patLen-1) == 0;
  } else {
    return strcmp(pattern, target.c_str()) == 0;
  }
}

/*
 * See if the event's mods match up with the contents of "basket".
 *
 * If we find a Count mod before rejecting an event, we decrement it.  We
 * need to do this even if later mods cause us to ignore the event.
 */
static bool ModsMatch(JdwpEvent* pEvent, const ModBasket& basket)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JdwpEventMod* pMod = pEvent->mods;

  for (int i = pEvent->modCount; i > 0; i--, pMod++) {
    switch (pMod->modKind) {
    case MK_COUNT:
      CHECK_GT(pMod->count.count, 0);
      pMod->count.count--;
      if (pMod->count.count > 0) {
        return false;
      }
      break;
    case MK_CONDITIONAL:
      CHECK(false);  // should not be getting these
      break;
    case MK_THREAD_ONLY:
      if (!Dbg::MatchThread(pMod->threadOnly.threadId, basket.thread)) {
        return false;
      }
      break;
    case MK_CLASS_ONLY:
      if (!Dbg::MatchType(basket.locationClass, pMod->classOnly.refTypeId)) {
        return false;
      }
      break;
    case MK_CLASS_MATCH:
      if (!PatternMatch(pMod->classMatch.classPattern, basket.className)) {
        return false;
      }
      break;
    case MK_CLASS_EXCLUDE:
      if (PatternMatch(pMod->classMatch.classPattern, basket.className)) {
        return false;
      }
      break;
    case MK_LOCATION_ONLY:
      if (!Dbg::MatchLocation(pMod->locationOnly.loc, *basket.pLoc)) {
        return false;
      }
      break;
    case MK_EXCEPTION_ONLY:
      if (pMod->exceptionOnly.refTypeId != 0 &&
          !Dbg::MatchType(basket.exceptionClass, pMod->exceptionOnly.refTypeId)) {
        return false;
      }
      if ((basket.caught && !pMod->exceptionOnly.caught) ||
          (!basket.caught && !pMod->exceptionOnly.uncaught)) {
        return false;
      }
      break;
    case MK_FIELD_ONLY:
      if (!Dbg::MatchField(pMod->fieldOnly.refTypeId, pMod->fieldOnly.fieldId, basket.field)) {
        return false;
      }
      break;
    case MK_STEP:
      if (!Dbg::MatchThread(pMod->step.threadId, basket.thread)) {
        return false;
      }
      break;
    case MK_INSTANCE_ONLY:
      if (!Dbg::MatchInstance(pMod->instanceOnly.objectId, basket.thisPtr)) {
        return false;
      }
      break;
    default:
      LOG(FATAL) << "unknown mod kind " << pMod->modKind;
      break;
    }
  }
  return true;
}

/*
 * Find all events of type "eventKind" with mods that match up with the
 * rest of the arguments.
 *
 * Found events are appended to "match_list", and "*pMatchCount" is advanced,
 * so this may be called multiple times for grouped events.
 *
 * DO NOT call this multiple times for the same eventKind, as Count mods are
 * decremented during the scan.
 */
void JdwpState::FindMatchingEvents(JdwpEventKind eventKind, const ModBasket& basket,
                                   JdwpEvent** match_list, size_t* pMatchCount) {
  /* start after the existing entries */
  match_list += *pMatchCount;

  for (JdwpEvent* pEvent = event_list_; pEvent != nullptr; pEvent = pEvent->next) {
    if (pEvent->eventKind == eventKind && ModsMatch(pEvent, basket)) {
      *match_list++ = pEvent;
      (*pMatchCount)++;
    }
  }
}

/*
 * Scan through the list of matches and determine the most severe
 * suspension policy.
 */
static JdwpSuspendPolicy scanSuspendPolicy(JdwpEvent** match_list, int match_count) {
  JdwpSuspendPolicy policy = SP_NONE;

  while (match_count--) {
    if ((*match_list)->suspend_policy > policy) {
      policy = (*match_list)->suspend_policy;
    }
    match_list++;
  }

  return policy;
}

/*
 * Three possibilities:
 *  SP_NONE - do nothing
 *  SP_EVENT_THREAD - suspend ourselves
 *  SP_ALL - suspend everybody except JDWP support thread
 */
void JdwpState::SuspendByPolicy(JdwpSuspendPolicy suspend_policy, JDWP::ObjectId thread_self_id) {
  VLOG(jdwp) << "SuspendByPolicy(" << suspend_policy << ")";
  if (suspend_policy == SP_NONE) {
    return;
  }

  if (suspend_policy == SP_ALL) {
    Dbg::SuspendVM();
  } else {
    CHECK_EQ(suspend_policy, SP_EVENT_THREAD);
  }

  /* this is rare but possible -- see CLASS_PREPARE handling */
  if (thread_self_id == debug_thread_id_) {
    LOG(INFO) << "NOTE: SuspendByPolicy not suspending JDWP thread";
    return;
  }

  DebugInvokeReq* pReq = Dbg::GetInvokeReq();
  while (true) {
    pReq->ready = true;
    Dbg::SuspendSelf();
    pReq->ready = false;

    /*
     * The JDWP thread has told us (and possibly all other threads) to
     * resume.  See if it has left anything in our DebugInvokeReq mailbox.
     */
    if (!pReq->invoke_needed) {
      /*LOGD("SuspendByPolicy: no invoke needed");*/
      break;
    }

    /* grab this before posting/suspending again */
    SetWaitForEventThread(thread_self_id);

    /* leave pReq->invoke_needed_ raised so we can check reentrancy */
    Dbg::ExecuteMethod(pReq);

    pReq->error = ERR_NONE;
  }
}

void JdwpState::SendRequestAndPossiblySuspend(ExpandBuf* pReq, JdwpSuspendPolicy suspend_policy,
                                              ObjectId threadId) {
  Thread* self = Thread::Current();
  self->AssertThreadSuspensionIsAllowable();
  /* send request and possibly suspend ourselves */
  if (pReq != NULL) {
    JDWP::ObjectId thread_self_id = Dbg::GetThreadSelfId();
    self->TransitionFromRunnableToSuspended(kWaitingForDebuggerSend);
    if (suspend_policy != SP_NONE) {
      SetWaitForEventThread(threadId);
    }
    EventFinish(pReq);
    SuspendByPolicy(suspend_policy, thread_self_id);
    self->TransitionFromSuspendedToRunnable();
  }
}

/*
 * Determine if there is a method invocation in progress in the current
 * thread.
 *
 * We look at the "invoke_needed" flag in the per-thread DebugInvokeReq
 * state.  If set, we're in the process of invoking a method.
 */
bool JdwpState::InvokeInProgress() {
  DebugInvokeReq* pReq = Dbg::GetInvokeReq();
  return pReq->invoke_needed;
}

/*
 * We need the JDWP thread to hold off on doing stuff while we post an
 * event and then suspend ourselves.
 *
 * Call this with a threadId of zero if you just want to wait for the
 * current thread operation to complete.
 *
 * This could go to sleep waiting for another thread, so it's important
 * that the thread be marked as VMWAIT before calling here.
 */
void JdwpState::SetWaitForEventThread(ObjectId threadId) {
  bool waited = false;

  /* this is held for very brief periods; contention is unlikely */
  Thread* self = Thread::Current();
  MutexLock mu(self, event_thread_lock_);

  /*
   * If another thread is already doing stuff, wait for it.  This can
   * go to sleep indefinitely.
   */
  while (event_thread_id_ != 0) {
    VLOG(jdwp) << StringPrintf("event in progress (%#" PRIx64 "), %#" PRIx64 " sleeping",
                               event_thread_id_, threadId);
    waited = true;
    event_thread_cond_.Wait(self);
  }

  if (waited || threadId != 0) {
    VLOG(jdwp) << StringPrintf("event token grabbed (%#" PRIx64 ")", threadId);
  }
  if (threadId != 0) {
    event_thread_id_ = threadId;
  }
}

/*
 * Clear the threadId and signal anybody waiting.
 */
void JdwpState::ClearWaitForEventThread() {
  /*
   * Grab the mutex.  Don't try to go in/out of VMWAIT mode, as this
   * function is called by dvmSuspendSelf(), and the transition back
   * to RUNNING would confuse it.
   */
  Thread* self = Thread::Current();
  MutexLock mu(self, event_thread_lock_);

  CHECK_NE(event_thread_id_, 0U);
  VLOG(jdwp) << StringPrintf("cleared event token (%#" PRIx64 ")", event_thread_id_);

  event_thread_id_ = 0;

  event_thread_cond_.Signal(self);
}


/*
 * Prep an event.  Allocates storage for the message and leaves space for
 * the header.
 */
static ExpandBuf* eventPrep() {
  ExpandBuf* pReq = expandBufAlloc();
  expandBufAddSpace(pReq, kJDWPHeaderLen);
  return pReq;
}

/*
 * Write the header into the buffer and send the packet off to the debugger.
 *
 * Takes ownership of "pReq" (currently discards it).
 */
void JdwpState::EventFinish(ExpandBuf* pReq) {
  uint8_t* buf = expandBufGetBuffer(pReq);

  Set4BE(buf, expandBufGetLength(pReq));
  Set4BE(buf+4, NextRequestSerial());
  Set1(buf+8, 0);     /* flags */
  Set1(buf+9, kJdwpEventCommandSet);
  Set1(buf+10, kJdwpCompositeCommand);

  // Prevents from interleaving commands and events. Otherwise we could end up in sending an event
  // before sending the reply of the command being processed and would lead to bad synchronization
  // between the debugger and the debuggee.
  WaitForProcessingRequest();

  SendRequest(pReq);

  expandBufFree(pReq);
}


/*
 * Tell the debugger that we have finished initializing.  This is always
 * sent, even if the debugger hasn't requested it.
 *
 * This should be sent "before the main thread is started and before
 * any application code has been executed".  The thread ID in the message
 * must be for the main thread.
 */
bool JdwpState::PostVMStart() {
  JdwpSuspendPolicy suspend_policy;
  ObjectId threadId = Dbg::GetThreadSelfId();

  if (options_->suspend) {
    suspend_policy = SP_ALL;
  } else {
    suspend_policy = SP_NONE;
  }

  ExpandBuf* pReq = eventPrep();
  {
    MutexLock mu(Thread::Current(), event_list_lock_);  // probably don't need this here

    VLOG(jdwp) << "EVENT: " << EK_VM_START;
    VLOG(jdwp) << "  suspend_policy=" << suspend_policy;

    expandBufAdd1(pReq, suspend_policy);
    expandBufAdd4BE(pReq, 1);

    expandBufAdd1(pReq, EK_VM_START);
    expandBufAdd4BE(pReq, 0);       /* requestId */
    expandBufAdd8BE(pReq, threadId);
  }

  Dbg::ManageDeoptimization();

  /* send request and possibly suspend ourselves */
  SendRequestAndPossiblySuspend(pReq, suspend_policy, threadId);

  return true;
}

static void LogMatchingEventsAndThread(JdwpEvent** match_list, size_t match_count,
                                       ObjectId thread_id)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  for (size_t i = 0; i < match_count; ++i) {
    JdwpEvent* pEvent = match_list[i];
    VLOG(jdwp) << "EVENT #" << i << ": " << pEvent->eventKind
               << StringPrintf(" (requestId=%#" PRIx32 ")", pEvent->requestId);
  }
  std::string thread_name;
  JdwpError error = Dbg::GetThreadName(thread_id, thread_name);
  if (error != JDWP::ERR_NONE) {
    thread_name = "<unknown>";
  }
  VLOG(jdwp) << StringPrintf("  thread=%#" PRIx64, thread_id) << " " << thread_name;
}

static void SetJdwpLocationFromEventLocation(const JDWP::EventLocation* event_location,
                                             JDWP::JdwpLocation* jdwp_location)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(event_location != nullptr);
  DCHECK(jdwp_location != nullptr);
  Dbg::SetJdwpLocation(jdwp_location, event_location->method, event_location->dex_pc);
}

/*
 * A location of interest has been reached.  This handles:
 *   Breakpoint
 *   SingleStep
 *   MethodEntry
 *   MethodExit
 * These four types must be grouped together in a single response.  The
 * "eventFlags" indicates the type of event(s) that have happened.
 *
 * Valid mods:
 *   Count, ThreadOnly, ClassOnly, ClassMatch, ClassExclude, InstanceOnly
 *   LocationOnly (for breakpoint/step only)
 *   Step (for step only)
 *
 * Interesting test cases:
 *  - Put a breakpoint on a native method.  Eclipse creates METHOD_ENTRY
 *    and METHOD_EXIT events with a ClassOnly mod on the method's class.
 *  - Use "run to line".  Eclipse creates a BREAKPOINT with Count=1.
 *  - Single-step to a line with a breakpoint.  Should get a single
 *    event message with both events in it.
 */
bool JdwpState::PostLocationEvent(const EventLocation* pLoc, mirror::Object* thisPtr,
                                  int eventFlags, const JValue* returnValue) {
  DCHECK(pLoc != nullptr);
  DCHECK(pLoc->method != nullptr);
  DCHECK_EQ(pLoc->method->IsStatic(), thisPtr == nullptr);

  ModBasket basket;
  basket.pLoc = pLoc;
  basket.locationClass = pLoc->method->GetDeclaringClass();
  basket.thisPtr = thisPtr;
  basket.thread = Thread::Current();
  basket.className = Dbg::GetClassName(basket.locationClass);

  /*
   * On rare occasions we may need to execute interpreted code in the VM
   * while handling a request from the debugger.  Don't fire breakpoints
   * while doing so.  (I don't think we currently do this at all, so
   * this is mostly paranoia.)
   */
  if (basket.thread == GetDebugThread()) {
    VLOG(jdwp) << "Ignoring location event in JDWP thread";
    return false;
  }

  /*
   * The debugger variable display tab may invoke the interpreter to format
   * complex objects.  We want to ignore breakpoints and method entry/exit
   * traps while working on behalf of the debugger.
   *
   * If we don't ignore them, the VM will get hung up, because we'll
   * suspend on a breakpoint while the debugger is still waiting for its
   * method invocation to complete.
   */
  if (InvokeInProgress()) {
    VLOG(jdwp) << "Not checking breakpoints during invoke (" << basket.className << ")";
    return false;
  }

  size_t match_count = 0;
  ExpandBuf* pReq = NULL;
  JdwpSuspendPolicy suspend_policy = SP_NONE;
  JdwpEvent** match_list = nullptr;
  ObjectId thread_id = 0;
  {
    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      match_list = AllocMatchList(event_list_size_);
      if ((eventFlags & Dbg::kBreakpoint) != 0) {
        FindMatchingEvents(EK_BREAKPOINT, basket, match_list, &match_count);
      }
      if ((eventFlags & Dbg::kSingleStep) != 0) {
        FindMatchingEvents(EK_SINGLE_STEP, basket, match_list, &match_count);
      }
      if ((eventFlags & Dbg::kMethodEntry) != 0) {
        FindMatchingEvents(EK_METHOD_ENTRY, basket, match_list, &match_count);
      }
      if ((eventFlags & Dbg::kMethodExit) != 0) {
        FindMatchingEvents(EK_METHOD_EXIT, basket, match_list, &match_count);
        FindMatchingEvents(EK_METHOD_EXIT_WITH_RETURN_VALUE, basket, match_list, &match_count);
      }
    }
    if (match_count != 0) {
      suspend_policy = scanSuspendPolicy(match_list, match_count);

      thread_id = Dbg::GetThreadId(basket.thread);
      JDWP::JdwpLocation jdwp_location;
      SetJdwpLocationFromEventLocation(pLoc, &jdwp_location);

      if (VLOG_IS_ON(jdwp)) {
        LogMatchingEventsAndThread(match_list, match_count, thread_id);
        VLOG(jdwp) << "  location=" << jdwp_location;
        VLOG(jdwp) << "  suspend_policy=" << suspend_policy;
      }

      pReq = eventPrep();
      expandBufAdd1(pReq, suspend_policy);
      expandBufAdd4BE(pReq, match_count);

      for (size_t i = 0; i < match_count; i++) {
        expandBufAdd1(pReq, match_list[i]->eventKind);
        expandBufAdd4BE(pReq, match_list[i]->requestId);
        expandBufAdd8BE(pReq, thread_id);
        expandBufAddLocation(pReq, jdwp_location);
        if (match_list[i]->eventKind == EK_METHOD_EXIT_WITH_RETURN_VALUE) {
          Dbg::OutputMethodReturnValue(jdwp_location.method_id, returnValue, pReq);
        }
      }
    }

    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      CleanupMatchList(match_list, match_count);
    }
  }

  Dbg::ManageDeoptimization();

  SendRequestAndPossiblySuspend(pReq, suspend_policy, thread_id);
  return match_count != 0;
}

bool JdwpState::PostFieldEvent(const EventLocation* pLoc, mirror::ArtField* field,
                               mirror::Object* this_object, const JValue* fieldValue,
                               bool is_modification) {
  DCHECK(pLoc != nullptr);
  DCHECK(field != nullptr);
  DCHECK_EQ(fieldValue != nullptr, is_modification);
  DCHECK_EQ(field->IsStatic(), this_object == nullptr);

  ModBasket basket;
  basket.pLoc = pLoc;
  basket.locationClass = pLoc->method->GetDeclaringClass();
  basket.thisPtr = this_object;
  basket.thread = Thread::Current();
  basket.className = Dbg::GetClassName(basket.locationClass);
  basket.field = field;

  if (InvokeInProgress()) {
    VLOG(jdwp) << "Not posting field event during invoke";
    return false;
  }

  size_t match_count = 0;
  ExpandBuf* pReq = NULL;
  JdwpSuspendPolicy suspend_policy = SP_NONE;
  JdwpEvent** match_list = nullptr;
  ObjectId thread_id = 0;
  {
    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      match_list = AllocMatchList(event_list_size_);
      if (is_modification) {
        FindMatchingEvents(EK_FIELD_MODIFICATION, basket, match_list, &match_count);
      } else {
        FindMatchingEvents(EK_FIELD_ACCESS, basket, match_list, &match_count);
      }
    }
    if (match_count != 0) {
      suspend_policy = scanSuspendPolicy(match_list, match_count);

      thread_id = Dbg::GetThreadId(basket.thread);
      ObjectRegistry* registry = Dbg::GetObjectRegistry();
      ObjectId instance_id = registry->Add(basket.thisPtr);
      RefTypeId field_type_id = registry->AddRefType(field->GetDeclaringClass());
      FieldId field_id = Dbg::ToFieldId(field);
      JDWP::JdwpLocation jdwp_location;
      SetJdwpLocationFromEventLocation(pLoc, &jdwp_location);

      if (VLOG_IS_ON(jdwp)) {
        LogMatchingEventsAndThread(match_list, match_count, thread_id);
        VLOG(jdwp) << "  location=" << jdwp_location;
        VLOG(jdwp) << StringPrintf("  this=%#" PRIx64, instance_id);
        VLOG(jdwp) << StringPrintf("  type=%#" PRIx64, field_type_id) << " "
                   << Dbg::GetClassName(field_id);
        VLOG(jdwp) << StringPrintf("  field=%#" PRIx32, field_id) << " "
                   << Dbg::GetFieldName(field_id);
        VLOG(jdwp) << "  suspend_policy=" << suspend_policy;
      }

      pReq = eventPrep();
      expandBufAdd1(pReq, suspend_policy);
      expandBufAdd4BE(pReq, match_count);

      // Get field's reference type tag.
      JDWP::JdwpTypeTag type_tag = Dbg::GetTypeTag(field->GetDeclaringClass());

      // Get instance type tag.
      uint8_t tag;
      {
        ScopedObjectAccessUnchecked soa(Thread::Current());
        tag = Dbg::TagFromObject(soa, basket.thisPtr);
      }

      for (size_t i = 0; i < match_count; i++) {
        expandBufAdd1(pReq, match_list[i]->eventKind);
        expandBufAdd4BE(pReq, match_list[i]->requestId);
        expandBufAdd8BE(pReq, thread_id);
        expandBufAddLocation(pReq, jdwp_location);
        expandBufAdd1(pReq, type_tag);
        expandBufAddRefTypeId(pReq, field_type_id);
        expandBufAddFieldId(pReq, field_id);
        expandBufAdd1(pReq, tag);
        expandBufAddObjectId(pReq, instance_id);
        if (is_modification) {
          Dbg::OutputFieldValue(field_id, fieldValue, pReq);
        }
      }
    }

    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      CleanupMatchList(match_list, match_count);
    }
  }

  Dbg::ManageDeoptimization();

  SendRequestAndPossiblySuspend(pReq, suspend_policy, thread_id);
  return match_count != 0;
}

/*
 * A thread is starting or stopping.
 *
 * Valid mods:
 *  Count, ThreadOnly
 */
bool JdwpState::PostThreadChange(Thread* thread, bool start) {
  CHECK_EQ(thread, Thread::Current());

  /*
   * I don't think this can happen.
   */
  if (InvokeInProgress()) {
    LOG(WARNING) << "Not posting thread change during invoke";
    return false;
  }

  ModBasket basket;
  basket.thread = thread;

  ExpandBuf* pReq = NULL;
  JdwpSuspendPolicy suspend_policy = SP_NONE;
  JdwpEvent** match_list = nullptr;
  size_t match_count = 0;
  ObjectId thread_id = 0;
  {
    {
      // Don't allow the list to be updated while we scan it.
      MutexLock mu(Thread::Current(), event_list_lock_);
      match_list = AllocMatchList(event_list_size_);
      if (start) {
        FindMatchingEvents(EK_THREAD_START, basket, match_list, &match_count);
      } else {
        FindMatchingEvents(EK_THREAD_DEATH, basket, match_list, &match_count);
      }
    }

    if (match_count != 0) {
      suspend_policy = scanSuspendPolicy(match_list, match_count);

      thread_id = Dbg::GetThreadId(basket.thread);

      if (VLOG_IS_ON(jdwp)) {
        LogMatchingEventsAndThread(match_list, match_count, thread_id);
        VLOG(jdwp) << "  suspend_policy=" << suspend_policy;
      }

      pReq = eventPrep();
      expandBufAdd1(pReq, suspend_policy);
      expandBufAdd4BE(pReq, match_count);

      for (size_t i = 0; i < match_count; i++) {
        expandBufAdd1(pReq, match_list[i]->eventKind);
        expandBufAdd4BE(pReq, match_list[i]->requestId);
        expandBufAdd8BE(pReq, thread_id);
      }
    }

    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      CleanupMatchList(match_list, match_count);
    }
  }

  Dbg::ManageDeoptimization();

  SendRequestAndPossiblySuspend(pReq, suspend_policy, thread_id);

  return match_count != 0;
}

/*
 * Send a polite "VM is dying" message to the debugger.
 *
 * Skips the usual "event token" stuff.
 */
bool JdwpState::PostVMDeath() {
  VLOG(jdwp) << "EVENT: " << EK_VM_DEATH;

  ExpandBuf* pReq = eventPrep();
  expandBufAdd1(pReq, SP_NONE);
  expandBufAdd4BE(pReq, 1);

  expandBufAdd1(pReq, EK_VM_DEATH);
  expandBufAdd4BE(pReq, 0);
  EventFinish(pReq);
  return true;
}

/*
 * An exception has been thrown.  It may or may not have been caught.
 *
 * Valid mods:
 *  Count, ThreadOnly, ClassOnly, ClassMatch, ClassExclude, LocationOnly,
 *    ExceptionOnly, InstanceOnly
 *
 * The "exceptionId" has not been added to the GC-visible object registry,
 * because there's a pretty good chance that we're not going to send it
 * up the debugger.
 */
bool JdwpState::PostException(const EventLocation* pThrowLoc, mirror::Throwable* exception_object,
                              const EventLocation* pCatchLoc, mirror::Object* thisPtr) {
  DCHECK(exception_object != nullptr);
  DCHECK(pThrowLoc != nullptr);
  DCHECK(pCatchLoc != nullptr);
  DCHECK(pThrowLoc->method != nullptr);
  DCHECK_EQ(pThrowLoc->method->IsStatic(), thisPtr == nullptr);

  ModBasket basket;
  basket.pLoc = pThrowLoc;
  basket.locationClass = pThrowLoc->method->GetDeclaringClass();
  basket.thread = Thread::Current();
  basket.className = Dbg::GetClassName(basket.locationClass);
  basket.exceptionClass = exception_object->GetClass();
  basket.caught = (pCatchLoc->method != 0);
  basket.thisPtr = thisPtr;

  /* don't try to post an exception caused by the debugger */
  if (InvokeInProgress()) {
    VLOG(jdwp) << "Not posting exception hit during invoke (" << basket.className << ")";
    return false;
  }

  size_t match_count = 0;
  ExpandBuf* pReq = NULL;
  JdwpSuspendPolicy suspend_policy = SP_NONE;
  JdwpEvent** match_list = nullptr;
  ObjectId thread_id = 0;
  {
    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      match_list = AllocMatchList(event_list_size_);
      FindMatchingEvents(EK_EXCEPTION, basket, match_list, &match_count);
    }
    if (match_count != 0) {
      suspend_policy = scanSuspendPolicy(match_list, match_count);

      thread_id = Dbg::GetThreadId(basket.thread);
      ObjectRegistry* registry = Dbg::GetObjectRegistry();
      ObjectId exceptionId = registry->Add(exception_object);
      JDWP::JdwpLocation jdwp_throw_location;
      JDWP::JdwpLocation jdwp_catch_location;
      SetJdwpLocationFromEventLocation(pThrowLoc, &jdwp_throw_location);
      SetJdwpLocationFromEventLocation(pCatchLoc, &jdwp_catch_location);

      if (VLOG_IS_ON(jdwp)) {
        std::string exceptionClassName(PrettyDescriptor(exception_object->GetClass()));

        LogMatchingEventsAndThread(match_list, match_count, thread_id);
        VLOG(jdwp) << "  throwLocation=" << jdwp_throw_location;
        if (jdwp_catch_location.class_id == 0) {
          VLOG(jdwp) << "  catchLocation=uncaught";
        } else {
          VLOG(jdwp) << "  catchLocation=" << jdwp_catch_location;
        }
        VLOG(jdwp) << StringPrintf("  exception=%#" PRIx64, exceptionId) << " "
                   << exceptionClassName;
        VLOG(jdwp) << "  suspend_policy=" << suspend_policy;
      }

      pReq = eventPrep();
      expandBufAdd1(pReq, suspend_policy);
      expandBufAdd4BE(pReq, match_count);

      for (size_t i = 0; i < match_count; i++) {
        expandBufAdd1(pReq, match_list[i]->eventKind);
        expandBufAdd4BE(pReq, match_list[i]->requestId);
        expandBufAdd8BE(pReq, thread_id);
        expandBufAddLocation(pReq, jdwp_throw_location);
        expandBufAdd1(pReq, JT_OBJECT);
        expandBufAdd8BE(pReq, exceptionId);
        expandBufAddLocation(pReq, jdwp_catch_location);
      }
    }

    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      CleanupMatchList(match_list, match_count);
    }
  }

  Dbg::ManageDeoptimization();

  SendRequestAndPossiblySuspend(pReq, suspend_policy, thread_id);

  return match_count != 0;
}

/*
 * Announce that a class has been loaded.
 *
 * Valid mods:
 *  Count, ThreadOnly, ClassOnly, ClassMatch, ClassExclude
 */
bool JdwpState::PostClassPrepare(mirror::Class* klass) {
  DCHECK(klass != nullptr);

  ModBasket basket;
  basket.locationClass = klass;
  basket.thread = Thread::Current();
  basket.className = Dbg::GetClassName(basket.locationClass);

  /* suppress class prep caused by debugger */
  if (InvokeInProgress()) {
    VLOG(jdwp) << "Not posting class prep caused by invoke (" << basket.className << ")";
    return false;
  }

  ExpandBuf* pReq = NULL;
  JdwpSuspendPolicy suspend_policy = SP_NONE;
  JdwpEvent** match_list = nullptr;
  size_t match_count = 0;
  ObjectId thread_id = 0;
  {
    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      match_list = AllocMatchList(event_list_size_);
      FindMatchingEvents(EK_CLASS_PREPARE, basket, match_list, &match_count);
    }
    if (match_count != 0) {
      suspend_policy = scanSuspendPolicy(match_list, match_count);

      thread_id = Dbg::GetThreadId(basket.thread);
      ObjectRegistry* registry = Dbg::GetObjectRegistry();
      RefTypeId class_id = registry->AddRefType(basket.locationClass);

      // OLD-TODO - we currently always send both "verified" and "prepared" since
      // debuggers seem to like that.  There might be some advantage to honesty,
      // since the class may not yet be verified.
      int status = JDWP::CS_VERIFIED | JDWP::CS_PREPARED;
      JDWP::JdwpTypeTag tag = Dbg::GetTypeTag(basket.locationClass);
      std::string temp;
      std::string signature(basket.locationClass->GetDescriptor(&temp));

      if (VLOG_IS_ON(jdwp)) {
        LogMatchingEventsAndThread(match_list, match_count, thread_id);
        VLOG(jdwp) << StringPrintf("  type=%#" PRIx64, class_id) << " " << signature;
        VLOG(jdwp) << "  suspend_policy=" << suspend_policy;
      }

      if (thread_id == debug_thread_id_) {
        /*
         * JDWP says that, for a class prep in the debugger thread, we
         * should set thread to null and if any threads were supposed
         * to be suspended then we suspend all other threads.
         */
        VLOG(jdwp) << "  NOTE: class prepare in debugger thread!";
        thread_id = 0;
        if (suspend_policy == SP_EVENT_THREAD) {
          suspend_policy = SP_ALL;
        }
      }

      pReq = eventPrep();
      expandBufAdd1(pReq, suspend_policy);
      expandBufAdd4BE(pReq, match_count);

      for (size_t i = 0; i < match_count; i++) {
        expandBufAdd1(pReq, match_list[i]->eventKind);
        expandBufAdd4BE(pReq, match_list[i]->requestId);
        expandBufAdd8BE(pReq, thread_id);
        expandBufAdd1(pReq, tag);
        expandBufAdd8BE(pReq, class_id);
        expandBufAddUtf8String(pReq, signature);
        expandBufAdd4BE(pReq, status);
      }
    }

    {
      MutexLock mu(Thread::Current(), event_list_lock_);
      CleanupMatchList(match_list, match_count);
    }
  }

  Dbg::ManageDeoptimization();

  SendRequestAndPossiblySuspend(pReq, suspend_policy, thread_id);

  return match_count != 0;
}

/*
 * Send up a chunk of DDM data.
 *
 * While this takes the form of a JDWP "event", it doesn't interact with
 * other debugger traffic, and can't suspend the VM, so we skip all of
 * the fun event token gymnastics.
 */
void JdwpState::DdmSendChunkV(uint32_t type, const iovec* iov, int iov_count) {
  uint8_t header[kJDWPHeaderLen + 8];
  size_t dataLen = 0;

  CHECK(iov != NULL);
  CHECK_GT(iov_count, 0);
  CHECK_LT(iov_count, 10);

  /*
   * "Wrap" the contents of the iovec with a JDWP/DDMS header.  We do
   * this by creating a new copy of the vector with space for the header.
   */
  std::vector<iovec> wrapiov;
  wrapiov.push_back(iovec());
  for (int i = 0; i < iov_count; i++) {
    wrapiov.push_back(iov[i]);
    dataLen += iov[i].iov_len;
  }

  /* form the header (JDWP plus DDMS) */
  Set4BE(header, sizeof(header) + dataLen);
  Set4BE(header+4, NextRequestSerial());
  Set1(header+8, 0);     /* flags */
  Set1(header+9, kJDWPDdmCmdSet);
  Set1(header+10, kJDWPDdmCmd);
  Set4BE(header+11, type);
  Set4BE(header+15, dataLen);

  wrapiov[0].iov_base = header;
  wrapiov[0].iov_len = sizeof(header);

  // Try to avoid blocking GC during a send, but only safe when not using mutexes at a lower-level
  // than mutator for lock ordering reasons.
  Thread* self = Thread::Current();
  bool safe_to_release_mutator_lock_over_send = !Locks::mutator_lock_->IsExclusiveHeld(self);
  if (safe_to_release_mutator_lock_over_send) {
    for (size_t i = 0; i < kMutatorLock; ++i) {
      if (self->GetHeldMutex(static_cast<LockLevel>(i)) != NULL) {
        safe_to_release_mutator_lock_over_send = false;
        break;
      }
    }
  }
  if (safe_to_release_mutator_lock_over_send) {
    // Change state to waiting to allow GC, ... while we're sending.
    self->TransitionFromRunnableToSuspended(kWaitingForDebuggerSend);
    SendBufferedRequest(type, wrapiov);
    self->TransitionFromSuspendedToRunnable();
  } else {
    // Send and possibly block GC...
    SendBufferedRequest(type, wrapiov);
  }
}

}  // namespace JDWP

}  // namespace art

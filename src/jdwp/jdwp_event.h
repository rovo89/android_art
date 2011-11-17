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
 * Handle registration of events, and debugger event notification.
 */
#ifndef ART_JDWP_JDWPEVENT_H_
#define ART_JDWP_JDWPEVENT_H_

#include "jdwp/jdwp_constants.h"
#include "jdwp/jdwp_expand_buf.h"

namespace art {

namespace JDWP {

/*
 * Event modifiers.  A JdwpEvent may have zero or more of these.
 */
union JdwpEventMod {
  uint8_t      modKind;                /* JdwpModKind */
  struct {
    uint8_t          modKind;
    int         count;
  } count;
  struct {
    uint8_t          modKind;
    uint32_t          exprId;
  } conditional;
  struct {
    uint8_t          modKind;
    ObjectId    threadId;
  } threadOnly;
  struct {
    uint8_t          modKind;
    RefTypeId   refTypeId;
  } classOnly;
  struct {
    uint8_t          modKind;
    char*       classPattern;
  } classMatch;
  struct {
    uint8_t          modKind;
    char*       classPattern;
  } classExclude;
  struct {
    uint8_t          modKind;
    JdwpLocation loc;
  } locationOnly;
  struct {
    uint8_t          modKind;
    uint8_t          caught;
    uint8_t          uncaught;
    RefTypeId   refTypeId;
  } exceptionOnly;
  struct {
    uint8_t          modKind;
    RefTypeId   refTypeId;
    FieldId     fieldId;
  } fieldOnly;
  struct {
    uint8_t          modKind;
    ObjectId    threadId;
    int         size;           /* JdwpStepSize */
    int         depth;          /* JdwpStepDepth */
  } step;
  struct {
    uint8_t          modKind;
    ObjectId    objectId;
  } instanceOnly;
};

/*
 * One of these for every registered event.
 *
 * We over-allocate the struct to hold the modifiers.
 */
struct JdwpEvent {
  JdwpEvent* prev;           /* linked list */
  JdwpEvent* next;

  JdwpEventKind eventKind;      /* what kind of event is this? */
  JdwpSuspendPolicy suspendPolicy;  /* suspend all, none, or self? */
  int modCount;       /* #of entries in mods[] */
  uint32_t requestId;      /* serial#, reported to debugger */

  JdwpEventMod mods[1];        /* MUST be last field in struct */
};

/*
 * Allocate an event structure with enough space.
 */
JdwpEvent* EventAlloc(int numMods);
void EventFree(JdwpEvent* pEvent);

}  // namespace JDWP

}  // namespace art

#endif  // ART_JDWP_JDWPEVENT_H_

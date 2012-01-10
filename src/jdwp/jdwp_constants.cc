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
 * String constants to go along with enumerated values.  (Pity we don't
 * have enumerated constant reflection in C.)  These are only needed for
 * making the output human-readable.
 */
#include "jdwp/jdwp_constants.h"

#include <iostream>

namespace art {

namespace JDWP {

std::ostream& operator<<(std::ostream& os, const JdwpError& rhs) {
  switch (rhs) {
  case ERR_NONE: os << "NONE"; break;
  case ERR_INVALID_THREAD: os << "INVALID_THREAD"; break;
  case ERR_INVALID_THREAD_GROUP: os << "INVALID_THREAD_GROUP"; break;
  case ERR_INVALID_PRIORITY: os << "INVALID_PRIORITY"; break;
  case ERR_THREAD_NOT_SUSPENDED: os << "THREAD_NOT_SUSPENDED"; break;
  case ERR_THREAD_SUSPENDED: os << "THREAD_SUSPENDED"; break;
  case ERR_THREAD_NOT_ALIVE: os << "THREAD_NOT_ALIVE"; break;
  case ERR_INVALID_OBJECT: os << "INVALID_OBJEC"; break;
  case ERR_INVALID_CLASS: os << "INVALID_CLASS"; break;
  case ERR_CLASS_NOT_PREPARED: os << "CLASS_NOT_PREPARED"; break;
  case ERR_INVALID_METHODID: os << "INVALID_METHODID"; break;
  case ERR_INVALID_LOCATION: os << "INVALID_LOCATION"; break;
  case ERR_INVALID_FIELDID: os << "INVALID_FIELDID"; break;
  case ERR_INVALID_FRAMEID: os << "INVALID_FRAMEID"; break;
  case ERR_NO_MORE_FRAMES: os << "NO_MORE_FRAMES"; break;
  case ERR_OPAQUE_FRAME: os << "OPAQUE_FRAME"; break;
  case ERR_NOT_CURRENT_FRAME: os << "NOT_CURRENT_FRAME"; break;
  case ERR_TYPE_MISMATCH: os << "TYPE_MISMATCH"; break;
  case ERR_INVALID_SLOT: os << "INVALID_SLOT"; break;
  case ERR_DUPLICATE: os << "DUPLICATE"; break;
  case ERR_NOT_FOUND: os << "NOT_FOUND"; break;
  case ERR_INVALID_MONITOR: os << "INVALID_MONITOR"; break;
  case ERR_NOT_MONITOR_OWNER: os << "NOT_MONITOR_OWNER"; break;
  case ERR_INTERRUPT: os << "INTERRUPT"; break;
  case ERR_INVALID_CLASS_FORMAT: os << "INVALID_CLASS_FORMAT"; break;
  case ERR_CIRCULAR_CLASS_DEFINITION: os << "CIRCULAR_CLASS_DEFINITION"; break;
  case ERR_FAILS_VERIFICATION: os << "FAILS_VERIFICATION"; break;
  case ERR_ADD_METHOD_NOT_IMPLEMENTED: os << "ADD_METHOD_NOT_IMPLEMENTED"; break;
  case ERR_SCHEMA_CHANGE_NOT_IMPLEMENTED: os << "SCHEMA_CHANGE_NOT_IMPLEMENTED"; break;
  case ERR_INVALID_TYPESTATE: os << "INVALID_TYPESTATE"; break;
  case ERR_HIERARCHY_CHANGE_NOT_IMPLEMENTED: os << "HIERARCHY_CHANGE_NOT_IMPLEMENTED"; break;
  case ERR_DELETE_METHOD_NOT_IMPLEMENTED: os << "DELETE_METHOD_NOT_IMPLEMENTED"; break;
  case ERR_UNSUPPORTED_VERSION: os << "UNSUPPORTED_VERSION"; break;
  case ERR_NAMES_DONT_MATCH: os << "NAMES_DONT_MATCH"; break;
  case ERR_CLASS_MODIFIERS_CHANGE_NOT_IMPLEMENTED: os << "CLASS_MODIFIERS_CHANGE_NOT_IMPLEMENTED"; break;
  case ERR_METHOD_MODIFIERS_CHANGE_NOT_IMPLEMENTED: os << "METHOD_MODIFIERS_CHANGE_NOT_IMPLEMENTED"; break;
  case ERR_NOT_IMPLEMENTED: os << "NOT_IMPLEMENTED"; break;
  case ERR_NULL_POINTER: os << "NULL_POINTER"; break;
  case ERR_ABSENT_INFORMATION: os << "ABSENT_INFORMATION"; break;
  case ERR_INVALID_EVENT_TYPE: os << "INVALID_EVENT_TYPE"; break;
  case ERR_ILLEGAL_ARGUMENT: os << "ILLEGAL_ARGUMENT"; break;
  case ERR_OUT_OF_MEMORY: os << "OUT_OF_MEMORY"; break;
  case ERR_ACCESS_DENIED: os << "ACCESS_DENIED"; break;
  case ERR_VM_DEAD: os << "VM_DEAD"; break;
  case ERR_INTERNAL: os << "INTERNAL"; break;
  case ERR_UNATTACHED_THREAD: os << "UNATTACHED_THREAD"; break;
  case ERR_INVALID_TAG: os << "INVALID_TAG"; break;
  case ERR_ALREADY_INVOKING: os << "ALREADY_INVOKING"; break;
  case ERR_INVALID_INDEX: os << "INVALID_INDEX"; break;
  case ERR_INVALID_LENGTH: os << "INVALID_LENGTH"; break;
  case ERR_INVALID_STRING: os << "INVALID_STRING"; break;
  case ERR_INVALID_CLASS_LOADER: os << "INVALID_CLASS_LOADER"; break;
  case ERR_INVALID_ARRAY: os << "INVALID_ARRAY"; break;
  case ERR_TRANSPORT_LOAD: os << "TRANSPORT_LOAD"; break;
  case ERR_TRANSPORT_INIT: os << "TRANSPORT_INIT"; break;
  case ERR_NATIVE_METHOD: os << "NATIVE_METHOD"; break;
  case ERR_INVALID_COUNT: os << "INVALID_COUNT"; break;
  default:
    os << "JdwpError[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpEventKind& rhs) {
  switch (rhs) {
  case EK_SINGLE_STEP: os << "SINGLE_STEP"; break;
  case EK_BREAKPOINT: os << "BREAKPOINT"; break;
  case EK_FRAME_POP: os << "FRAME_POP"; break;
  case EK_EXCEPTION: os << "EXCEPTION"; break;
  case EK_USER_DEFINED: os << "USER_DEFINED"; break;
  case EK_THREAD_START: os << "THREAD_START"; break;
  case EK_THREAD_DEATH: os << "THREAD_DEATH"; break;
  case EK_CLASS_PREPARE: os << "CLASS_PREPARE"; break;
  case EK_CLASS_UNLOAD: os << "CLASS_UNLOAD"; break;
  case EK_CLASS_LOAD: os << "CLASS_LOAD"; break;
  case EK_FIELD_ACCESS: os << "FIELD_ACCESS"; break;
  case EK_FIELD_MODIFICATION: os << "FIELD_MODIFICATION"; break;
  case EK_EXCEPTION_CATCH: os << "EXCEPTION_CATCH"; break;
  case EK_METHOD_ENTRY: os << "METHOD_ENTRY"; break;
  case EK_METHOD_EXIT: os << "METHOD_EXIT"; break;
  case EK_METHOD_EXIT_WITH_RETURN_VALUE: os << "METHOD_EXIT_WITH_RETURN_VALUE"; break;
  case EK_MONITOR_CONTENDED_ENTER: os << "MONITOR_CONTENDED_ENTER"; break;
  case EK_MONITOR_CONTENDED_ENTERED: os << "MONITOR_CONTENDED_ENTERED"; break;
  case EK_MONITOR_WAIT: os << "MONITOR_WAIT"; break;
  case EK_MONITOR_WAITED: os << "MONITOR_WAITED"; break;
  case EK_VM_START: os << "VM_START"; break;
  case EK_VM_DEATH: os << "VM_DEATH"; break;
  case EK_VM_DISCONNECTED: os << "VM_DISCONNECTED"; break;
  default:
    os << "JdwpEventKind[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpModKind& rhs) {
  switch (rhs) {
  case MK_COUNT: os << "COUNT"; break;
  case MK_CONDITIONAL: os << "CONDITIONAL"; break;
  case MK_THREAD_ONLY: os << "THREAD_ONLY"; break;
  case MK_CLASS_ONLY: os << "CLASS_ONLY"; break;
  case MK_CLASS_MATCH: os << "CLASS_MATCH"; break;
  case MK_CLASS_EXCLUDE: os << "CLASS_EXCLUDE"; break;
  case MK_LOCATION_ONLY: os << "LOCATION_ONLY"; break;
  case MK_EXCEPTION_ONLY: os << "EXCEPTION_ONLY"; break;
  case MK_FIELD_ONLY: os << "FIELD_ONLY"; break;
  case MK_STEP: os << "STEP"; break;
  case MK_INSTANCE_ONLY: os << "INSTANCE_ONLY"; break;
  case MK_SOURCE_NAME_MATCH: os << "SOURCE_NAME_MATCH"; break;
  default:
    os << "JdwpModKind[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpStepDepth& rhs) {
  switch (rhs) {
  case SD_INTO: os << "INTO"; break;
  case SD_OVER: os << "OVER"; break;
  case SD_OUT: os << "OUT"; break;
  default:
    os << "JdwpStepDepth[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpStepSize& rhs) {
  switch (rhs) {
  case SS_MIN: os << "MIN"; break;
  case SS_LINE: os << "LINE"; break;
  default:
    os << "JdwpStepSize[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpSuspendPolicy& rhs) {
  switch (rhs) {
  case SP_NONE: os << "NONE"; break;
  case SP_EVENT_THREAD: os << "EVENT_THREAD"; break;
  case SP_ALL: os << "ALL"; break;
  default:
    os << "JdwpSuspendPolicy[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpSuspendStatus& rhs) {
  switch (rhs) {
  case SUSPEND_STATUS_NOT_SUSPENDED: os << "NOT SUSPENDED"; break;
  case SUSPEND_STATUS_SUSPENDED: os << "SUSPENDED"; break;
  default:
    os << "JdwpSuspendStatus[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const JdwpThreadStatus& rhs) {
  switch (rhs) {
  case TS_ZOMBIE: os << "ZOMBIE"; break;
  case TS_RUNNING: os << "RUNNING"; break;
  case TS_SLEEPING: os << "SLEEPING"; break;
  case TS_MONITOR: os << "MONITOR"; break;
  case TS_WAIT: os << "WAIT"; break;
  default:
    os << "JdwpThreadStatus[" << static_cast<int>(rhs) << "]";
    break;
  }
  return os;
}

}  // namespace JDWP

}  // namespace art

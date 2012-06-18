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
 * Handle messages from debugger.
 *
 * GENERAL NOTE: we're not currently testing the message length for
 * correctness.  This is usually a bad idea, but here we can probably
 * get away with it so long as the debugger isn't broken.  We can
 * change the "read" macros to use "dataLen" to avoid wandering into
 * bad territory, and have a single "is dataLen correct" check at the
 * end of each function.  Not needed at this time.
 */

#include "atomic.h"
#include "debugger.h"
#include "jdwp/jdwp_priv.h"
#include "jdwp/jdwp_handler.h"
#include "jdwp/jdwp_event.h"
#include "jdwp/jdwp_constants.h"
#include "jdwp/jdwp_expand_buf.h"
#include "logging.h"
#include "macros.h"
#include "stringprintf.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace art {

namespace JDWP {

/*
 * Helper function: read a "location" from an input buffer.
 */
static void jdwpReadLocation(const uint8_t** pBuf, JdwpLocation* pLoc) {
  memset(pLoc, 0, sizeof(*pLoc));     /* allows memcmp() later */
  pLoc->typeTag = ReadTypeTag(pBuf);
  pLoc->classId = ReadObjectId(pBuf);
  pLoc->methodId = ReadMethodId(pBuf);
  pLoc->dex_pc = Read8BE(pBuf);
}

/*
 * Helper function: write a "location" into the reply buffer.
 */
void AddLocation(ExpandBuf* pReply, const JdwpLocation* pLoc) {
  expandBufAdd1(pReply, pLoc->typeTag);
  expandBufAddObjectId(pReply, pLoc->classId);
  expandBufAddMethodId(pReply, pLoc->methodId);
  expandBufAdd8BE(pReply, pLoc->dex_pc);
}

/*
 * Helper function: read a variable-width value from the input buffer.
 */
static uint64_t jdwpReadValue(const uint8_t** pBuf, size_t width) {
  uint64_t value = -1;
  switch (width) {
  case 1:     value = Read1(pBuf); break;
  case 2:     value = Read2BE(pBuf); break;
  case 4:     value = Read4BE(pBuf); break;
  case 8:     value = Read8BE(pBuf); break;
  default:    LOG(FATAL) << width; break;
  }
  return value;
}

/*
 * Helper function: write a variable-width value into the output input buffer.
 */
static void jdwpWriteValue(ExpandBuf* pReply, int width, uint64_t value) {
  switch (width) {
  case 1:     expandBufAdd1(pReply, value); break;
  case 2:     expandBufAdd2BE(pReply, value); break;
  case 4:     expandBufAdd4BE(pReply, value); break;
  case 8:     expandBufAdd8BE(pReply, value); break;
  default:    LOG(FATAL) << width; break;
  }
}

/*
 * Common code for *_InvokeMethod requests.
 *
 * If "is_constructor" is set, this returns "objectId" rather than the
 * expected-to-be-void return value of the called function.
 */
static JdwpError finishInvoke(JdwpState*,
    const uint8_t* buf, int, ExpandBuf* pReply,
    ObjectId threadId, ObjectId objectId, RefTypeId classId, MethodId methodId,
    bool is_constructor)
{
  CHECK(!is_constructor || objectId != 0);

  uint32_t arg_count = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("    --> threadId=%#llx objectId=%#llx", threadId, objectId);
  VLOG(jdwp) << StringPrintf("        classId=%#llx methodId=%x %s.%s", classId, methodId, Dbg::GetClassName(classId).c_str(), Dbg::GetMethodName(classId, methodId).c_str());
  VLOG(jdwp) << StringPrintf("        %d args:", arg_count);

  UniquePtr<JdwpTag[]> argTypes(arg_count > 0 ? new JdwpTag[arg_count] : NULL);
  UniquePtr<uint64_t[]> argValues(arg_count > 0 ? new uint64_t[arg_count] : NULL);
  for (uint32_t i = 0; i < arg_count; ++i) {
    argTypes[i] = ReadTag(&buf);
    size_t width = Dbg::GetTagWidth(argTypes[i]);
    argValues[i] = jdwpReadValue(&buf, width);
    VLOG(jdwp) << "          " << argTypes[i] << StringPrintf("(%zd): %#llx", width, argValues[i]);
  }

  uint32_t options = Read4BE(&buf);  /* enum InvokeOptions bit flags */
  VLOG(jdwp) << StringPrintf("        options=0x%04x%s%s", options, (options & INVOKE_SINGLE_THREADED) ? " (SINGLE_THREADED)" : "", (options & INVOKE_NONVIRTUAL) ? " (NONVIRTUAL)" : "");

  JdwpTag resultTag;
  uint64_t resultValue;
  ObjectId exceptObjId;
  JdwpError err = Dbg::InvokeMethod(threadId, objectId, classId, methodId, arg_count, argValues.get(), argTypes.get(), options, &resultTag, &resultValue, &exceptObjId);
  if (err != ERR_NONE) {
    return err;
  }

  if (err == ERR_NONE) {
    if (is_constructor) {
      // If we invoked a constructor (which actually returns void), return the receiver,
      // unless we threw, in which case we return NULL.
      resultTag = JT_OBJECT;
      resultValue = (exceptObjId == 0) ? objectId : 0;
    }

    size_t width = Dbg::GetTagWidth(resultTag);
    expandBufAdd1(pReply, resultTag);
    if (width != 0) {
      jdwpWriteValue(pReply, width, resultValue);
    }
    expandBufAdd1(pReply, JT_OBJECT);
    expandBufAddObjectId(pReply, exceptObjId);

    VLOG(jdwp) << "  --> returned " << resultTag << StringPrintf(" %#llx (except=%#llx)", resultValue, exceptObjId);

    /* show detailed debug output */
    if (resultTag == JT_STRING && exceptObjId == 0) {
      if (resultValue != 0) {
        VLOG(jdwp) << "      string '" << Dbg::StringToUtf8(resultValue) << "'";
      } else {
        VLOG(jdwp) << "      string (null)";
      }
    }
  }

  return err;
}


/*
 * Request for version info.
 */
static JdwpError handleVM_Version(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  /* text information on runtime version */
  std::string version(StringPrintf("Android Runtime %s", Runtime::Current()->GetVersion()));
  expandBufAddUtf8String(pReply, version);
  /* JDWP version numbers */
  expandBufAdd4BE(pReply, 1);        // major
  expandBufAdd4BE(pReply, 5);        // minor
  /* VM JRE version */
  expandBufAddUtf8String(pReply, "1.6.0");  /* e.g. 1.6.0_22 */
  /* target VM name */
  expandBufAddUtf8String(pReply, "DalvikVM");

  return ERR_NONE;
}

/*
 * Given a class JNI signature (e.g. "Ljava/lang/Error;"), return the
 * referenceTypeID.  We need to send back more than one if the class has
 * been loaded by multiple class loaders.
 */
static JdwpError handleVM_ClassesBySignature(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  std::string classDescriptor(ReadNewUtf8String(&buf));
  VLOG(jdwp) << "  Req for class by signature '" << classDescriptor << "'";

  std::vector<RefTypeId> ids;
  Dbg::FindLoadedClassBySignature(classDescriptor.c_str(), ids);

  expandBufAdd4BE(pReply, ids.size());

  for (size_t i = 0; i < ids.size(); ++i) {
    // Get class vs. interface and status flags.
    JDWP::JdwpTypeTag type_tag;
    uint32_t class_status;
    JDWP::JdwpError status = Dbg::GetClassInfo(ids[i], &type_tag, &class_status, NULL);
    if (status != ERR_NONE) {
      return status;
    }

    expandBufAdd1(pReply, type_tag);
    expandBufAddRefTypeId(pReply, ids[i]);
    expandBufAdd4BE(pReply, class_status);
  }

  return ERR_NONE;
}

/*
 * Handle request for the thread IDs of all running threads.
 *
 * We exclude ourselves from the list, because we don't allow ourselves
 * to be suspended, and that violates some JDWP expectations.
 */
static JdwpError handleVM_AllThreads(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  ObjectId* pThreadIds;
  uint32_t threadCount;
  Dbg::GetAllThreads(&pThreadIds, &threadCount);

  expandBufAdd4BE(pReply, threadCount);

  ObjectId* walker = pThreadIds;
  for (uint32_t i = 0; i < threadCount; i++) {
    expandBufAddObjectId(pReply, *walker++);
  }

  free(pThreadIds);

  return ERR_NONE;
}

/*
 * List all thread groups that do not have a parent.
 */
static JdwpError handleVM_TopLevelThreadGroups(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  /*
   * TODO: maintain a list of parentless thread groups in the VM.
   *
   * For now, just return "system".  Application threads are created
   * in "main", which is a child of "system".
   */
  uint32_t groups = 1;
  expandBufAdd4BE(pReply, groups);
  //threadGroupId = debugGetMainThreadGroup();
  //expandBufAdd8BE(pReply, threadGroupId);
  ObjectId threadGroupId = Dbg::GetSystemThreadGroupId();
  expandBufAddObjectId(pReply, threadGroupId);

  return ERR_NONE;
}

/*
 * Respond with the sizes of the basic debugger types.
 *
 * All IDs are 8 bytes.
 */
static JdwpError handleVM_IDSizes(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  expandBufAdd4BE(pReply, sizeof(FieldId));
  expandBufAdd4BE(pReply, sizeof(MethodId));
  expandBufAdd4BE(pReply, sizeof(ObjectId));
  expandBufAdd4BE(pReply, sizeof(RefTypeId));
  expandBufAdd4BE(pReply, sizeof(FrameId));
  return ERR_NONE;
}

static JdwpError handleVM_Dispose(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  Dbg::Disposed();
  return ERR_NONE;
}

/*
 * Suspend the execution of the application running in the VM (i.e. suspend
 * all threads).
 *
 * This needs to increment the "suspend count" on all threads.
 */
static JdwpError handleVM_Suspend(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  Dbg::SuspendVM();
  return ERR_NONE;
}

/*
 * Resume execution.  Decrements the "suspend count" of all threads.
 */
static JdwpError handleVM_Resume(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  Dbg::ResumeVM();
  return ERR_NONE;
}

/*
 * The debugger wants the entire VM to exit.
 */
static JdwpError handleVM_Exit(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  uint32_t exitCode = Get4BE(buf);

  LOG(WARNING) << "Debugger is telling the VM to exit with code=" << exitCode;

  Dbg::Exit(exitCode);
  return ERR_NOT_IMPLEMENTED;     // shouldn't get here
}

/*
 * Create a new string in the VM and return its ID.
 *
 * (Ctrl-Shift-I in Eclipse on an array of objects causes it to create the
 * string "java.util.Arrays".)
 */
static JdwpError handleVM_CreateString(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  std::string str(ReadNewUtf8String(&buf));
  VLOG(jdwp) << "  Req to create string '" << str << "'";
  ObjectId stringId = Dbg::CreateString(str);
  if (stringId == 0) {
    return ERR_OUT_OF_MEMORY;
  }
  expandBufAddObjectId(pReply, stringId);
  return ERR_NONE;
}

/*
 * Tell the debugger what we are capable of.
 */
static JdwpError handleVM_Capabilities(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  expandBufAdd1(pReply, false);   /* canWatchFieldModification */
  expandBufAdd1(pReply, false);   /* canWatchFieldAccess */
  expandBufAdd1(pReply, false);   /* canGetBytecodes */
  expandBufAdd1(pReply, true);    /* canGetSyntheticAttribute */
  expandBufAdd1(pReply, false);   /* canGetOwnedMonitorInfo */
  expandBufAdd1(pReply, false);   /* canGetCurrentContendedMonitor */
  expandBufAdd1(pReply, false);   /* canGetMonitorInfo */
  return ERR_NONE;
}

static JdwpError handleVM_ClassPaths(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  expandBufAddUtf8String(pReply, "/");

  std::vector<std::string> class_path;
  Split(Runtime::Current()->GetClassPathString(), ':', class_path);
  expandBufAdd4BE(pReply, class_path.size());
  for (size_t i = 0; i < class_path.size(); ++i) {
    expandBufAddUtf8String(pReply, class_path[i]);
  }

  std::vector<std::string> boot_class_path;
  Split(Runtime::Current()->GetBootClassPathString(), ':', boot_class_path);
  expandBufAdd4BE(pReply, boot_class_path.size());
  for (size_t i = 0; i < boot_class_path.size(); ++i) {
    expandBufAddUtf8String(pReply, boot_class_path[i]);
  }

  return ERR_NONE;
}

/*
 * Release a list of object IDs.  (Seen in jdb.)
 *
 * Currently does nothing.
 */
static JdwpError HandleVM_DisposeObjects(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  return ERR_NONE;
}

/*
 * Tell the debugger what we are capable of.
 */
static JdwpError handleVM_CapabilitiesNew(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  expandBufAdd1(pReply, false);   /* canWatchFieldModification */
  expandBufAdd1(pReply, false);   /* canWatchFieldAccess */
  expandBufAdd1(pReply, false);   /* canGetBytecodes */
  expandBufAdd1(pReply, true);    /* canGetSyntheticAttribute */
  expandBufAdd1(pReply, false);   /* canGetOwnedMonitorInfo */
  expandBufAdd1(pReply, false);   /* canGetCurrentContendedMonitor */
  expandBufAdd1(pReply, false);   /* canGetMonitorInfo */
  expandBufAdd1(pReply, false);   /* canRedefineClasses */
  expandBufAdd1(pReply, false);   /* canAddMethod */
  expandBufAdd1(pReply, false);   /* canUnrestrictedlyRedefineClasses */
  expandBufAdd1(pReply, false);   /* canPopFrames */
  expandBufAdd1(pReply, false);   /* canUseInstanceFilters */
  expandBufAdd1(pReply, false);   /* canGetSourceDebugExtension */
  expandBufAdd1(pReply, false);   /* canRequestVMDeathEvent */
  expandBufAdd1(pReply, false);   /* canSetDefaultStratum */
  expandBufAdd1(pReply, false);   /* 1.6: canGetInstanceInfo */
  expandBufAdd1(pReply, false);   /* 1.6: canRequestMonitorEvents */
  expandBufAdd1(pReply, false);   /* 1.6: canGetMonitorFrameInfo */
  expandBufAdd1(pReply, false);   /* 1.6: canUseSourceNameFilters */
  expandBufAdd1(pReply, false);   /* 1.6: canGetConstantPool */
  expandBufAdd1(pReply, false);   /* 1.6: canForceEarlyReturn */

  /* fill in reserved22 through reserved32; note count started at 1 */
  for (int i = 22; i <= 32; i++) {
    expandBufAdd1(pReply, false);   /* reservedN */
  }
  return ERR_NONE;
}

static JdwpError handleVM_AllClasses(ExpandBuf* pReply, bool descriptor_and_status, bool generic) {
  std::vector<JDWP::RefTypeId> classes;
  Dbg::GetClassList(classes);

  expandBufAdd4BE(pReply, classes.size());

  for (size_t i = 0; i < classes.size(); ++i) {
    static const char genericSignature[1] = "";
    JDWP::JdwpTypeTag type_tag;
    std::string descriptor;
    uint32_t class_status;
    JDWP::JdwpError status = Dbg::GetClassInfo(classes[i], &type_tag, &class_status, &descriptor);
    if (status != ERR_NONE) {
      return status;
    }

    expandBufAdd1(pReply, type_tag);
    expandBufAddRefTypeId(pReply, classes[i]);
    if (descriptor_and_status) {
      expandBufAddUtf8String(pReply, descriptor);
      if (generic) {
        expandBufAddUtf8String(pReply, genericSignature);
      }
      expandBufAdd4BE(pReply, class_status);
    }
  }

  return ERR_NONE;
}

static JdwpError handleVM_AllClasses(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  return handleVM_AllClasses(pReply, true, false);
}

static JdwpError handleVM_AllClassesWithGeneric(JdwpState*, const uint8_t*, int, ExpandBuf* pReply) {
  return handleVM_AllClasses(pReply, true, true);
}

static JdwpError handleRT_Modifiers(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  return Dbg::GetModifiers(refTypeId, pReply);
}

/*
 * Get values from static fields in a reference type.
 */
static JdwpError handleRT_GetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  uint32_t field_count = Read4BE(&buf);
  expandBufAdd4BE(pReply, field_count);
  for (uint32_t i = 0; i < field_count; i++) {
    FieldId fieldId = ReadFieldId(&buf);
    JdwpError status = Dbg::GetStaticFieldValue(refTypeId, fieldId, pReply);
    if (status != ERR_NONE) {
      return status;
    }
  }
  return ERR_NONE;
}

/*
 * Get the name of the source file in which a reference type was declared.
 */
static JdwpError handleRT_SourceFile(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  std::string source_file;
  JdwpError status = Dbg::GetSourceFile(refTypeId, source_file);
  if (status != ERR_NONE) {
    return status;
  }
  expandBufAddUtf8String(pReply, source_file);
  return ERR_NONE;
}

/*
 * Return the current status of the reference type.
 */
static JdwpError handleRT_Status(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  JDWP::JdwpTypeTag type_tag;
  uint32_t class_status;
  JDWP::JdwpError status = Dbg::GetClassInfo(refTypeId, &type_tag, &class_status, NULL);
  if (status != ERR_NONE) {
    return status;
  }
  expandBufAdd4BE(pReply, class_status);
  return ERR_NONE;
}

/*
 * Return interfaces implemented directly by this class.
 */
static JdwpError handleRT_Interfaces(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  VLOG(jdwp) << StringPrintf("  Req for interfaces in %#llx (%s)", refTypeId, Dbg::GetClassName(refTypeId).c_str());
  return Dbg::OutputDeclaredInterfaces(refTypeId, pReply);
}

/*
 * Return the class object corresponding to this type.
 */
static JdwpError handleRT_ClassObject(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  ObjectId classObjectId;
  JdwpError status = Dbg::GetClassObject(refTypeId, classObjectId);
  if (status != ERR_NONE) {
    return status;
  }
  VLOG(jdwp) << StringPrintf("  RefTypeId %#llx -> ObjectId %#llx", refTypeId, classObjectId);
  expandBufAddObjectId(pReply, classObjectId);
  return ERR_NONE;
}

/*
 * Returns the value of the SourceDebugExtension attribute.
 *
 * JDB seems interested, but DEX files don't currently support this.
 */
static JdwpError handleRT_SourceDebugExtension(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  /* referenceTypeId in, string out */
  return ERR_ABSENT_INFORMATION;
}

static JdwpError handleRT_Signature(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply, bool with_generic) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  VLOG(jdwp) << StringPrintf("  Req for signature of refTypeId=%#llx", refTypeId);
  std::string signature;

  JdwpError status = Dbg::GetSignature(refTypeId, signature);
  if (status != ERR_NONE) {
    return status;
  }
  expandBufAddUtf8String(pReply, signature);
  if (with_generic) {
    expandBufAddUtf8String(pReply, "");
  }
  return ERR_NONE;
}

static JdwpError handleRT_Signature(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  return handleRT_Signature(state, buf, dataLen, pReply, false);
}

static JdwpError handleRT_SignatureWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  return handleRT_Signature(state, buf, dataLen, pReply, true);
}

/*
 * Return the instance of java.lang.ClassLoader that loaded the specified
 * reference type, or null if it was loaded by the system loader.
 */
static JdwpError handleRT_ClassLoader(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  return Dbg::GetClassLoader(refTypeId, pReply);
}

static std::string Describe(const RefTypeId& refTypeId) {
  std::string signature("unknown");
  Dbg::GetSignature(refTypeId, signature);
  return StringPrintf("refTypeId=%#llx (%s)", refTypeId, signature.c_str());
}

/*
 * Given a referenceTypeId, return a block of stuff that describes the
 * fields declared by a class.
 */
static JdwpError handleRT_FieldsWithGeneric(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  VLOG(jdwp) << "  Req for fields in " << Describe(refTypeId);
  return Dbg::OutputDeclaredFields(refTypeId, true, pReply);
}

// Obsolete equivalent of FieldsWithGeneric, without the generic type information.
static JdwpError handleRT_Fields(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  VLOG(jdwp) << "  Req for fields in " << Describe(refTypeId);
  return Dbg::OutputDeclaredFields(refTypeId, false, pReply);
}

/*
 * Given a referenceTypeID, return a block of goodies describing the
 * methods declared by a class.
 */
static JdwpError handleRT_MethodsWithGeneric(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  VLOG(jdwp) << "  Req for methods in " << Describe(refTypeId);
  return Dbg::OutputDeclaredMethods(refTypeId, true, pReply);
}

// Obsolete equivalent of MethodsWithGeneric, without the generic type information.
static JdwpError handleRT_Methods(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  VLOG(jdwp) << "  Req for methods in " << Describe(refTypeId);
  return Dbg::OutputDeclaredMethods(refTypeId, false, pReply);
}

/*
 * Return the immediate superclass of a class.
 */
static JdwpError handleCT_Superclass(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId classId = ReadRefTypeId(&buf);
  RefTypeId superClassId;
  JdwpError status = Dbg::GetSuperclass(classId, superClassId);
  if (status != ERR_NONE) {
    return status;
  }
  expandBufAddRefTypeId(pReply, superClassId);
  return ERR_NONE;
}

/*
 * Set static class values.
 */
static JdwpError handleCT_SetValues(JdwpState* , const uint8_t* buf, int, ExpandBuf*) {
  RefTypeId classId = ReadRefTypeId(&buf);
  uint32_t values = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req to set %d values in classId=%#llx", values, classId);

  for (uint32_t i = 0; i < values; i++) {
    FieldId fieldId = ReadFieldId(&buf);
    JDWP::JdwpTag fieldTag = Dbg::GetStaticFieldBasicTag(fieldId);
    size_t width = Dbg::GetTagWidth(fieldTag);
    uint64_t value = jdwpReadValue(&buf, width);

    VLOG(jdwp) << "    --> field=" << fieldId << " tag=" << fieldTag << " -> " << value;
    JdwpError status = Dbg::SetStaticFieldValue(fieldId, value, width);
    if (status != ERR_NONE) {
      return status;
    }
  }

  return ERR_NONE;
}

/*
 * Invoke a static method.
 *
 * Example: Eclipse sometimes uses java/lang/Class.forName(String s) on
 * values in the "variables" display.
 */
static JdwpError handleCT_InvokeMethod(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId classId = ReadRefTypeId(&buf);
  ObjectId threadId = ReadObjectId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  return finishInvoke(state, buf, dataLen, pReply, threadId, 0, classId, methodId, false);
}

/*
 * Create a new object of the requested type, and invoke the specified
 * constructor.
 *
 * Example: in IntelliJ, create a watch on "new String(myByteArray)" to
 * see the contents of a byte[] as a string.
 */
static JdwpError handleCT_NewInstance(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId classId = ReadRefTypeId(&buf);
  ObjectId threadId = ReadObjectId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  VLOG(jdwp) << "Creating instance of " << Dbg::GetClassName(classId);
  ObjectId objectId;
  JdwpError status = Dbg::CreateObject(classId, objectId);
  if (status != ERR_NONE) {
    return status;
  }
  if (objectId == 0) {
    return ERR_OUT_OF_MEMORY;
  }
  return finishInvoke(state, buf, dataLen, pReply, threadId, objectId, classId, methodId, true);
}

/*
 * Create a new array object of the requested type and length.
 */
static JdwpError handleAT_newInstance(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId arrayTypeId = ReadRefTypeId(&buf);
  uint32_t length = Read4BE(&buf);

  VLOG(jdwp) << "Creating array " << Dbg::GetClassName(arrayTypeId) << "[" << length << "]";
  ObjectId objectId;
  JdwpError status = Dbg::CreateArrayObject(arrayTypeId, length, objectId);
  if (status != ERR_NONE) {
    return status;
  }
  if (objectId == 0) {
    return ERR_OUT_OF_MEMORY;
  }
  expandBufAdd1(pReply, JT_ARRAY);
  expandBufAddObjectId(pReply, objectId);
  return ERR_NONE;
}

/*
 * Return line number information for the method, if present.
 */
static JdwpError handleM_LineTable(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  VLOG(jdwp) << "  Req for line table in " << Dbg::GetClassName(refTypeId) << "." << Dbg::GetMethodName(refTypeId,methodId);

  Dbg::OutputLineTable(refTypeId, methodId, pReply);

  return ERR_NONE;
}

static JdwpError handleM_VariableTable(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply, bool generic) {
  RefTypeId classId = ReadRefTypeId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  VLOG(jdwp) << StringPrintf("  Req for LocalVarTab in class=%s method=%s", Dbg::GetClassName(classId).c_str(), Dbg::GetMethodName(classId, methodId).c_str());

  // We could return ERR_ABSENT_INFORMATION here if the DEX file was built without local variable
  // information. That will cause Eclipse to make a best-effort attempt at displaying local
  // variables anonymously. However, the attempt isn't very good, so we're probably better off just
  // not showing anything.
  Dbg::OutputVariableTable(classId, methodId, generic, pReply);
  return ERR_NONE;
}

static JdwpError handleM_VariableTable(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  return handleM_VariableTable(state, buf, dataLen, pReply, false);
}

static JdwpError handleM_VariableTableWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  return handleM_VariableTable(state, buf, dataLen, pReply, true);
}

/*
 * Given an object reference, return the runtime type of the object
 * (class or array).
 *
 * This can get called on different things, e.g. threadId gets
 * passed in here.
 */
static JdwpError handleOR_ReferenceType(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId objectId = ReadObjectId(&buf);
  VLOG(jdwp) << StringPrintf("  Req for type of objectId=%#llx", objectId);
  return Dbg::GetReferenceType(objectId, pReply);
}

/*
 * Get values from the fields of an object.
 */
static JdwpError handleOR_GetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId objectId = ReadObjectId(&buf);
  uint32_t field_count = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req for %d fields from objectId=%#llx", field_count, objectId);

  expandBufAdd4BE(pReply, field_count);

  for (uint32_t i = 0; i < field_count; i++) {
    FieldId fieldId = ReadFieldId(&buf);
    JdwpError status = Dbg::GetFieldValue(objectId, fieldId, pReply);
    if (status != ERR_NONE) {
      return status;
    }
  }

  return ERR_NONE;
}

/*
 * Set values in the fields of an object.
 */
static JdwpError handleOR_SetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  ObjectId objectId = ReadObjectId(&buf);
  uint32_t field_count = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req to set %d fields in objectId=%#llx", field_count, objectId);

  for (uint32_t i = 0; i < field_count; i++) {
    FieldId fieldId = ReadFieldId(&buf);

    JDWP::JdwpTag fieldTag = Dbg::GetFieldBasicTag(fieldId);
    size_t width = Dbg::GetTagWidth(fieldTag);
    uint64_t value = jdwpReadValue(&buf, width);

    VLOG(jdwp) << "    --> fieldId=" << fieldId << " tag=" << fieldTag << "(" << width << ") value=" << value;
    JdwpError status = Dbg::SetFieldValue(objectId, fieldId, value, width);
    if (status != ERR_NONE) {
      return status;
    }
  }

  return ERR_NONE;
}

/*
 * Invoke an instance method.  The invocation must occur in the specified
 * thread, which must have been suspended by an event.
 *
 * The call is synchronous.  All threads in the VM are resumed, unless the
 * SINGLE_THREADED flag is set.
 *
 * If you ask Eclipse to "inspect" an object (or ask JDB to "print" an
 * object), it will try to invoke the object's toString() function.  This
 * feature becomes crucial when examining ArrayLists with Eclipse.
 */
static JdwpError handleOR_InvokeMethod(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId objectId = ReadObjectId(&buf);
  ObjectId threadId = ReadObjectId(&buf);
  RefTypeId classId = ReadRefTypeId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  return finishInvoke(state, buf, dataLen, pReply, threadId, objectId, classId, methodId, false);
}

/*
 * Disable garbage collection of the specified object.
 */
static JdwpError handleOR_DisableCollection(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  // this is currently a no-op
  return ERR_NONE;
}

/*
 * Enable garbage collection of the specified object.
 */
static JdwpError handleOR_EnableCollection(JdwpState*, const uint8_t*, int, ExpandBuf*) {
  // this is currently a no-op
  return ERR_NONE;
}

/*
 * Determine whether an object has been garbage collected.
 */
static JdwpError handleOR_IsCollected(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId objectId;

  objectId = ReadObjectId(&buf);
  VLOG(jdwp) << StringPrintf("  Req IsCollected(%#llx)", objectId);

  // TODO: currently returning false; must integrate with GC
  expandBufAdd1(pReply, 0);

  return ERR_NONE;
}

/*
 * Return the string value in a string object.
 */
static JdwpError handleSR_Value(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId stringObject = ReadObjectId(&buf);
  std::string str(Dbg::StringToUtf8(stringObject));

  VLOG(jdwp) << StringPrintf("  Req for str %#llx --> %s", stringObject, PrintableString(str).c_str());

  expandBufAddUtf8String(pReply, str);

  return ERR_NONE;
}

/*
 * Return a thread's name.
 */
static JdwpError handleTR_Name(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  VLOG(jdwp) << StringPrintf("  Req for name of thread %#llx", threadId);
  std::string name;
  if (!Dbg::GetThreadName(threadId, name)) {
    return ERR_INVALID_THREAD;
  }
  VLOG(jdwp) << StringPrintf("  Name of thread %#llx is \"%s\"", threadId, name.c_str());
  expandBufAddUtf8String(pReply, name);

  return ERR_NONE;
}

/*
 * Suspend the specified thread.
 *
 * It's supposed to remain suspended even if interpreted code wants to
 * resume it; only the JDI is allowed to resume it.
 */
static JdwpError handleTR_Suspend(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  ObjectId threadId = ReadObjectId(&buf);

  if (threadId == Dbg::GetThreadSelfId()) {
    LOG(INFO) << "  Warning: ignoring request to suspend self";
    return ERR_THREAD_NOT_SUSPENDED;
  }
  VLOG(jdwp) << StringPrintf("  Req to suspend thread %#llx", threadId);
  Dbg::SuspendThread(threadId);
  return ERR_NONE;
}

/*
 * Resume the specified thread.
 */
static JdwpError handleTR_Resume(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  ObjectId threadId = ReadObjectId(&buf);

  if (threadId == Dbg::GetThreadSelfId()) {
    LOG(INFO) << "  Warning: ignoring request to resume self";
    return ERR_NONE;
  }
  VLOG(jdwp) << StringPrintf("  Req to resume thread %#llx", threadId);
  Dbg::ResumeThread(threadId);
  return ERR_NONE;
}

/*
 * Return status of specified thread.
 */
static JdwpError handleTR_Status(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  VLOG(jdwp) << StringPrintf("  Req for status of thread %#llx", threadId);

  JDWP::JdwpThreadStatus threadStatus;
  JDWP::JdwpSuspendStatus suspendStatus;
  if (!Dbg::GetThreadStatus(threadId, &threadStatus, &suspendStatus)) {
    return ERR_INVALID_THREAD;
  }

  VLOG(jdwp) << "    --> " << threadStatus << ", " << suspendStatus;

  expandBufAdd4BE(pReply, threadStatus);
  expandBufAdd4BE(pReply, suspendStatus);

  return ERR_NONE;
}

/*
 * Return the thread group that the specified thread is a member of.
 */
static JdwpError handleTR_ThreadGroup(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  return Dbg::GetThreadGroup(threadId, pReply);
}

/*
 * Return the current call stack of a suspended thread.
 *
 * If the thread isn't suspended, the error code isn't defined, but should
 * be THREAD_NOT_SUSPENDED.
 */
static JdwpError handleTR_Frames(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  uint32_t start_frame = Read4BE(&buf);
  uint32_t length = Read4BE(&buf);

  if (!Dbg::ThreadExists(threadId)) {
    return ERR_INVALID_THREAD;
  }
  if (!Dbg::IsSuspended(threadId)) {
    LOG(WARNING) << StringPrintf("  Rejecting req for frames in running thread %#llx", threadId);
    return ERR_THREAD_NOT_SUSPENDED;
  }

  size_t actual_frame_count = Dbg::GetThreadFrameCount(threadId);

  VLOG(jdwp) << StringPrintf("  Request for frames: threadId=%#llx start=%d length=%d [count=%zd]", threadId, start_frame, length, actual_frame_count);
  if (actual_frame_count <= 0) {
    return ERR_THREAD_NOT_SUSPENDED;    /* == 0 means 100% native */
  }

  if (start_frame > actual_frame_count) {
    return ERR_INVALID_INDEX;
  }
  if (length == static_cast<uint32_t>(-1)) {
    length = actual_frame_count - start_frame;
  }
  if (start_frame + length > actual_frame_count) {
    return ERR_INVALID_LENGTH;
  }

  expandBufAdd4BE(pReply, length);
  for (uint32_t i = start_frame; i < (start_frame + length); ++i) {
    FrameId frameId;
    JdwpLocation loc;
    // TODO: switch to GetThreadFrames so we don't have to search for each frame
    // even though we only want them in order.
    Dbg::GetThreadFrame(threadId, i, &frameId, &loc);

    expandBufAdd8BE(pReply, frameId);
    AddLocation(pReply, &loc);

    VLOG(jdwp) << StringPrintf("    Frame %d: id=%#llx ", i, frameId) << loc;
  }

  return ERR_NONE;
}

/*
 * Returns the #of frames on the specified thread, which must be suspended.
 */
static JdwpError handleTR_FrameCount(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  if (!Dbg::ThreadExists(threadId)) {
    return ERR_INVALID_THREAD;
  }
  if (!Dbg::IsSuspended(threadId)) {
    LOG(WARNING) << StringPrintf("  Rejecting req for frames in running thread %#llx", threadId);
    return ERR_THREAD_NOT_SUSPENDED;
  }

  int frame_count = Dbg::GetThreadFrameCount(threadId);
  if (frame_count < 0) {
    return ERR_INVALID_THREAD;
  }
  expandBufAdd4BE(pReply, static_cast<uint32_t>(frame_count));

  return ERR_NONE;
}

/*
 * Get the monitor that the thread is waiting on.
 */
static JdwpError handleTR_CurrentContendedMonitor(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  ReadObjectId(&buf);  // threadId

  // TODO: create an Object to represent the monitor (we're currently
  // just using a raw Monitor struct in the VM)

  return ERR_NOT_IMPLEMENTED;
}

/*
 * Return the suspend count for the specified thread.
 *
 * (The thread *might* still be running -- it might not have examined
 * its suspend count recently.)
 */
static JdwpError handleTR_SuspendCount(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  return Dbg::GetThreadSuspendCount(threadId, pReply);
}

/*
 * Return the name of a thread group.
 *
 * The Eclipse debugger recognizes "main" and "system" as special.
 */
static JdwpError handleTGR_Name(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadGroupId = ReadObjectId(&buf);
  VLOG(jdwp) << StringPrintf("  Req for name of threadGroupId=%#llx", threadGroupId);

  expandBufAddUtf8String(pReply, Dbg::GetThreadGroupName(threadGroupId));

  return ERR_NONE;
}

/*
 * Returns the thread group -- if any -- that contains the specified
 * thread group.
 */
static JdwpError handleTGR_Parent(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId groupId = ReadObjectId(&buf);

  ObjectId parentGroup = Dbg::GetThreadGroupParent(groupId);
  expandBufAddObjectId(pReply, parentGroup);

  return ERR_NONE;
}

/*
 * Return the active threads and thread groups that are part of the
 * specified thread group.
 */
static JdwpError handleTGR_Children(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadGroupId = ReadObjectId(&buf);
  VLOG(jdwp) << StringPrintf("  Req for threads in threadGroupId=%#llx", threadGroupId);

  ObjectId* pThreadIds;
  uint32_t threadCount;
  Dbg::GetThreadGroupThreads(threadGroupId, &pThreadIds, &threadCount);

  expandBufAdd4BE(pReply, threadCount);

  for (uint32_t i = 0; i < threadCount; i++) {
    expandBufAddObjectId(pReply, pThreadIds[i]);
  }
  free(pThreadIds);

  /*
   * TODO: finish support for child groups
   *
   * For now, just show that "main" is a child of "system".
   */
  if (threadGroupId == Dbg::GetSystemThreadGroupId()) {
    expandBufAdd4BE(pReply, 1);
    expandBufAddObjectId(pReply, Dbg::GetMainThreadGroupId());
  } else {
    expandBufAdd4BE(pReply, 0);
  }

  return ERR_NONE;
}

/*
 * Return the #of components in the array.
 */
static JdwpError handleAR_Length(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId arrayId = ReadObjectId(&buf);
  VLOG(jdwp) << StringPrintf("  Req for length of array %#llx", arrayId);

  int length;
  JdwpError status = Dbg::GetArrayLength(arrayId, length);
  if (status != ERR_NONE) {
    return status;
  }
  VLOG(jdwp) << "    --> " << length;

  expandBufAdd4BE(pReply, length);

  return ERR_NONE;
}

/*
 * Return the values from an array.
 */
static JdwpError handleAR_GetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId arrayId = ReadObjectId(&buf);
  uint32_t firstIndex = Read4BE(&buf);
  uint32_t length = Read4BE(&buf);
  VLOG(jdwp) << StringPrintf("  Req for array values %#llx first=%d len=%d", arrayId, firstIndex, length);

  return Dbg::OutputArray(arrayId, firstIndex, length, pReply);
}

/*
 * Set values in an array.
 */
static JdwpError handleAR_SetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  ObjectId arrayId = ReadObjectId(&buf);
  uint32_t firstIndex = Read4BE(&buf);
  uint32_t values = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req to set array values %#llx first=%d count=%d", arrayId, firstIndex, values);

  return Dbg::SetArrayElements(arrayId, firstIndex, values, buf);
}

static JdwpError handleCLR_VisibleClasses(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ReadObjectId(&buf);  // classLoaderObject
  // TODO: we should only return classes which have the given class loader as a defining or
  // initiating loader. The former would be easy; the latter is hard, because we don't have
  // any such notion.
  return handleVM_AllClasses(pReply, false, false);
}

/*
 * Set an event trigger.
 *
 * Reply with a requestID.
 */
static JdwpError handleER_Set(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  const uint8_t* origBuf = buf;

  uint8_t eventKind = Read1(&buf);
  uint8_t suspend_policy = Read1(&buf);
  uint32_t modifierCount = Read4BE(&buf);

  VLOG(jdwp) << "  Set(kind=" << JdwpEventKind(eventKind)
               << " suspend=" << JdwpSuspendPolicy(suspend_policy)
               << " mods=" << modifierCount << ")";

  CHECK_LT(modifierCount, 256U);    /* reasonableness check */

  JdwpEvent* pEvent = EventAlloc(modifierCount);
  pEvent->eventKind = static_cast<JdwpEventKind>(eventKind);
  pEvent->suspend_policy = static_cast<JdwpSuspendPolicy>(suspend_policy);
  pEvent->modCount = modifierCount;

  /*
   * Read modifiers.  Ordering may be significant (see explanation of Count
   * mods in JDWP doc).
   */
  for (uint32_t i = 0; i < modifierCount; ++i) {
    JdwpEventMod& mod = pEvent->mods[i];
    mod.modKind = static_cast<JdwpModKind>(Read1(&buf));
    switch (mod.modKind) {
    case MK_COUNT:          /* report once, when "--count" reaches 0 */
      {
        uint32_t count = Read4BE(&buf);
        VLOG(jdwp) << "    Count: " << count;
        if (count == 0) {
          return ERR_INVALID_COUNT;
        }
        mod.count.count = count;
      }
      break;
    case MK_CONDITIONAL:    /* conditional on expression) */
      {
        uint32_t exprId = Read4BE(&buf);
        VLOG(jdwp) << "    Conditional: " << exprId;
        mod.conditional.exprId = exprId;
      }
      break;
    case MK_THREAD_ONLY:    /* only report events in specified thread */
      {
        ObjectId threadId = ReadObjectId(&buf);
        VLOG(jdwp) << StringPrintf("    ThreadOnly: %#llx", threadId);
        mod.threadOnly.threadId = threadId;
      }
      break;
    case MK_CLASS_ONLY:     /* for ClassPrepare, MethodEntry */
      {
        RefTypeId classId = ReadRefTypeId(&buf);
        VLOG(jdwp) << StringPrintf("    ClassOnly: %#llx (%s)", classId, Dbg::GetClassName(classId).c_str());
        mod.classOnly.refTypeId = classId;
      }
      break;
    case MK_CLASS_MATCH:    /* restrict events to matching classes */
      {
        // pattern is "java.foo.*", we want "java/foo/*".
        std::string pattern(ReadNewUtf8String(&buf));
        std::replace(pattern.begin(), pattern.end(), '.', '/');
        VLOG(jdwp) << "    ClassMatch: '" << pattern << "'";
        mod.classMatch.classPattern = strdup(pattern.c_str());
      }
      break;
    case MK_CLASS_EXCLUDE:  /* restrict events to non-matching classes */
      {
        // pattern is "java.foo.*", we want "java/foo/*".
        std::string pattern(ReadNewUtf8String(&buf));
        std::replace(pattern.begin(), pattern.end(), '.', '/');
        VLOG(jdwp) << "    ClassExclude: '" << pattern << "'";
        mod.classExclude.classPattern = strdup(pattern.c_str());
      }
      break;
    case MK_LOCATION_ONLY:  /* restrict certain events based on loc */
      {
        JdwpLocation loc;
        jdwpReadLocation(&buf, &loc);
        VLOG(jdwp) << "    LocationOnly: " << loc;
        mod.locationOnly.loc = loc;
      }
      break;
    case MK_EXCEPTION_ONLY: /* modifies EK_EXCEPTION events */
      {
        RefTypeId exceptionOrNull;      /* null == all exceptions */
        uint8_t caught, uncaught;

        exceptionOrNull = ReadRefTypeId(&buf);
        caught = Read1(&buf);
        uncaught = Read1(&buf);
        VLOG(jdwp) << StringPrintf("    ExceptionOnly: type=%#llx(%s) caught=%d uncaught=%d",
            exceptionOrNull, (exceptionOrNull == 0) ? "null" : Dbg::GetClassName(exceptionOrNull).c_str(), caught, uncaught);

        mod.exceptionOnly.refTypeId = exceptionOrNull;
        mod.exceptionOnly.caught = caught;
        mod.exceptionOnly.uncaught = uncaught;
      }
      break;
    case MK_FIELD_ONLY:     /* for field access/mod events */
      {
        RefTypeId declaring = ReadRefTypeId(&buf);
        FieldId fieldId = ReadFieldId(&buf);
        VLOG(jdwp) << StringPrintf("    FieldOnly: %#llx %x", declaring, fieldId);
        mod.fieldOnly.refTypeId = declaring;
        mod.fieldOnly.fieldId = fieldId;
      }
      break;
    case MK_STEP:           /* for use with EK_SINGLE_STEP */
      {
        ObjectId threadId;
        uint32_t size, depth;

        threadId = ReadObjectId(&buf);
        size = Read4BE(&buf);
        depth = Read4BE(&buf);
        VLOG(jdwp) << StringPrintf("    Step: thread=%#llx", threadId)
                     << " size=" << JdwpStepSize(size) << " depth=" << JdwpStepDepth(depth);

        mod.step.threadId = threadId;
        mod.step.size = size;
        mod.step.depth = depth;
      }
      break;
    case MK_INSTANCE_ONLY:  /* report events related to a specific obj */
      {
        ObjectId instance = ReadObjectId(&buf);
        VLOG(jdwp) << StringPrintf("    InstanceOnly: %#llx", instance);
        mod.instanceOnly.objectId = instance;
      }
      break;
    default:
      LOG(WARNING) << "GLITCH: unsupported modKind=" << mod.modKind;
      break;
    }
  }

  /*
   * Make sure we consumed all data.  It is possible that the remote side
   * has sent us bad stuff, but for now we blame ourselves.
   */
  if (buf != origBuf + dataLen) {
    LOG(WARNING) << "GLITCH: dataLen is " << dataLen << ", we have consumed " << (buf - origBuf);
  }

  /*
   * We reply with an integer "requestID".
   */
  uint32_t requestId = state->NextEventSerial();
  expandBufAdd4BE(pReply, requestId);

  pEvent->requestId = requestId;

  VLOG(jdwp) << StringPrintf("    --> event requestId=%#x", requestId);

  /* add it to the list */
  JdwpError err = state->RegisterEvent(pEvent);
  if (err != ERR_NONE) {
    /* registration failed, probably because event is bogus */
    EventFree(pEvent);
    LOG(WARNING) << "WARNING: event request rejected";
  }
  return err;
}

/*
 * Clear an event.  Failure to find an event with a matching ID is a no-op
 * and does not return an error.
 */
static JdwpError handleER_Clear(JdwpState* state, const uint8_t* buf, int, ExpandBuf*) {
  uint8_t eventKind;
  eventKind = Read1(&buf);
  uint32_t requestId = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req to clear eventKind=%d requestId=%#x", eventKind, requestId);

  state->UnregisterEventById(requestId);

  return ERR_NONE;
}

/*
 * Return the values of arguments and local variables.
 */
static JdwpError handleSF_GetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  FrameId frameId = ReadFrameId(&buf);
  uint32_t slots = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req for %d slots in threadId=%#llx frameId=%#llx", slots, threadId, frameId);

  expandBufAdd4BE(pReply, slots);     /* "int values" */
  for (uint32_t i = 0; i < slots; i++) {
    uint32_t slot = Read4BE(&buf);
    JDWP::JdwpTag reqSigByte = ReadTag(&buf);

    VLOG(jdwp) << "    --> slot " << slot << " " << reqSigByte;

    size_t width = Dbg::GetTagWidth(reqSigByte);
    uint8_t* ptr = expandBufAddSpace(pReply, width+1);
    Dbg::GetLocalValue(threadId, frameId, slot, reqSigByte, ptr, width);
  }

  return ERR_NONE;
}

/*
 * Set the values of arguments and local variables.
 */
static JdwpError handleSF_SetValues(JdwpState*, const uint8_t* buf, int, ExpandBuf*) {
  ObjectId threadId = ReadObjectId(&buf);
  FrameId frameId = ReadFrameId(&buf);
  uint32_t slots = Read4BE(&buf);

  VLOG(jdwp) << StringPrintf("  Req to set %d slots in threadId=%#llx frameId=%#llx", slots, threadId, frameId);

  for (uint32_t i = 0; i < slots; i++) {
    uint32_t slot = Read4BE(&buf);
    JDWP::JdwpTag sigByte = ReadTag(&buf);
    size_t width = Dbg::GetTagWidth(sigByte);
    uint64_t value = jdwpReadValue(&buf, width);

    VLOG(jdwp) << "    --> slot " << slot << " " << sigByte << " " << value;
    Dbg::SetLocalValue(threadId, frameId, slot, sigByte, value, width);
  }

  return ERR_NONE;
}

/*
 * Returns the value of "this" for the specified frame.
 */
static JdwpError handleSF_ThisObject(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  ReadObjectId(&buf); // Skip thread id.
  FrameId frameId = ReadFrameId(&buf);

  ObjectId objectId;
  Dbg::GetThisObject(frameId, &objectId);

  uint8_t objectTag = Dbg::GetObjectTag(objectId);
  VLOG(jdwp) << StringPrintf("  Req for 'this' in frame=%#llx --> %#llx '%c'", frameId, objectId, (char)objectTag);

  expandBufAdd1(pReply, objectTag);
  expandBufAddObjectId(pReply, objectId);

  return ERR_NONE;
}

/*
 * Return the reference type reflected by this class object.
 *
 * This appears to be required because ReferenceTypeId values are NEVER
 * reused, whereas ClassIds can be recycled like any other object.  (Either
 * that, or I have no idea what this is for.)
 */
static JdwpError handleCOR_ReflectedType(JdwpState*, const uint8_t* buf, int, ExpandBuf* pReply) {
  RefTypeId classObjectId = ReadRefTypeId(&buf);
  VLOG(jdwp) << StringPrintf("  Req for refTypeId for class=%#llx (%s)", classObjectId, Dbg::GetClassName(classObjectId).c_str());
  return Dbg::GetReflectedType(classObjectId, pReply);
}

/*
 * Handle a DDM packet with a single chunk in it.
 */
static JdwpError handleDDM_Chunk(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  uint8_t* replyBuf = NULL;
  int replyLen = -1;

  VLOG(jdwp) << StringPrintf("  Handling DDM packet (%.4s)", buf);

  /*
   * On first DDM packet, notify all handlers that DDM is running.
   */
  if (!state->ddmActive) {
    state->ddmActive = true;
    Dbg::DdmConnected();
  }

  /*
   * If they want to send something back, we copy it into the buffer.
   * A no-copy approach would be nicer.
   *
   * TODO: consider altering the JDWP stuff to hold the packet header
   * in a separate buffer.  That would allow us to writev() DDM traffic
   * instead of copying it into the expanding buffer.  The reduction in
   * heap requirements is probably more valuable than the efficiency.
   */
  if (Dbg::DdmHandlePacket(buf, dataLen, &replyBuf, &replyLen)) {
    CHECK(replyLen > 0 && replyLen < 1*1024*1024);
    memcpy(expandBufAddSpace(pReply, replyLen), replyBuf, replyLen);
    free(replyBuf);
  }
  return ERR_NONE;
}

/*
 * Handler map decl.
 */
typedef JdwpError (*JdwpRequestHandler)(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* reply);

struct JdwpHandlerMap {
  uint8_t  cmdSet;
  uint8_t  cmd;
  JdwpRequestHandler  func;
  const char* descr;
};

/*
 * Map commands to functions.
 *
 * Command sets 0-63 are incoming requests, 64-127 are outbound requests,
 * and 128-256 are vendor-defined.
 */
static const JdwpHandlerMap gHandlerMap[] = {
  /* VirtualMachine command set (1) */
  { 1,    1,  handleVM_Version,       "VirtualMachine.Version" },
  { 1,    2,  handleVM_ClassesBySignature, "VirtualMachine.ClassesBySignature" },
  { 1,    3,  handleVM_AllClasses,    "VirtualMachine.AllClasses" },
  { 1,    4,  handleVM_AllThreads,    "VirtualMachine.AllThreads" },
  { 1,    5,  handleVM_TopLevelThreadGroups, "VirtualMachine.TopLevelThreadGroups" },
  { 1,    6,  handleVM_Dispose,       "VirtualMachine.Dispose" },
  { 1,    7,  handleVM_IDSizes,       "VirtualMachine.IDSizes" },
  { 1,    8,  handleVM_Suspend,       "VirtualMachine.Suspend" },
  { 1,    9,  handleVM_Resume,        "VirtualMachine.Resume" },
  { 1,    10, handleVM_Exit,          "VirtualMachine.Exit" },
  { 1,    11, handleVM_CreateString,  "VirtualMachine.CreateString" },
  { 1,    12, handleVM_Capabilities,  "VirtualMachine.Capabilities" },
  { 1,    13, handleVM_ClassPaths,    "VirtualMachine.ClassPaths" },
  { 1,    14, HandleVM_DisposeObjects, "VirtualMachine.DisposeObjects" },
  { 1,    15, NULL, "VirtualMachine.HoldEvents" },
  { 1,    16, NULL, "VirtualMachine.ReleaseEvents" },
  { 1,    17, handleVM_CapabilitiesNew, "VirtualMachine.CapabilitiesNew" },
  { 1,    18, NULL, "VirtualMachine.RedefineClasses" },
  { 1,    19, NULL, "VirtualMachine.SetDefaultStratum" },
  { 1,    20, handleVM_AllClassesWithGeneric, "VirtualMachine.AllClassesWithGeneric" },
  { 1,    21, NULL, "VirtualMachine.InstanceCounts" },

  /* ReferenceType command set (2) */
  { 2,    1,  handleRT_Signature,     "ReferenceType.Signature" },
  { 2,    2,  handleRT_ClassLoader,   "ReferenceType.ClassLoader" },
  { 2,    3,  handleRT_Modifiers,     "ReferenceType.Modifiers" },
  { 2,    4,  handleRT_Fields,        "ReferenceType.Fields" },
  { 2,    5,  handleRT_Methods,       "ReferenceType.Methods" },
  { 2,    6,  handleRT_GetValues,     "ReferenceType.GetValues" },
  { 2,    7,  handleRT_SourceFile,    "ReferenceType.SourceFile" },
  { 2,    8,  NULL, "ReferenceType.NestedTypes" },
  { 2,    9,  handleRT_Status,        "ReferenceType.Status" },
  { 2,    10, handleRT_Interfaces,    "ReferenceType.Interfaces" },
  { 2,    11, handleRT_ClassObject,   "ReferenceType.ClassObject" },
  { 2,    12, handleRT_SourceDebugExtension, "ReferenceType.SourceDebugExtension" },
  { 2,    13, handleRT_SignatureWithGeneric, "ReferenceType.SignatureWithGeneric" },
  { 2,    14, handleRT_FieldsWithGeneric, "ReferenceType.FieldsWithGeneric" },
  { 2,    15, handleRT_MethodsWithGeneric, "ReferenceType.MethodsWithGeneric" },
  { 2,    16, NULL, "ReferenceType.Instances" },
  { 2,    17, NULL, "ReferenceType.ClassFileVersion" },
  { 2,    18, NULL, "ReferenceType.ConstantPool" },

  /* ClassType command set (3) */
  { 3,    1,  handleCT_Superclass,    "ClassType.Superclass" },
  { 3,    2,  handleCT_SetValues,     "ClassType.SetValues" },
  { 3,    3,  handleCT_InvokeMethod,  "ClassType.InvokeMethod" },
  { 3,    4,  handleCT_NewInstance,   "ClassType.NewInstance" },

  /* ArrayType command set (4) */
  { 4,    1,  handleAT_newInstance,   "ArrayType.NewInstance" },

  /* InterfaceType command set (5) */

  /* Method command set (6) */
  { 6,    1,  handleM_LineTable,      "Method.LineTable" },
  { 6,    2,  handleM_VariableTable,  "Method.VariableTable" },
  { 6,    3,  NULL, "Method.Bytecodes" },
  { 6,    4,  NULL, "Method.IsObsolete" },
  { 6,    5,  handleM_VariableTableWithGeneric, "Method.VariableTableWithGeneric" },

  /* Field command set (8) */

  /* ObjectReference command set (9) */
  { 9,    1,  handleOR_ReferenceType, "ObjectReference.ReferenceType" },
  { 9,    2,  handleOR_GetValues,     "ObjectReference.GetValues" },
  { 9,    3,  handleOR_SetValues,     "ObjectReference.SetValues" },
  { 9,    4,  NULL, "ObjectReference.UNUSED" },
  { 9,    5,  NULL, "ObjectReference.MonitorInfo" },
  { 9,    6,  handleOR_InvokeMethod,  "ObjectReference.InvokeMethod" },
  { 9,    7,  handleOR_DisableCollection, "ObjectReference.DisableCollection" },
  { 9,    8,  handleOR_EnableCollection, "ObjectReference.EnableCollection" },
  { 9,    9,  handleOR_IsCollected,   "ObjectReference.IsCollected" },
  { 9,    10, NULL, "ObjectReference.ReferringObjects" },

  /* StringReference command set (10) */
  { 10,   1,  handleSR_Value,         "StringReference.Value" },

  /* ThreadReference command set (11) */
  { 11,   1,  handleTR_Name,          "ThreadReference.Name" },
  { 11,   2,  handleTR_Suspend,       "ThreadReference.Suspend" },
  { 11,   3,  handleTR_Resume,        "ThreadReference.Resume" },
  { 11,   4,  handleTR_Status,        "ThreadReference.Status" },
  { 11,   5,  handleTR_ThreadGroup,   "ThreadReference.ThreadGroup" },
  { 11,   6,  handleTR_Frames,        "ThreadReference.Frames" },
  { 11,   7,  handleTR_FrameCount,    "ThreadReference.FrameCount" },
  { 11,   8,  NULL, "ThreadReference.OwnedMonitors" },
  { 11,   9,  handleTR_CurrentContendedMonitor, "ThreadReference.CurrentContendedMonitor" },
  { 11,   10, NULL, "ThreadReference.Stop" },
  { 11,   11, NULL,"ThreadReference.Interrupt" },
  { 11,   12, handleTR_SuspendCount,  "ThreadReference.SuspendCount" },
  { 11,   13, NULL, "ThreadReference.OwnedMonitorsStackDepthInfo" },
  { 11,   14, NULL, "ThreadReference.ForceEarlyReturn" },

  /* ThreadGroupReference command set (12) */
  { 12,   1,  handleTGR_Name,         "ThreadGroupReference.Name" },
  { 12,   2,  handleTGR_Parent,       "ThreadGroupReference.Parent" },
  { 12,   3,  handleTGR_Children,     "ThreadGroupReference.Children" },

  /* ArrayReference command set (13) */
  { 13,   1,  handleAR_Length,        "ArrayReference.Length" },
  { 13,   2,  handleAR_GetValues,     "ArrayReference.GetValues" },
  { 13,   3,  handleAR_SetValues,     "ArrayReference.SetValues" },

  /* ClassLoaderReference command set (14) */
  { 14,   1,  handleCLR_VisibleClasses, "ClassLoaderReference.VisibleClasses" },

  /* EventRequest command set (15) */
  { 15,   1,  handleER_Set,           "EventRequest.Set" },
  { 15,   2,  handleER_Clear,         "EventRequest.Clear" },
  { 15,   3,  NULL, "EventRequest.ClearAllBreakpoints" },

  /* StackFrame command set (16) */
  { 16,   1,  handleSF_GetValues,     "StackFrame.GetValues" },
  { 16,   2,  handleSF_SetValues,     "StackFrame.SetValues" },
  { 16,   3,  handleSF_ThisObject,    "StackFrame.ThisObject" },
  { 16,   4,  NULL, "StackFrame.PopFrames" },

  /* ClassObjectReference command set (17) */
  { 17,   1,  handleCOR_ReflectedType,"ClassObjectReference.ReflectedType" },

  /* Event command set (64) */
  { 64,  100, NULL, "Event.Composite" }, // sent from VM to debugger, never received by VM

  { 199,  1,  handleDDM_Chunk,        "DDM.Chunk" },
};

static const char* GetCommandName(size_t cmdSet, size_t cmd) {
  for (int i = 0; i < (int) arraysize(gHandlerMap); i++) {
    if (gHandlerMap[i].cmdSet == cmdSet && gHandlerMap[i].cmd == cmd) {
      return gHandlerMap[i].descr;
    }
  }
  return "?UNKNOWN?";
}

static std::string DescribeCommand(const JdwpReqHeader* pHeader, int dataLen) {
  std::string result;
  result += "REQ: ";
  result += GetCommandName(pHeader->cmdSet, pHeader->cmd);
  result += StringPrintf(" (dataLen=%d id=0x%06x)", dataLen, pHeader->id);
  return result;
}

/*
 * Process a request from the debugger.
 *
 * On entry, the JDWP thread is in VMWAIT.
 */
void JdwpState::ProcessRequest(const JdwpReqHeader* pHeader, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  JdwpError result = ERR_NONE;

  if (pHeader->cmdSet != kJDWPDdmCmdSet) {
    /*
     * Activity from a debugger, not merely ddms.  Mark us as having an
     * active debugger session, and zero out the last-activity timestamp
     * so waitForDebugger() doesn't return if we stall for a bit here.
     */
    Dbg::GoActive();
    QuasiAtomic::Swap64(0, &lastActivityWhen);
  }

  /*
   * If a debugger event has fired in another thread, wait until the
   * initiating thread has suspended itself before processing messages
   * from the debugger.  Otherwise we (the JDWP thread) could be told to
   * resume the thread before it has suspended.
   *
   * We call with an argument of zero to wait for the current event
   * thread to finish, and then clear the block.  Depending on the thread
   * suspend policy, this may allow events in other threads to fire,
   * but those events have no bearing on what the debugger has sent us
   * in the current request.
   *
   * Note that we MUST clear the event token before waking the event
   * thread up, or risk waiting for the thread to suspend after we've
   * told it to resume.
   */
  SetWaitForEventThread(0);

  /*
   * Tell the VM that we're running and shouldn't be interrupted by GC.
   * Do this after anything that can stall indefinitely.
   */
  Dbg::ThreadRunning();

  expandBufAddSpace(pReply, kJDWPHeaderLen);

  size_t i;
  for (i = 0; i < arraysize(gHandlerMap); i++) {
    if (gHandlerMap[i].cmdSet == pHeader->cmdSet && gHandlerMap[i].cmd == pHeader->cmd && gHandlerMap[i].func != NULL) {
      VLOG(jdwp) << DescribeCommand(pHeader, dataLen);
      result = (*gHandlerMap[i].func)(this, buf, dataLen, pReply);
      break;
    }
  }
  if (i == arraysize(gHandlerMap)) {
    LOG(ERROR) << "Command not implemented: " << DescribeCommand(pHeader, dataLen);
    LOG(ERROR) << HexDump(buf, dataLen);
    result = ERR_NOT_IMPLEMENTED;
  }

  /*
   * Set up the reply header.
   *
   * If we encountered an error, only send the header back.
   */
  uint8_t* replyBuf = expandBufGetBuffer(pReply);
  Set4BE(replyBuf + 4, pHeader->id);
  Set1(replyBuf + 8, kJDWPFlagReply);
  Set2BE(replyBuf + 9, result);
  if (result == ERR_NONE) {
    Set4BE(replyBuf + 0, expandBufGetLength(pReply));
  } else {
    Set4BE(replyBuf + 0, kJDWPHeaderLen);
  }

  size_t respLen = expandBufGetLength(pReply) - kJDWPHeaderLen;
  if (false) {
    LOG(INFO) << "reply: dataLen=" << respLen << " err=" << result << (result != ERR_NONE ? " **FAILED**" : "");
    LOG(INFO) << HexDump(expandBufGetBuffer(pReply) + kJDWPHeaderLen, respLen);
  }

  /*
   * Update last-activity timestamp.  We really only need this during
   * the initial setup.  Only update if this is a non-DDMS packet.
   */
  if (pHeader->cmdSet != kJDWPDdmCmdSet) {
    QuasiAtomic::Swap64(MilliTime(), &lastActivityWhen);
  }

  /* tell the VM that GC is okay again */
  Dbg::ThreadWaiting();
}

}  // namespace JDWP

}  // namespace art

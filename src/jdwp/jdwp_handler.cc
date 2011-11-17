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
  pLoc->typeTag = Read1(pBuf);
  pLoc->classId = ReadObjectId(pBuf);
  pLoc->methodId = ReadMethodId(pBuf);
  pLoc->idx = Read8BE(pBuf);
}

/*
 * Helper function: write a "location" into the reply buffer.
 */
void AddLocation(ExpandBuf* pReply, const JdwpLocation* pLoc) {
  expandBufAdd1(pReply, pLoc->typeTag);
  expandBufAddObjectId(pReply, pLoc->classId);
  expandBufAddMethodId(pReply, pLoc->methodId);
  expandBufAdd8BE(pReply, pLoc->idx);
}

/*
 * Helper function: read a variable-width value from the input buffer.
 */
static uint64_t jdwpReadValue(const uint8_t** pBuf, int width) {
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
 * If "isConstructor" is set, this returns "objectId" rather than the
 * expected-to-be-void return value of the called function.
 */
static JdwpError finishInvoke(JdwpState* state,
    const uint8_t* buf, int dataLen, ExpandBuf* pReply,
    ObjectId threadId, ObjectId objectId, RefTypeId classId, MethodId methodId,
    bool isConstructor)
{
  CHECK(!isConstructor || objectId != 0);

  uint32_t numArgs = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("    --> threadId=%llx objectId=%llx", threadId, objectId);
  LOG(VERBOSE) << StringPrintf("        classId=%llx methodId=%x %s.%s", classId, methodId, Dbg::GetClassDescriptor(classId).c_str(), Dbg::GetMethodName(classId, methodId));
  LOG(VERBOSE) << StringPrintf("        %d args:", numArgs);

  uint64_t* argArray = NULL;
  if (numArgs > 0) {
    argArray = (ObjectId*) malloc(sizeof(ObjectId) * numArgs);
  }

  for (uint32_t i = 0; i < numArgs; i++) {
    uint8_t typeTag = Read1(&buf);
    int width = Dbg::GetTagWidth(typeTag);
    uint64_t value = jdwpReadValue(&buf, width);

    LOG(VERBOSE) << StringPrintf("          '%c'(%d): 0x%llx", typeTag, width, value);
    argArray[i] = value;
  }

  uint32_t options = Read4BE(&buf);  /* enum InvokeOptions bit flags */
  LOG(VERBOSE) << StringPrintf("        options=0x%04x%s%s", options, (options & INVOKE_SINGLE_THREADED) ? " (SINGLE_THREADED)" : "", (options & INVOKE_NONVIRTUAL) ? " (NONVIRTUAL)" : "");

  uint8_t resultTag;
  uint64_t resultValue;
  ObjectId exceptObjId;
  JdwpError err = Dbg::InvokeMethod(threadId, objectId, classId, methodId, numArgs, argArray, options, &resultTag, &resultValue, &exceptObjId);
  if (err != ERR_NONE) {
    goto bail;
  }

  if (err == ERR_NONE) {
    if (isConstructor) {
      expandBufAdd1(pReply, JT_OBJECT);
      expandBufAddObjectId(pReply, objectId);
    } else {
      int width = Dbg::GetTagWidth(resultTag);

      expandBufAdd1(pReply, resultTag);
      if (width != 0) {
        jdwpWriteValue(pReply, width, resultValue);
      }
    }
    expandBufAdd1(pReply, JT_OBJECT);
    expandBufAddObjectId(pReply, exceptObjId);

    LOG(VERBOSE) << StringPrintf("  --> returned '%c' 0x%llx (except=%08llx)", resultTag, resultValue, exceptObjId);

    /* show detailed debug output */
    if (resultTag == JT_STRING && exceptObjId == 0) {
      if (resultValue != 0) {
        char* str = Dbg::StringToUtf8(resultValue);
        LOG(VERBOSE) << StringPrintf("      string '%s'", str);
        free(str);
      } else {
        LOG(VERBOSE) << "      string (null)";
      }
    }
  }

bail:
  free(argArray);
  return err;
}


/*
 * Request for version info.
 */
static JdwpError handleVM_Version(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  /* text information on runtime version */
  std::string version(StringPrintf("Android Runtime %s", Runtime::Current()->GetVersion()));
  expandBufAddUtf8String(pReply, version.c_str());
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
static JdwpError handleVM_ClassesBySignature(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  size_t strLen;
  char* classDescriptor = ReadNewUtf8String(&buf, &strLen);
  LOG(VERBOSE) << "  Req for class by signature '" << classDescriptor << "'";

  /*
   * TODO: if a class with the same name has been loaded multiple times
   * (by different class loaders), we're supposed to return each of them.
   *
   * NOTE: this may mangle "className".
   */
  uint32_t numClasses;
  RefTypeId refTypeId;
  if (!Dbg::FindLoadedClassBySignature(classDescriptor, &refTypeId)) {
    /* not currently loaded */
    LOG(VERBOSE) << "    --> no match!";
    numClasses = 0;
  } else {
    /* just the one */
    numClasses = 1;
  }

  expandBufAdd4BE(pReply, numClasses);

  if (numClasses > 0) {
    uint8_t typeTag;
    uint32_t status;

    /* get class vs. interface and status flags */
    Dbg::GetClassInfo(refTypeId, &typeTag, &status, NULL);

    expandBufAdd1(pReply, typeTag);
    expandBufAddRefTypeId(pReply, refTypeId);
    expandBufAdd4BE(pReply, status);
  }

  free(classDescriptor);

  return ERR_NONE;
}

/*
 * Handle request for the thread IDs of all running threads.
 *
 * We exclude ourselves from the list, because we don't allow ourselves
 * to be suspended, and that violates some JDWP expectations.
 */
static JdwpError handleVM_AllThreads(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
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
static JdwpError handleVM_TopLevelThreadGroups(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
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
static JdwpError handleVM_IDSizes(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  expandBufAdd4BE(pReply, sizeof(FieldId));
  expandBufAdd4BE(pReply, sizeof(MethodId));
  expandBufAdd4BE(pReply, sizeof(ObjectId));
  expandBufAdd4BE(pReply, sizeof(RefTypeId));
  expandBufAdd4BE(pReply, sizeof(FrameId));
  return ERR_NONE;
}

/*
 * The debugger is politely asking to disconnect.  We're good with that.
 *
 * We could resume threads and clean up pinned references, but we can do
 * that when the TCP connection drops.
 */
static JdwpError handleVM_Dispose(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  return ERR_NONE;
}

/*
 * Suspend the execution of the application running in the VM (i.e. suspend
 * all threads).
 *
 * This needs to increment the "suspend count" on all threads.
 */
static JdwpError handleVM_Suspend(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  Dbg::SuspendVM();
  return ERR_NONE;
}

/*
 * Resume execution.  Decrements the "suspend count" of all threads.
 */
static JdwpError handleVM_Resume(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  Dbg::ResumeVM();
  return ERR_NONE;
}

/*
 * The debugger wants the entire VM to exit.
 */
static JdwpError handleVM_Exit(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
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
static JdwpError handleVM_CreateString(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  size_t strLen;
  char* str = ReadNewUtf8String(&buf, &strLen);

  LOG(VERBOSE) << "  Req to create string '" << str << "'";

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
static JdwpError handleVM_Capabilities(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  expandBufAdd1(pReply, false);   /* canWatchFieldModification */
  expandBufAdd1(pReply, false);   /* canWatchFieldAccess */
  expandBufAdd1(pReply, false);   /* canGetBytecodes */
  expandBufAdd1(pReply, true);    /* canGetSyntheticAttribute */
  expandBufAdd1(pReply, false);   /* canGetOwnedMonitorInfo */
  expandBufAdd1(pReply, false);   /* canGetCurrentContendedMonitor */
  expandBufAdd1(pReply, false);   /* canGetMonitorInfo */
  return ERR_NONE;
}

/*
 * Return classpath and bootclasspath.
 */
static JdwpError handleVM_ClassPaths(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  char baseDir[2] = "/";

  /*
   * TODO: make this real.  Not important for remote debugging, but
   * might be useful for local debugging.
   */
  uint32_t classPaths = 1;
  uint32_t bootClassPaths = 0;

  expandBufAddUtf8String(pReply, baseDir);
  expandBufAdd4BE(pReply, classPaths);
  for (uint32_t i = 0; i < classPaths; i++) {
    expandBufAddUtf8String(pReply, ".");
  }

  expandBufAdd4BE(pReply, bootClassPaths);
  for (uint32_t i = 0; i < classPaths; i++) {
    /* add bootclasspath components as strings */
  }

  return ERR_NONE;
}

/*
 * Release a list of object IDs.  (Seen in jdb.)
 *
 * Currently does nothing.
 */
static JdwpError HandleVM_DisposeObjects(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  return ERR_NONE;
}

/*
 * Tell the debugger what we are capable of.
 */
static JdwpError handleVM_CapabilitiesNew(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
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

/*
 * Cough up the complete list of classes.
 */
static JdwpError handleVM_AllClassesWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  uint32_t numClasses = 0;
  RefTypeId* classRefBuf = NULL;

  Dbg::GetClassList(&numClasses, &classRefBuf);

  expandBufAdd4BE(pReply, numClasses);

  for (uint32_t i = 0; i < numClasses; i++) {
    static const char genericSignature[1] = "";
    uint8_t refTypeTag;
    std::string descriptor;
    uint32_t status;

    Dbg::GetClassInfo(classRefBuf[i], &refTypeTag, &status, &descriptor);

    expandBufAdd1(pReply, refTypeTag);
    expandBufAddRefTypeId(pReply, classRefBuf[i]);
    expandBufAddUtf8String(pReply, descriptor.c_str());
    expandBufAddUtf8String(pReply, genericSignature);
    expandBufAdd4BE(pReply, status);
  }

  free(classRefBuf);

  return ERR_NONE;
}

/*
 * Given a referenceTypeID, return a string with the JNI reference type
 * signature (e.g. "Ljava/lang/Error;").
 */
static JdwpError handleRT_Signature(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for signature of refTypeId=0x%llx", refTypeId);
  const char* signature = Dbg::GetSignature(refTypeId);
  expandBufAddUtf8String(pReply, signature);

  return ERR_NONE;
}

/*
 * Return the modifiers (a/k/a access flags) for a reference type.
 */
static JdwpError handleRT_Modifiers(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  uint32_t modBits = Dbg::GetAccessFlags(refTypeId);
  expandBufAdd4BE(pReply, modBits);
  return ERR_NONE;
}

/*
 * Get values from static fields in a reference type.
 */
static JdwpError handleRT_GetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  uint32_t numFields = Read4BE(&buf);

  LOG(VERBOSE) << "  RT_GetValues " << numFields << ":";

  expandBufAdd4BE(pReply, numFields);
  for (uint32_t i = 0; i < numFields; i++) {
    FieldId fieldId = ReadFieldId(&buf);
    Dbg::GetStaticFieldValue(refTypeId, fieldId, pReply);
  }

  return ERR_NONE;
}

/*
 * Get the name of the source file in which a reference type was declared.
 */
static JdwpError handleRT_SourceFile(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  const char* fileName = Dbg::GetSourceFile(refTypeId);
  if (fileName != NULL) {
    expandBufAddUtf8String(pReply, fileName);
    return ERR_NONE;
  } else {
    return ERR_ABSENT_INFORMATION;
  }
}

/*
 * Return the current status of the reference type.
 */
static JdwpError handleRT_Status(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  /* get status flags */
  uint8_t typeTag;
  uint32_t status;
  Dbg::GetClassInfo(refTypeId, &typeTag, &status, NULL);
  expandBufAdd4BE(pReply, status);
  return ERR_NONE;
}

/*
 * Return interfaces implemented directly by this class.
 */
static JdwpError handleRT_Interfaces(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for interfaces in %llx (%s)", refTypeId, Dbg::GetClassDescriptor(refTypeId).c_str());

  Dbg::OutputAllInterfaces(refTypeId, pReply);

  return ERR_NONE;
}

/*
 * Return the class object corresponding to this type.
 */
static JdwpError handleRT_ClassObject(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  ObjectId classObjId = Dbg::GetClassObject(refTypeId);

  LOG(VERBOSE) << StringPrintf("  RefTypeId %llx -> ObjectId %llx", refTypeId, classObjId);

  expandBufAddObjectId(pReply, classObjId);

  return ERR_NONE;
}

/*
 * Returns the value of the SourceDebugExtension attribute.
 *
 * JDB seems interested, but DEX files don't currently support this.
 */
static JdwpError handleRT_SourceDebugExtension(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  /* referenceTypeId in, string out */
  return ERR_ABSENT_INFORMATION;
}

/*
 * Like RT_Signature but with the possibility of a "generic signature".
 */
static JdwpError handleRT_SignatureWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  static const char genericSignature[1] = "";

  RefTypeId refTypeId = ReadRefTypeId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for signature of refTypeId=0x%llx", refTypeId);
  const char* signature = Dbg::GetSignature(refTypeId);
  if (signature != NULL) {
    expandBufAddUtf8String(pReply, signature);
  } else {
    LOG(WARNING) << StringPrintf("No signature for refTypeId=0x%llx", refTypeId);
    expandBufAddUtf8String(pReply, "Lunknown;");
  }
  expandBufAddUtf8String(pReply, genericSignature);

  return ERR_NONE;
}

/*
 * Return the instance of java.lang.ClassLoader that loaded the specified
 * reference type, or null if it was loaded by the system loader.
 */
static JdwpError handleRT_ClassLoader(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  expandBufAddObjectId(pReply, Dbg::GetClassLoader(refTypeId));

  return ERR_NONE;
}

/*
 * Given a referenceTypeId, return a block of stuff that describes the
 * fields declared by a class.
 */
static JdwpError handleRT_FieldsWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  LOG(VERBOSE) << StringPrintf("  Req for fields in refTypeId=0x%llx", refTypeId);
  LOG(VERBOSE) << StringPrintf("  --> '%s'", Dbg::GetSignature(refTypeId));
  Dbg::OutputAllFields(refTypeId, true, pReply);
  return ERR_NONE;
}

/*
 * Given a referenceTypeID, return a block of goodies describing the
 * methods declared by a class.
 */
static JdwpError handleRT_MethodsWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for methods in refTypeId=0x%llx", refTypeId);
  LOG(VERBOSE) << StringPrintf("  --> '%s'", Dbg::GetSignature(refTypeId));

  Dbg::OutputAllMethods(refTypeId, true, pReply);

  return ERR_NONE;
}

/*
 * Return the immediate superclass of a class.
 */
static JdwpError handleCT_Superclass(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId classId = ReadRefTypeId(&buf);

  RefTypeId superClassId = Dbg::GetSuperclass(classId);

  expandBufAddRefTypeId(pReply, superClassId);

  return ERR_NONE;
}

/*
 * Set static class values.
 */
static JdwpError handleCT_SetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId classId = ReadRefTypeId(&buf);
  uint32_t values = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req to set %d values in classId=%llx", values, classId);

  for (uint32_t i = 0; i < values; i++) {
    FieldId fieldId = ReadFieldId(&buf);
    uint8_t fieldTag = Dbg::GetStaticFieldBasicTag(classId, fieldId);
    int width = Dbg::GetTagWidth(fieldTag);
    uint64_t value = jdwpReadValue(&buf, width);

    LOG(VERBOSE) << StringPrintf("    --> field=%x tag=%c -> %lld", fieldId, fieldTag, value);
    Dbg::SetStaticFieldValue(classId, fieldId, value, width);
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

  LOG(VERBOSE) << "Creating instance of " << Dbg::GetClassDescriptor(classId);
  ObjectId objectId = Dbg::CreateObject(classId);
  if (objectId == 0) {
    return ERR_OUT_OF_MEMORY;
  }
  return finishInvoke(state, buf, dataLen, pReply, threadId, objectId, classId, methodId, true);
}

/*
 * Create a new array object of the requested type and length.
 */
static JdwpError handleAT_newInstance(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId arrayTypeId = ReadRefTypeId(&buf);
  uint32_t length = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("Creating array %s[%u]", Dbg::GetClassDescriptor(arrayTypeId).c_str(), length);
  ObjectId objectId = Dbg::CreateArrayObject(arrayTypeId, length);
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
static JdwpError handleM_LineTable(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId refTypeId = ReadRefTypeId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for line table in %s.%s", Dbg::GetClassDescriptor(refTypeId).c_str(), Dbg::GetMethodName(refTypeId,methodId));

  Dbg::OutputLineTable(refTypeId, methodId, pReply);

  return ERR_NONE;
}

/*
 * Pull out the LocalVariableTable goodies.
 */
static JdwpError handleM_VariableTableWithGeneric(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId classId = ReadRefTypeId(&buf);
  MethodId methodId = ReadMethodId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for LocalVarTab in class=%s method=%s", Dbg::GetClassDescriptor(classId).c_str(), Dbg::GetMethodName(classId, methodId));

  /*
   * We could return ERR_ABSENT_INFORMATION here if the DEX file was
   * built without local variable information.  That will cause Eclipse
   * to make a best-effort attempt at displaying local variables
   * anonymously.  However, the attempt isn't very good, so we're probably
   * better off just not showing anything.
   */
  Dbg::OutputVariableTable(classId, methodId, true, pReply);
  return ERR_NONE;
}

/*
 * Given an object reference, return the runtime type of the object
 * (class or array).
 *
 * This can get called on different things, e.g. threadId gets
 * passed in here.
 */
static JdwpError handleOR_ReferenceType(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId objectId = ReadObjectId(&buf);
  LOG(VERBOSE) << StringPrintf("  Req for type of objectId=0x%llx", objectId);

  uint8_t refTypeTag;
  RefTypeId typeId;
  Dbg::GetObjectType(objectId, &refTypeTag, &typeId);

  expandBufAdd1(pReply, refTypeTag);
  expandBufAddRefTypeId(pReply, typeId);

  return ERR_NONE;
}

/*
 * Get values from the fields of an object.
 */
static JdwpError handleOR_GetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId objectId = ReadObjectId(&buf);
  uint32_t numFields = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for %d fields from objectId=0x%llx", numFields, objectId);

  expandBufAdd4BE(pReply, numFields);

  for (uint32_t i = 0; i < numFields; i++) {
    FieldId fieldId = ReadFieldId(&buf);
    Dbg::GetFieldValue(objectId, fieldId, pReply);
  }

  return ERR_NONE;
}

/*
 * Set values in the fields of an object.
 */
static JdwpError handleOR_SetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId objectId = ReadObjectId(&buf);
  uint32_t numFields = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req to set %d fields in objectId=0x%llx", numFields, objectId);

  for (uint32_t i = 0; i < numFields; i++) {
    FieldId fieldId = ReadFieldId(&buf);

    uint8_t fieldTag = Dbg::GetFieldBasicTag(objectId, fieldId);
    int width = Dbg::GetTagWidth(fieldTag);
    uint64_t value = jdwpReadValue(&buf, width);

    LOG(VERBOSE) << StringPrintf("    --> fieldId=%x tag='%c'(%d) value=%lld", fieldId, fieldTag, width, value);

    Dbg::SetFieldValue(objectId, fieldId, value, width);
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
static JdwpError handleOR_DisableCollection(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  // this is currently a no-op
  return ERR_NONE;
}

/*
 * Enable garbage collection of the specified object.
 */
static JdwpError handleOR_EnableCollection(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  // this is currently a no-op
  return ERR_NONE;
}

/*
 * Determine whether an object has been garbage collected.
 */
static JdwpError handleOR_IsCollected(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId objectId;

  objectId = ReadObjectId(&buf);
  LOG(VERBOSE) << StringPrintf("  Req IsCollected(0x%llx)", objectId);

  // TODO: currently returning false; must integrate with GC
  expandBufAdd1(pReply, 0);

  return ERR_NONE;
}

/*
 * Return the string value in a string object.
 */
static JdwpError handleSR_Value(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId stringObject = ReadObjectId(&buf);
  char* str = Dbg::StringToUtf8(stringObject);

  LOG(VERBOSE) << StringPrintf("  Req for str %llx --> '%s'", stringObject, str);

  expandBufAddUtf8String(pReply, str);
  free(str);

  return ERR_NONE;
}

/*
 * Return a thread's name.
 */
static JdwpError handleTR_Name(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for name of thread 0x%llx", threadId);
  char* name = Dbg::GetThreadName(threadId);
  if (name == NULL) {
    return ERR_INVALID_THREAD;
  }
  expandBufAddUtf8String(pReply, name);
  free(name);

  return ERR_NONE;
}

/*
 * Suspend the specified thread.
 *
 * It's supposed to remain suspended even if interpreted code wants to
 * resume it; only the JDI is allowed to resume it.
 */
static JdwpError handleTR_Suspend(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  if (threadId == Dbg::GetThreadSelfId()) {
    LOG(INFO) << "  Warning: ignoring request to suspend self";
    return ERR_THREAD_NOT_SUSPENDED;
  }
  LOG(VERBOSE) << StringPrintf("  Req to suspend thread 0x%llx", threadId);
  Dbg::SuspendThread(threadId);
  return ERR_NONE;
}

/*
 * Resume the specified thread.
 */
static JdwpError handleTR_Resume(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  if (threadId == Dbg::GetThreadSelfId()) {
    LOG(INFO) << "  Warning: ignoring request to resume self";
    return ERR_NONE;
  }
  LOG(VERBOSE) << StringPrintf("  Req to resume thread 0x%llx", threadId);
  Dbg::ResumeThread(threadId);
  return ERR_NONE;
}

/*
 * Return status of specified thread.
 */
static JdwpError handleTR_Status(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for status of thread 0x%llx", threadId);

  uint32_t threadStatus;
  uint32_t suspendStatus;
  if (!Dbg::GetThreadStatus(threadId, &threadStatus, &suspendStatus)) {
    return ERR_INVALID_THREAD;
  }

  LOG(VERBOSE) << "    --> " << JdwpThreadStatus(threadStatus) << ", " << JdwpSuspendStatus(suspendStatus);

  expandBufAdd4BE(pReply, threadStatus);
  expandBufAdd4BE(pReply, suspendStatus);

  return ERR_NONE;
}

/*
 * Return the thread group that the specified thread is a member of.
 */
static JdwpError handleTR_ThreadGroup(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  /* currently not handling these */
  ObjectId threadGroupId = Dbg::GetThreadGroup(threadId);
  expandBufAddObjectId(pReply, threadGroupId);

  return ERR_NONE;
}

/*
 * Return the current call stack of a suspended thread.
 *
 * If the thread isn't suspended, the error code isn't defined, but should
 * be THREAD_NOT_SUSPENDED.
 */
static JdwpError handleTR_Frames(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  uint32_t startFrame = Read4BE(&buf);
  uint32_t length = Read4BE(&buf);

  if (!Dbg::ThreadExists(threadId)) {
    return ERR_INVALID_THREAD;
  }
  if (!Dbg::IsSuspended(threadId)) {
    LOG(VERBOSE) << StringPrintf("  Rejecting req for frames in running thread '%s' (%llx)", Dbg::GetThreadName(threadId), threadId);
    return ERR_THREAD_NOT_SUSPENDED;
  }

  int frameCount = Dbg::GetThreadFrameCount(threadId);

  LOG(VERBOSE) << StringPrintf("  Request for frames: threadId=%llx start=%d length=%d [count=%d]", threadId, startFrame, length, frameCount);
  if (frameCount <= 0) {
    return ERR_THREAD_NOT_SUSPENDED;    /* == 0 means 100% native */
  }
  if (length == (uint32_t) -1) {
    length = frameCount;
  }
  CHECK((int) startFrame >= 0 && (int) startFrame < frameCount);
  CHECK_LE((int) (startFrame + length), frameCount);

  uint32_t frames = length;
  expandBufAdd4BE(pReply, frames);
  for (uint32_t i = startFrame; i < (startFrame+length); i++) {
    FrameId frameId;
    JdwpLocation loc;

    Dbg::GetThreadFrame(threadId, i, &frameId, &loc);

    expandBufAdd8BE(pReply, frameId);
    AddLocation(pReply, &loc);

    LOG(VERBOSE) << StringPrintf("    Frame %d: id=%llx loc={type=%d cls=%llx mth=%x loc=%llx}", i, frameId, loc.typeTag, loc.classId, loc.methodId, loc.idx);
  }

  return ERR_NONE;
}

/*
 * Returns the #of frames on the specified thread, which must be suspended.
 */
static JdwpError handleTR_FrameCount(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  if (!Dbg::ThreadExists(threadId)) {
    return ERR_INVALID_THREAD;
  }
  if (!Dbg::IsSuspended(threadId)) {
    LOG(VERBOSE) << StringPrintf("  Rejecting req for frames in running thread '%s' (%llx)", Dbg::GetThreadName(threadId), threadId);
    return ERR_THREAD_NOT_SUSPENDED;
  }

  int frameCount = Dbg::GetThreadFrameCount(threadId);
  if (frameCount < 0) {
    return ERR_INVALID_THREAD;
  }
  expandBufAdd4BE(pReply, (uint32_t)frameCount);

  return ERR_NONE;
}

/*
 * Get the monitor that the thread is waiting on.
 */
static JdwpError handleTR_CurrentContendedMonitor(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId;

  threadId = ReadObjectId(&buf);

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
static JdwpError handleTR_SuspendCount(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);

  uint32_t suspendCount = Dbg::GetThreadSuspendCount(threadId);
  expandBufAdd4BE(pReply, suspendCount);

  return ERR_NONE;
}

/*
 * Return the name of a thread group.
 *
 * The Eclipse debugger recognizes "main" and "system" as special.
 */
static JdwpError handleTGR_Name(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadGroupId = ReadObjectId(&buf);
  LOG(VERBOSE) << StringPrintf("  Req for name of threadGroupId=0x%llx", threadGroupId);

  char* name = Dbg::GetThreadGroupName(threadGroupId);
  if (name != NULL) {
    expandBufAddUtf8String(pReply, name);
  } else {
    expandBufAddUtf8String(pReply, "BAD-GROUP-ID");
    LOG(VERBOSE) << StringPrintf("bad thread group ID");
  }

  free(name);

  return ERR_NONE;
}

/*
 * Returns the thread group -- if any -- that contains the specified
 * thread group.
 */
static JdwpError handleTGR_Parent(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId groupId = ReadObjectId(&buf);

  ObjectId parentGroup = Dbg::GetThreadGroupParent(groupId);
  expandBufAddObjectId(pReply, parentGroup);

  return ERR_NONE;
}

/*
 * Return the active threads and thread groups that are part of the
 * specified thread group.
 */
static JdwpError handleTGR_Children(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadGroupId = ReadObjectId(&buf);
  LOG(VERBOSE) << StringPrintf("  Req for threads in threadGroupId=0x%llx", threadGroupId);

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
static JdwpError handleAR_Length(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId arrayId = ReadObjectId(&buf);
  LOG(VERBOSE) << StringPrintf("  Req for length of array 0x%llx", arrayId);

  uint32_t arrayLength = Dbg::GetArrayLength(arrayId);

  LOG(VERBOSE) << StringPrintf("    --> %d", arrayLength);

  expandBufAdd4BE(pReply, arrayLength);

  return ERR_NONE;
}

/*
 * Return the values from an array.
 */
static JdwpError handleAR_GetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId arrayId = ReadObjectId(&buf);
  uint32_t firstIndex = Read4BE(&buf);
  uint32_t length = Read4BE(&buf);

  uint8_t tag = Dbg::GetArrayElementTag(arrayId);
  LOG(VERBOSE) << StringPrintf("  Req for array values 0x%llx first=%d len=%d (elem tag=%c)", arrayId, firstIndex, length, tag);

  expandBufAdd1(pReply, tag);
  expandBufAdd4BE(pReply, length);

  if (!Dbg::OutputArray(arrayId, firstIndex, length, pReply)) {
    return ERR_INVALID_LENGTH;
  }

  return ERR_NONE;
}

/*
 * Set values in an array.
 */
static JdwpError handleAR_SetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId arrayId = ReadObjectId(&buf);
  uint32_t firstIndex = Read4BE(&buf);
  uint32_t values = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req to set array values 0x%llx first=%d count=%d", arrayId, firstIndex, values);

  if (!Dbg::SetArrayElements(arrayId, firstIndex, values, buf)) {
    return ERR_INVALID_LENGTH;
  }

  return ERR_NONE;
}

/*
 * Return the set of classes visible to a class loader.  All classes which
 * have the class loader as a defining or initiating loader are returned.
 */
static JdwpError handleCLR_VisibleClasses(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId classLoaderObject;
  uint32_t numClasses = 0;
  RefTypeId* classRefBuf = NULL;
  int i;

  classLoaderObject = ReadObjectId(&buf);

  Dbg::GetVisibleClassList(classLoaderObject, &numClasses, &classRefBuf);

  expandBufAdd4BE(pReply, numClasses);
  for (i = 0; i < (int) numClasses; i++) {
    uint8_t refTypeTag = Dbg::GetClassObjectType(classRefBuf[i]);

    expandBufAdd1(pReply, refTypeTag);
    expandBufAddRefTypeId(pReply, classRefBuf[i]);
  }

  return ERR_NONE;
}

/*
 * Return a newly-allocated string in which all occurrences of '.' have
 * been changed to '/'.  If we find a '/' in the original string, NULL
 * is returned to avoid ambiguity.
 */
char* dvmDotToSlash(const char* str) {
  char* newStr = strdup(str);
  char* cp = newStr;

  if (newStr == NULL) {
    return NULL;
  }

  while (*cp != '\0') {
    if (*cp == '/') {
      CHECK(false);
      return NULL;
    }
    if (*cp == '.') {
      *cp = '/';
    }
    cp++;
  }

  return newStr;
}

/*
 * Set an event trigger.
 *
 * Reply with a requestID.
 */
static JdwpError handleER_Set(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  const uint8_t* origBuf = buf;

  uint8_t eventKind = Read1(&buf);
  uint8_t suspendPolicy = Read1(&buf);
  uint32_t modifierCount = Read4BE(&buf);

  LOG(VERBOSE) << "  Set(kind=" << JdwpEventKind(eventKind)
               << " suspend=" << JdwpSuspendPolicy(suspendPolicy)
               << " mods=" << modifierCount << ")";

  CHECK_LT(modifierCount, 256U);    /* reasonableness check */

  JdwpEvent* pEvent = EventAlloc(modifierCount);
  pEvent->eventKind = static_cast<JdwpEventKind>(eventKind);
  pEvent->suspendPolicy = static_cast<JdwpSuspendPolicy>(suspendPolicy);
  pEvent->modCount = modifierCount;

  /*
   * Read modifiers.  Ordering may be significant (see explanation of Count
   * mods in JDWP doc).
   */
  for (uint32_t idx = 0; idx < modifierCount; idx++) {
    uint8_t modKind = Read1(&buf);

    pEvent->mods[idx].modKind = modKind;

    switch (modKind) {
    case MK_COUNT:          /* report once, when "--count" reaches 0 */
      {
        uint32_t count = Read4BE(&buf);
        LOG(VERBOSE) << "    Count: " << count;
        if (count == 0) {
          return ERR_INVALID_COUNT;
        }
        pEvent->mods[idx].count.count = count;
      }
      break;
    case MK_CONDITIONAL:    /* conditional on expression) */
      {
        uint32_t exprId = Read4BE(&buf);
        LOG(VERBOSE) << "    Conditional: " << exprId;
        pEvent->mods[idx].conditional.exprId = exprId;
      }
      break;
    case MK_THREAD_ONLY:    /* only report events in specified thread */
      {
        ObjectId threadId = ReadObjectId(&buf);
        LOG(VERBOSE) << StringPrintf("    ThreadOnly: %llx", threadId);
        pEvent->mods[idx].threadOnly.threadId = threadId;
      }
      break;
    case MK_CLASS_ONLY:     /* for ClassPrepare, MethodEntry */
      {
        RefTypeId clazzId = ReadRefTypeId(&buf);
        LOG(VERBOSE) << StringPrintf("    ClassOnly: %llx (%s)", clazzId, Dbg::GetClassDescriptor(clazzId).c_str());
        pEvent->mods[idx].classOnly.refTypeId = clazzId;
      }
      break;
    case MK_CLASS_MATCH:    /* restrict events to matching classes */
      {
        char* pattern;
        size_t strLen;

        pattern = ReadNewUtf8String(&buf, &strLen);
        LOG(VERBOSE) << StringPrintf("    ClassMatch: '%s'", pattern);
        /* pattern is "java.foo.*", we want "java/foo/ *" */
        pEvent->mods[idx].classMatch.classPattern = dvmDotToSlash(pattern);
        free(pattern);
      }
      break;
    case MK_CLASS_EXCLUDE:  /* restrict events to non-matching classes */
      {
        char* pattern;
        size_t strLen;

        pattern = ReadNewUtf8String(&buf, &strLen);
        LOG(VERBOSE) << StringPrintf("    ClassExclude: '%s'", pattern);
        pEvent->mods[idx].classExclude.classPattern = dvmDotToSlash(pattern);
        free(pattern);
      }
      break;
    case MK_LOCATION_ONLY:  /* restrict certain events based on loc */
      {
        JdwpLocation loc;

        jdwpReadLocation(&buf, &loc);
        LOG(VERBOSE) << StringPrintf("    LocationOnly: typeTag=%d classId=%llx methodId=%x idx=%llx",
        loc.typeTag, loc.classId, loc.methodId, loc.idx);
        pEvent->mods[idx].locationOnly.loc = loc;
      }
      break;
    case MK_EXCEPTION_ONLY: /* modifies EK_EXCEPTION events */
      {
        RefTypeId exceptionOrNull;      /* null == all exceptions */
        uint8_t caught, uncaught;

        exceptionOrNull = ReadRefTypeId(&buf);
        caught = Read1(&buf);
        uncaught = Read1(&buf);
        LOG(VERBOSE) << StringPrintf("    ExceptionOnly: type=%llx(%s) caught=%d uncaught=%d",
            exceptionOrNull, (exceptionOrNull == 0) ? "null" : Dbg::GetClassDescriptor(exceptionOrNull).c_str(), caught, uncaught);

        pEvent->mods[idx].exceptionOnly.refTypeId = exceptionOrNull;
        pEvent->mods[idx].exceptionOnly.caught = caught;
        pEvent->mods[idx].exceptionOnly.uncaught = uncaught;
      }
      break;
    case MK_FIELD_ONLY:     /* for field access/mod events */
      {
        RefTypeId declaring = ReadRefTypeId(&buf);
        FieldId fieldId = ReadFieldId(&buf);
        LOG(VERBOSE) << StringPrintf("    FieldOnly: %llx %x", declaring, fieldId);
        pEvent->mods[idx].fieldOnly.refTypeId = declaring;
        pEvent->mods[idx].fieldOnly.fieldId = fieldId;
      }
      break;
    case MK_STEP:           /* for use with EK_SINGLE_STEP */
      {
        ObjectId threadId;
        uint32_t size, depth;

        threadId = ReadObjectId(&buf);
        size = Read4BE(&buf);
        depth = Read4BE(&buf);
        LOG(VERBOSE) << StringPrintf("    Step: thread=%llx", threadId)
                     << " size=" << JdwpStepSize(size) << " depth=" << JdwpStepDepth(depth);

        pEvent->mods[idx].step.threadId = threadId;
        pEvent->mods[idx].step.size = size;
        pEvent->mods[idx].step.depth = depth;
      }
      break;
    case MK_INSTANCE_ONLY:  /* report events related to a specific obj */
      {
        ObjectId instance = ReadObjectId(&buf);
        LOG(VERBOSE) << StringPrintf("    InstanceOnly: %llx", instance);
        pEvent->mods[idx].instanceOnly.objectId = instance;
      }
      break;
    default:
      LOG(WARNING) << "GLITCH: unsupported modKind=" << modKind;
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

  LOG(VERBOSE) << StringPrintf("    --> event requestId=%#x", requestId);

  /* add it to the list */
  JdwpError err = RegisterEvent(state, pEvent);
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
static JdwpError handleER_Clear(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  uint8_t eventKind;
  eventKind = Read1(&buf);
  uint32_t requestId = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req to clear eventKind=%d requestId=%#x", eventKind, requestId);

  UnregisterEventById(state, requestId);

  return ERR_NONE;
}

/*
 * Return the values of arguments and local variables.
 */
static JdwpError handleSF_GetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  FrameId frameId = ReadFrameId(&buf);
  uint32_t slots = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for %d slots in threadId=%llx frameId=%llx", slots, threadId, frameId);

  expandBufAdd4BE(pReply, slots);     /* "int values" */
  for (uint32_t i = 0; i < slots; i++) {
    uint32_t slot = Read4BE(&buf);
    uint8_t reqSigByte = Read1(&buf);

    LOG(VERBOSE) << StringPrintf("    --> slot %d '%c'", slot, reqSigByte);

    int width = Dbg::GetTagWidth(reqSigByte);
    uint8_t* ptr = expandBufAddSpace(pReply, width+1);
    Dbg::GetLocalValue(threadId, frameId, slot, reqSigByte, ptr, width);
  }

  return ERR_NONE;
}

/*
 * Set the values of arguments and local variables.
 */
static JdwpError handleSF_SetValues(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  FrameId frameId = ReadFrameId(&buf);
  uint32_t slots = Read4BE(&buf);

  LOG(VERBOSE) << StringPrintf("  Req to set %d slots in threadId=%llx frameId=%llx", slots, threadId, frameId);

  for (uint32_t i = 0; i < slots; i++) {
    uint32_t slot = Read4BE(&buf);
    uint8_t sigByte = Read1(&buf);
    int width = Dbg::GetTagWidth(sigByte);
    uint64_t value = jdwpReadValue(&buf, width);

    LOG(VERBOSE) << StringPrintf("    --> slot %d '%c' %llx", slot, sigByte, value);
    Dbg::SetLocalValue(threadId, frameId, slot, sigByte, value, width);
  }

  return ERR_NONE;
}

/*
 * Returns the value of "this" for the specified frame.
 */
static JdwpError handleSF_ThisObject(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  ObjectId threadId = ReadObjectId(&buf);
  FrameId frameId = ReadFrameId(&buf);

  ObjectId objectId;
  if (!Dbg::GetThisObject(threadId, frameId, &objectId)) {
    return ERR_INVALID_FRAMEID;
  }

  uint8_t objectTag = Dbg::GetObjectTag(objectId);
  LOG(VERBOSE) << StringPrintf("  Req for 'this' in thread=%llx frame=%llx --> %llx %s '%c'", threadId, frameId, objectId, Dbg::GetObjectTypeName(objectId), (char)objectTag);

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
static JdwpError handleCOR_ReflectedType(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  RefTypeId classObjectId = ReadRefTypeId(&buf);

  LOG(VERBOSE) << StringPrintf("  Req for refTypeId for class=%llx (%s)", classObjectId, Dbg::GetClassDescriptor(classObjectId).c_str());

  /* just hand the type back to them */
  if (Dbg::IsInterface(classObjectId)) {
    expandBufAdd1(pReply, TT_INTERFACE);
  } else {
    expandBufAdd1(pReply, TT_CLASS);
  }
  expandBufAddRefTypeId(pReply, classObjectId);

  return ERR_NONE;
}

/*
 * Handle a DDM packet with a single chunk in it.
 */
static JdwpError handleDDM_Chunk(JdwpState* state, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  uint8_t* replyBuf = NULL;
  int replyLen = -1;

  LOG(VERBOSE) << StringPrintf("  Handling DDM packet (%.4s)", buf);

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
  //1,    3,  VirtualMachine.AllClasses
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
  //1,    15, HoldEvents
  //1,    16, ReleaseEvents
  { 1,    17, handleVM_CapabilitiesNew, "VirtualMachine.CapabilitiesNew" },
  //1,    18, RedefineClasses
  //1,    19, SetDefaultStratum
  { 1,    20, handleVM_AllClassesWithGeneric, "VirtualMachine.AllClassesWithGeneric"},
  //1,    21, InstanceCounts

  /* ReferenceType command set (2) */
  { 2,    1,  handleRT_Signature,     "ReferenceType.Signature" },
  { 2,    2,  handleRT_ClassLoader,   "ReferenceType.ClassLoader" },
  { 2,    3,  handleRT_Modifiers,     "ReferenceType.Modifiers" },
  //2,    4,  Fields
  //2,    5,  Methods
  { 2,    6,  handleRT_GetValues,     "ReferenceType.GetValues" },
  { 2,    7,  handleRT_SourceFile,    "ReferenceType.SourceFile" },
  //2,    8,  NestedTypes
  { 2,    9,  handleRT_Status,        "ReferenceType.Status" },
  { 2,    10, handleRT_Interfaces,    "ReferenceType.Interfaces" },
  { 2,    11, handleRT_ClassObject,   "ReferenceType.ClassObject" },
  { 2,    12, handleRT_SourceDebugExtension, "ReferenceType.SourceDebugExtension" },
  { 2,    13, handleRT_SignatureWithGeneric, "ReferenceType.SignatureWithGeneric" },
  { 2,    14, handleRT_FieldsWithGeneric, "ReferenceType.FieldsWithGeneric" },
  { 2,    15, handleRT_MethodsWithGeneric, "ReferenceType.MethodsWithGeneric" },
  //2,    16, Instances
  //2,    17, ClassFileVersion
  //2,    18, ConstantPool

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
  //6,    2,  VariableTable
  //6,    3,  Bytecodes
  //6,    4,  IsObsolete
  { 6,    5,  handleM_VariableTableWithGeneric, "Method.VariableTableWithGeneric" },

  /* Field command set (8) */

  /* ObjectReference command set (9) */
  { 9,    1,  handleOR_ReferenceType, "ObjectReference.ReferenceType" },
  { 9,    2,  handleOR_GetValues,     "ObjectReference.GetValues" },
  { 9,    3,  handleOR_SetValues,     "ObjectReference.SetValues" },
  //9,    4,  (not defined)
  //9,    5,  MonitorInfo
  { 9,    6,  handleOR_InvokeMethod,  "ObjectReference.InvokeMethod" },
  { 9,    7,  handleOR_DisableCollection, "ObjectReference.DisableCollection" },
  { 9,    8,  handleOR_EnableCollection, "ObjectReference.EnableCollection" },
  { 9,    9,  handleOR_IsCollected,   "ObjectReference.IsCollected" },
  //9,    10, ReferringObjects

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
  //11,   8,  OwnedMonitors
  { 11,   9,  handleTR_CurrentContendedMonitor, "ThreadReference.CurrentContendedMonitor" },
  //11,   10, Stop
  //11,   11, Interrupt
  { 11,   12, handleTR_SuspendCount,  "ThreadReference.SuspendCount" },
  //11,   13, OwnedMonitorsStackDepthInfo
  //11,   14, ForceEarlyReturn

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
  //15,   3,  ClearAllBreakpoints

  /* StackFrame command set (16) */
  { 16,   1,  handleSF_GetValues,     "StackFrame.GetValues" },
  { 16,   2,  handleSF_SetValues,     "StackFrame.SetValues" },
  { 16,   3,  handleSF_ThisObject,    "StackFrame.ThisObject" },
  //16,   4,  PopFrames

  /* ClassObjectReference command set (17) */
  { 17,   1,  handleCOR_ReflectedType,"ClassObjectReference.ReflectedType" },

  /* Event command set (64) */
  //64,  100, Composite   <-- sent from VM to debugger, never received by VM

  { 199,  1,  handleDDM_Chunk,        "DDM.Chunk" },
};

/*
 * Process a request from the debugger.
 *
 * On entry, the JDWP thread is in VMWAIT.
 */
void JdwpState::ProcessRequest(const JdwpReqHeader* pHeader, const uint8_t* buf, int dataLen, ExpandBuf* pReply) {
  JdwpError result = ERR_NONE;
  int i, respLen;

  if (pHeader->cmdSet != kJDWPDdmCmdSet) {
    /*
     * Activity from a debugger, not merely ddms.  Mark us as having an
     * active debugger session, and zero out the last-activity timestamp
     * so waitForDebugger() doesn't return if we stall for a bit here.
     */
    Dbg::GoActive();
    QuasiAtomicSwap64(0, &lastActivityWhen);
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

  for (i = 0; i < (int) arraysize(gHandlerMap); i++) {
    if (gHandlerMap[i].cmdSet == pHeader->cmdSet && gHandlerMap[i].cmd == pHeader->cmd) {
      LOG(VERBOSE) << StringPrintf("REQ: %s (cmd=%d/%d dataLen=%d id=0x%06x)", gHandlerMap[i].descr, pHeader->cmdSet, pHeader->cmd, dataLen, pHeader->id);
      result = (*gHandlerMap[i].func)(this, buf, dataLen, pReply);
      break;
    }
  }
  if (i == arraysize(gHandlerMap)) {
    LOG(ERROR) << StringPrintf("REQ: UNSUPPORTED (cmd=%d/%d dataLen=%d id=0x%06x)", pHeader->cmdSet, pHeader->cmd, dataLen, pHeader->id);
    if (dataLen > 0) {
      HexDump(buf, dataLen);
    }
    LOG(FATAL) << "command not implemented";      // make it *really* obvious
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

  respLen = expandBufGetLength(pReply) - kJDWPHeaderLen;
  if (false) {
    LOG(INFO) << "reply: dataLen=" << respLen << " err=" << result << (result != ERR_NONE ? " **FAILED**" : "");
    if (respLen > 0) {
      HexDump(expandBufGetBuffer(pReply) + kJDWPHeaderLen, respLen);
    }
  }

  /*
   * Update last-activity timestamp.  We really only need this during
   * the initial setup.  Only update if this is a non-DDMS packet.
   */
  if (pHeader->cmdSet != kJDWPDdmCmdSet) {
    QuasiAtomicSwap64(MilliTime(), &lastActivityWhen);
  }

  /* tell the VM that GC is okay again */
  Dbg::ThreadWaiting();
}

}  // namespace JDWP

}  // namespace art

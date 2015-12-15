/*
 * Copyright (C) 2015 The Android Open Source Project
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
 *
 * Implementation file of the dexdump utility.
 *
 * This is a re-implementation of the original dexdump utility that was
 * based on Dalvik functions in libdex into a new dexdump that is now
 * based on Art functions in libart instead. The output is identical to
 * the original for correct DEX files. Error messages may differ, however.
 * Also, ODEX files are no longer supported.
 *
 * The dexdump tool is intended to mimic objdump.  When possible, use
 * similar command-line arguments.
 *
 * Differences between XML output and the "current.xml" file:
 * - classes in same package are not all grouped together; nothing is sorted
 * - no "deprecated" on fields and methods
 * - no parameter names
 * - no generic signatures on parameters, e.g. type="java.lang.Class&lt;?&gt;"
 * - class shows declared fields and methods; does not show inherited fields
 */

#include "dexdump.h"

#include <inttypes.h>
#include <stdio.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "utils.h"

namespace art {

/*
 * Options parsed in main driver.
 */
struct Options gOptions;

/*
 * Output file. Defaults to stdout.
 */
FILE* gOutFile = stdout;

/*
 * Data types that match the definitions in the VM specification.
 */
typedef uint8_t  u1;
typedef uint16_t u2;
typedef uint32_t u4;
typedef uint64_t u8;
typedef int32_t  s4;
typedef int64_t  s8;

/*
 * Basic information about a field or a method.
 */
struct FieldMethodInfo {
  const char* classDescriptor;
  const char* name;
  const char* signature;
};

/*
 * Flags for use with createAccessFlagStr().
 */
enum AccessFor {
  kAccessForClass = 0, kAccessForMethod = 1, kAccessForField = 2, kAccessForMAX
};
const int kNumFlags = 18;

/*
 * Gets 2 little-endian bytes.
 */
static inline u2 get2LE(unsigned char const* pSrc) {
  return pSrc[0] | (pSrc[1] << 8);
}

/*
 * Converts a single-character primitive type into human-readable form.
 */
static const char* primitiveTypeLabel(char typeChar) {
  switch (typeChar) {
    case 'B': return "byte";
    case 'C': return "char";
    case 'D': return "double";
    case 'F': return "float";
    case 'I': return "int";
    case 'J': return "long";
    case 'S': return "short";
    case 'V': return "void";
    case 'Z': return "boolean";
    default:  return "UNKNOWN";
  }  // switch
}

/*
 * Converts a type descriptor to human-readable "dotted" form.  For
 * example, "Ljava/lang/String;" becomes "java.lang.String", and
 * "[I" becomes "int[]".  Also converts '$' to '.', which means this
 * form can't be converted back to a descriptor.
 */
static char* descriptorToDot(const char* str) {
  int targetLen = strlen(str);
  int offset = 0;

  // Strip leading [s; will be added to end.
  while (targetLen > 1 && str[offset] == '[') {
    offset++;
    targetLen--;
  }  // while

  const int arrayDepth = offset;

  if (targetLen == 1) {
    // Primitive type.
    str = primitiveTypeLabel(str[offset]);
    offset = 0;
    targetLen = strlen(str);
  } else {
    // Account for leading 'L' and trailing ';'.
    if (targetLen >= 2 && str[offset] == 'L' &&
        str[offset + targetLen - 1] == ';') {
      targetLen -= 2;
      offset++;
    }
  }

  // Copy class name over.
  char* newStr = reinterpret_cast<char*>(
      malloc(targetLen + arrayDepth * 2 + 1));
  int i = 0;
  for (; i < targetLen; i++) {
    const char ch = str[offset + i];
    newStr[i] = (ch == '/' || ch == '$') ? '.' : ch;
  }  // for

  // Add the appropriate number of brackets for arrays.
  for (int j = 0; j < arrayDepth; j++) {
    newStr[i++] = '[';
    newStr[i++] = ']';
  }  // for

  newStr[i] = '\0';
  return newStr;
}

/*
 * Converts the class name portion of a type descriptor to human-readable
 * "dotted" form.
 *
 * Returns a newly-allocated string.
 */
static char* descriptorClassToDot(const char* str) {
  // Reduce to just the class name, trimming trailing ';'.
  const char* lastSlash = strrchr(str, '/');
  if (lastSlash == nullptr) {
    lastSlash = str + 1;  // start past 'L'
  } else {
    lastSlash++;          // start past '/'
  }

  char* newStr = strdup(lastSlash);
  newStr[strlen(lastSlash) - 1] = '\0';
  for (char* cp = newStr; *cp != '\0'; cp++) {
    if (*cp == '$') {
      *cp = '.';
    }
  }  // for
  return newStr;
}

/*
 * Returns a quoted string representing the boolean value.
 */
static const char* quotedBool(bool val) {
  return val ? "\"true\"" : "\"false\"";
}

/*
 * Returns a quoted string representing the access flags.
 */
static const char* quotedVisibility(u4 accessFlags) {
  if (accessFlags & kAccPublic) {
    return "\"public\"";
  } else if (accessFlags & kAccProtected) {
    return "\"protected\"";
  } else if (accessFlags & kAccPrivate) {
    return "\"private\"";
  } else {
    return "\"package\"";
  }
}

/*
 * Counts the number of '1' bits in a word.
 */
static int countOnes(u4 val) {
  val = val - ((val >> 1) & 0x55555555);
  val = (val & 0x33333333) + ((val >> 2) & 0x33333333);
  return (((val + (val >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

/*
 * Creates a new string with human-readable access flags.
 *
 * In the base language the access_flags fields are type u2; in Dalvik
 * they're u4.
 */
static char* createAccessFlagStr(u4 flags, AccessFor forWhat) {
  static const char* kAccessStrings[kAccessForMAX][kNumFlags] = {
    {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "?",                     /* 0x00020 */
      "?",                     /* 0x00040 */
      "?",                     /* 0x00080 */
      "?",                     /* 0x00100 */
      "INTERFACE",             /* 0x00200 */
      "ABSTRACT",              /* 0x00400 */
      "?",                     /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "ANNOTATION",            /* 0x02000 */
      "ENUM",                  /* 0x04000 */
      "?",                     /* 0x08000 */
      "VERIFIED",              /* 0x10000 */
      "OPTIMIZED",             /* 0x20000 */
    }, {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "SYNCHRONIZED",          /* 0x00020 */
      "BRIDGE",                /* 0x00040 */
      "VARARGS",               /* 0x00080 */
      "NATIVE",                /* 0x00100 */
      "?",                     /* 0x00200 */
      "ABSTRACT",              /* 0x00400 */
      "STRICT",                /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "?",                     /* 0x02000 */
      "?",                     /* 0x04000 */
      "MIRANDA",               /* 0x08000 */
      "CONSTRUCTOR",           /* 0x10000 */
      "DECLARED_SYNCHRONIZED", /* 0x20000 */
    }, {
      "PUBLIC",                /* 0x00001 */
      "PRIVATE",               /* 0x00002 */
      "PROTECTED",             /* 0x00004 */
      "STATIC",                /* 0x00008 */
      "FINAL",                 /* 0x00010 */
      "?",                     /* 0x00020 */
      "VOLATILE",              /* 0x00040 */
      "TRANSIENT",             /* 0x00080 */
      "?",                     /* 0x00100 */
      "?",                     /* 0x00200 */
      "?",                     /* 0x00400 */
      "?",                     /* 0x00800 */
      "SYNTHETIC",             /* 0x01000 */
      "?",                     /* 0x02000 */
      "ENUM",                  /* 0x04000 */
      "?",                     /* 0x08000 */
      "?",                     /* 0x10000 */
      "?",                     /* 0x20000 */
    },
  };

  // Allocate enough storage to hold the expected number of strings,
  // plus a space between each.  We over-allocate, using the longest
  // string above as the base metric.
  const int kLongest = 21;  // The strlen of longest string above.
  const int count = countOnes(flags);
  char* str;
  char* cp;
  cp = str = reinterpret_cast<char*>(malloc(count * (kLongest + 1) + 1));

  for (int i = 0; i < kNumFlags; i++) {
    if (flags & 0x01) {
      const char* accessStr = kAccessStrings[forWhat][i];
      const int len = strlen(accessStr);
      if (cp != str) {
        *cp++ = ' ';
      }
      memcpy(cp, accessStr, len);
      cp += len;
    }
    flags >>= 1;
  }  // for

  *cp = '\0';
  return str;
}

/*
 * Copies character data from "data" to "out", converting non-ASCII values
 * to fprintf format chars or an ASCII filler ('.' or '?').
 *
 * The output buffer must be able to hold (2*len)+1 bytes.  The result is
 * NULL-terminated.
 */
static void asciify(char* out, const unsigned char* data, size_t len) {
  while (len--) {
    if (*data < 0x20) {
      // Could do more here, but we don't need them yet.
      switch (*data) {
        case '\0':
          *out++ = '\\';
          *out++ = '0';
          break;
        case '\n':
          *out++ = '\\';
          *out++ = 'n';
          break;
        default:
          *out++ = '.';
          break;
      }  // switch
    } else if (*data >= 0x80) {
      *out++ = '?';
    } else {
      *out++ = *data;
    }
    data++;
  }  // while
  *out = '\0';
}

/*
 * Dumps the file header.
 *
 * Note that some of the : are misaligned on purpose to preserve
 * the exact output of the original Dalvik dexdump.
 */
static void dumpFileHeader(const DexFile* pDexFile) {
  const DexFile::Header& pHeader = pDexFile->GetHeader();
  char sanitized[sizeof(pHeader.magic_) * 2 + 1];
  fprintf(gOutFile, "DEX file header:\n");
  asciify(sanitized, pHeader.magic_, sizeof(pHeader.magic_));
  fprintf(gOutFile, "magic               : '%s'\n", sanitized);
  fprintf(gOutFile, "checksum            : %08x\n", pHeader.checksum_);
  fprintf(gOutFile, "signature           : %02x%02x...%02x%02x\n",
          pHeader.signature_[0], pHeader.signature_[1],
          pHeader.signature_[DexFile::kSha1DigestSize - 2],
          pHeader.signature_[DexFile::kSha1DigestSize - 1]);
  fprintf(gOutFile, "file_size           : %d\n", pHeader.file_size_);
  fprintf(gOutFile, "header_size         : %d\n", pHeader.header_size_);
  fprintf(gOutFile, "link_size           : %d\n", pHeader.link_size_);
  fprintf(gOutFile, "link_off            : %d (0x%06x)\n",
          pHeader.link_off_, pHeader.link_off_);
  fprintf(gOutFile, "string_ids_size     : %d\n", pHeader.string_ids_size_);
  fprintf(gOutFile, "string_ids_off      : %d (0x%06x)\n",
          pHeader.string_ids_off_, pHeader.string_ids_off_);
  fprintf(gOutFile, "type_ids_size       : %d\n", pHeader.type_ids_size_);
  fprintf(gOutFile, "type_ids_off        : %d (0x%06x)\n",
          pHeader.type_ids_off_, pHeader.type_ids_off_);
  fprintf(gOutFile, "proto_ids_size       : %d\n", pHeader.proto_ids_size_);
  fprintf(gOutFile, "proto_ids_off        : %d (0x%06x)\n",
          pHeader.proto_ids_off_, pHeader.proto_ids_off_);
  fprintf(gOutFile, "field_ids_size      : %d\n", pHeader.field_ids_size_);
  fprintf(gOutFile, "field_ids_off       : %d (0x%06x)\n",
          pHeader.field_ids_off_, pHeader.field_ids_off_);
  fprintf(gOutFile, "method_ids_size     : %d\n", pHeader.method_ids_size_);
  fprintf(gOutFile, "method_ids_off      : %d (0x%06x)\n",
          pHeader.method_ids_off_, pHeader.method_ids_off_);
  fprintf(gOutFile, "class_defs_size     : %d\n", pHeader.class_defs_size_);
  fprintf(gOutFile, "class_defs_off      : %d (0x%06x)\n",
          pHeader.class_defs_off_, pHeader.class_defs_off_);
  fprintf(gOutFile, "data_size           : %d\n", pHeader.data_size_);
  fprintf(gOutFile, "data_off            : %d (0x%06x)\n\n",
          pHeader.data_off_, pHeader.data_off_);
}

/*
 * Dumps a class_def_item.
 */
static void dumpClassDef(const DexFile* pDexFile, int idx) {
  // General class information.
  const DexFile::ClassDef& pClassDef = pDexFile->GetClassDef(idx);
  fprintf(gOutFile, "Class #%d header:\n", idx);
  fprintf(gOutFile, "class_idx           : %d\n", pClassDef.class_idx_);
  fprintf(gOutFile, "access_flags        : %d (0x%04x)\n",
          pClassDef.access_flags_, pClassDef.access_flags_);
  fprintf(gOutFile, "superclass_idx      : %d\n", pClassDef.superclass_idx_);
  fprintf(gOutFile, "interfaces_off      : %d (0x%06x)\n",
          pClassDef.interfaces_off_, pClassDef.interfaces_off_);
  fprintf(gOutFile, "source_file_idx     : %d\n", pClassDef.source_file_idx_);
  fprintf(gOutFile, "annotations_off     : %d (0x%06x)\n",
          pClassDef.annotations_off_, pClassDef.annotations_off_);
  fprintf(gOutFile, "class_data_off      : %d (0x%06x)\n",
          pClassDef.class_data_off_, pClassDef.class_data_off_);

  // Fields and methods.
  const u1* pEncodedData = pDexFile->GetClassData(pClassDef);
  if (pEncodedData != nullptr) {
    ClassDataItemIterator pClassData(*pDexFile, pEncodedData);
    fprintf(gOutFile, "static_fields_size  : %d\n", pClassData.NumStaticFields());
    fprintf(gOutFile, "instance_fields_size: %d\n", pClassData.NumInstanceFields());
    fprintf(gOutFile, "direct_methods_size : %d\n", pClassData.NumDirectMethods());
    fprintf(gOutFile, "virtual_methods_size: %d\n", pClassData.NumVirtualMethods());
  } else {
    fprintf(gOutFile, "static_fields_size  : 0\n");
    fprintf(gOutFile, "instance_fields_size: 0\n");
    fprintf(gOutFile, "direct_methods_size : 0\n");
    fprintf(gOutFile, "virtual_methods_size: 0\n");
  }
  fprintf(gOutFile, "\n");
}

/*
 * Dumps an interface that a class declares to implement.
 */
static void dumpInterface(const DexFile* pDexFile, const DexFile::TypeItem& pTypeItem, int i) {
  const char* interfaceName = pDexFile->StringByTypeIdx(pTypeItem.type_idx_);
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : '%s'\n", i, interfaceName);
  } else {
    char* dotted = descriptorToDot(interfaceName);
    fprintf(gOutFile, "<implements name=\"%s\">\n</implements>\n", dotted);
    free(dotted);
  }
}

/*
 * Dumps the catches table associated with the code.
 */
static void dumpCatches(const DexFile* pDexFile, const DexFile::CodeItem* pCode) {
  const u4 triesSize = pCode->tries_size_;

  // No catch table.
  if (triesSize == 0) {
    fprintf(gOutFile, "      catches       : (none)\n");
    return;
  }

  // Dump all table entries.
  fprintf(gOutFile, "      catches       : %d\n", triesSize);
  for (u4 i = 0; i < triesSize; i++) {
    const DexFile::TryItem* pTry = pDexFile->GetTryItems(*pCode, i);
    const u4 start = pTry->start_addr_;
    const u4 end = start + pTry->insn_count_;
    fprintf(gOutFile, "        0x%04x - 0x%04x\n", start, end);
    for (CatchHandlerIterator it(*pCode, *pTry); it.HasNext(); it.Next()) {
      const u2 tidx = it.GetHandlerTypeIndex();
      const char* descriptor =
          (tidx == DexFile::kDexNoIndex16) ? "<any>" : pDexFile->StringByTypeIdx(tidx);
      fprintf(gOutFile, "          %s -> 0x%04x\n", descriptor, it.GetHandlerAddress());
    }  // for
  }  // for
}

/*
 * Callback for dumping each positions table entry.
 */
static bool dumpPositionsCb(void* /*context*/, const DexFile::PositionInfo& entry) {
  fprintf(gOutFile, "        0x%04x line=%d\n", entry.address_, entry.line_);
  return false;
}

/*
 * Callback for dumping locals table entry.
 */
static void dumpLocalsCb(void* /*context*/, const DexFile::LocalInfo& entry) {
  const char* signature = entry.signature_ != nullptr ? entry.signature_ : "";
  fprintf(gOutFile, "        0x%04x - 0x%04x reg=%d %s %s %s\n",
          entry.start_address_, entry.end_address_, entry.reg_,
          entry.name_, entry.descriptor_, signature);
}

/*
 * Helper for dumpInstruction(), which builds the string
 * representation for the index in the given instruction. This will
 * first try to use the given buffer, but if the result won't fit,
 * then this will allocate a new buffer to hold the result. A pointer
 * to the buffer which holds the full result is always returned, and
 * this can be compared with the one passed in, to see if the result
 * needs to be free()d.
 */
static char* indexString(const DexFile* pDexFile,
                         const Instruction* pDecInsn, char* buf, size_t bufSize) {
  // Determine index and width of the string.
  u4 index = 0;
  u4 width = 4;
  switch (Instruction::FormatOf(pDecInsn->Opcode())) {
    // SOME NOT SUPPORTED:
    // case Instruction::k20bc:
    case Instruction::k21c:
    case Instruction::k35c:
    // case Instruction::k35ms:
    case Instruction::k3rc:
    // case Instruction::k3rms:
    // case Instruction::k35mi:
    // case Instruction::k3rmi:
      index = pDecInsn->VRegB();
      width = 4;
      break;
    case Instruction::k31c:
      index = pDecInsn->VRegB();
      width = 8;
      break;
    case Instruction::k22c:
    // case Instruction::k22cs:
      index = pDecInsn->VRegC();
      width = 4;
      break;
    default:
      break;
  }  // switch

  // Determine index type.
  size_t outSize = 0;
  switch (Instruction::IndexTypeOf(pDecInsn->Opcode())) {
    case Instruction::kIndexUnknown:
      // This function should never get called for this type, but do
      // something sensible here, just to help with debugging.
      outSize = snprintf(buf, bufSize, "<unknown-index>");
      break;
    case Instruction::kIndexNone:
      // This function should never get called for this type, but do
      // something sensible here, just to help with debugging.
      outSize = snprintf(buf, bufSize, "<no-index>");
      break;
    case Instruction::kIndexTypeRef:
      if (index < pDexFile->GetHeader().type_ids_size_) {
        const char* tp = pDexFile->StringByTypeIdx(index);
        outSize = snprintf(buf, bufSize, "%s // type@%0*x", tp, width, index);
      } else {
        outSize = snprintf(buf, bufSize, "<type?> // type@%0*x", width, index);
      }
      break;
    case Instruction::kIndexStringRef:
      if (index < pDexFile->GetHeader().string_ids_size_) {
        const char* st = pDexFile->StringDataByIdx(index);
        outSize = snprintf(buf, bufSize, "\"%s\" // string@%0*x", st, width, index);
      } else {
        outSize = snprintf(buf, bufSize, "<string?> // string@%0*x", width, index);
      }
      break;
    case Instruction::kIndexMethodRef:
      if (index < pDexFile->GetHeader().method_ids_size_) {
        const DexFile::MethodId& pMethodId = pDexFile->GetMethodId(index);
        const char* name = pDexFile->StringDataByIdx(pMethodId.name_idx_);
        const Signature signature = pDexFile->GetMethodSignature(pMethodId);
        const char* backDescriptor = pDexFile->StringByTypeIdx(pMethodId.class_idx_);
        outSize = snprintf(buf, bufSize, "%s.%s:%s // method@%0*x",
                           backDescriptor, name, signature.ToString().c_str(), width, index);
      } else {
        outSize = snprintf(buf, bufSize, "<method?> // method@%0*x", width, index);
      }
      break;
    case Instruction::kIndexFieldRef:
      if (index < pDexFile->GetHeader().field_ids_size_) {
        const DexFile::FieldId& pFieldId = pDexFile->GetFieldId(index);
        const char* name = pDexFile->StringDataByIdx(pFieldId.name_idx_);
        const char* typeDescriptor = pDexFile->StringByTypeIdx(pFieldId.type_idx_);
        const char* backDescriptor = pDexFile->StringByTypeIdx(pFieldId.class_idx_);
        outSize = snprintf(buf, bufSize, "%s.%s:%s // field@%0*x",
                           backDescriptor, name, typeDescriptor, width, index);
      } else {
        outSize = snprintf(buf, bufSize, "<field?> // field@%0*x", width, index);
      }
      break;
    case Instruction::kIndexVtableOffset:
      outSize = snprintf(buf, bufSize, "[%0*x] // vtable #%0*x",
                         width, index, width, index);
      break;
    case Instruction::kIndexFieldOffset:
      outSize = snprintf(buf, bufSize, "[obj+%0*x]", width, index);
      break;
    // SOME NOT SUPPORTED:
    // case Instruction::kIndexVaries:
    // case Instruction::kIndexInlineMethod:
    default:
      outSize = snprintf(buf, bufSize, "<?>");
      break;
  }  // switch

  // Determine success of string construction.
  if (outSize >= bufSize) {
    // The buffer wasn't big enough; allocate and retry. Note:
    // snprintf() doesn't count the '\0' as part of its returned
    // size, so we add explicit space for it here.
    outSize++;
    buf = reinterpret_cast<char*>(malloc(outSize));
    if (buf == nullptr) {
      return nullptr;
    }
    return indexString(pDexFile, pDecInsn, buf, outSize);
  }
  return buf;
}

/*
 * Dumps a single instruction.
 */
static void dumpInstruction(const DexFile* pDexFile,
                            const DexFile::CodeItem* pCode,
                            u4 codeOffset, u4 insnIdx, u4 insnWidth,
                            const Instruction* pDecInsn) {
  // Address of instruction (expressed as byte offset).
  fprintf(gOutFile, "%06x:", codeOffset + 0x10 + insnIdx * 2);

  // Dump (part of) raw bytes.
  const u2* insns = pCode->insns_;
  for (u4 i = 0; i < 8; i++) {
    if (i < insnWidth) {
      if (i == 7) {
        fprintf(gOutFile, " ... ");
      } else {
        // Print 16-bit value in little-endian order.
        const u1* bytePtr = (const u1*) &insns[insnIdx + i];
        fprintf(gOutFile, " %02x%02x", bytePtr[0], bytePtr[1]);
      }
    } else {
      fputs("     ", gOutFile);
    }
  }  // for

  // Dump pseudo-instruction or opcode.
  if (pDecInsn->Opcode() == Instruction::NOP) {
    const u2 instr = get2LE((const u1*) &insns[insnIdx]);
    if (instr == Instruction::kPackedSwitchSignature) {
      fprintf(gOutFile, "|%04x: packed-switch-data (%d units)", insnIdx, insnWidth);
    } else if (instr == Instruction::kSparseSwitchSignature) {
      fprintf(gOutFile, "|%04x: sparse-switch-data (%d units)", insnIdx, insnWidth);
    } else if (instr == Instruction::kArrayDataSignature) {
      fprintf(gOutFile, "|%04x: array-data (%d units)", insnIdx, insnWidth);
    } else {
      fprintf(gOutFile, "|%04x: nop // spacer", insnIdx);
    }
  } else {
    fprintf(gOutFile, "|%04x: %s", insnIdx, pDecInsn->Name());
  }

  // Set up additional argument.
  char indexBufChars[200];
  char *indexBuf = indexBufChars;
  if (Instruction::IndexTypeOf(pDecInsn->Opcode()) != Instruction::kIndexNone) {
    indexBuf = indexString(pDexFile, pDecInsn,
                           indexBufChars, sizeof(indexBufChars));
  }

  // Dump the instruction.
  //
  // NOTE: pDecInsn->DumpString(pDexFile) differs too much from original.
  //
  switch (Instruction::FormatOf(pDecInsn->Opcode())) {
    case Instruction::k10x:        // op
      break;
    case Instruction::k12x:        // op vA, vB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k11n:        // op vA, #+B
      fprintf(gOutFile, " v%d, #int %d // #%x",
              pDecInsn->VRegA(), (s4) pDecInsn->VRegB(), (u1)pDecInsn->VRegB());
      break;
    case Instruction::k11x:        // op vAA
      fprintf(gOutFile, " v%d", pDecInsn->VRegA());
      break;
    case Instruction::k10t:        // op +AA
    case Instruction::k20t:        // op +AAAA
      {
        const s4 targ = (s4) pDecInsn->VRegA();
        fprintf(gOutFile, " %04x // %c%04x",
                insnIdx + targ,
                (targ < 0) ? '-' : '+',
                (targ < 0) ? -targ : targ);
      }
      break;
    case Instruction::k22x:        // op vAA, vBBBB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k21t:        // op vAA, +BBBB
      {
        const s4 targ = (s4) pDecInsn->VRegB();
        fprintf(gOutFile, " v%d, %04x // %c%04x", pDecInsn->VRegA(),
                insnIdx + targ,
                (targ < 0) ? '-' : '+',
                (targ < 0) ? -targ : targ);
      }
      break;
    case Instruction::k21s:        // op vAA, #+BBBB
      fprintf(gOutFile, " v%d, #int %d // #%x",
              pDecInsn->VRegA(), (s4) pDecInsn->VRegB(), (u2)pDecInsn->VRegB());
      break;
    case Instruction::k21h:        // op vAA, #+BBBB0000[00000000]
      // The printed format varies a bit based on the actual opcode.
      if (pDecInsn->Opcode() == Instruction::CONST_HIGH16) {
        const s4 value = pDecInsn->VRegB() << 16;
        fprintf(gOutFile, " v%d, #int %d // #%x",
                pDecInsn->VRegA(), value, (u2) pDecInsn->VRegB());
      } else {
        const s8 value = ((s8) pDecInsn->VRegB()) << 48;
        fprintf(gOutFile, " v%d, #long %" PRId64 " // #%x",
                pDecInsn->VRegA(), value, (u2) pDecInsn->VRegB());
      }
      break;
    case Instruction::k21c:        // op vAA, thing@BBBB
    case Instruction::k31c:        // op vAA, thing@BBBBBBBB
      fprintf(gOutFile, " v%d, %s", pDecInsn->VRegA(), indexBuf);
      break;
    case Instruction::k23x:        // op vAA, vBB, vCC
      fprintf(gOutFile, " v%d, v%d, v%d",
              pDecInsn->VRegA(), pDecInsn->VRegB(), pDecInsn->VRegC());
      break;
    case Instruction::k22b:        // op vAA, vBB, #+CC
      fprintf(gOutFile, " v%d, v%d, #int %d // #%02x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              (s4) pDecInsn->VRegC(), (u1) pDecInsn->VRegC());
      break;
    case Instruction::k22t:        // op vA, vB, +CCCC
      {
        const s4 targ = (s4) pDecInsn->VRegC();
        fprintf(gOutFile, " v%d, v%d, %04x // %c%04x",
                pDecInsn->VRegA(), pDecInsn->VRegB(),
                insnIdx + targ,
                (targ < 0) ? '-' : '+',
                (targ < 0) ? -targ : targ);
      }
      break;
    case Instruction::k22s:        // op vA, vB, #+CCCC
      fprintf(gOutFile, " v%d, v%d, #int %d // #%04x",
              pDecInsn->VRegA(), pDecInsn->VRegB(),
              (s4) pDecInsn->VRegC(), (u2) pDecInsn->VRegC());
      break;
    case Instruction::k22c:        // op vA, vB, thing@CCCC
    // NOT SUPPORTED:
    // case Instruction::k22cs:    // [opt] op vA, vB, field offset CCCC
      fprintf(gOutFile, " v%d, v%d, %s",
              pDecInsn->VRegA(), pDecInsn->VRegB(), indexBuf);
      break;
    case Instruction::k30t:
      fprintf(gOutFile, " #%08x", pDecInsn->VRegA());
      break;
    case Instruction::k31i:        // op vAA, #+BBBBBBBB
      {
        // This is often, but not always, a float.
        union {
          float f;
          u4 i;
        } conv;
        conv.i = pDecInsn->VRegB();
        fprintf(gOutFile, " v%d, #float %f // #%08x",
                pDecInsn->VRegA(), conv.f, pDecInsn->VRegB());
      }
      break;
    case Instruction::k31t:       // op vAA, offset +BBBBBBBB
      fprintf(gOutFile, " v%d, %08x // +%08x",
              pDecInsn->VRegA(), insnIdx + pDecInsn->VRegB(), pDecInsn->VRegB());
      break;
    case Instruction::k32x:        // op vAAAA, vBBBB
      fprintf(gOutFile, " v%d, v%d", pDecInsn->VRegA(), pDecInsn->VRegB());
      break;
    case Instruction::k35c:        // op {vC, vD, vE, vF, vG}, thing@BBBB
    // NOT SUPPORTED:
    // case Instruction::k35ms:       // [opt] invoke-virtual+super
    // case Instruction::k35mi:       // [opt] inline invoke
      {
        u4 arg[Instruction::kMaxVarArgRegs];
        pDecInsn->GetVarArgs(arg);
        fputs(" {", gOutFile);
        for (int i = 0, n = pDecInsn->VRegA(); i < n; i++) {
          if (i == 0) {
            fprintf(gOutFile, "v%d", arg[i]);
          } else {
            fprintf(gOutFile, ", v%d", arg[i]);
          }
        }  // for
        fprintf(gOutFile, "}, %s", indexBuf);
      }
      break;
    case Instruction::k25x:        // op vC, {vD, vE, vF, vG} (B: count)
      {
        u4 arg[Instruction::kMaxVarArgRegs25x];
        pDecInsn->GetAllArgs25x(arg);
        fprintf(gOutFile, " v%d, {", arg[0]);
        for (int i = 0, n = pDecInsn->VRegB(); i < n; i++) {
          if (i == 0) {
            fprintf(gOutFile, "v%d", arg[Instruction::kLambdaVirtualRegisterWidth + i]);
          } else {
            fprintf(gOutFile, ", v%d", arg[Instruction::kLambdaVirtualRegisterWidth + i]);
          }
        }  // for
        fputc('}', gOutFile);
      }
      break;
    case Instruction::k3rc:        // op {vCCCC .. v(CCCC+AA-1)}, thing@BBBB
    // NOT SUPPORTED:
    // case Instruction::k3rms:       // [opt] invoke-virtual+super/range
    // case Instruction::k3rmi:       // [opt] execute-inline/range
      {
        // This doesn't match the "dx" output when some of the args are
        // 64-bit values -- dx only shows the first register.
        fputs(" {", gOutFile);
        for (int i = 0, n = pDecInsn->VRegA(); i < n; i++) {
          if (i == 0) {
            fprintf(gOutFile, "v%d", pDecInsn->VRegC() + i);
          } else {
            fprintf(gOutFile, ", v%d", pDecInsn->VRegC() + i);
          }
        }  // for
        fprintf(gOutFile, "}, %s", indexBuf);
      }
      break;
    case Instruction::k51l:        // op vAA, #+BBBBBBBBBBBBBBBB
      {
        // This is often, but not always, a double.
        union {
          double d;
          u8 j;
        } conv;
        conv.j = pDecInsn->WideVRegB();
        fprintf(gOutFile, " v%d, #double %f // #%016" PRIx64,
                pDecInsn->VRegA(), conv.d, pDecInsn->WideVRegB());
      }
      break;
    // NOT SUPPORTED:
    // case Instruction::k00x:        // unknown op or breakpoint
    //    break;
    default:
      fprintf(gOutFile, " ???");
      break;
  }  // switch

  fputc('\n', gOutFile);

  if (indexBuf != indexBufChars) {
    free(indexBuf);
  }
}

/*
 * Dumps a bytecode disassembly.
 */
static void dumpBytecodes(const DexFile* pDexFile, u4 idx,
                          const DexFile::CodeItem* pCode, u4 codeOffset) {
  const DexFile::MethodId& pMethodId = pDexFile->GetMethodId(idx);
  const char* name = pDexFile->StringDataByIdx(pMethodId.name_idx_);
  const Signature signature = pDexFile->GetMethodSignature(pMethodId);
  const char* backDescriptor = pDexFile->StringByTypeIdx(pMethodId.class_idx_);

  // Generate header.
  char* tmp = descriptorToDot(backDescriptor);
  fprintf(gOutFile, "%06x:                                        "
          "|[%06x] %s.%s:%s\n",
          codeOffset, codeOffset, tmp, name, signature.ToString().c_str());
  free(tmp);

  // Iterate over all instructions.
  const u2* insns = pCode->insns_;
  for (u4 insnIdx = 0; insnIdx < pCode->insns_size_in_code_units_;) {
    const Instruction* instruction = Instruction::At(&insns[insnIdx]);
    const u4 insnWidth = instruction->SizeInCodeUnits();
    if (insnWidth == 0) {
      fprintf(stderr, "GLITCH: zero-width instruction at idx=0x%04x\n", insnIdx);
      break;
    }
    dumpInstruction(pDexFile, pCode, codeOffset, insnIdx, insnWidth, instruction);
    insnIdx += insnWidth;
  }  // for
}

/*
 * Dumps code of a method.
 */
static void dumpCode(const DexFile* pDexFile, u4 idx, u4 flags,
                     const DexFile::CodeItem* pCode, u4 codeOffset) {
  fprintf(gOutFile, "      registers     : %d\n", pCode->registers_size_);
  fprintf(gOutFile, "      ins           : %d\n", pCode->ins_size_);
  fprintf(gOutFile, "      outs          : %d\n", pCode->outs_size_);
  fprintf(gOutFile, "      insns size    : %d 16-bit code units\n",
          pCode->insns_size_in_code_units_);

  // Bytecode disassembly, if requested.
  if (gOptions.disassemble) {
    dumpBytecodes(pDexFile, idx, pCode, codeOffset);
  }

  // Try-catch blocks.
  dumpCatches(pDexFile, pCode);

  // Positions and locals table in the debug info.
  bool is_static = (flags & kAccStatic) != 0;
  fprintf(gOutFile, "      positions     : \n");
  pDexFile->DecodeDebugPositionInfo(pCode, dumpPositionsCb, nullptr);
  fprintf(gOutFile, "      locals        : \n");
  pDexFile->DecodeDebugLocalInfo(pCode, is_static, idx, dumpLocalsCb, nullptr);
}

/*
 * Dumps a method.
 */
static void dumpMethod(const DexFile* pDexFile, u4 idx, u4 flags,
                       const DexFile::CodeItem* pCode, u4 codeOffset, int i) {
  // Bail for anything private if export only requested.
  if (gOptions.exportsOnly && (flags & (kAccPublic | kAccProtected)) == 0) {
    return;
  }

  const DexFile::MethodId& pMethodId = pDexFile->GetMethodId(idx);
  const char* name = pDexFile->StringDataByIdx(pMethodId.name_idx_);
  const Signature signature = pDexFile->GetMethodSignature(pMethodId);
  char* typeDescriptor = strdup(signature.ToString().c_str());
  const char* backDescriptor = pDexFile->StringByTypeIdx(pMethodId.class_idx_);
  char* accessStr = createAccessFlagStr(flags, kAccessForMethod);

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : (in %s)\n", i, backDescriptor);
    fprintf(gOutFile, "      name          : '%s'\n", name);
    fprintf(gOutFile, "      type          : '%s'\n", typeDescriptor);
    fprintf(gOutFile, "      access        : 0x%04x (%s)\n", flags, accessStr);
    if (pCode == nullptr) {
      fprintf(gOutFile, "      code          : (none)\n");
    } else {
      fprintf(gOutFile, "      code          -\n");
      dumpCode(pDexFile, idx, flags, pCode, codeOffset);
    }
    if (gOptions.disassemble) {
      fputc('\n', gOutFile);
    }
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    const bool constructor = (name[0] == '<');

    // Method name and prototype.
    if (constructor) {
      char* tmp = descriptorClassToDot(backDescriptor);
      fprintf(gOutFile, "<constructor name=\"%s\"\n", tmp);
      free(tmp);
      tmp = descriptorToDot(backDescriptor);
      fprintf(gOutFile, " type=\"%s\"\n", tmp);
      free(tmp);
    } else {
      fprintf(gOutFile, "<method name=\"%s\"\n", name);
      const char* returnType = strrchr(typeDescriptor, ')');
      if (returnType == nullptr) {
        fprintf(stderr, "bad method type descriptor '%s'\n", typeDescriptor);
        goto bail;
      }
      char* tmp = descriptorToDot(returnType+1);
      fprintf(gOutFile, " return=\"%s\"\n", tmp);
      free(tmp);
      fprintf(gOutFile, " abstract=%s\n", quotedBool((flags & kAccAbstract) != 0));
      fprintf(gOutFile, " native=%s\n", quotedBool((flags & kAccNative) != 0));
      fprintf(gOutFile, " synchronized=%s\n", quotedBool(
          (flags & (kAccSynchronized | kAccDeclaredSynchronized)) != 0));
    }

    // Additional method flags.
    fprintf(gOutFile, " static=%s\n", quotedBool((flags & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((flags & kAccFinal) != 0));
    // The "deprecated=" not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n>\n", quotedVisibility(flags));

    // Parameters.
    if (typeDescriptor[0] != '(') {
      fprintf(stderr, "ERROR: bad descriptor '%s'\n", typeDescriptor);
      goto bail;
    }
    char* tmpBuf = reinterpret_cast<char*>(malloc(strlen(typeDescriptor) + 1));
    const char* base = typeDescriptor + 1;
    int argNum = 0;
    while (*base != ')') {
      char* cp = tmpBuf;
      while (*base == '[') {
        *cp++ = *base++;
      }
      if (*base == 'L') {
        // Copy through ';'.
        do {
          *cp = *base++;
        } while (*cp++ != ';');
      } else {
        // Primitive char, copy it.
        if (strchr("ZBCSIFJD", *base) == NULL) {
          fprintf(stderr, "ERROR: bad method signature '%s'\n", base);
          goto bail;
        }
        *cp++ = *base++;
      }
      // Null terminate and display.
      *cp++ = '\0';
      char* tmp = descriptorToDot(tmpBuf);
      fprintf(gOutFile, "<parameter name=\"arg%d\" type=\"%s\">\n"
                        "</parameter>\n", argNum++, tmp);
      free(tmp);
    }  // while
    free(tmpBuf);
    if (constructor) {
      fprintf(gOutFile, "</constructor>\n");
    } else {
      fprintf(gOutFile, "</method>\n");
    }
  }

 bail:
  free(typeDescriptor);
  free(accessStr);
}

/*
 * Dumps a string value with some escape characters.
 */
static void dumpEscapedString(const char* p) {
  for (; *p; p++) {
    switch (*p) {
      case '\\':
        fputs("\\\\", gOutFile);
        break;
      case '\"':
        fputs("\\\"", gOutFile);
        break;
      case '\t':
        fputs("\\t", gOutFile);
        break;
      case '\n':
        fputs("\\n", gOutFile);
        break;
      case '\r':
        fputs("\\r", gOutFile);
        break;
      default:
        putc(*p, gOutFile);
    }
  }
}

/*
 * Dumps an XML attribute value between double-quotes.
 */
static void dumpXmlAttribute(const char* p) {
  for (; *p; p++) {
    switch (*p) {
      case '&':
        fputs("&amp;", gOutFile);
        break;
      case '<':
        fputs("&lt;", gOutFile);
        break;
      case '"':
        fputs("&quot;", gOutFile);
        break;
      case '\t':
        fputs("&#x9;", gOutFile);
        break;
      case '\n':
        fputs("&#xA;", gOutFile);
        break;
      case '\r':
        fputs("&#xD;", gOutFile);
        break;
      default:
        putc(*p, gOutFile);
    }
  }
}

/*
 * Dumps a value of static (class) field.
 */
static void dumpSFieldValue(const DexFile* pDexFile,
                            EncodedStaticFieldValueIterator::ValueType valueType,
                            const jvalue* pValue) {
  switch (valueType) {
    case EncodedStaticFieldValueIterator::kByte:
      fprintf(gOutFile, "%" PRIu8, pValue->b);
      break;
    case EncodedStaticFieldValueIterator::kShort:
      fprintf(gOutFile, "%" PRId16, pValue->s);
      break;
    case EncodedStaticFieldValueIterator::kChar:
      fprintf(gOutFile, "%" PRIu16, pValue->c);
      break;
    case EncodedStaticFieldValueIterator::kInt:
      fprintf(gOutFile, "%" PRId32, pValue->i);
      break;
    case EncodedStaticFieldValueIterator::kLong:
      fprintf(gOutFile, "%" PRId64, pValue->j);
      break;
    case EncodedStaticFieldValueIterator::kFloat:
      fprintf(gOutFile, "%f", pValue->f);
      break;
    case EncodedStaticFieldValueIterator::kDouble:
      fprintf(gOutFile, "%f", pValue->d);
      break;
    case EncodedStaticFieldValueIterator::kString: {
      const char* str =
          pDexFile->GetStringData(pDexFile->GetStringId(pValue->i));
      if (gOptions.outputFormat == OUTPUT_PLAIN) {
        fputs("\"", gOutFile);
        dumpEscapedString(str);
        fputs("\"", gOutFile);
      } else {
        dumpXmlAttribute(str);
      }
      break;
    }
    case EncodedStaticFieldValueIterator::kNull:
      fputs("null", gOutFile);
      break;
    case EncodedStaticFieldValueIterator::kBoolean:
      fputs(pValue->z ? "true" : "false", gOutFile);
      break;

    case EncodedStaticFieldValueIterator::kAnnotation:
    case EncodedStaticFieldValueIterator::kArray:
    case EncodedStaticFieldValueIterator::kEnum:
    case EncodedStaticFieldValueIterator::kField:
    case EncodedStaticFieldValueIterator::kMethod:
    case EncodedStaticFieldValueIterator::kType:
    default:
      fprintf(gOutFile, "Unexpected static field type: %d", valueType);
  }
}

/*
 * Dumps a static (class) field.
 */
static void dumpSField(const DexFile* pDexFile, u4 idx, u4 flags, int i,
                       EncodedStaticFieldValueIterator::ValueType valueType,
                       const jvalue* pValue) {
  // Bail for anything private if export only requested.
  if (gOptions.exportsOnly && (flags & (kAccPublic | kAccProtected)) == 0) {
    return;
  }

  const DexFile::FieldId& pFieldId = pDexFile->GetFieldId(idx);
  const char* name = pDexFile->StringDataByIdx(pFieldId.name_idx_);
  const char* typeDescriptor = pDexFile->StringByTypeIdx(pFieldId.type_idx_);
  const char* backDescriptor = pDexFile->StringByTypeIdx(pFieldId.class_idx_);
  char* accessStr = createAccessFlagStr(flags, kAccessForField);

  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "    #%d              : (in %s)\n", i, backDescriptor);
    fprintf(gOutFile, "      name          : '%s'\n", name);
    fprintf(gOutFile, "      type          : '%s'\n", typeDescriptor);
    fprintf(gOutFile, "      access        : 0x%04x (%s)\n", flags, accessStr);
    if (pValue != nullptr) {
      fputs("      value         : ", gOutFile);
      dumpSFieldValue(pDexFile, valueType, pValue);
      fputs("\n", gOutFile);
    }
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "<field name=\"%s\"\n", name);
    char *tmp = descriptorToDot(typeDescriptor);
    fprintf(gOutFile, " type=\"%s\"\n", tmp);
    free(tmp);
    fprintf(gOutFile, " transient=%s\n", quotedBool((flags & kAccTransient) != 0));
    fprintf(gOutFile, " volatile=%s\n", quotedBool((flags & kAccVolatile) != 0));
    // The "value=" is not knowable w/o parsing annotations.
    fprintf(gOutFile, " static=%s\n", quotedBool((flags & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((flags & kAccFinal) != 0));
    // The "deprecated=" is not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n", quotedVisibility(flags));
    if (pValue != nullptr) {
      fputs(" value=\"", gOutFile);
      dumpSFieldValue(pDexFile, valueType, pValue);
      fputs("\"\n", gOutFile);
    }
    fputs(">\n</field>\n", gOutFile);
  }

  free(accessStr);
}

/*
 * Dumps an instance field.
 */
static void dumpIField(const DexFile* pDexFile, u4 idx, u4 flags, int i) {
  dumpSField(pDexFile, idx, flags, i,
             EncodedStaticFieldValueIterator::kByte, nullptr);
}

/*
 * Dumping a CFG. Note that this will do duplicate work. utils.h doesn't expose the code-item
 * version, so the DumpMethodCFG code will have to iterate again to find it. But dexdump is a
 * tool, so this is not performance-critical.
 */

static void dumpCfg(const DexFile* dex_file,
                    uint32_t dex_method_idx,
                    const DexFile::CodeItem* code_item) {
  if (code_item != nullptr) {
    std::ostringstream oss;
    DumpMethodCFG(dex_file, dex_method_idx, oss);
    fprintf(gOutFile, "%s", oss.str().c_str());
  }
}

static void dumpCfg(const DexFile* dex_file, int idx) {
  const DexFile::ClassDef& class_def = dex_file->GetClassDef(idx);
  const uint8_t* class_data = dex_file->GetClassData(class_def);
  if (class_data == nullptr) {  // empty class such as a marker interface?
    return;
  }
  ClassDataItemIterator it(*dex_file, class_data);
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  while (it.HasNextDirectMethod()) {
    dumpCfg(dex_file,
            it.GetMemberIndex(),
            it.GetMethodCodeItem());
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    dumpCfg(dex_file,
                it.GetMemberIndex(),
                it.GetMethodCodeItem());
    it.Next();
  }
}

/*
 * Dumps the class.
 *
 * Note "idx" is a DexClassDef index, not a DexTypeId index.
 *
 * If "*pLastPackage" is nullptr or does not match the current class' package,
 * the value will be replaced with a newly-allocated string.
 */
static void dumpClass(const DexFile* pDexFile, int idx, char** pLastPackage) {
  const DexFile::ClassDef& pClassDef = pDexFile->GetClassDef(idx);

  // Omitting non-public class.
  if (gOptions.exportsOnly && (pClassDef.access_flags_ & kAccPublic) == 0) {
    return;
  }

  if (gOptions.cfg) {
    dumpCfg(pDexFile, idx);
    return;
  }

  // For the XML output, show the package name.  Ideally we'd gather
  // up the classes, sort them, and dump them alphabetically so the
  // package name wouldn't jump around, but that's not a great plan
  // for something that needs to run on the device.
  const char* classDescriptor = pDexFile->StringByTypeIdx(pClassDef.class_idx_);
  if (!(classDescriptor[0] == 'L' &&
        classDescriptor[strlen(classDescriptor)-1] == ';')) {
    // Arrays and primitives should not be defined explicitly. Keep going?
    fprintf(stderr, "Malformed class name '%s'\n", classDescriptor);
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    char* mangle = strdup(classDescriptor + 1);
    mangle[strlen(mangle)-1] = '\0';

    // Reduce to just the package name.
    char* lastSlash = strrchr(mangle, '/');
    if (lastSlash != nullptr) {
      *lastSlash = '\0';
    } else {
      *mangle = '\0';
    }

    for (char* cp = mangle; *cp != '\0'; cp++) {
      if (*cp == '/') {
        *cp = '.';
      }
    }  // for

    if (*pLastPackage == nullptr || strcmp(mangle, *pLastPackage) != 0) {
      // Start of a new package.
      if (*pLastPackage != nullptr) {
        fprintf(gOutFile, "</package>\n");
      }
      fprintf(gOutFile, "<package name=\"%s\"\n>\n", mangle);
      free(*pLastPackage);
      *pLastPackage = mangle;
    } else {
      free(mangle);
    }
  }

  // General class information.
  char* accessStr = createAccessFlagStr(pClassDef.access_flags_, kAccessForClass);
  const char* superclassDescriptor;
  if (pClassDef.superclass_idx_ == DexFile::kDexNoIndex16) {
    superclassDescriptor = nullptr;
  } else {
    superclassDescriptor = pDexFile->StringByTypeIdx(pClassDef.superclass_idx_);
  }
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    fprintf(gOutFile, "Class #%d            -\n", idx);
    fprintf(gOutFile, "  Class descriptor  : '%s'\n", classDescriptor);
    fprintf(gOutFile, "  Access flags      : 0x%04x (%s)\n", pClassDef.access_flags_, accessStr);
    if (superclassDescriptor != nullptr) {
      fprintf(gOutFile, "  Superclass        : '%s'\n", superclassDescriptor);
    }
    fprintf(gOutFile, "  Interfaces        -\n");
  } else {
    char* tmp = descriptorClassToDot(classDescriptor);
    fprintf(gOutFile, "<class name=\"%s\"\n", tmp);
    free(tmp);
    if (superclassDescriptor != nullptr) {
      tmp = descriptorToDot(superclassDescriptor);
      fprintf(gOutFile, " extends=\"%s\"\n", tmp);
      free(tmp);
    }
    fprintf(gOutFile, " interface=%s\n",
            quotedBool((pClassDef.access_flags_ & kAccInterface) != 0));
    fprintf(gOutFile, " abstract=%s\n", quotedBool((pClassDef.access_flags_ & kAccAbstract) != 0));
    fprintf(gOutFile, " static=%s\n", quotedBool((pClassDef.access_flags_ & kAccStatic) != 0));
    fprintf(gOutFile, " final=%s\n", quotedBool((pClassDef.access_flags_ & kAccFinal) != 0));
    // The "deprecated=" not knowable w/o parsing annotations.
    fprintf(gOutFile, " visibility=%s\n", quotedVisibility(pClassDef.access_flags_));
    fprintf(gOutFile, ">\n");
  }

  // Interfaces.
  const DexFile::TypeList* pInterfaces = pDexFile->GetInterfacesList(pClassDef);
  if (pInterfaces != nullptr) {
    for (u4 i = 0; i < pInterfaces->Size(); i++) {
      dumpInterface(pDexFile, pInterfaces->GetTypeItem(i), i);
    }  // for
  }

  // Fields and methods.
  const u1* pEncodedData = pDexFile->GetClassData(pClassDef);
  if (pEncodedData == nullptr) {
    if (gOptions.outputFormat == OUTPUT_PLAIN) {
      fprintf(gOutFile, "  Static fields     -\n");
      fprintf(gOutFile, "  Instance fields   -\n");
      fprintf(gOutFile, "  Direct methods    -\n");
      fprintf(gOutFile, "  Virtual methods   -\n");
    }
  } else {
    ClassDataItemIterator pClassData(*pDexFile, pEncodedData);
    if (gOptions.outputFormat == OUTPUT_PLAIN) {
      fprintf(gOutFile, "  Static fields     -\n");
    }
    EncodedStaticFieldValueIterator staticFieldValues(*pDexFile, pClassDef);
    for (int i = 0; pClassData.HasNextStaticField(); i++, pClassData.Next()) {
      EncodedStaticFieldValueIterator::ValueType valueType =
          EncodedStaticFieldValueIterator::kByte;
      const jvalue* pValue = nullptr;
      if (staticFieldValues.HasNext()) {
        valueType = staticFieldValues.GetValueType();
        pValue = &staticFieldValues.GetJavaValue();
      }
      dumpSField(pDexFile, pClassData.GetMemberIndex(),
                           pClassData.GetRawMemberAccessFlags(), i,
                 valueType, pValue);
      if (staticFieldValues.HasNext()) {
        staticFieldValues.Next();
      }
    }  // for
    DCHECK(!staticFieldValues.HasNext());
    if (gOptions.outputFormat == OUTPUT_PLAIN) {
      fprintf(gOutFile, "  Instance fields   -\n");
    }
    for (int i = 0; pClassData.HasNextInstanceField(); i++, pClassData.Next()) {
      dumpIField(pDexFile, pClassData.GetMemberIndex(),
                          pClassData.GetRawMemberAccessFlags(), i);
    }  // for
    if (gOptions.outputFormat == OUTPUT_PLAIN) {
      fprintf(gOutFile, "  Direct methods    -\n");
    }
    for (int i = 0; pClassData.HasNextDirectMethod(); i++, pClassData.Next()) {
      dumpMethod(pDexFile, pClassData.GetMemberIndex(),
                           pClassData.GetRawMemberAccessFlags(),
                           pClassData.GetMethodCodeItem(),
                           pClassData.GetMethodCodeItemOffset(), i);
    }  // for
    if (gOptions.outputFormat == OUTPUT_PLAIN) {
      fprintf(gOutFile, "  Virtual methods   -\n");
    }
    for (int i = 0; pClassData.HasNextVirtualMethod(); i++, pClassData.Next()) {
      dumpMethod(pDexFile, pClassData.GetMemberIndex(),
                           pClassData.GetRawMemberAccessFlags(),
                           pClassData.GetMethodCodeItem(),
                           pClassData.GetMethodCodeItemOffset(), i);
    }  // for
  }

  // End of class.
  if (gOptions.outputFormat == OUTPUT_PLAIN) {
    const char* fileName;
    if (pClassDef.source_file_idx_ != DexFile::kDexNoIndex) {
      fileName = pDexFile->StringDataByIdx(pClassDef.source_file_idx_);
    } else {
      fileName = "unknown";
    }
    fprintf(gOutFile, "  source_file_idx   : %d (%s)\n\n",
            pClassDef.source_file_idx_, fileName);
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "</class>\n");
  }

  free(accessStr);
}

/*
 * Dumps the requested sections of the file.
 */
static void processDexFile(const char* fileName, const DexFile* pDexFile) {
  if (gOptions.verbose) {
    fprintf(gOutFile, "Opened '%s', DEX version '%.3s'\n",
            fileName, pDexFile->GetHeader().magic_ + 4);
  }

  // Headers.
  if (gOptions.showFileHeaders) {
    dumpFileHeader(pDexFile);
  }

  // Open XML context.
  if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "<api>\n");
  }

  // Iterate over all classes.
  char* package = nullptr;
  const u4 classDefsSize = pDexFile->GetHeader().class_defs_size_;
  for (u4 i = 0; i < classDefsSize; i++) {
    if (gOptions.showSectionHeaders) {
      dumpClassDef(pDexFile, i);
    }
    dumpClass(pDexFile, i, &package);
  }  // for

  // Free the last package allocated.
  if (package != nullptr) {
    fprintf(gOutFile, "</package>\n");
    free(package);
  }

  // Close XML context.
  if (gOptions.outputFormat == OUTPUT_XML) {
    fprintf(gOutFile, "</api>\n");
  }
}

/*
 * Processes a single file (either direct .dex or indirect .zip/.jar/.apk).
 */
int processFile(const char* fileName) {
  if (gOptions.verbose) {
    fprintf(gOutFile, "Processing '%s'...\n", fileName);
  }

  // If the file is not a .dex file, the function tries .zip/.jar/.apk files,
  // all of which are Zip archives with "classes.dex" inside. The compressed
  // data needs to be extracted to a temp file, the location of which varies.
  //
  // TODO(ajcbik): fix following issues
  //
  // (1) gOptions.tempFileName is not accounted for
  // (2) gOptions.ignoreBadChecksum is not accounted for
  //
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  if (!DexFile::Open(fileName, fileName, &error_msg, &dex_files)) {
    // Display returned error message to user. Note that this error behavior
    // differs from the error messages shown by the original Dalvik dexdump.
    fputs(error_msg.c_str(), stderr);
    fputc('\n', stderr);
    return -1;
  }

  // Success. Either report checksum verification or process
  // all dex files found in given file.
  if (gOptions.checksumOnly) {
    fprintf(gOutFile, "Checksum verified\n");
  } else {
    for (size_t i = 0; i < dex_files.size(); i++) {
      processDexFile(fileName, dex_files[i].get());
    }
  }
  return 0;
}

}  // namespace art

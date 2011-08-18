/*
 * Copyright (C) 2011 The Android Open Source Project
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
 * Common defines for all Dalvik code.
 */
#ifndef DALVIK_COMMON_H_
#define DALVIK_COMMON_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include "../logging.h"

// From Common.h
#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#define MAX(x,y) (((x) > (y)) ? (x) : (y))

typedef uint8_t             u1;
typedef uint16_t            u2;
typedef uint32_t            u4;
typedef uint64_t            u8;
typedef int8_t              s1;
typedef int16_t             s2;
typedef int32_t             s4;
typedef int64_t             s8;
typedef unsigned long long  u8;

struct Object;

union JValue {
    u1      z;
    s1      b;
    u2      c;
    s2      s;
    s4      i;
    s8      j;
    float   f;
    double  d;
    Object* l;
};


// From libdex/DexOpcodes.h
#define kNumPackedOpcodes 0x200
#define kPackedSwitchSignature 0x0100
#define kSparseSwitchSignature 0x0200
#define kArrayDataSignature 0x0300
enum Opcode {
    // BEGIN(libdex-opcode-enum); GENERATED AUTOMATICALLY BY opcode-gen
    OP_NOP                          = 0x00,
    OP_MOVE                         = 0x01,
    OP_MOVE_FROM16                  = 0x02,
    OP_MOVE_16                      = 0x03,
    OP_MOVE_WIDE                    = 0x04,
    OP_MOVE_WIDE_FROM16             = 0x05,
    OP_MOVE_WIDE_16                 = 0x06,
    OP_MOVE_OBJECT                  = 0x07,
    OP_MOVE_OBJECT_FROM16           = 0x08,
    OP_MOVE_OBJECT_16               = 0x09,
    OP_MOVE_RESULT                  = 0x0a,
    OP_MOVE_RESULT_WIDE             = 0x0b,
    OP_MOVE_RESULT_OBJECT           = 0x0c,
    OP_MOVE_EXCEPTION               = 0x0d,
    OP_RETURN_VOID                  = 0x0e,
    OP_RETURN                       = 0x0f,
    OP_RETURN_WIDE                  = 0x10,
    OP_RETURN_OBJECT                = 0x11,
    OP_CONST_4                      = 0x12,
    OP_CONST_16                     = 0x13,
    OP_CONST                        = 0x14,
    OP_CONST_HIGH16                 = 0x15,
    OP_CONST_WIDE_16                = 0x16,
    OP_CONST_WIDE_32                = 0x17,
    OP_CONST_WIDE                   = 0x18,
    OP_CONST_WIDE_HIGH16            = 0x19,
    OP_CONST_STRING                 = 0x1a,
    OP_CONST_STRING_JUMBO           = 0x1b,
    OP_CONST_CLASS                  = 0x1c,
    OP_MONITOR_ENTER                = 0x1d,
    OP_MONITOR_EXIT                 = 0x1e,
    OP_CHECK_CAST                   = 0x1f,
    OP_INSTANCE_OF                  = 0x20,
    OP_ARRAY_LENGTH                 = 0x21,
    OP_NEW_INSTANCE                 = 0x22,
    OP_NEW_ARRAY                    = 0x23,
    OP_FILLED_NEW_ARRAY             = 0x24,
    OP_FILLED_NEW_ARRAY_RANGE       = 0x25,
    OP_FILL_ARRAY_DATA              = 0x26,
    OP_THROW                        = 0x27,
    OP_GOTO                         = 0x28,
    OP_GOTO_16                      = 0x29,
    OP_GOTO_32                      = 0x2a,
    OP_PACKED_SWITCH                = 0x2b,
    OP_SPARSE_SWITCH                = 0x2c,
    OP_CMPL_FLOAT                   = 0x2d,
    OP_CMPG_FLOAT                   = 0x2e,
    OP_CMPL_DOUBLE                  = 0x2f,
    OP_CMPG_DOUBLE                  = 0x30,
    OP_CMP_LONG                     = 0x31,
    OP_IF_EQ                        = 0x32,
    OP_IF_NE                        = 0x33,
    OP_IF_LT                        = 0x34,
    OP_IF_GE                        = 0x35,
    OP_IF_GT                        = 0x36,
    OP_IF_LE                        = 0x37,
    OP_IF_EQZ                       = 0x38,
    OP_IF_NEZ                       = 0x39,
    OP_IF_LTZ                       = 0x3a,
    OP_IF_GEZ                       = 0x3b,
    OP_IF_GTZ                       = 0x3c,
    OP_IF_LEZ                       = 0x3d,
    OP_UNUSED_3E                    = 0x3e,
    OP_UNUSED_3F                    = 0x3f,
    OP_UNUSED_40                    = 0x40,
    OP_UNUSED_41                    = 0x41,
    OP_UNUSED_42                    = 0x42,
    OP_UNUSED_43                    = 0x43,
    OP_AGET                         = 0x44,
    OP_AGET_WIDE                    = 0x45,
    OP_AGET_OBJECT                  = 0x46,
    OP_AGET_BOOLEAN                 = 0x47,
    OP_AGET_BYTE                    = 0x48,
    OP_AGET_CHAR                    = 0x49,
    OP_AGET_SHORT                   = 0x4a,
    OP_APUT                         = 0x4b,
    OP_APUT_WIDE                    = 0x4c,
    OP_APUT_OBJECT                  = 0x4d,
    OP_APUT_BOOLEAN                 = 0x4e,
    OP_APUT_BYTE                    = 0x4f,
    OP_APUT_CHAR                    = 0x50,
    OP_APUT_SHORT                   = 0x51,
    OP_IGET                         = 0x52,
    OP_IGET_WIDE                    = 0x53,
    OP_IGET_OBJECT                  = 0x54,
    OP_IGET_BOOLEAN                 = 0x55,
    OP_IGET_BYTE                    = 0x56,
    OP_IGET_CHAR                    = 0x57,
    OP_IGET_SHORT                   = 0x58,
    OP_IPUT                         = 0x59,
    OP_IPUT_WIDE                    = 0x5a,
    OP_IPUT_OBJECT                  = 0x5b,
    OP_IPUT_BOOLEAN                 = 0x5c,
    OP_IPUT_BYTE                    = 0x5d,
    OP_IPUT_CHAR                    = 0x5e,
    OP_IPUT_SHORT                   = 0x5f,
    OP_SGET                         = 0x60,
    OP_SGET_WIDE                    = 0x61,
    OP_SGET_OBJECT                  = 0x62,
    OP_SGET_BOOLEAN                 = 0x63,
    OP_SGET_BYTE                    = 0x64,
    OP_SGET_CHAR                    = 0x65,
    OP_SGET_SHORT                   = 0x66,
    OP_SPUT                         = 0x67,
    OP_SPUT_WIDE                    = 0x68,
    OP_SPUT_OBJECT                  = 0x69,
    OP_SPUT_BOOLEAN                 = 0x6a,
    OP_SPUT_BYTE                    = 0x6b,
    OP_SPUT_CHAR                    = 0x6c,
    OP_SPUT_SHORT                   = 0x6d,
    OP_INVOKE_VIRTUAL               = 0x6e,
    OP_INVOKE_SUPER                 = 0x6f,
    OP_INVOKE_DIRECT                = 0x70,
    OP_INVOKE_STATIC                = 0x71,
    OP_INVOKE_INTERFACE             = 0x72,
    OP_UNUSED_73                    = 0x73,
    OP_INVOKE_VIRTUAL_RANGE         = 0x74,
    OP_INVOKE_SUPER_RANGE           = 0x75,
    OP_INVOKE_DIRECT_RANGE          = 0x76,
    OP_INVOKE_STATIC_RANGE          = 0x77,
    OP_INVOKE_INTERFACE_RANGE       = 0x78,
    OP_UNUSED_79                    = 0x79,
    OP_UNUSED_7A                    = 0x7a,
    OP_NEG_INT                      = 0x7b,
    OP_NOT_INT                      = 0x7c,
    OP_NEG_LONG                     = 0x7d,
    OP_NOT_LONG                     = 0x7e,
    OP_NEG_FLOAT                    = 0x7f,
    OP_NEG_DOUBLE                   = 0x80,
    OP_INT_TO_LONG                  = 0x81,
    OP_INT_TO_FLOAT                 = 0x82,
    OP_INT_TO_DOUBLE                = 0x83,
    OP_LONG_TO_INT                  = 0x84,
    OP_LONG_TO_FLOAT                = 0x85,
    OP_LONG_TO_DOUBLE               = 0x86,
    OP_FLOAT_TO_INT                 = 0x87,
    OP_FLOAT_TO_LONG                = 0x88,
    OP_FLOAT_TO_DOUBLE              = 0x89,
    OP_DOUBLE_TO_INT                = 0x8a,
    OP_DOUBLE_TO_LONG               = 0x8b,
    OP_DOUBLE_TO_FLOAT              = 0x8c,
    OP_INT_TO_BYTE                  = 0x8d,
    OP_INT_TO_CHAR                  = 0x8e,
    OP_INT_TO_SHORT                 = 0x8f,
    OP_ADD_INT                      = 0x90,
    OP_SUB_INT                      = 0x91,
    OP_MUL_INT                      = 0x92,
    OP_DIV_INT                      = 0x93,
    OP_REM_INT                      = 0x94,
    OP_AND_INT                      = 0x95,
    OP_OR_INT                       = 0x96,
    OP_XOR_INT                      = 0x97,
    OP_SHL_INT                      = 0x98,
    OP_SHR_INT                      = 0x99,
    OP_USHR_INT                     = 0x9a,
    OP_ADD_LONG                     = 0x9b,
    OP_SUB_LONG                     = 0x9c,
    OP_MUL_LONG                     = 0x9d,
    OP_DIV_LONG                     = 0x9e,
    OP_REM_LONG                     = 0x9f,
    OP_AND_LONG                     = 0xa0,
    OP_OR_LONG                      = 0xa1,
    OP_XOR_LONG                     = 0xa2,
    OP_SHL_LONG                     = 0xa3,
    OP_SHR_LONG                     = 0xa4,
    OP_USHR_LONG                    = 0xa5,
    OP_ADD_FLOAT                    = 0xa6,
    OP_SUB_FLOAT                    = 0xa7,
    OP_MUL_FLOAT                    = 0xa8,
    OP_DIV_FLOAT                    = 0xa9,
    OP_REM_FLOAT                    = 0xaa,
    OP_ADD_DOUBLE                   = 0xab,
    OP_SUB_DOUBLE                   = 0xac,
    OP_MUL_DOUBLE                   = 0xad,
    OP_DIV_DOUBLE                   = 0xae,
    OP_REM_DOUBLE                   = 0xaf,
    OP_ADD_INT_2ADDR                = 0xb0,
    OP_SUB_INT_2ADDR                = 0xb1,
    OP_MUL_INT_2ADDR                = 0xb2,
    OP_DIV_INT_2ADDR                = 0xb3,
    OP_REM_INT_2ADDR                = 0xb4,
    OP_AND_INT_2ADDR                = 0xb5,
    OP_OR_INT_2ADDR                 = 0xb6,
    OP_XOR_INT_2ADDR                = 0xb7,
    OP_SHL_INT_2ADDR                = 0xb8,
    OP_SHR_INT_2ADDR                = 0xb9,
    OP_USHR_INT_2ADDR               = 0xba,
    OP_ADD_LONG_2ADDR               = 0xbb,
    OP_SUB_LONG_2ADDR               = 0xbc,
    OP_MUL_LONG_2ADDR               = 0xbd,
    OP_DIV_LONG_2ADDR               = 0xbe,
    OP_REM_LONG_2ADDR               = 0xbf,
    OP_AND_LONG_2ADDR               = 0xc0,
    OP_OR_LONG_2ADDR                = 0xc1,
    OP_XOR_LONG_2ADDR               = 0xc2,
    OP_SHL_LONG_2ADDR               = 0xc3,
    OP_SHR_LONG_2ADDR               = 0xc4,
    OP_USHR_LONG_2ADDR              = 0xc5,
    OP_ADD_FLOAT_2ADDR              = 0xc6,
    OP_SUB_FLOAT_2ADDR              = 0xc7,
    OP_MUL_FLOAT_2ADDR              = 0xc8,
    OP_DIV_FLOAT_2ADDR              = 0xc9,
    OP_REM_FLOAT_2ADDR              = 0xca,
    OP_ADD_DOUBLE_2ADDR             = 0xcb,
    OP_SUB_DOUBLE_2ADDR             = 0xcc,
    OP_MUL_DOUBLE_2ADDR             = 0xcd,
    OP_DIV_DOUBLE_2ADDR             = 0xce,
    OP_REM_DOUBLE_2ADDR             = 0xcf,
    OP_ADD_INT_LIT16                = 0xd0,
    OP_RSUB_INT                     = 0xd1,
    OP_MUL_INT_LIT16                = 0xd2,
    OP_DIV_INT_LIT16                = 0xd3,
    OP_REM_INT_LIT16                = 0xd4,
    OP_AND_INT_LIT16                = 0xd5,
    OP_OR_INT_LIT16                 = 0xd6,
    OP_XOR_INT_LIT16                = 0xd7,
    OP_ADD_INT_LIT8                 = 0xd8,
    OP_RSUB_INT_LIT8                = 0xd9,
    OP_MUL_INT_LIT8                 = 0xda,
    OP_DIV_INT_LIT8                 = 0xdb,
    OP_REM_INT_LIT8                 = 0xdc,
    OP_AND_INT_LIT8                 = 0xdd,
    OP_OR_INT_LIT8                  = 0xde,
    OP_XOR_INT_LIT8                 = 0xdf,
    OP_SHL_INT_LIT8                 = 0xe0,
    OP_SHR_INT_LIT8                 = 0xe1,
    OP_USHR_INT_LIT8                = 0xe2,
    OP_IGET_VOLATILE                = 0xe3,
    OP_IPUT_VOLATILE                = 0xe4,
    OP_SGET_VOLATILE                = 0xe5,
    OP_SPUT_VOLATILE                = 0xe6,
    OP_IGET_OBJECT_VOLATILE         = 0xe7,
    OP_IGET_WIDE_VOLATILE           = 0xe8,
    OP_IPUT_WIDE_VOLATILE           = 0xe9,
    OP_SGET_WIDE_VOLATILE           = 0xea,
    OP_SPUT_WIDE_VOLATILE           = 0xeb,
    OP_BREAKPOINT                   = 0xec,
    OP_THROW_VERIFICATION_ERROR     = 0xed,
    OP_EXECUTE_INLINE               = 0xee,
    OP_EXECUTE_INLINE_RANGE         = 0xef,
    OP_INVOKE_OBJECT_INIT_RANGE     = 0xf0,
    OP_RETURN_VOID_BARRIER          = 0xf1,
    OP_IGET_QUICK                   = 0xf2,
    OP_IGET_WIDE_QUICK              = 0xf3,
    OP_IGET_OBJECT_QUICK            = 0xf4,
    OP_IPUT_QUICK                   = 0xf5,
    OP_IPUT_WIDE_QUICK              = 0xf6,
    OP_IPUT_OBJECT_QUICK            = 0xf7,
    OP_INVOKE_VIRTUAL_QUICK         = 0xf8,
    OP_INVOKE_VIRTUAL_QUICK_RANGE   = 0xf9,
    OP_INVOKE_SUPER_QUICK           = 0xfa,
    OP_INVOKE_SUPER_QUICK_RANGE     = 0xfb,
    OP_IPUT_OBJECT_VOLATILE         = 0xfc,
    OP_SGET_OBJECT_VOLATILE         = 0xfd,
    OP_SPUT_OBJECT_VOLATILE         = 0xfe,
};

// From alloc/CardTable.h
#define GC_CARD_SHIFT 7

// From all/Alloc.h
/* flags for dvmMalloc */
enum {
    ALLOC_DEFAULT = 0x00,
    ALLOC_DONT_TRACK = 0x01,  /* don't add to internal tracking list */
    ALLOC_NON_MOVING = 0x02,
};


// From oo/Object.h
struct ClassObject;
struct ArrayObject;
struct DvmDex;
struct Method;

struct Object {
    ClassObject* clazz;
    u4 lock;
    Method** vtable;
    int vtableCount;
    u4 accessFlags;
};

struct ArrayObject : Object {
    u4              length;
    u8              contents[1];
};

struct ClassObject : Object {
    const char* descriptor;
    DvmDex* pDvmDex;
    ClassObject* super;
};

// From InstrUtils.h
enum InstructionFormat {
    kFmt00x = 0,    // unknown format (also used for "breakpoint" opcode)
    kFmt10x,        // op
    kFmt12x,        // op vA, vB
    kFmt11n,        // op vA, #+B
    kFmt11x,        // op vAA
    kFmt10t,        // op +AA
    kFmt20bc,       // [opt] op AA, thing@BBBB
    kFmt20t,        // op +AAAA
    kFmt22x,        // op vAA, vBBBB
    kFmt21t,        // op vAA, +BBBB
    kFmt21s,        // op vAA, #+BBBB
    kFmt21h,        // op vAA, #+BBBB00000[00000000]
    kFmt21c,        // op vAA, thing@BBBB
    kFmt23x,        // op vAA, vBB, vCC
    kFmt22b,        // op vAA, vBB, #+CC
    kFmt22t,        // op vA, vB, +CCCC
    kFmt22s,        // op vA, vB, #+CCCC
    kFmt22c,        // op vA, vB, thing@CCCC
    kFmt22cs,       // [opt] op vA, vB, field offset CCCC
    kFmt30t,        // op +AAAAAAAA
    kFmt32x,        // op vAAAA, vBBBB
    kFmt31i,        // op vAA, #+BBBBBBBB
    kFmt31t,        // op vAA, +BBBBBBBB
    kFmt31c,        // op vAA, string@BBBBBBBB
    kFmt35c,        // op {vC,vD,vE,vF,vG}, thing@BBBB
    kFmt35ms,       // [opt] invoke-virtual+super
    kFmt3rc,        // op {vCCCC .. v(CCCC+AA-1)}, thing@BBBB
    kFmt3rms,       // [opt] invoke-virtual+super/range
    kFmt51l,        // op vAA, #+BBBBBBBBBBBBBBBB
    kFmt35mi,       // [opt] inline invoke
    kFmt3rmi,       // [opt] inline invoke/range
    kFmt33x,        // exop vAA, vBB, vCCCC
    kFmt32s,        // exop vAA, vBB, #+CCCC
    kFmt40sc,       // [opt] exop AAAA, thing@BBBBBBBB
    kFmt41c,        // exop vAAAA, thing@BBBBBBBB
    kFmt52c,        // exop vAAAA, vBBBB, thing@CCCCCCCC
    kFmt5rc,        // exop {vCCCC .. v(CCCC+AAAA-1)}, thing@BBBBBBBB
};

enum InstructionIndexType {
    kIndexUnknown = 0,
    kIndexNone,         // has no index
    kIndexVaries,       // "It depends." Used for throw-verification-error
    kIndexTypeRef,      // type reference index
    kIndexStringRef,    // string reference index
    kIndexMethodRef,    // method reference index
    kIndexFieldRef,     // field reference index
    kIndexInlineMethod, // inline method index (for inline linked methods)
    kIndexVtableOffset, // vtable offset (for static linked methods)
    kIndexFieldOffset   // field offset (for static linked fields)
};

typedef u1 InstructionWidth;

typedef u1 OpcodeFlags;
enum OpcodeFlagsBits {
    kInstrCanBranch     = 1,        // conditional or unconditional branch
    kInstrCanContinue   = 1 << 1,   // flow can continue to next statement
    kInstrCanSwitch     = 1 << 2,   // switch statement
    kInstrCanThrow      = 1 << 3,   // could cause an exception to be thrown
    kInstrCanReturn     = 1 << 4,   // returns, no additional statements
    kInstrInvoke        = 1 << 5,   // a flavor of invoke
};

struct InstructionInfoTables {
    u1*                formats;    /* InstructionFormat elements */
    u1*                indexTypes; /* InstructionIndexType elements */
    OpcodeFlags*       flags;
    InstructionWidth*  widths;
};

extern InstructionInfoTables gDexOpcodeInfo;

struct DecodedInstruction {
    u4      vA;
    u4      vB;
    u8      vB_wide;        /* for kFmt51l */
    u4      vC;
    u4      arg[5];         /* vC/D/E/F/G in invoke or filled-new-array */
    Opcode  opcode;
    InstructionIndexType indexType;
};

#define DEX_INLINE inline
#define INLINE inline

DEX_INLINE size_t dexGetWidthFromOpcode(Opcode opcode)
{
    assert((u4) opcode < kNumPackedOpcodes);
    return gDexOpcodeInfo.widths[opcode];
}

size_t dexGetWidthFromInstruction(const u2* insns);

DEX_INLINE OpcodeFlags dexGetFlagsFromOpcode(Opcode opcode)
{
    assert((u4) opcode < kNumPackedOpcodes);
    return gDexOpcodeInfo.flags[opcode];
}

DEX_INLINE bool dexIsGoto(OpcodeFlags flags)
{
    return (flags & (kInstrCanBranch | kInstrCanContinue)) == kInstrCanBranch;
}

DEX_INLINE InstructionFormat dexGetFormatFromOpcode(Opcode opcode)
{
    assert((u4) opcode < kNumPackedOpcodes);
    return (InstructionFormat) gDexOpcodeInfo.formats[opcode];
}

DEX_INLINE InstructionIndexType dexGetIndexTypeFromOpcode(Opcode opcode)
{
    assert((u4) opcode < kNumPackedOpcodes);
    return (InstructionIndexType) gDexOpcodeInfo.indexTypes[opcode];
}

void dexDecodeInstruction(const u2* insns, DecodedInstruction* pDec);

// From DexOpcodes
const char* dexGetOpcodeName(Opcode op);

DEX_INLINE Opcode dexOpcodeFromCodeUnit(u2 codeUnit) {
    int lowByte = codeUnit & 0xff;
    if (lowByte != 0xff) {
        return (Opcode) lowByte;
    } else {
        return (Opcode) ((codeUnit >> 8) | 0x100);
    }
}

// From DexFile
struct DexFile;
struct DexTry {
    u4  startAddr;          /* start address, in 16-bit code units */
    u2  insnCount;          /* instruction count, in 16-bit code units */
    u2  handlerOff;         /* offset in encoded handler data to handlers */
};
struct DexCode {
    u2  registersSize;
    u2  insSize;
    u2  outsSize;
    u2  triesSize;
    u4  debugInfoOff;       /* file offset to debug info stream */
    u4  insnsSize;          /* size of the insns array, in u2 units */
    u2  insns[1];
};

DEX_INLINE const DexTry* dexGetTries(const DexCode* pCode) {
    const u2* insnsEnd = &pCode->insns[pCode->insnsSize];

    // Round to four bytes.
    if ((((u4) insnsEnd) & 3) != 0) {
        insnsEnd++;
    }

    return (const DexTry*) insnsEnd;
}

enum {
    ACC_PUBLIC       = 0x00000001,       // class, field, method, ic
    ACC_PRIVATE      = 0x00000002,       // field, method, ic
    ACC_PROTECTED    = 0x00000004,       // field, method, ic
    ACC_STATIC       = 0x00000008,       // field, method, ic
    ACC_FINAL        = 0x00000010,       // class, field, method, ic
    ACC_SYNCHRONIZED = 0x00000020,       // method (only allowed on natives)
    ACC_SUPER        = 0x00000020,       // class (not used in Dalvik)
    ACC_VOLATILE     = 0x00000040,       // field
    ACC_BRIDGE       = 0x00000040,       // method (1.5)
    ACC_TRANSIENT    = 0x00000080,       // field
    ACC_VARARGS      = 0x00000080,       // method (1.5)
    ACC_NATIVE       = 0x00000100,       // method
    ACC_INTERFACE    = 0x00000200,       // class, ic
    ACC_ABSTRACT     = 0x00000400,       // class, method, ic
    ACC_STRICT       = 0x00000800,       // method
    ACC_SYNTHETIC    = 0x00001000,       // field, method, ic
    ACC_ANNOTATION   = 0x00002000,       // class, ic (1.5)
    ACC_ENUM         = 0x00004000,       // class, field, ic (1.5)
    ACC_CONSTRUCTOR  = 0x00010000,       // method (Dalvik only)
    ACC_DECLARED_SYNCHRONIZED =
                       0x00020000,       // method (Dalvik only)
    ACC_CLASS_MASK =
        (ACC_PUBLIC | ACC_FINAL | ACC_INTERFACE | ACC_ABSTRACT
                | ACC_SYNTHETIC | ACC_ANNOTATION | ACC_ENUM),
    ACC_INNER_CLASS_MASK =
        (ACC_CLASS_MASK | ACC_PRIVATE | ACC_PROTECTED | ACC_STATIC),
    ACC_FIELD_MASK =
        (ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED | ACC_STATIC | ACC_FINAL
                | ACC_VOLATILE | ACC_TRANSIENT | ACC_SYNTHETIC | ACC_ENUM),
    ACC_METHOD_MASK =
        (ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED | ACC_STATIC | ACC_FINAL
                | ACC_SYNCHRONIZED | ACC_BRIDGE | ACC_VARARGS | ACC_NATIVE
                | ACC_ABSTRACT | ACC_STRICT | ACC_SYNTHETIC | ACC_CONSTRUCTOR
                | ACC_DECLARED_SYNCHRONIZED),
};

// From DexProto
struct DexProto {
    const DexFile* dexFile;     /* file the idx refers to */
    u4 protoIdx;                /* index into proto_ids table of dexFile */
};
char* dexProtoCopyMethodDescriptor(const DexProto* pProto);

// From DexCatch.h
struct DexCatchHandler {
    u4          typeIdx;    /* type index of the caught exception type */
    u4          address;    /* handler address */
};
struct DexCatchIterator {
    const u1* pEncodedData;
    bool catchesAll;
    u4 countRemaining;
    DexCatchHandler handler;
};
bool dexFindCatchHandler(DexCatchIterator *pIterator,
                         const DexCode* pCode, u4 address);
DexCatchHandler* dexCatchIteratorNext(DexCatchIterator* pIterator);
u4 dexGetFirstHandlerOffset(const DexCode* pCode);
u4 dexGetHandlersSize(const DexCode* pCode);
u4 dexCatchIteratorGetEndOffset(DexCatchIterator* pIterator,
                                const DexCode* pCode);
void dexCatchIteratorInit(DexCatchIterator* pIterator,
    const DexCode* pCode, u4 offset);



// From DvmDex.h
struct Field;
struct StringObject;
struct DvmDex {
    struct Method** pResMethods;
    struct ClassObject** pResClasses;
    struct Field** pResFields;
    struct StringObject** pResStrings;
};

// from oo/Object.h
struct Method {
    ClassObject* clazz;
    u2 methodIndex;
    u2 registersSize;
    u2 outsSize;
    u2 insSize;
    u4 accessFlags;
    const char* name;
    const char* shorty;
    DexProto prototype;
    void* compiledInsns;
    DvmDex* pDvmDex;
    u2* insns;
    struct Method** pResMethods;
};

struct Field {
    ClassObject*    clazz;          /* class in which the field is declared */
    const char*     name;
    const char*     signature;      /* e.g. "I", "[C", "Landroid/os/Debug;" */
    u4              accessFlags;
};

struct InstField : Field {
    int             byteOffset;
};
/*
 * Static field.
 */
struct StaticField : Field {
    JValue          value;          /* initially set from DEX for primitives */
};

INLINE bool dvmIsVolatileField(const Field* field) {
    return (field->accessFlags & ACC_VOLATILE) != 0;
}

INLINE bool dvmIsBytecodeMethod(const Method* method) {
    return (method->accessFlags & (ACC_NATIVE | ACC_ABSTRACT)) == 0;
}

INLINE const DexCode* dvmGetMethodCode(const Method* meth) {
    if (dvmIsBytecodeMethod(meth)) {
        return (const DexCode*)
            (((const u1*) meth->insns) - offsetof(DexCode, insns));
    } else {
        return NULL;
    }
}

// From Thread.h
struct Thread {

    u4 threadId;
    u1* cardTable;
    Object* exception;

    /* Runtime support function pointers */
    void* (*pMemcpy)(void*, const void*, size_t);
    float (*pI2f)(int);
    int (*pF2iz)(float);
    float (*pD2f)(double);
    double (*pF2d)(float);
    double (*pI2d)(int);
    int (*pD2iz)(double);
    float (*pL2f)(long);
    double (*pL2d)(long);
    s8 (*pArtF2l)(float);
    s8 (*pArtD2l)(double);
    float (*pFadd)(float, float);
    float (*pFsub)(float, float);
    float (*pFdiv)(float, float);
    float (*pFmul)(float, float);
    float (*pFmodf)(float, float);
    double (*pDadd)(double, double);
    double (*pDsub)(double, double);
    double (*pDdiv)(double, double);
    double (*pDmul)(double, double);
    double (*pFmod)(double, double);
    int (*pIdivmod)(int, int);
    int (*pIdiv)(int, int);
    long long (*pLdivmod)(long long, long long);
    bool (*pArtUnlockObject)(struct Thread*, struct Object*);
    bool (*pArtCanPutArrayElementNoThrow)(const ClassObject*,
          const ClassObject*);
    int (*pArtInstanceofNonTrivialNoThrow)
      (const ClassObject*, const ClassObject*);
    int (*pArtInstanceofNonTrivial) (const ClassObject*, const ClassObject*);
    ArrayObject* (*pArtAllocArrayByClass)(ClassObject*, size_t, int);
    Method* (*pArtFindInterfaceMethodInCache)(ClassObject*, u4, const Method*,
             DvmDex*);
    bool (*pArtUnlockObjectNoThrow)(Thread*, Object*);
    void (*pArtLockObjectNoThrow)(Thread*, Object*);
    Object* (*pArtAllocObjectNoThrow)(ClassObject*, int);
    void (*pArtThrowException)(Thread*, Object*);
    bool (*pArtHandleFillArrayDataNoThrow)(ArrayObject*, const u2*);

};

// From Sync.h
#define LW_LOCK_OWNER_SHIFT 3
#define LW_HASH_STATE_SHIFT 1
#define LW_HASH_STATE_MASK 0x3
#define LW_SHAPE_THIN 0


#include "Compiler.h"

#endif

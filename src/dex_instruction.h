// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_INSTRUCTION_H_
#define ART_SRC_DEX_INSTRUCTION_H_

#include "src/globals.h"
#include "src/logging.h"
#include "src/macros.h"

namespace art {

#define DEX_INSTRUCTION_LIST(V) \
  V(NOP, 0x0) \
  V(MOVE, 0x1) \
  V(MOVE_FROM16, 0x2) \
  V(MOVE_16, 0x3) \
  V(MOVE_WIDE, 0x4) \
  V(MOVE_WIDE_FROM16, 0x5) \
  V(MOVE_WIDE_16, 0x6) \
  V(MOVE_OBJECT, 0x7) \
  V(MOVE_OBJECT_FROM16, 0x8) \
  V(MOVE_OBJECT_16, 0x9) \
  V(MOVE_RESULT, 0xA) \
  V(MOVE_RESULT_WIDE, 0xB) \
  V(MOVE_RESULT_OBJECT, 0xC) \
  V(MOVE_EXCEPTION, 0xD) \
  V(RETURN_VOID, 0xE) \
  V(RETURN, 0xF) \
  V(RETURN_WIDE, 0x10) \
  V(RETURN_OBJECT, 0x11) \
  V(CONST_4, 0x12) \
  V(CONST_16, 0x13) \
  V(CONST, 0x14) \
  V(CONST_HIGH16, 0x15) \
  V(CONST_WIDE_16, 0x16) \
  V(CONST_WIDE_32, 0x17) \
  V(CONST_WIDE, 0x18) \
  V(CONST_WIDE_HIGH16, 0x19) \
  V(CONST_STRING, 0x1A) \
  V(CONST_STRING_JUMBO, 0x1B) \
  V(CONST_CLASS, 0x1C) \
  V(MONITOR_ENTER, 0x1D) \
  V(MONITOR_EXIT, 0x1E) \
  V(CHECK_CAST, 0x1F) \
  V(INSTANCE_OF, 0x20) \
  V(ARRAY_LENGTH, 0x21) \
  V(NEW_INSTANCE, 0x22) \
  V(NEW_ARRAY, 0x23) \
  V(FILLED_NEW_ARRAY, 0x24) \
  V(FILLED_NEW_ARRAY_RANGE, 0x25) \
  V(FILL_ARRAY_DATA, 0x26) \
  V(THROW, 0x27) \
  V(GOTO, 0x28) \
  V(GOTO_16, 0x29) \
  V(GOTO_32, 0x2A) \
  V(PACKED_SWITCH, 0x2B) \
  V(SPARSE_SWITCH, 0x2C) \
  V(CMPL_FLOAT, 0x2D) \
  V(CMPG_FLOAT, 0x2E) \
  V(CMPL_DOUBLE, 0x2F) \
  V(CMPG_DOUBLE, 0x30) \
  V(CMP_LONG, 0x31) \
  V(IF_EQ, 0x32) \
  V(IF_NE, 0x33) \
  V(IF_LT, 0x34) \
  V(IF_GE, 0x35) \
  V(IF_GT, 0x36) \
  V(IF_LE, 0x37) \
  V(IF_EQZ, 0x38) \
  V(IF_NEZ, 0x39) \
  V(IF_LTZ, 0x3A) \
  V(IF_GEZ, 0x3B) \
  V(IF_GTZ, 0x3C) \
  V(IF_LEZ, 0x3D) \
  V(AGET, 0x44) \
  V(AGET_WIDE, 0x45) \
  V(AGET_OBJECT, 0x46) \
  V(AGET_BOOLEAN, 0x47) \
  V(AGET_BYTE, 0x48) \
  V(AGET_CHAR, 0x49) \
  V(AGET_SHORT, 0x4A) \
  V(APUT, 0x4B) \
  V(APUT_WIDE, 0x4C) \
  V(APUT_OBJECT, 0x4D) \
  V(APUT_BOOLEAN, 0x4E) \
  V(APUT_BYTE, 0x4F) \
  V(APUT_CHAR, 0x50) \
  V(APUT_SHORT, 0x51) \
  V(IGET, 0x52) \
  V(IGET_WIDE, 0x53) \
  V(IGET_OBJECT, 0x54) \
  V(IGET_BOOLEAN, 0x55) \
  V(IGET_BYTE, 0x56) \
  V(IGET_CHAR, 0x57) \
  V(IGET_SHORT, 0x58) \
  V(IPUT, 0x59) \
  V(IPUT_WIDE, 0x5A) \
  V(IPUT_OBJECT, 0x5B) \
  V(IPUT_BOOLEAN, 0x5C) \
  V(IPUT_BYTE, 0x5D) \
  V(IPUT_CHAR, 0x5E) \
  V(IPUT_SHORT, 0x5F) \
  V(SGET, 0x60) \
  V(SGET_WIDE, 0x61) \
  V(SGET_OBJECT, 0x62) \
  V(SGET_BOOLEAN, 0x63) \
  V(SGET_BYTE, 0x64) \
  V(SGET_CHAR, 0x65) \
  V(SGET_SHORT, 0x66) \
  V(SPUT, 0x67) \
  V(SPUT_WIDE, 0x68) \
  V(SPUT_OBJECT, 0x69) \
  V(SPUT_BOOLEAN, 0x6A) \
  V(SPUT_BYTE, 0x6B) \
  V(SPUT_CHAR, 0x6C) \
  V(SPUT_SHORT, 0x6D) \
  V(INVOKE_VIRTUAL, 0x6E) \
  V(INVOKE_SUPER, 0x6F) \
  V(INVOKE_DIRECT, 0x70) \
  V(INVOKE_STATIC, 0x71) \
  V(INVOKE_INTERFACE, 0x72) \
  V(INVOKE_VIRTUAL_RANGE, 0x74) \
  V(INVOKE_SUPER_RANGE, 0x75) \
  V(INVOKE_DIRECT_RANGE, 0x76) \
  V(INVOKE_STATIC_RANGE, 0x77) \
  V(INVOKE_INTERFACE_RANGE, 0x78) \
  V(NEG_INT, 0x7B) \
  V(NOT_INT, 0x7C) \
  V(NEG_LONG, 0x7D) \
  V(NOT_LONG, 0x7E) \
  V(NEG_FLOAT, 0x7F) \
  V(NEG_DOUBLE, 0x80) \
  V(INT_TO_LONG, 0x81) \
  V(INT_TO_FLOAT, 0x82) \
  V(INT_TO_DOUBLE, 0x83) \
  V(LONG_TO_INT, 0x84) \
  V(LONG_TO_FLOAT, 0x85) \
  V(LONG_TO_DOUBLE, 0x86) \
  V(FLOAT_TO_INT, 0x87) \
  V(FLOAT_TO_LONG, 0x88) \
  V(FLOAT_TO_DOUBLE, 0x89) \
  V(DOUBLE_TO_INT, 0x8A) \
  V(DOUBLE_TO_LONG, 0x8B) \
  V(DOUBLE_TO_FLOAT, 0x8C) \
  V(INT_TO_BYTE, 0x8D) \
  V(INT_TO_CHAR, 0x8E) \
  V(INT_TO_SHORT, 0x8F) \
  V(ADD_INT, 0x90) \
  V(SUB_INT, 0x91) \
  V(MUL_INT, 0x92) \
  V(DIV_INT, 0x93) \
  V(REM_INT, 0x94) \
  V(AND_INT, 0x95) \
  V(OR_INT, 0x96) \
  V(XOR_INT, 0x97) \
  V(SHL_INT, 0x98) \
  V(SHR_INT, 0x99) \
  V(USHR_INT, 0x9A) \
  V(ADD_LONG, 0x9B) \
  V(SUB_LONG, 0x9C) \
  V(MUL_LONG, 0x9D) \
  V(DIV_LONG, 0x9E) \
  V(REM_LONG, 0x9F) \
  V(AND_LONG, 0xA0) \
  V(OR_LONG, 0xA1) \
  V(XOR_LONG, 0xA2) \
  V(SHL_LONG, 0xA3) \
  V(SHR_LONG, 0xA4) \
  V(USHR_LONG, 0xA5) \
  V(ADD_FLOAT, 0xA6) \
  V(SUB_FLOAT, 0xA7) \
  V(MUL_FLOAT, 0xA8) \
  V(DIV_FLOAT, 0xA9) \
  V(REM_FLOAT, 0xAA) \
  V(ADD_DOUBLE, 0xAB) \
  V(SUB_DOUBLE, 0xAC) \
  V(MUL_DOUBLE, 0xAD) \
  V(DIV_DOUBLE, 0xAE) \
  V(REM_DOUBLE, 0xAF) \
  V(ADD_INT_2ADDR, 0xB0) \
  V(SUB_INT_2ADDR, 0xB1) \
  V(MUL_INT_2ADDR, 0xB2) \
  V(DIV_INT_2ADDR, 0xB3) \
  V(REM_INT_2ADDR, 0xB4) \
  V(AND_INT_2ADDR, 0xB5) \
  V(OR_INT_2ADDR, 0xB6) \
  V(XOR_INT_2ADDR, 0xB7) \
  V(SHL_INT_2ADDR, 0xB8) \
  V(SHR_INT_2ADDR, 0xB9) \
  V(USHR_INT_2ADDR, 0xBA) \
  V(ADD_LONG_2ADDR, 0xBB) \
  V(SUB_LONG_2ADDR, 0xBC) \
  V(MUL_LONG_2ADDR, 0xBD) \
  V(DIV_LONG_2ADDR, 0xBE) \
  V(REM_LONG_2ADDR, 0xBF) \
  V(AND_LONG_2ADDR, 0xC0) \
  V(OR_LONG_2ADDR, 0xC1) \
  V(XOR_LONG_2ADDR, 0xC2) \
  V(SHL_LONG_2ADDR, 0xC3) \
  V(SHR_LONG_2ADDR, 0xC4) \
  V(USHR_LONG_2ADDR, 0xC5) \
  V(ADD_FLOAT_2ADDR, 0xC6) \
  V(SUB_FLOAT_2ADDR, 0xC7) \
  V(MUL_FLOAT_2ADDR, 0xC8) \
  V(DIV_FLOAT_2ADDR, 0xC9) \
  V(REM_FLOAT_2ADDR, 0xCA) \
  V(ADD_DOUBLE_2ADDR, 0xCB) \
  V(SUB_DOUBLE_2ADDR, 0xCC) \
  V(MUL_DOUBLE_2ADDR, 0xCD) \
  V(DIV_DOUBLE_2ADDR, 0xCE) \
  V(REM_DOUBLE_2ADDR, 0xCF) \
  V(ADD_INT_LIT16, 0xD0) \
  V(RSUB_INT, 0xD1) \
  V(MUL_INT_LIT16, 0xD2) \
  V(DIV_INT_LIT16, 0xD3) \
  V(REM_INT_LIT16, 0xD4) \
  V(AND_INT_LIT16, 0xD5) \
  V(OR_INT_LIT16, 0xD6) \
  V(XOR_INT_LIT16, 0xD7) \
  V(ADD_INT_LIT8, 0xD8) \
  V(RSUB_INT_LIT8, 0xD9) \
  V(MUL_INT_LIT8, 0xDA) \
  V(DIV_INT_LIT8, 0xDB) \
  V(REM_INT_LIT8, 0xDC) \
  V(AND_INT_LIT8, 0xDD) \
  V(OR_INT_LIT8, 0xDE) \
  V(XOR_INT_LIT8, 0xDF) \
  V(SHL_INT_LIT8, 0xE0) \
  V(SHR_INT_LIT8, 0xE1) \
  V(USHR_INT_LIT8, 0xE2)

class Instruction {
 public:
#define INSTRUCTION_ENUM(cname, opcode) cname = opcode,
  enum Code {
    DEX_INSTRUCTION_LIST(INSTRUCTION_ENUM)
  };
#undef INSTRUCTION_ENUM

  // Returns the size in bytes of this instruction.
  size_t Size();

  // Returns a pointer to the next instruction in the stream.
  const Instruction* Next();

  // Returns the opcode field of the instruction.
  Code Opcode();

  // Reads an instruction out of the stream at the specified address.
  static Instruction* At(byte* code) {
    CHECK(code != NULL);
    return reinterpret_cast<Instruction*>(code);
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Instruction);
};

}  // namespace art

#endif  // ART_SRC_DEX_INSTRUCTION_H_

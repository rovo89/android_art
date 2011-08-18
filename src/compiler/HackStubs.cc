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

#include "Dalvik.h"

/* Hacky stubs for old-word functionality */
InstructionInfoTables gDexOpcodeInfo;

const char* dexGetOpcodeName(Opcode op) { return NULL; }

void dexDecodeInstruction(unsigned short const* insns,
                          DecodedInstruction* pDec) {}

char* dexProtoCopyMethodDescriptor(DexProto const* gProto) {return NULL;}

u4 dexGetFirstHandlerOffset(DexCode const* pCode) { return 0; }

u4 dexGetHandlersSize(DexCode const* pCode) { return 0; }

void dexCatchIteratorInit(DexCatchIterator* pIterator,
                          DexCode const* pCode, unsigned int offset) {}

DexCatchHandler* dexCatchIteratorNext(DexCatchIterator* pCode) { return NULL; }

u4 dexCatchIteratorGetEndOffset(DexCatchIterator* pIterator,
                                DexCode const* pCode) { return 0; }

bool dexFindCatchHandler(DexCatchIterator* pIterator, DexCode const* pCode,
                       unsigned int address) { return false; }

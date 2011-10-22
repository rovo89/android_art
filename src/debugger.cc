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

#include "debugger.h"

namespace art {

bool Dbg::DebuggerStartup() {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::DebuggerShutdown() {
  UNIMPLEMENTED(FATAL);
}

DebugInvokeReq* Dbg::GetInvokeReq() {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void Dbg::Connected() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::Active() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::Disconnected() {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::IsDebuggerConnected() {
  UNIMPLEMENTED(WARNING);
  return false;
}

bool Dbg::IsDebuggingEnabled() {
  UNIMPLEMENTED(WARNING);
  return false; //return gDvm.jdwpConfigured;
}

int64_t Dbg::LastDebuggerActivity() {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int Dbg::ThreadRunning() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

int Dbg::ThreadWaiting() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

int Dbg::ThreadContinuing(int status) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void Dbg::UndoDebuggerSuspensions() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::Exit(int status) {
  UNIMPLEMENTED(FATAL);
}

const char* Dbg::GetClassDescriptor(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

JDWP::ObjectId Dbg::GetClassObject(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::RefTypeId Dbg::GetSuperclass(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::GetClassLoader(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

uint32_t Dbg::GetAccessFlags(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::IsInterface(JDWP::RefTypeId id) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::GetClassList(uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetVisibleClassList(JDWP::ObjectId classLoaderId, uint32_t* pNumClasses, JDWP::RefTypeId** pClassRefBuf) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetClassInfo(JDWP::RefTypeId classId, uint8_t* pTypeTag, uint32_t* pStatus, const char** pSignature) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::FindLoadedClassBySignature(const char* classDescriptor, JDWP::RefTypeId* pRefTypeId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::GetObjectType(JDWP::ObjectId objectId, uint8_t* pRefTypeTag, JDWP::RefTypeId* pRefTypeId) {
  UNIMPLEMENTED(FATAL);
}

uint8_t Dbg::GetClassObjectType(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

const char* Dbg::GetSignature(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

const char* Dbg::GetSourceFile(JDWP::RefTypeId refTypeId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

const char* Dbg::GetObjectTypeName(JDWP::ObjectId objectId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

uint8_t Dbg::GetObjectTag(JDWP::ObjectId objectId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

int Dbg::GetTagWidth(int tag) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

int Dbg::GetArrayLength(JDWP::ObjectId arrayId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

uint8_t Dbg::GetArrayElementTag(JDWP::ObjectId arrayId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::OutputArray(JDWP::ObjectId arrayId, int firstIndex, int count, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
  return false;
}

bool Dbg::SetArrayElements(JDWP::ObjectId arrayId, int firstIndex, int count, const uint8_t* buf) {
  UNIMPLEMENTED(FATAL);
  return false;
}

JDWP::ObjectId Dbg::CreateString(const char* str) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::CreateObject(JDWP::RefTypeId classId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::CreateArrayObject(JDWP::RefTypeId arrayTypeId, uint32_t length) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::MatchType(JDWP::RefTypeId instClassId, JDWP::RefTypeId classId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

const char* Dbg::GetMethodName(JDWP::RefTypeId refTypeId, JDWP::MethodId id) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

void Dbg::OutputAllFields(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputAllMethods(JDWP::RefTypeId refTypeId, bool withGeneric, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputAllInterfaces(JDWP::RefTypeId refTypeId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputLineTable(JDWP::RefTypeId refTypeId, JDWP::MethodId methodId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::OutputVariableTable(JDWP::RefTypeId refTypeId, JDWP::MethodId id, bool withGeneric, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

uint8_t Dbg::GetFieldBasicTag(JDWP::ObjectId objId, JDWP::FieldId fieldId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

uint8_t Dbg::GetStaticFieldBasicTag(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void Dbg::GetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SetFieldValue(JDWP::ObjectId objectId, JDWP::FieldId fieldId, uint64_t value, int width) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, JDWP::ExpandBuf* pReply) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SetStaticFieldValue(JDWP::RefTypeId refTypeId, JDWP::FieldId fieldId, uint64_t rawValue, int width) {
  UNIMPLEMENTED(FATAL);
}

char* Dbg::StringToUtf8(JDWP::ObjectId strId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

char* Dbg::GetThreadName(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

JDWP::ObjectId Dbg::GetThreadGroup(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

char* Dbg::GetThreadGroupName(JDWP::ObjectId threadGroupId) {
  UNIMPLEMENTED(FATAL);
  return NULL;
}

JDWP::ObjectId Dbg::GetThreadGroupParent(JDWP::ObjectId threadGroupId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::GetSystemThreadGroupId() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

JDWP::ObjectId Dbg::GetMainThreadGroupId() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::GetThreadStatus(JDWP::ObjectId threadId, uint32_t* threadStatus, uint32_t* suspendStatus) {
  UNIMPLEMENTED(FATAL);
  return false;
}

uint32_t Dbg::GetThreadSuspendCount(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::ThreadExists(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

bool Dbg::IsSuspended(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

//void Dbg::WaitForSuspend(JDWP::ObjectId threadId);

void Dbg::GetThreadGroupThreads(JDWP::ObjectId threadGroupId, JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::GetAllThreads(JDWP::ObjectId** ppThreadIds, uint32_t* pThreadCount) {
  UNIMPLEMENTED(FATAL);
}

int Dbg::GetThreadFrameCount(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
  return 0;
}

bool Dbg::GetThreadFrame(JDWP::ObjectId threadId, int num, JDWP::FrameId* pFrameId, JDWP::JdwpLocation* pLoc) {
  UNIMPLEMENTED(FATAL);
  return false;
}

JDWP::ObjectId Dbg::GetThreadSelfId() {
  UNIMPLEMENTED(FATAL);
  return 0;
}

void Dbg::SuspendVM(bool isEvent) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::ResumeVM() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SuspendThread(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::ResumeThread(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SuspendSelf() {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::GetThisObject(JDWP::ObjectId threadId, JDWP::FrameId frameId, JDWP::ObjectId* pThisId) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::GetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, uint8_t tag, uint8_t* buf, int expectedLen) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::SetLocalValue(JDWP::ObjectId threadId, JDWP::FrameId frameId, int slot, uint8_t tag, uint64_t value, int width) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::PostLocationEvent(const Method* method, int pcOffset, Object* thisPtr, int eventFlags) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::PostException(void* throwFp, int throwRelPc, void* catchFp, int catchRelPc, Object* exception) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::PostThreadStart(Thread* t) {
  UNIMPLEMENTED(WARNING);
}

void Dbg::PostThreadDeath(Thread* t) {
  UNIMPLEMENTED(WARNING);
}

void Dbg::PostClassPrepare(Class* c) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::WatchLocation(const JDWP::JdwpLocation* pLoc) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::UnwatchLocation(const JDWP::JdwpLocation* pLoc) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::ConfigureStep(JDWP::ObjectId threadId, JDWP::JdwpStepSize size, JDWP::JdwpStepDepth depth) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::UnconfigureStep(JDWP::ObjectId threadId) {
  UNIMPLEMENTED(FATAL);
}

JDWP::JdwpError Dbg::InvokeMethod(JDWP::ObjectId threadId, JDWP::ObjectId objectId, JDWP::RefTypeId classId, JDWP::MethodId methodId, uint32_t numArgs, uint64_t* argArray, uint32_t options, uint8_t* pResultTag, uint64_t* pResultValue, JDWP::ObjectId* pExceptObj) {
  UNIMPLEMENTED(FATAL);
  return JDWP::ERR_NONE;
}

void Dbg::ExecuteMethod(DebugInvokeReq* pReq) {
  UNIMPLEMENTED(FATAL);
}

void Dbg::RegisterObjectId(JDWP::ObjectId id) {
  UNIMPLEMENTED(FATAL);
}

bool Dbg::DdmHandlePacket(const uint8_t* buf, int dataLen, uint8_t** pReplyBuf, int* pReplyLen) {
  UNIMPLEMENTED(FATAL);
  return false;
}

void Dbg::DdmConnected() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::DdmDisconnected() {
  UNIMPLEMENTED(FATAL);
}

void Dbg::DdmSendChunk(int type, size_t length, const uint8_t* buf) {
  UNIMPLEMENTED(WARNING) << "DdmSendChunk(" << type << ", " << length << ", " << (void*) buf << ");";
}

void Dbg::DdmSendChunkV(int type, const struct iovec* iov, int iovcnt) {
  UNIMPLEMENTED(FATAL);
}

}  // namespace art

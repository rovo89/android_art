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

#include "trace.h"

#include <sys/uio.h>

#include "class_linker.h"
#include "debugger.h"
#include "dex_cache.h"
#if !defined(ART_USE_LLVM_COMPILER)
#include "oat/runtime/oat_support_entrypoints.h"
#endif
#include "object_utils.h"
#include "os.h"
#include "scoped_thread_list_lock.h"
#include "thread.h"
#include "thread_list.h"

namespace art {

static const uint32_t kTraceMethodActionMask      = 0x03; // two bits
static const char     kTraceTokenChar             = '*';
static const uint16_t kTraceHeaderLength          = 32;
static const uint32_t kTraceMagicValue            = 0x574f4c53;
static const uint16_t kTraceVersionSingleClock    = 2;
static const uint16_t kTraceVersionDualClock      = 3;
static const uint16_t kTraceRecordSizeSingleClock = 10; // using v2
static const uint16_t kTraceRecordSizeDualClock   = 14; // using v3 with two timestamps

static inline uint32_t TraceMethodId(uint32_t methodValue) {
  return (methodValue & ~kTraceMethodActionMask);
}
static inline uint32_t TraceMethodCombine(uint32_t method, uint8_t traceEvent) {
  return (method | traceEvent);
}

static bool UseThreadCpuClock() {
  // TODO: Allow control over which clock is used
  return true;
}

static bool UseWallClock() {
  // TODO: Allow control over which clock is used
  return true;
}

static void MeasureClockOverhead() {
  if (UseThreadCpuClock()) {
    ThreadCpuMicroTime();
  }
  if (UseWallClock()) {
    MicroTime();
  }
}

static uint32_t GetClockOverhead() {
  uint64_t start = ThreadCpuMicroTime();

  for (int i = 4000; i > 0; i--) {
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
    MeasureClockOverhead();
  }

  uint64_t elapsed = ThreadCpuMicroTime() - start;
  return uint32_t (elapsed / 32);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append2LE(uint8_t* buf, uint16_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append4LE(uint8_t* buf, uint32_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
  *buf++ = (uint8_t) (val >> 16);
  *buf++ = (uint8_t) (val >> 24);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append8LE(uint8_t* buf, uint64_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
  *buf++ = (uint8_t) (val >> 16);
  *buf++ = (uint8_t) (val >> 24);
  *buf++ = (uint8_t) (val >> 32);
  *buf++ = (uint8_t) (val >> 40);
  *buf++ = (uint8_t) (val >> 48);
  *buf++ = (uint8_t) (val >> 56);
}

static bool InstallStubsClassVisitor(Class* klass, void*) {
  Trace* tracer = Runtime::Current()->GetTracer();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    Method* method = klass->GetDirectMethod(i);
    if (tracer->GetSavedCodeFromMap(method) == NULL) {
      tracer->SaveAndUpdateCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    Method* method = klass->GetVirtualMethod(i);
    if (tracer->GetSavedCodeFromMap(method) == NULL) {
      tracer->SaveAndUpdateCode(method);
    }
  }
  return true;
}

static bool UninstallStubsClassVisitor(Class* klass, void*) {
  Trace* tracer = Runtime::Current()->GetTracer();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    Method* method = klass->GetDirectMethod(i);
    if (tracer->GetSavedCodeFromMap(method) != NULL) {
      tracer->ResetSavedCode(method);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    Method* method = klass->GetVirtualMethod(i);
    if (tracer->GetSavedCodeFromMap(method) != NULL) {
      tracer->ResetSavedCode(method);
    }
  }
  return true;
}

static void TraceRestoreStack(Thread* t, void*) {
  Frame frame = t->GetTopOfStack();
  if (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      if (t->IsTraceStackEmpty()) {
        break;
      }
#if defined(ART_USE_LLVM_COMPILER)
      UNIMPLEMENTED(FATAL);
#else
      uintptr_t pc = frame.GetReturnPC();
      Method* method = frame.GetMethod();
      if (IsTraceExitPc(pc)) {
        TraceStackFrame trace_frame = t->PopTraceStackFrame();
        frame.SetReturnPC(trace_frame.return_pc_);
        CHECK(method == trace_frame.method_);
      }
#endif
    }
  }
}

void Trace::AddSavedCodeToMap(const Method* method, const void* code) {
  saved_code_map_.Put(method, code);
}

void Trace::RemoveSavedCodeFromMap(const Method* method) {
  saved_code_map_.erase(method);
}

const void* Trace::GetSavedCodeFromMap(const Method* method) {
  typedef SafeMap<const Method*, const void*>::const_iterator It; // TODO: C++0x auto
  It it = saved_code_map_.find(method);
  if (it == saved_code_map_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

void Trace::SaveAndUpdateCode(Method* method) {
#if defined(ART_USE_LLVM_COMPILER)
  UNIMPLEMENTED(FATAL);
#else
  void* trace_stub = GetLogTraceEntryPoint();
  CHECK(GetSavedCodeFromMap(method) == NULL);
  AddSavedCodeToMap(method, method->GetCode());
  method->SetCode(trace_stub);
#endif
}

void Trace::ResetSavedCode(Method* method) {
  CHECK(GetSavedCodeFromMap(method) != NULL);
  method->SetCode(GetSavedCodeFromMap(method));
  RemoveSavedCodeFromMap(method);
}

void Trace::Start(const char* trace_filename, int trace_fd, int buffer_size, int flags, bool direct_to_ddms) {
  if (Runtime::Current()->IsMethodTracingActive()) {
    LOG(INFO) << "Trace already in progress, ignoring this request";
    return;
  }

  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  Runtime::Current()->GetThreadList()->SuspendAll(false);

  // Open trace file if not going directly to ddms.
  File* trace_file = NULL;
  if (!direct_to_ddms) {
    if (trace_fd < 0) {
      trace_file = OS::OpenFile(trace_filename, true);
    } else {
      trace_file = OS::FileFromFd("tracefile", trace_fd);
    }
    if (trace_file == NULL) {
      PLOG(ERROR) << "Unable to open trace file '" << trace_filename << "'";
      Thread::Current()->ThrowNewException("Ljava/lang/RuntimeException;",
          StringPrintf("Unable to open trace file '%s'", trace_filename).c_str());
      Runtime::Current()->GetThreadList()->ResumeAll(false);
      return;
    }
  }

  // Create Trace object.
  Trace* tracer(new Trace(trace_file, buffer_size, flags));

  // Enable count of allocs if specified in the flags.
  if ((flags && kTraceCountAllocs) != 0) {
    Runtime::Current()->SetStatsEnabled(true);
  }

  Runtime::Current()->EnableMethodTracing(tracer);
  tracer->BeginTracing();

  Runtime::Current()->GetThreadList()->ResumeAll(false);
}

void Trace::Stop() {
  if (!Runtime::Current()->IsMethodTracingActive()) {
    LOG(INFO) << "Trace stop requested, but no trace currently running";
    return;
  }

  ScopedThreadStateChange tsc(Thread::Current(), kRunnable);
  Runtime::Current()->GetThreadList()->SuspendAll(false);

  Runtime::Current()->GetTracer()->FinishTracing();
  Runtime::Current()->DisableMethodTracing();

  Runtime::Current()->GetThreadList()->ResumeAll(false);
}

void Trace::Shutdown() {
  if (!Runtime::Current()->IsMethodTracingActive()) {
    LOG(INFO) << "Trace shutdown requested, but no trace currently running";
    return;
  }
  Runtime::Current()->GetTracer()->FinishTracing();
  Runtime::Current()->DisableMethodTracing();
}

void Trace::BeginTracing() {
  // Set the start time of tracing.
  start_time_ = MicroTime();

  // Set trace version and record size.
  if (UseThreadCpuClock() && UseWallClock()) {
    trace_version_ = kTraceVersionDualClock;
    record_size_ = kTraceRecordSizeDualClock;
  } else {
    trace_version_ = kTraceVersionSingleClock;
    record_size_ = kTraceRecordSizeSingleClock;
  }

  // Set up the beginning of the trace.
  memset(buf_.get(), 0, kTraceHeaderLength);
  Append4LE(buf_.get(), kTraceMagicValue);
  Append2LE(buf_.get() + 4, trace_version_);
  Append2LE(buf_.get() + 6, kTraceHeaderLength);
  Append8LE(buf_.get() + 8, start_time_);
  if (trace_version_ >= kTraceVersionDualClock) {
    Append2LE(buf_.get() + 16, record_size_);
  }

  // Update current offset.
  cur_offset_ = kTraceHeaderLength;

  // Install all method tracing stubs.
  InstallStubs();
}

void Trace::FinishTracing() {
  // Uninstall all method tracing stubs.
  UninstallStubs();

  // Compute elapsed time.
  uint64_t elapsed = MicroTime() - start_time_;

  size_t final_offset = cur_offset_;
  uint32_t clock_overhead = GetClockOverhead();

  if ((flags_ & kTraceCountAllocs) != 0) {
    Runtime::Current()->SetStatsEnabled(false);
  }

  GetVisitedMethods(final_offset);

  std::ostringstream os;

  os << StringPrintf("%cversion\n", kTraceTokenChar);
  os << StringPrintf("%d\n", trace_version_);
  os << StringPrintf("data-file-overflow=%s\n", overflow_ ? "true" : "false");
  if (UseThreadCpuClock()) {
    if (UseWallClock()) {
      os << StringPrintf("clock=dual\n");
    } else {
      os << StringPrintf("clock=thread-cpu\n");
    }
  } else {
    os << StringPrintf("clock=wall\n");
  }
  os << StringPrintf("elapsed-time-usec=%llu\n", elapsed);
  os << StringPrintf("num-method-calls=%zd\n", (final_offset - kTraceHeaderLength) / record_size_);
  os << StringPrintf("clock-call-overhead-nsec=%d\n", clock_overhead);
  os << StringPrintf("vm=art\n");
  if ((flags_ & kTraceCountAllocs) != 0) {
    os << StringPrintf("alloc-count=%d\n", Runtime::Current()->GetStat(KIND_ALLOCATED_OBJECTS));
    os << StringPrintf("alloc-size=%d\n", Runtime::Current()->GetStat(KIND_ALLOCATED_BYTES));
    os << StringPrintf("gc-count=%d\n", Runtime::Current()->GetStat(KIND_GC_INVOCATIONS));
  }
  os << StringPrintf("%cthreads\n", kTraceTokenChar);
  DumpThreadList(os);
  os << StringPrintf("%cmethods\n", kTraceTokenChar);
  DumpMethodList(os);
  os << StringPrintf("%cend\n", kTraceTokenChar);

  std::string header(os.str());
  if (trace_file_.get() == NULL) {
    struct iovec iov[2];
    iov[0].iov_base = reinterpret_cast<void*>(const_cast<char*>(header.c_str()));
    iov[0].iov_len = header.length();
    iov[1].iov_base = buf_.get();
    iov[1].iov_len = final_offset;
    Dbg::DdmSendChunkV(CHUNK_TYPE("MPSE"), iov, 2);
  } else {
    if (!trace_file_->WriteFully(header.c_str(), header.length()) ||
        !trace_file_->WriteFully(buf_.get(), final_offset)) {
      int err = errno;
      LOG(ERROR) << "Trace data write failed: " << strerror(err);
      Thread::Current()->ThrowNewException("Ljava/lang/RuntimeException;",
          StringPrintf("Trace data write failed: %s", strerror(err)).c_str());
    }
  }
}

void Trace::LogMethodTraceEvent(Thread* self, const Method* method, Trace::TraceEvent event) {
  if (thread_clock_base_map_.find(self) == thread_clock_base_map_.end()) {
    uint64_t time = ThreadCpuMicroTime();
    thread_clock_base_map_.Put(self, time);
  }

  // Advance cur_offset_ atomically.
  int32_t new_offset;
  int32_t old_offset;
  do {
    old_offset = cur_offset_;
    new_offset = old_offset + record_size_;
    if (new_offset > buffer_size_) {
      overflow_ = true;
      return;
    }
  } while (android_atomic_release_cas(old_offset, new_offset, &cur_offset_) != 0);

  uint32_t method_value = TraceMethodCombine(reinterpret_cast<uint32_t>(method), event);

  // Write data
  uint8_t* ptr = buf_.get() + old_offset;
  Append2LE(ptr, self->GetTid());
  Append4LE(ptr + 2, method_value);
  ptr += 6;

  if (UseThreadCpuClock()) {
    uint64_t thread_clock_base = thread_clock_base_map_.find(self)->second;
    uint32_t thread_clock_diff = ThreadCpuMicroTime() - thread_clock_base;
    Append4LE(ptr, thread_clock_diff);
    ptr += 4;
  }

  if (UseWallClock()) {
    uint32_t wall_clock_diff = MicroTime() - start_time_;
    Append4LE(ptr, wall_clock_diff);
  }
}

void Trace::GetVisitedMethods(size_t end_offset) {
  uint8_t* ptr = buf_.get() + kTraceHeaderLength;
  uint8_t* end = buf_.get() + end_offset;

  while (ptr < end) {
    uint32_t method_value = ptr[2] | (ptr[3] << 8) | (ptr[4] << 16) | (ptr[5] << 24);
    Method* method = reinterpret_cast<Method*>(TraceMethodId(method_value));
    visited_methods_.insert(method);
    ptr += record_size_;
  }
}

void Trace::DumpMethodList(std::ostream& os) {
  typedef std::set<const Method*>::const_iterator It; // TODO: C++0x auto
  for (It it = visited_methods_.begin(); it != visited_methods_.end(); ++it) {
    const Method* method = *it;
    MethodHelper mh(method);
    os << StringPrintf("%p\t%s\t%s\t%s\t%s\t%d\n", method,
        PrettyDescriptor(mh.GetDeclaringClassDescriptor()).c_str(), mh.GetName(),
        mh.GetSignature().c_str(), mh.GetDeclaringClassSourceFile(),
        mh.GetLineNumFromNativePC(0));
  }
}

static void DumpThread(Thread* t, void* arg) {
  std::ostream& os = *reinterpret_cast<std::ostream*>(arg);
  std::string name;
  t->GetThreadName(name);
  os << t->GetTid() << "\t" << name << "\n";
}

void Trace::DumpThreadList(std::ostream& os) {
  ScopedThreadListLock thread_list_lock;
  Runtime::Current()->GetThreadList()->ForEach(DumpThread, &os);
}

void Trace::InstallStubs() {
  Runtime::Current()->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, NULL);
}

void Trace::UninstallStubs() {
  Runtime::Current()->GetClassLinker()->VisitClasses(UninstallStubsClassVisitor, NULL);

  // Restore stacks of all threads
  {
    ScopedThreadListLock thread_list_lock;
    Runtime::Current()->GetThreadList()->ForEach(TraceRestoreStack, NULL);
  }
}

uint32_t TraceMethodUnwindFromCode(Thread* self) {
  Trace* tracer = Runtime::Current()->GetTracer();
  TraceStackFrame trace_frame = self->PopTraceStackFrame();
  Method* method = trace_frame.method_;
  uint32_t lr = trace_frame.return_pc_;

  tracer->LogMethodTraceEvent(self, method, Trace::kMethodTraceUnwind);

  return lr;
}

}  // namespace art

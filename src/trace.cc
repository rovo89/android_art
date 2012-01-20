// Copyright 2011 Google Inc. All Rights Reserved.

#include "trace.h"

#include <sys/uio.h>

#include "class_linker.h"
#include "debugger.h"
#include "dex_cache.h"
#include "object_utils.h"
#include "os.h"
#include "runtime_support.h"
#include "thread.h"


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

bool UseThreadCpuClock() {
  // TODO: Allow control over which clock is used
  return true;
}

bool UseWallClock() {
  // TODO: Allow control over which clock is used
  return true;
}

void MeasureClockOverhead() {
  if (UseThreadCpuClock()) {
    ThreadCpuMicroTime();
  }
  if (UseWallClock()) {
    MicroTime();
  }
}

uint32_t GetClockOverhead() {
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

void Append2LE(uint8_t* buf, uint16_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
}

void Append4LE(uint8_t* buf, uint32_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
  *buf++ = (uint8_t) (val >> 16);
  *buf++ = (uint8_t) (val >> 24);
}

void Append8LE(uint8_t* buf, uint64_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
  *buf++ = (uint8_t) (val >> 16);
  *buf++ = (uint8_t) (val >> 24);
  *buf++ = (uint8_t) (val >> 32);
  *buf++ = (uint8_t) (val >> 40);
  *buf++ = (uint8_t) (val >> 48);
  *buf++ = (uint8_t) (val >> 56);
}

#if defined(__arm__)
static bool InstallStubsClassVisitor(Class* klass, void* trace_stub) {
  Trace* tracer = Runtime::Current()->GetTracer();
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    Method* method = klass->GetDirectMethod(i);
    if (method->GetCode() != trace_stub) {
      tracer->SaveAndUpdateCode(method, trace_stub);
    }
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    Method* method = klass->GetVirtualMethod(i);
    if (method->GetCode() != trace_stub) {
      tracer->SaveAndUpdateCode(method, trace_stub);
    }
  }

  if (!klass->IsArrayClass() && !klass->IsPrimitive()) {
    CodeAndDirectMethods* c_and_dm = klass->GetDexCache()->GetCodeAndDirectMethods();
    for (size_t i = 0; i < c_and_dm->NumCodeAndDirectMethods(); i++) {
      Method* method = c_and_dm->GetResolvedMethod(i);
      if (method != NULL && (size_t) method != i) {
        c_and_dm->SetResolvedDirectMethodTraceEntry(i, trace_stub);
      }
    }
  }
  return true;
}

static bool UninstallStubsClassVisitor(Class* klass, void* trace_stub) {
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

  if (!klass->IsArrayClass() && !klass->IsPrimitive()) {
    CodeAndDirectMethods* c_and_dm = klass->GetDexCache()->GetCodeAndDirectMethods();
    for (size_t i = 0; i < c_and_dm->NumCodeAndDirectMethods(); i++) {
      const void* code = c_and_dm->GetResolvedCode(i);
      if (code == trace_stub) {
        Method* method = klass->GetDexCache()->GetResolvedMethod(i);
        if (tracer->GetSavedCodeFromMap(method) != NULL) {
          tracer->ResetSavedCode(method);
        }
        c_and_dm->SetResolvedDirectMethod(i, method);
      }
    }
  }
  return true;
}

static void TraceRestoreStack(Thread* t, void*) {
  uintptr_t trace_exit = reinterpret_cast<uintptr_t>(art_trace_exit_from_code);

  Frame frame = t->GetTopOfStack();
  if (frame.GetSP() != 0) {
    for ( ; frame.GetMethod() != 0; frame.Next()) {
      if (t->IsTraceStackEmpty()) {
        break;
      }
      uintptr_t pc = frame.GetReturnPC();
      Method* method = frame.GetMethod();
      if (trace_exit == pc) {
        TraceStackFrame trace_frame = t->PopTraceStackFrame();
        frame.SetReturnPC(trace_frame.return_pc_);
        CHECK(method == trace_frame.method_);
      }
    }
  }
}
#endif

void Trace::AddSavedCodeToMap(const Method* method, const void* code) {
  saved_code_map_.insert(std::make_pair(method, code));
}

void Trace::RemoveSavedCodeFromMap(const Method* method) {
  saved_code_map_.erase(method);
}

const void* Trace::GetSavedCodeFromMap(const Method* method) {
  typedef std::map<const Method*, const void*>::const_iterator It; // TODO: C++0x auto
  It it = saved_code_map_.find(method);
  if (it == saved_code_map_.end()) {
    return NULL;
  } else {
    return it->second;
  }
}

void Trace::SaveAndUpdateCode(Method* method, const void* new_code) {
  CHECK(GetSavedCodeFromMap(method) == NULL);
  AddSavedCodeToMap(method, method->GetCode());
  method->SetCode(new_code);
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

  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
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
      PLOG(ERROR) << "Unable to open trace file '" << trace_filename;
      Thread::Current()->ThrowNewException("Ljava/lang/RuntimeException;",
          StringPrintf("Unable to open trace file '%s'", trace_filename).c_str());
      Runtime::Current()->GetThreadList()->ResumeAll(false);
      return;
    }
  }

  // Create Trace object.
  Trace* tracer(new Trace(trace_file, buffer_size));
  Runtime::Current()->EnableMethodTracing(tracer);
  tracer->BeginTracing();

  Runtime::Current()->GetThreadList()->ResumeAll(false);
}

void Trace::Stop() {
  if (!Runtime::Current()->IsMethodTracingActive()) {
    LOG(INFO) << "Trace stop requested, but no trace currently running";
    return;
  }

  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Runtime::Current()->GetThreadList()->SuspendAll(false);

  Runtime::Current()->GetTracer()->FinishTracing();
  Runtime::Current()->DisableMethodTracing();

  Runtime::Current()->GetThreadList()->ResumeAll(false);
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
    thread_clock_base_map_.insert(std::make_pair(self, time));
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
    os << StringPrintf("0x%08x\t%s\t%s\t%s\t%s\t%d\n", (int) method,
        PrettyDescriptor(mh.GetDeclaringClassDescriptor()).c_str(), mh.GetName(),
        mh.GetSignature().c_str(), mh.GetDeclaringClassSourceFile(),
        mh.GetLineNumFromNativePC(0));
  }
}

static void DumpThread(Thread* t, void* arg) {
  std::ostream* os = reinterpret_cast<std::ostream*>(arg);
  *os << StringPrintf("%d\t%s\n", t->GetTid(), t->GetName()->ToModifiedUtf8().c_str());
}

void Trace::DumpThreadList(std::ostream& os) {
  ScopedThreadListLock thread_list_lock;
  Runtime::Current()->GetThreadList()->ForEach(DumpThread, &os);
}

void Trace::InstallStubs() {
#if defined(__arm__)
  void* trace_stub = reinterpret_cast<void*>(art_trace_entry_from_code);
  Runtime::Current()->GetClassLinker()->VisitClasses(InstallStubsClassVisitor, trace_stub);
#else
  UNIMPLEMENTED(WARNING);
#endif
}

void Trace::UninstallStubs() {
#if defined(__arm__)
  void* trace_stub = reinterpret_cast<void*>(art_trace_entry_from_code);
  Runtime::Current()->GetClassLinker()->VisitClasses(UninstallStubsClassVisitor, trace_stub);

  // Restore stacks of all threads
  {
    ScopedThreadListLock thread_list_lock;
    Runtime::Current()->GetThreadList()->ForEach(TraceRestoreStack, NULL);
  }
#else
  UNIMPLEMENTED(WARNING);
#endif
}

}  // namespace art

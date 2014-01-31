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

#include "profiler.h"

#include <sys/uio.h>

#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "instrumentation.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "thread.h"
#include "thread_list.h"

#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#endif

#if !defined(ART_USE_PORTABLE_COMPILER)
#include "entrypoints/quick/quick_entrypoints.h"
#endif

namespace art {

BackgroundMethodSamplingProfiler* BackgroundMethodSamplingProfiler::profiler_ = nullptr;
pthread_t BackgroundMethodSamplingProfiler::profiler_pthread_ = 0U;
volatile bool BackgroundMethodSamplingProfiler::shutting_down_ = false;


// TODO: this profiler runs regardless of the state of the machine.  Maybe we should use the
// wakelock or something to modify the run characteristics.  This can be done when we
// have some performance data after it's been used for a while.


// This is called from either a thread list traversal or from a checkpoint.  Regardless
// of which caller, the mutator lock must be held.
static void GetSample(Thread* thread, void* arg) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  BackgroundMethodSamplingProfiler* profiler =
      reinterpret_cast<BackgroundMethodSamplingProfiler*>(arg);
  mirror::ArtMethod* method = thread->GetCurrentMethod(nullptr);
  if (false && method == nullptr) {
    LOG(INFO) << "No current method available";
    std::ostringstream os;
    thread->Dump(os);
    std::string data(os.str());
    LOG(INFO) << data;
  }
  profiler->RecordMethod(method);
}



// A closure that is called by the thread checkpoint code.
class SampleCheckpoint : public Closure {
 public:
  explicit SampleCheckpoint(BackgroundMethodSamplingProfiler* const profiler) :
    profiler_(profiler) {}

  virtual void Run(Thread* thread) NO_THREAD_SAFETY_ANALYSIS {
    Thread* self = Thread::Current();
    if (thread == nullptr) {
      LOG(ERROR) << "Checkpoint with nullptr thread";
      return;
    }

    // Grab the mutator lock (shared access).
    ScopedObjectAccess soa(self);

    // Grab a sample.
    GetSample(thread, this->profiler_);

    // And finally tell the barrier that we're done.
    this->profiler_->GetBarrier().Pass(self);
  }

 private:
  BackgroundMethodSamplingProfiler* const profiler_;
};

bool BackgroundMethodSamplingProfiler::ShuttingDown(Thread* self) {
  MutexLock mu(self, *Locks::profiler_lock_);
  return shutting_down_;
}

void* BackgroundMethodSamplingProfiler::RunProfilerThread(void* arg) {
  Runtime* runtime = Runtime::Current();
  BackgroundMethodSamplingProfiler* profiler =
      reinterpret_cast<BackgroundMethodSamplingProfiler*>(arg);

  // Add a random delay for the first time run so that we don't hammer the CPU
  // with all profiles running at the same time.
  const int kRandomDelayMaxSecs = 30;
  const double kMaxBackoffSecs = 24*60*60;   // Max backoff time.

  srand(MicroTime() * getpid());
  int startup_delay = rand() % kRandomDelayMaxSecs;   // random delay for startup.


  CHECK(runtime->AttachCurrentThread("Profiler", true, runtime->GetSystemThreadGroup(),
                                      !runtime->IsCompiler()));

  Thread* self = Thread::Current();

  while (true) {
    if (ShuttingDown(self)) {
      break;
    }

    {
      // wait until we need to run another profile
      uint64_t delay_secs = profiler->period_s_ * profiler->backoff_factor_;

      // Add a startup delay to prevent all the profiles running at once.
      delay_secs += startup_delay;

      // Immediate startup for benchmarking?
      if (profiler->start_immediately_ && startup_delay > 0) {
        delay_secs = 0;
      }

      startup_delay = 0;

      LOG(DEBUG) << "Delaying profile start for " << delay_secs << " secs";
      MutexLock mu(self, profiler->wait_lock_);
      profiler->period_condition_.TimedWait(self, delay_secs * 1000, 0);

      // Expand the backoff by its coefficient, but don't go beyond the max.
      double new_backoff = profiler->backoff_factor_ * profiler->backoff_coefficient_;
      if (new_backoff < kMaxBackoffSecs) {
        profiler->backoff_factor_ = new_backoff;
      }
    }

    if (ShuttingDown(self)) {
      break;
    }


    uint64_t start_us = MicroTime();
    uint64_t end_us = start_us + profiler->duration_s_ * 1000000LL;
    uint64_t now_us = start_us;

    LOG(DEBUG) << "Starting profiling run now for " << PrettyDuration((end_us - start_us) * 1000);


    SampleCheckpoint check_point(profiler);

    while (now_us < end_us) {
      if (ShuttingDown(self)) {
        break;
      }

      usleep(profiler->interval_us_);    // Non-interruptible sleep.

      ThreadList* thread_list = runtime->GetThreadList();

      profiler->profiler_barrier_->Init(self, 0);
      size_t barrier_count = thread_list->RunCheckpoint(&check_point);

      ThreadState old_state = self->SetState(kWaitingForCheckPointsToRun);

      // Wait for the barrier to be crossed by all runnable threads.  This wait
      // is done with a timeout so that we can detect problems with the checkpoint
      // running code.  We should never see this.
      const uint32_t kWaitTimeoutMs = 10000;
      const uint32_t kWaitTimeoutUs = kWaitTimeoutMs * 1000;

      uint64_t waitstart_us = MicroTime();
      // Wait for all threads to pass the barrier.
      profiler->profiler_barrier_->Increment(self, barrier_count, kWaitTimeoutMs);
      uint64_t waitend_us = MicroTime();
      uint64_t waitdiff_us = waitend_us - waitstart_us;

      // We should never get a timeout.  If we do, it suggests a problem with the checkpoint
      // code.  Crash the process in this case.
      CHECK_LT(waitdiff_us, kWaitTimeoutUs);

      self->SetState(old_state);

      // Update the current time.
      now_us = MicroTime();
    }

    if (!ShuttingDown(self)) {
      // After the profile has been taken, write it out.
      ScopedObjectAccess soa(self);   // Acquire the mutator lock.
      uint32_t size = profiler->WriteProfile();
      LOG(DEBUG) << "Profile size: " << size;
    }
  }

  LOG(INFO) << "Profiler shutdown";
  runtime->DetachCurrentThread();
  return nullptr;
}

// Write out the profile file if we are generating a profile.
uint32_t BackgroundMethodSamplingProfiler::WriteProfile() {
  UniquePtr<File> profile_file;
  Runtime* runtime = Runtime::Current();
  std::string classpath = runtime->GetClassPathString();
  size_t colon = classpath.find(':');
  if (colon != std::string::npos) {
    // More than one file in the classpath.  Possible?
    classpath = classpath.substr(0, colon);
  }

  std::replace(classpath.begin(), classpath.end(), '/', '@');
  std::string full_name = profile_file_name_;
  if (classpath != "") {
    full_name = StringPrintf("%s-%s", profile_file_name_.c_str(), classpath.c_str());
  }
  LOG(DEBUG) << "Saving profile to " << full_name;

  profile_file.reset(OS::CreateEmptyFile(full_name.c_str()));
  if (profile_file.get() == nullptr) {
    // Failed to open the profile file, ignore.
    LOG(INFO) << "Failed to op file";
    return 0;
  }
  std::ostringstream os;
  uint32_t num_methods = DumpProfile(os);
  std::string data(os.str());
  profile_file->WriteFully(data.c_str(), data.length());
  profile_file->Close();
  return num_methods;
}

// Start a profile thread with the user-supplied arguments.
void BackgroundMethodSamplingProfiler::Start(int period, int duration,
                  std::string profile_file_name, int interval_us,
                  double backoff_coefficient, bool startImmediately) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::profiler_lock_);
    // Don't start two profiler threads.
    if (profiler_ != nullptr) {
      return;
    }
  }

  // Only on target...
#ifdef HAVE_ANDROID_OS
  // Switch off profiler if the dalvik.vm.profiler property has value 0.
  char buf[PROP_VALUE_MAX];
  property_get("dalvik.vm.profiler", buf, "0");
  if (strcmp(buf, "0") == 0) {
    LOG(INFO) << "Profiler disabled.  To enable setprop dalvik.vm.profiler 1";
    return;
  }
#endif

  LOG(INFO) << "Starting profile with period " << period << "s, duration " << duration <<
      "s, interval " << interval_us << "us.  Profile file " << profile_file_name;

  {
    MutexLock mu(self, *Locks::profiler_lock_);
    profiler_ = new BackgroundMethodSamplingProfiler(period, duration, profile_file_name,
                                      backoff_coefficient,
                                      interval_us, startImmediately);

    CHECK_PTHREAD_CALL(pthread_create, (&profiler_pthread_, nullptr, &RunProfilerThread,
        reinterpret_cast<void*>(profiler_)),
                       "Profiler thread");
  }
}



void BackgroundMethodSamplingProfiler::Stop() {
  BackgroundMethodSamplingProfiler* profiler = nullptr;
  pthread_t profiler_pthread = 0U;
  {
    MutexLock trace_mu(Thread::Current(), *Locks::profiler_lock_);
    profiler = profiler_;
    shutting_down_ = true;
    profiler_pthread = profiler_pthread_;
  }

  // Now wake up the sampler thread if it sleeping.
  {
    MutexLock profile_mu(Thread::Current(), profiler->wait_lock_);
    profiler->period_condition_.Signal(Thread::Current());
  }
  // Wait for the sample thread to stop.
  CHECK_PTHREAD_CALL(pthread_join, (profiler_pthread, nullptr), "profiler thread shutdown");

  {
    MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
    profiler_ = nullptr;
  }
  delete profiler;
}


void BackgroundMethodSamplingProfiler::Shutdown() {
  Stop();
}

BackgroundMethodSamplingProfiler::BackgroundMethodSamplingProfiler(int period, int duration,
                   std::string profile_file_name,
                   double backoff_coefficient, int interval_us, bool startImmediately)
    : profile_file_name_(profile_file_name),
      period_s_(period), start_immediately_(startImmediately),
      interval_us_(interval_us), backoff_factor_(1.0),
      backoff_coefficient_(backoff_coefficient), duration_s_(duration),
      wait_lock_("Profile wait lock"),
      period_condition_("Profile condition", wait_lock_),
      profile_table_(wait_lock_),
      profiler_barrier_(new Barrier(0)) {
  // Populate the filtered_methods set.
  // This is empty right now, but to add a method, do this:
  //
  // filtered_methods_.insert("void java.lang.Object.wait(long, int)");
}

// A method has been hit, record its invocation in the method map.
// The mutator_lock must be held (shared) when this is called.
void BackgroundMethodSamplingProfiler::RecordMethod(mirror::ArtMethod* method) {
  if (method == nullptr) {
    profile_table_.NullMethod();
    // Don't record a nullptr method.
    return;
  }

  mirror::Class* cls = method->GetDeclaringClass();
  if (cls != nullptr) {
    if (cls->GetClassLoader() == nullptr) {
      // Don't include things in the boot
      profile_table_.BootMethod();
      return;
    }
  }

  bool is_filtered = false;

  MethodHelper mh(method);
  if (strcmp(mh.GetName(), "<clinit>") == 0) {
    // always filter out class init
    is_filtered = true;
  }

  // Filter out methods by name if there are any.
  if (!is_filtered && filtered_methods_.size() > 0) {
    std::string method_full_name = PrettyMethod(method);

    // Don't include specific filtered methods.
    is_filtered = filtered_methods_.count(method_full_name) != 0;
  }

  // Add to the profile table unless it is filtered out.
  if (!is_filtered) {
    profile_table_.Put(method);
  }
}

// Clean out any recordings for the method traces.
void BackgroundMethodSamplingProfiler::CleanProfile() {
  profile_table_.Clear();
}

uint32_t BackgroundMethodSamplingProfiler::DumpProfile(std::ostream& os) {
  return profile_table_.Write(os);
}

// Profile Table.
// This holds a mapping of mirror::ArtMethod* to a count of how many times a sample
// hit it at the top of the stack.
ProfileSampleResults::ProfileSampleResults(Mutex& lock) : lock_(lock), num_samples_(0),
    num_null_methods_(0),
    num_boot_methods_(0) {
  for (int i = 0; i < kHashSize; i++) {
    table[i] = nullptr;
  }
}

ProfileSampleResults::~ProfileSampleResults() {
  for (int i = 0; i < kHashSize; i++) {
     delete table[i];
  }
}

// Add a method to the profile table.  If it the first time the method
// has been seen, add it with count=1, otherwise increment the count.
void ProfileSampleResults::Put(mirror::ArtMethod* method) {
  lock_.Lock(Thread::Current());
  uint32_t index = Hash(method);
  if (table[index] == nullptr) {
    table[index] = new Map();
  }
  Map::iterator i = table[index]->find(method);
  if (i == table[index]->end()) {
    (*table[index])[method] = 1;
  } else {
    i->second++;
  }
  num_samples_++;
  lock_.Unlock(Thread::Current());
}

// Write the profile table to the output stream.
uint32_t ProfileSampleResults::Write(std::ostream &os) {
  ScopedObjectAccess soa(Thread::Current());
  LOG(DEBUG) << "Profile: " << num_samples_ << "/" << num_null_methods_ << "/" << num_boot_methods_;
  os << num_samples_ << "/" << num_null_methods_ << "/" << num_boot_methods_ << "\n";
  uint32_t num_methods = 0;
  for (int i = 0 ; i < kHashSize; i++) {
    Map *map = table[i];
    if (map != nullptr) {
      for (const auto &meth_iter : *map) {
         mirror::ArtMethod *method = meth_iter.first;
         std::string method_name = PrettyMethod(method);
         uint32_t method_size = method->GetCodeSize();
         os << StringPrintf("%s/%u/%u\n",  method_name.c_str(), meth_iter.second, method_size);
         ++num_methods;
       }
    }
  }
  return num_methods;
}

void ProfileSampleResults::Clear() {
  num_samples_ = 0;
  num_null_methods_ = 0;
  num_boot_methods_ = 0;
  for (int i = 0; i < kHashSize; i++) {
     delete table[i];
     table[i] = nullptr;
  }
}

uint32_t ProfileSampleResults::Hash(mirror::ArtMethod* method) {
  uint32_t value = reinterpret_cast<uint32_t>(method);
  value >>= 2;
  return value % kHashSize;
}

}  // namespace art


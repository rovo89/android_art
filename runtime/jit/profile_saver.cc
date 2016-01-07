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
 */

#include "profile_saver.h"

#include "art_method-inl.h"
#include "scoped_thread_state_change.h"
#include "oat_file_manager.h"

namespace art {

// An arbitrary value to throttle save requests. Set to 500ms for now.
static constexpr const uint64_t kMilisecondsToNano = 1000000;
static constexpr const uint64_t kMinimumTimeBetweenCodeCacheUpdatesNs = 500 * kMilisecondsToNano;

// TODO: read the constants from ProfileOptions,
// Add a random delay each time we go to sleep so that we don't hammer the CPU
// with all profile savers running at the same time.
static constexpr const uint64_t kRandomDelayMaxMs = 10 * 1000;  // 10 seconds
static constexpr const uint64_t kMaxBackoffMs = 4 * 60 * 1000;  // 4 minutes
static constexpr const uint64_t kSavePeriodMs = 4 * 1000;  // 4 seconds
static constexpr const double kBackoffCoef = 1.5;

static constexpr const uint32_t kMinimumNrOrMethodsToSave = 10;

ProfileSaver* ProfileSaver::instance_ = nullptr;
pthread_t ProfileSaver::profiler_pthread_ = 0U;

ProfileSaver::ProfileSaver(const std::string& output_filename,
                           jit::JitCodeCache* jit_code_cache,
                           const std::vector<std::string>& code_paths)
    : output_filename_(output_filename),
      jit_code_cache_(jit_code_cache),
      tracked_dex_base_locations_(code_paths.begin(), code_paths.end()),
      code_cache_last_update_time_ns_(0),
      shutting_down_(false),
      wait_lock_("ProfileSaver wait lock"),
      period_condition_("ProfileSaver period condition", wait_lock_) {
}

void ProfileSaver::Run() {
  srand(MicroTime() * getpid());
  Thread* self = Thread::Current();

  uint64_t save_period_ms = kSavePeriodMs;
  VLOG(profiler) << "Save profiling information every " << save_period_ms << " ms";
  while (true) {
    if (ShuttingDown(self)) {
      break;
    }

    uint64_t random_sleep_delay_ms = rand() % kRandomDelayMaxMs;
    uint64_t sleep_time_ms = save_period_ms + random_sleep_delay_ms;
    {
      MutexLock mu(self, wait_lock_);
      period_condition_.TimedWait(self, sleep_time_ms, 0);
    }

    if (ShuttingDown(self)) {
      break;
    }

    if (!ProcessProfilingInfo() && save_period_ms < kMaxBackoffMs) {
      // If we don't need to save now it is less likely that we will need to do
      // so in the future. Increase the time between saves according to the
      // kBackoffCoef, but make it no larger than kMaxBackoffMs.
      save_period_ms = static_cast<uint64_t>(kBackoffCoef * save_period_ms);
    } else {
      // Reset the period to the initial value as it's highly likely to JIT again.
      save_period_ms = kSavePeriodMs;
    }
  }
}

bool ProfileSaver::ProcessProfilingInfo() {
  VLOG(profiler) << "Initiating save profiling information to: " << output_filename_;

  uint64_t last_update_time_ns = jit_code_cache_->GetLastUpdateTimeNs();
  if (last_update_time_ns - code_cache_last_update_time_ns_
      > kMinimumTimeBetweenCodeCacheUpdatesNs) {
    VLOG(profiler) << "Not enough time has passed since the last code cache update.";
    return false;
  }

  uint64_t start = NanoTime();
  code_cache_last_update_time_ns_ = last_update_time_ns;
  std::vector<ArtMethod*> methods;
  {
    ScopedObjectAccess soa(Thread::Current());
    jit_code_cache_->GetCompiledArtMethods(tracked_dex_base_locations_, methods);
  }
  if (methods.size() < kMinimumNrOrMethodsToSave) {
    VLOG(profiler) << "Not enough information to save. Nr of methods: " << methods.size();
    return false;
  }

  ProfileCompilationInfo::SaveProfilingInfo(output_filename_, methods);
  VLOG(profiler) << "Profile process time: " << PrettyDuration(NanoTime() - start);
  return true;
}

void* ProfileSaver::RunProfileSaverThread(void* arg) {
  Runtime* runtime = Runtime::Current();
  ProfileSaver* profile_saver = reinterpret_cast<ProfileSaver*>(arg);

  CHECK(runtime->AttachCurrentThread("Profile Saver",
                                     /*as_daemon*/true,
                                     runtime->GetSystemThreadGroup(),
                                     /*create_peer*/true));
  profile_saver->Run();

  runtime->DetachCurrentThread();
  VLOG(profiler) << "Profile saver shutdown";
  return nullptr;
}

void ProfileSaver::Start(const std::string& output_filename,
                         jit::JitCodeCache* jit_code_cache,
                         const std::vector<std::string>& code_paths) {
  DCHECK(Runtime::Current()->UseJit());
  DCHECK(!output_filename.empty());
  DCHECK(jit_code_cache != nullptr);

  MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
  // Don't start two profile saver threads.
  if (instance_ != nullptr) {
    DCHECK(false) << "Tried to start two profile savers";
    return;
  }

  VLOG(profiler) << "Starting profile saver using output file: " << output_filename
      << ". Tracking: " << Join(code_paths, ':');

  instance_ = new ProfileSaver(output_filename, jit_code_cache, code_paths);

  // Create a new thread which does the saving.
  CHECK_PTHREAD_CALL(
      pthread_create,
      (&profiler_pthread_, nullptr, &RunProfileSaverThread, reinterpret_cast<void*>(instance_)),
      "Profile saver thread");
}

void ProfileSaver::Stop() {
  ProfileSaver* profile_saver = nullptr;
  pthread_t profiler_pthread = 0U;

  {
    MutexLock profiler_mutex(Thread::Current(), *Locks::profiler_lock_);
    VLOG(profiler) << "Stopping profile saver thread for file: " << instance_->output_filename_;
    profile_saver = instance_;
    profiler_pthread = profiler_pthread_;
    if (instance_ == nullptr) {
      DCHECK(false) << "Tried to stop a profile saver which was not started";
      return;
    }
    if (instance_->shutting_down_) {
      DCHECK(false) << "Tried to stop the profile saver twice";
      return;
    }
    instance_->shutting_down_ = true;
  }

  {
    // Wake up the saver thread if it is sleeping to allow for a clean exit.
    MutexLock wait_mutex(Thread::Current(), profile_saver->wait_lock_);
    profile_saver->period_condition_.Signal(Thread::Current());
  }

  // Wait for the saver thread to stop.
  CHECK_PTHREAD_CALL(pthread_join, (profiler_pthread, nullptr), "profile saver thread shutdown");

  {
    MutexLock profiler_mutex(Thread::Current(), *Locks::profiler_lock_);
    instance_ = nullptr;
    profiler_pthread_ = 0U;
  }
  delete profile_saver;
}

bool ProfileSaver::ShuttingDown(Thread* self) {
  MutexLock mu(self, *Locks::profiler_lock_);
  return shutting_down_;
}

bool ProfileSaver::IsStarted() {
  MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
  return instance_ != nullptr;
}

}   // namespace art

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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "art_method-inl.h"
#include "scoped_thread_state_change.h"
#include "oat_file_manager.h"

namespace art {

// An arbitrary value to throttle save requests. Set to 2s for now.
static constexpr const uint64_t kMilisecondsToNano = 1000000;
static constexpr const uint64_t kMinimumTimeBetweenCodeCacheUpdatesNs = 2000 * kMilisecondsToNano;

// TODO: read the constants from ProfileOptions,
// Add a random delay each time we go to sleep so that we don't hammer the CPU
// with all profile savers running at the same time.
static constexpr const uint64_t kRandomDelayMaxMs = 20 * 1000;  // 20 seconds
static constexpr const uint64_t kMaxBackoffMs = 5 * 60 * 1000;  // 5 minutes
static constexpr const uint64_t kSavePeriodMs = 10 * 1000;  // 10 seconds
static constexpr const uint64_t kInitialDelayMs = 2 * 1000;  // 2 seconds
static constexpr const double kBackoffCoef = 1.5;

static constexpr const uint32_t kMinimumNrOrMethodsToSave = 10;

ProfileSaver* ProfileSaver::instance_ = nullptr;
pthread_t ProfileSaver::profiler_pthread_ = 0U;

ProfileSaver::ProfileSaver(const std::string& output_filename,
                           jit::JitCodeCache* jit_code_cache,
                           const std::vector<std::string>& code_paths,
                           const std::string& foreign_dex_profile_path,
                           const std::string& app_data_dir)
    : jit_code_cache_(jit_code_cache),
      foreign_dex_profile_path_(foreign_dex_profile_path),
      code_cache_last_update_time_ns_(0),
      shutting_down_(false),
      first_profile_(true),
      wait_lock_("ProfileSaver wait lock"),
      period_condition_("ProfileSaver period condition", wait_lock_) {
  AddTrackedLocations(output_filename, code_paths);
  app_data_dir_ = "";
  if (!app_data_dir.empty()) {
    // The application directory is used to determine which dex files are owned by app.
    // Since it could be a symlink (e.g. /data/data instead of /data/user/0), and we
    // don't have control over how the dex files are actually loaded (symlink or canonical path),
    // store it's canonical form to be sure we use the same base when comparing.
    UniqueCPtr<const char[]> app_data_dir_real_path(realpath(app_data_dir.c_str(), nullptr));
    if (app_data_dir_real_path != nullptr) {
      app_data_dir_.assign(app_data_dir_real_path.get());
    } else {
      LOG(WARNING) << "Failed to get the real path for app dir: " << app_data_dir_
          << ". The app dir will not be used to determine which dex files belong to the app";
    }
  }
}

void ProfileSaver::Run() {
  srand(MicroTime() * getpid());
  Thread* self = Thread::Current();

  uint64_t save_period_ms = kSavePeriodMs;
  VLOG(profiler) << "Save profiling information every " << save_period_ms << " ms";

  bool first_iteration = true;
  while (!ShuttingDown(self)) {
    uint64_t sleep_time_ms;
    if (first_iteration) {
      // Sleep less long for the first iteration since we want to record loaded classes shortly
      // after app launch.
      sleep_time_ms = kInitialDelayMs;
    } else {
      const uint64_t random_sleep_delay_ms = rand() % kRandomDelayMaxMs;
      sleep_time_ms = save_period_ms + random_sleep_delay_ms;
    }
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
    first_iteration = false;
  }
}

bool ProfileSaver::ProcessProfilingInfo() {
  uint64_t last_update_time_ns = jit_code_cache_->GetLastUpdateTimeNs();
  if (!first_profile_ && last_update_time_ns - code_cache_last_update_time_ns_
          < kMinimumTimeBetweenCodeCacheUpdatesNs) {
    VLOG(profiler) << "Not enough time has passed since the last code cache update."
        << "Last update: " << last_update_time_ns
        << " Last save: " << code_cache_last_update_time_ns_;
    return false;
  }

  uint64_t start = NanoTime();
  code_cache_last_update_time_ns_ = last_update_time_ns;
  SafeMap<std::string, std::set<std::string>> tracked_locations;
  {
    // Make a copy so that we don't hold the lock while doing I/O.
    MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
    tracked_locations = tracked_dex_base_locations_;
  }
  for (const auto& it : tracked_locations) {
    if (ShuttingDown(Thread::Current())) {
      return true;
    }
    const std::string& filename = it.first;
    const std::set<std::string>& locations = it.second;
    std::vector<ArtMethod*> methods;
    {
      ScopedObjectAccess soa(Thread::Current());
      jit_code_cache_->GetCompiledArtMethods(locations, methods);
    }
    // Always save for the first one for loaded classes profile.
    if (methods.size() < kMinimumNrOrMethodsToSave && !first_profile_) {
      VLOG(profiler) << "Not enough information to save to: " << filename
          <<" Nr of methods: " << methods.size();
      return false;
    }

    std::set<DexCacheResolvedClasses> resolved_classes;
    if (first_profile_) {
      ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
      resolved_classes = class_linker->GetResolvedClasses(/*ignore boot classes*/true);
    }

    if (!ProfileCompilationInfo::SaveProfilingInfo(filename, methods, resolved_classes)) {
      LOG(WARNING) << "Could not save profiling info to " << filename;
      return false;
    }

    VLOG(profiler) << "Profile process time: " << PrettyDuration(NanoTime() - start);
  }
  first_profile_ = false;
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
                         const std::vector<std::string>& code_paths,
                         const std::string& foreign_dex_profile_path,
                         const std::string& app_data_dir) {
  DCHECK(Runtime::Current()->UseJit());
  DCHECK(!output_filename.empty());
  DCHECK(jit_code_cache != nullptr);

  MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
  if (instance_ != nullptr) {
    // If we already have an instance, make sure it uses the same jit_code_cache.
    // This may be called multiple times via Runtime::registerAppInfo (e.g. for
    // apps which share the same runtime).
    DCHECK_EQ(instance_->jit_code_cache_, jit_code_cache);
    // Add the code_paths to the tracked locations.
    instance_->AddTrackedLocations(output_filename, code_paths);
    return;
  }

  VLOG(profiler) << "Starting profile saver using output file: " << output_filename
      << ". Tracking: " << Join(code_paths, ':');

  instance_ = new ProfileSaver(output_filename,
                               jit_code_cache,
                               code_paths,
                               foreign_dex_profile_path,
                               app_data_dir);

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
    VLOG(profiler) << "Stopping profile saver thread";
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

void ProfileSaver::AddTrackedLocations(const std::string& output_filename,
                                       const std::vector<std::string>& code_paths) {
  auto it = tracked_dex_base_locations_.find(output_filename);
  if (it == tracked_dex_base_locations_.end()) {
    tracked_dex_base_locations_.Put(output_filename,
                                    std::set<std::string>(code_paths.begin(), code_paths.end()));
  } else {
    it->second.insert(code_paths.begin(), code_paths.end());
  }
}

void ProfileSaver::NotifyDexUse(const std::string& dex_location) {
  std::set<std::string> app_code_paths;
  std::string foreign_dex_profile_path;
  std::string app_data_dir;
  {
    MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
    DCHECK(instance_ != nullptr);
    // Make a copy so that we don't hold the lock while doing I/O.
    for (const auto& it : instance_->tracked_dex_base_locations_) {
      app_code_paths.insert(it.second.begin(), it.second.end());
    }
    foreign_dex_profile_path = instance_->foreign_dex_profile_path_;
    app_data_dir = instance_->app_data_dir_;
  }

  MaybeRecordDexUseInternal(dex_location,
                            app_code_paths,
                            foreign_dex_profile_path,
                            app_data_dir);
}

void ProfileSaver::MaybeRecordDexUseInternal(
      const std::string& dex_location,
      const std::set<std::string>& app_code_paths,
      const std::string& foreign_dex_profile_path,
      const std::string& app_data_dir) {
  if (foreign_dex_profile_path.empty()) {
    LOG(WARNING) << "Asked to record foreign dex use without a valid profile path ";
    return;
  }

  UniqueCPtr<const char[]> dex_location_real_path(realpath(dex_location.c_str(), nullptr));
  std::string dex_location_real_path_str(dex_location_real_path.get());

  if (dex_location_real_path_str.compare(0, app_data_dir.length(), app_data_dir) == 0) {
    // The dex location is under the application folder. Nothing to record.
    return;
  }

  if (app_code_paths.find(dex_location) != app_code_paths.end()) {
    // The dex location belongs to the application code paths. Nothing to record.
    return;
  }
  // Do another round of checks with the real paths.
  // Note that we could cache all the real locations in the saver (since it's an expensive
  // operation). However we expect that app_code_paths is small (usually 1 element), and
  // NotifyDexUse is called just a few times in the app lifetime. So we make the compromise
  // to save some bytes of memory usage.
  for (const auto& app_code_location : app_code_paths) {
    UniqueCPtr<const char[]> real_app_code_location(realpath(app_code_location.c_str(), nullptr));
    std::string real_app_code_location_str(real_app_code_location.get());
    if (real_app_code_location_str == dex_location_real_path_str) {
      // The dex location belongs to the application code paths. Nothing to record.
      return;
    }
  }

  // For foreign dex files we record a flag on disk. PackageManager will (potentially) take this
  // into account when deciding how to optimize the loaded dex file.
  // The expected flag name is the canonical path of the apk where '/' is substituted to '@'.
  // (it needs to be kept in sync with
  // frameworks/base/services/core/java/com/android/server/pm/PackageDexOptimizer.java)
  std::replace(dex_location_real_path_str.begin(), dex_location_real_path_str.end(), '/', '@');
  std::string flag_path = foreign_dex_profile_path + "/" + dex_location_real_path_str;
  // No need to give any sort of access to flag_path. The system has enough permissions
  // to test for its existence.
  int fd = TEMP_FAILURE_RETRY(open(flag_path.c_str(), O_CREAT | O_EXCL, 0));
  if (fd != -1) {
    if (close(fd) != 0) {
      PLOG(WARNING) << "Could not close file after flagging foreign dex use " << flag_path;
    }
  } else {
    if (errno != EEXIST) {
      // Another app could have already created the file.
      PLOG(WARNING) << "Could not create foreign dex use mark " << flag_path;
    }
  }
}

}   // namespace art

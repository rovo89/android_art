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
#include "base/systrace.h"
#include "base/time_utils.h"
#include "compiler_filter.h"
#include "oat_file_manager.h"
#include "scoped_thread_state_change.h"


namespace art {

// TODO: read the constants from ProfileOptions,
// Add a random delay each time we go to sleep so that we don't hammer the CPU
// with all profile savers running at the same time.
static constexpr const uint64_t kRandomDelayMaxMs = 30 * 1000;  // 30 seconds
static constexpr const uint64_t kMaxBackoffMs = 10 * 60 * 1000;  // 10 minutes
static constexpr const uint64_t kSavePeriodMs = 20 * 1000;  // 20 seconds
static constexpr const uint64_t kSaveResolvedClassesDelayMs = 2 * 1000;  // 2 seconds
static constexpr const double kBackoffCoef = 2.0;

static constexpr const uint32_t kMinimumNumberOfMethodsToSave = 10;
static constexpr const uint32_t kMinimumNumberOfClassesToSave = 10;

ProfileSaver* ProfileSaver::instance_ = nullptr;
pthread_t ProfileSaver::profiler_pthread_ = 0U;

ProfileSaver::ProfileSaver(const std::string& output_filename,
                           jit::JitCodeCache* jit_code_cache,
                           const std::vector<std::string>& code_paths,
                           const std::string& foreign_dex_profile_path,
                           const std::string& app_data_dir)
    : jit_code_cache_(jit_code_cache),
      foreign_dex_profile_path_(foreign_dex_profile_path),
      shutting_down_(false),
      last_save_number_of_methods_(0),
      last_save_number_of_classes_(0),
      wait_lock_("ProfileSaver wait lock"),
      period_condition_("ProfileSaver period condition", wait_lock_),
      total_bytes_written_(0),
      total_number_of_writes_(0),
      total_number_of_code_cache_queries_(0),
      total_number_of_skipped_writes_(0),
      total_number_of_failed_writes_(0),
      total_ms_of_sleep_(0),
      total_ns_of_work_(0),
      total_number_of_foreign_dex_marks_(0),
      max_number_of_profile_entries_cached_(0) {
  AddTrackedLocations(output_filename, app_data_dir, code_paths);
  if (!app_data_dir.empty()) {
    // The application directory is used to determine which dex files are owned by app.
    // Since it could be a symlink (e.g. /data/data instead of /data/user/0), and we
    // don't have control over how the dex files are actually loaded (symlink or canonical path),
    // store it's canonical form to be sure we use the same base when comparing.
    UniqueCPtr<const char[]> app_data_dir_real_path(realpath(app_data_dir.c_str(), nullptr));
    if (app_data_dir_real_path != nullptr) {
      app_data_dirs_.emplace(app_data_dir_real_path.get());
    } else {
      LOG(WARNING) << "Failed to get the real path for app dir: " << app_data_dir
          << ". The app dir will not be used to determine which dex files belong to the app";
    }
  }
}

void ProfileSaver::Run() {
  srand(MicroTime() * getpid());
  Thread* self = Thread::Current();

  uint64_t save_period_ms = kSavePeriodMs;
  VLOG(profiler) << "Save profiling information every " << save_period_ms << " ms";
  bool cache_resolved_classes = true;
  while (!ShuttingDown(self)) {
    uint64_t sleep_time_ms;
    if (cache_resolved_classes) {
      // Sleep less long for the first iteration since we want to record loaded classes shortly
      // after app launch.
      sleep_time_ms = kSaveResolvedClassesDelayMs;
    } else {
      const uint64_t random_sleep_delay_ms = rand() % kRandomDelayMaxMs;
      sleep_time_ms = save_period_ms + random_sleep_delay_ms;
    }
    {
      MutexLock mu(self, wait_lock_);
      period_condition_.TimedWait(self, sleep_time_ms, 0);
    }
    total_ms_of_sleep_ += sleep_time_ms;
    if (ShuttingDown(self)) {
      break;
    }

    uint64_t start = NanoTime();
    if (cache_resolved_classes) {
      // TODO(calin) This only considers the case of the primary profile file.
      // Anything that gets loaded in the same VM will not have their resolved
      // classes save (unless they started before the initial saving was done).
      FetchAndCacheResolvedClasses();
    } else {
      bool profile_saved_to_disk = ProcessProfilingInfo();
      if (profile_saved_to_disk) {
        // Reset the period to the initial value as it's highly likely to JIT again.
        save_period_ms = kSavePeriodMs;
        VLOG(profiler) << "Profile saver: saved something, period reset to: " << save_period_ms;
      } else {
        // If we don't need to save now it is less likely that we will need to do
        // so in the future. Increase the time between saves according to the
        // kBackoffCoef, but make it no larger than kMaxBackoffMs.
        save_period_ms = std::min(kMaxBackoffMs,
                                  static_cast<uint64_t>(kBackoffCoef * save_period_ms));
        VLOG(profiler) << "Profile saver: nothing to save, delaying period to: " << save_period_ms;
      }
    }
    cache_resolved_classes = false;

    total_ns_of_work_ += (NanoTime() - start);
  }
}

ProfileCompilationInfo* ProfileSaver::GetCachedProfiledInfo(const std::string& filename) {
  auto info_it = profile_cache_.find(filename);
  if (info_it == profile_cache_.end()) {
    info_it = profile_cache_.Put(filename, ProfileCompilationInfo());
  }
  return &info_it->second;
}

void ProfileSaver::FetchAndCacheResolvedClasses() {
  ScopedTrace trace(__PRETTY_FUNCTION__);

  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  std::set<DexCacheResolvedClasses> resolved_classes =
      class_linker->GetResolvedClasses(/*ignore boot classes*/ true);
  MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
  uint64_t total_number_of_profile_entries_cached = 0;
  for (const auto& it : tracked_dex_base_locations_) {
      std::set<DexCacheResolvedClasses> resolved_classes_for_location;
    const std::string& filename = it.first;
    const std::set<std::string>& locations = it.second;

    for (const DexCacheResolvedClasses& classes : resolved_classes) {
      if (locations.find(classes.GetDexLocation()) != locations.end()) {
        resolved_classes_for_location.insert(classes);
      }
    }
    ProfileCompilationInfo* info = GetCachedProfiledInfo(filename);
    info->AddMethodsAndClasses(std::vector<ArtMethod*>(), resolved_classes_for_location);
    total_number_of_profile_entries_cached += resolved_classes_for_location.size();
  }
  max_number_of_profile_entries_cached_ = std::max(
      max_number_of_profile_entries_cached_,
      total_number_of_profile_entries_cached);
}

bool ProfileSaver::ProcessProfilingInfo() {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  SafeMap<std::string, std::set<std::string>> tracked_locations;
  {
    // Make a copy so that we don't hold the lock while doing I/O.
    MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
    tracked_locations = tracked_dex_base_locations_;
  }

  bool profile_file_saved = false;
  uint64_t total_number_of_profile_entries_cached = 0;
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
      total_number_of_code_cache_queries_++;
    }

    ProfileCompilationInfo* cached_info = GetCachedProfiledInfo(filename);
    cached_info->AddMethodsAndClasses(methods, std::set<DexCacheResolvedClasses>());
    int64_t delta_number_of_methods =
        cached_info->GetNumberOfMethods() -
        static_cast<int64_t>(last_save_number_of_methods_);
    int64_t delta_number_of_classes =
        cached_info->GetNumberOfResolvedClasses() -
        static_cast<int64_t>(last_save_number_of_classes_);

    if (delta_number_of_methods < kMinimumNumberOfMethodsToSave &&
        delta_number_of_classes < kMinimumNumberOfClassesToSave) {
      VLOG(profiler) << "Not enough information to save to: " << filename
          << " Nr of methods: " << delta_number_of_methods
          << " Nr of classes: " << delta_number_of_classes;
      total_number_of_skipped_writes_++;
      continue;
    }
    uint64_t bytes_written;
    // Force the save. In case the profile data is corrupted or the the profile
    // has the wrong version this will "fix" the file to the correct format.
    if (cached_info->MergeAndSave(filename, &bytes_written, /*force*/ true)) {
      last_save_number_of_methods_ = cached_info->GetNumberOfMethods();
      last_save_number_of_classes_ = cached_info->GetNumberOfResolvedClasses();
      // Clear resolved classes. No need to store them around as
      // they don't change after the first write.
      cached_info->ClearResolvedClasses();
      if (bytes_written > 0) {
        total_number_of_writes_++;
        total_bytes_written_ += bytes_written;
        profile_file_saved = true;
      } else {
        // At this point we could still have avoided the write.
        // We load and merge the data from the file lazily at its first ever
        // save attempt. So, whatever we are trying to save could already be
        // in the file.
        total_number_of_skipped_writes_++;
      }
    } else {
      LOG(WARNING) << "Could not save profiling info to " << filename;
      total_number_of_failed_writes_++;
    }
    total_number_of_profile_entries_cached +=
        cached_info->GetNumberOfMethods() +
        cached_info->GetNumberOfResolvedClasses();
  }
  max_number_of_profile_entries_cached_ = std::max(
      max_number_of_profile_entries_cached_,
      total_number_of_profile_entries_cached);
  return profile_file_saved;
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

static bool ShouldProfileLocation(const std::string& location) {
  OatFileManager& oat_manager = Runtime::Current()->GetOatFileManager();
  const OatFile* oat_file = oat_manager.FindOpenedOatFileFromDexLocation(location);
  if (oat_file == nullptr) {
    // This can happen if we fallback to run code directly from the APK.
    // Profile it with the hope that the background dexopt will get us back into
    // a good state.
    VLOG(profiler) << "Asked to profile a location without an oat file:" << location;
    return true;
  }
  CompilerFilter::Filter filter = oat_file->GetCompilerFilter();
  if ((filter == CompilerFilter::kSpeed) || (filter == CompilerFilter::kEverything)) {
    VLOG(profiler)
        << "Skip profiling oat file because it's already speed|everything compiled: "
        << location << " oat location: " << oat_file->GetLocation();
    return false;
  }
  return true;
}

void ProfileSaver::Start(const std::string& output_filename,
                         jit::JitCodeCache* jit_code_cache,
                         const std::vector<std::string>& code_paths,
                         const std::string& foreign_dex_profile_path,
                         const std::string& app_data_dir) {
  DCHECK(Runtime::Current()->UseJit());
  DCHECK(!output_filename.empty());
  DCHECK(jit_code_cache != nullptr);

  std::vector<std::string> code_paths_to_profile;

  for (const std::string& location : code_paths) {
    if (ShouldProfileLocation(location))  {
      code_paths_to_profile.push_back(location);
    }
  }
  if (code_paths_to_profile.empty()) {
    VLOG(profiler) << "No code paths should be profiled.";
    return;
  }

  MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
  if (instance_ != nullptr) {
    // If we already have an instance, make sure it uses the same jit_code_cache.
    // This may be called multiple times via Runtime::registerAppInfo (e.g. for
    // apps which share the same runtime).
    DCHECK_EQ(instance_->jit_code_cache_, jit_code_cache);
    // Add the code_paths to the tracked locations.
    instance_->AddTrackedLocations(output_filename, app_data_dir, code_paths_to_profile);
    return;
  }

  VLOG(profiler) << "Starting profile saver using output file: " << output_filename
      << ". Tracking: " << Join(code_paths_to_profile, ':');

  instance_ = new ProfileSaver(output_filename,
                               jit_code_cache,
                               code_paths_to_profile,
                               foreign_dex_profile_path,
                               app_data_dir);

  // Create a new thread which does the saving.
  CHECK_PTHREAD_CALL(
      pthread_create,
      (&profiler_pthread_, nullptr, &RunProfileSaverThread, reinterpret_cast<void*>(instance_)),
      "Profile saver thread");
}

void ProfileSaver::Stop(bool dump_info) {
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
    if (dump_info) {
      instance_->DumpInfo(LOG(INFO));
    }
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
                                       const std::string& app_data_dir,
                                       const std::vector<std::string>& code_paths) {
  auto it = tracked_dex_base_locations_.find(output_filename);
  if (it == tracked_dex_base_locations_.end()) {
    tracked_dex_base_locations_.Put(output_filename,
                                    std::set<std::string>(code_paths.begin(), code_paths.end()));
    app_data_dirs_.insert(app_data_dir);
  } else {
    it->second.insert(code_paths.begin(), code_paths.end());
  }
}

void ProfileSaver::NotifyDexUse(const std::string& dex_location) {
  if (!ShouldProfileLocation(dex_location)) {
    return;
  }
  std::set<std::string> app_code_paths;
  std::string foreign_dex_profile_path;
  std::set<std::string> app_data_dirs;
  {
    MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
    if (instance_ == nullptr) {
      return;
    }
    // Make a copy so that we don't hold the lock while doing I/O.
    for (const auto& it : instance_->tracked_dex_base_locations_) {
      app_code_paths.insert(it.second.begin(), it.second.end());
    }
    foreign_dex_profile_path = instance_->foreign_dex_profile_path_;
    app_data_dirs.insert(instance_->app_data_dirs_.begin(), instance_->app_data_dirs_.end());
  }

  bool mark_created = MaybeRecordDexUseInternal(dex_location,
                                                app_code_paths,
                                                foreign_dex_profile_path,
                                                app_data_dirs);
  if (mark_created) {
    MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
    if (instance_ != nullptr) {
      instance_->total_number_of_foreign_dex_marks_++;
    }
  }
}

bool ProfileSaver::MaybeRecordDexUseInternal(
      const std::string& dex_location,
      const std::set<std::string>& app_code_paths,
      const std::string& foreign_dex_profile_path,
      const std::set<std::string>& app_data_dirs) {
  if (dex_location.empty()) {
    LOG(WARNING) << "Asked to record foreign dex use with an empty dex location.";
    return false;
  }
  if (foreign_dex_profile_path.empty()) {
    LOG(WARNING) << "Asked to record foreign dex use without a valid profile path ";
    return false;
  }

  UniqueCPtr<const char[]> dex_location_real_path(realpath(dex_location.c_str(), nullptr));
  if (dex_location_real_path == nullptr) {
    PLOG(WARNING) << "Could not get realpath for " << dex_location;
  }
  std::string dex_location_real_path_str((dex_location_real_path == nullptr)
    ? dex_location.c_str()
    : dex_location_real_path.get());

  if (app_data_dirs.find(dex_location_real_path_str) != app_data_dirs.end()) {
    // The dex location is under the application folder. Nothing to record.
    return false;
  }

  if (app_code_paths.find(dex_location) != app_code_paths.end()) {
    // The dex location belongs to the application code paths. Nothing to record.
    return false;
  }
  // Do another round of checks with the real paths.
  // Note that we could cache all the real locations in the saver (since it's an expensive
  // operation). However we expect that app_code_paths is small (usually 1 element), and
  // NotifyDexUse is called just a few times in the app lifetime. So we make the compromise
  // to save some bytes of memory usage.
  for (const auto& app_code_location : app_code_paths) {
    UniqueCPtr<const char[]> real_app_code_location(realpath(app_code_location.c_str(), nullptr));
    if (real_app_code_location == nullptr) {
      PLOG(WARNING) << "Could not get realpath for " << app_code_location;
    }
    std::string real_app_code_location_str((real_app_code_location == nullptr)
        ? app_code_location.c_str()
        : real_app_code_location.get());
    if (real_app_code_location_str == dex_location_real_path_str) {
      // The dex location belongs to the application code paths. Nothing to record.
      return false;
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
    return true;
  } else {
    if (errno != EEXIST) {
      // Another app could have already created the file.
      PLOG(WARNING) << "Could not create foreign dex use mark " << flag_path;
      return false;
    }
    return true;
  }
}

void ProfileSaver::DumpInstanceInfo(std::ostream& os) {
  MutexLock mu(Thread::Current(), *Locks::profiler_lock_);
  if (instance_ != nullptr) {
    instance_->DumpInfo(os);
  }
}

void ProfileSaver::DumpInfo(std::ostream& os) {
  os << "ProfileSaver total_bytes_written=" << total_bytes_written_ << '\n'
     << "ProfileSaver total_number_of_writes=" << total_number_of_writes_ << '\n'
     << "ProfileSaver total_number_of_code_cache_queries="
     << total_number_of_code_cache_queries_ << '\n'
     << "ProfileSaver total_number_of_skipped_writes=" << total_number_of_skipped_writes_ << '\n'
     << "ProfileSaver total_number_of_failed_writes=" << total_number_of_failed_writes_ << '\n'
     << "ProfileSaver total_ms_of_sleep=" << total_ms_of_sleep_ << '\n'
     << "ProfileSaver total_ms_of_work=" << NsToMs(total_ns_of_work_) << '\n'
     << "ProfileSaver total_number_of_foreign_dex_marks="
     << total_number_of_foreign_dex_marks_ << '\n'
     << "ProfileSaver max_number_profile_entries_cached="
    << max_number_of_profile_entries_cached_ << '\n';
}

}   // namespace art

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

#ifndef ART_RUNTIME_PROFILER_H_
#define ART_RUNTIME_PROFILER_H_

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "barrier.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "globals.h"
#include "instrumentation.h"
#include "os.h"
#include "safe_map.h"
#include "UniquePtrCompat.h"

namespace art {

namespace mirror {
  class ArtMethod;
  class Class;
}  // namespace mirror
class Thread;

//
// This class holds all the results for all runs of the profiler.  It also
// counts the number of null methods (where we can't determine the method) and
// the number of methods in the boot path (where we have already compiled the method).
//
// This object is an internal profiler object and uses the same locking as the profiler
// itself.
class ProfileSampleResults {
 public:
  explicit ProfileSampleResults(Mutex& lock);
  ~ProfileSampleResults();

  void Put(mirror::ArtMethod* method);
  uint32_t Write(std::ostream &os);
  void ReadPrevious(int fd);
  void Clear();
  uint32_t GetNumSamples() { return num_samples_; }
  void NullMethod() { ++num_null_methods_; }
  void BootMethod() { ++num_boot_methods_; }

 private:
  uint32_t Hash(mirror::ArtMethod* method);
  static constexpr int kHashSize = 17;
  Mutex& lock_;                   // Reference to the main profiler lock - we don't need two of them.
  uint32_t num_samples_;          // Total number of samples taken.
  uint32_t num_null_methods_;     // Number of samples where can don't know the method.
  uint32_t num_boot_methods_;     // Number of samples in the boot path.

  typedef std::map<mirror::ArtMethod*, uint32_t> Map;   // Map of method vs its count.
  Map *table[kHashSize];

  struct PreviousValue {
    PreviousValue() : count_(0), method_size_(0) {}
    PreviousValue(uint32_t count, uint32_t method_size) : count_(count), method_size_(method_size) {}
    uint32_t count_;
    uint32_t method_size_;
  };

  typedef std::map<std::string, PreviousValue> PreviousProfile;
  PreviousProfile previous_;
  uint32_t previous_num_samples_;
  uint32_t previous_num_null_methods_;     // Number of samples where can don't know the method.
  uint32_t previous_num_boot_methods_;     // Number of samples in the boot path.
};

//
// The BackgroundMethodSamplingProfiler runs in a thread.  Most of the time it is sleeping but
// occasionally wakes up and counts the number of times a method is called.  Each time
// it ticks, it looks at the current method and records it in the ProfileSampleResults
// table.
//
// The timing is controlled by a number of variables:
// 1.  Period: the time between sampling runs.
// 2.  Interval: the time between each sample in a run.
// 3.  Duration: the duration of a run.
//
// So the profiler thread is sleeping for the 'period' time.  It wakes up and runs for the
// 'duration'.  The run consists of a series of samples, each of which is 'interval' microseconds
// apart.  At the end of a run, it writes the results table to a file and goes back to sleep.

class BackgroundMethodSamplingProfiler {
 public:
  static void Start(int period, int duration, const std::string& profile_filename,
                    const std::string& procName, int interval_us,
                    double backoff_coefficient, bool startImmediately)
  LOCKS_EXCLUDED(Locks::mutator_lock_,
                 Locks::thread_list_lock_,
                 Locks::thread_suspend_count_lock_,
                 Locks::profiler_lock_);

  static void Stop() LOCKS_EXCLUDED(Locks::profiler_lock_, wait_lock_);
  static void Shutdown() LOCKS_EXCLUDED(Locks::profiler_lock_);

  void RecordMethod(mirror::ArtMethod *method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  Barrier& GetBarrier() {
    return *profiler_barrier_;
  }

 private:
  explicit BackgroundMethodSamplingProfiler(int period, int duration,
                                            const std::string& profile_filename,
                                            const std::string& process_name,
                                            double backoff_coefficient, int interval_us, bool startImmediately);

  // The sampling interval in microseconds is passed as an argument.
  static void* RunProfilerThread(void* arg) LOCKS_EXCLUDED(Locks::profiler_lock_);

  uint32_t WriteProfile() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void CleanProfile();
  uint32_t DumpProfile(std::ostream& os) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  static bool ShuttingDown(Thread* self) LOCKS_EXCLUDED(Locks::profiler_lock_);

  static BackgroundMethodSamplingProfiler* profiler_ GUARDED_BY(Locks::profiler_lock_);

  // We need to shut the sample thread down at exit.  Setting this to true will do that.
  static volatile bool shutting_down_ GUARDED_BY(Locks::profiler_lock_);

  // Sampling thread, non-zero when sampling.
  static pthread_t profiler_pthread_;

  // Some measure of the number of samples that are significant
  static constexpr uint32_t kSignificantSamples = 10;

  // File to write profile data out to.  Cannot be empty if we are profiling.
  std::string profile_file_name_;

  // Process name.
  std::string process_name_;

  // Number of seconds between profile runs.
  uint32_t period_s_;

  // Most of the time we want to delay the profiler startup to prevent everything
  // running at the same time (all processes).  This is the default, but if we
  // want to override this, set the 'start_immediately_' to true.  This is done
  // if the -Xprofile option is given on the command line.
  bool start_immediately_;

  uint32_t interval_us_;

  // A backoff coefficent to adjust the profile period based on time.
  double backoff_factor_;

  // How much to increase the backoff by on each profile iteration.
  double backoff_coefficient_;

  // Duration of each profile run.  The profile file will be written at the end
  // of each run.
  uint32_t duration_s_;

  // Profile condition support.
  Mutex wait_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ConditionVariable period_condition_ GUARDED_BY(wait_lock_);

  ProfileSampleResults profile_table_;

  UniquePtr<Barrier> profiler_barrier_;

  // Set of methods to be filtered out.  This will probably be rare because
  // most of the methods we want to be filtered reside in the boot path and
  // are automatically filtered.
  typedef std::set<std::string> FilteredMethods;
  FilteredMethods filtered_methods_;

  DISALLOW_COPY_AND_ASSIGN(BackgroundMethodSamplingProfiler);
};

// TODO: incorporate in ProfileSampleResults

// Profile data.  This is generated from previous runs of the program and stored
// in a file.  It is used to determine whether to compile a particular method or not.
class ProfileData {
 public:
  ProfileData() : count_(0), method_size_(0), usedPercent_(0) {}
  ProfileData(const std::string& method_name, uint32_t count, uint32_t method_size,
    double usedPercent, double topKUsedPercentage) :
    method_name_(method_name), count_(count), method_size_(method_size),
    usedPercent_(usedPercent), topKUsedPercentage_(topKUsedPercentage) {
    // TODO: currently method_size_ and count_ are unused.
    UNUSED(method_size_);
    UNUSED(count_);
  }

  bool IsAbove(double v) const { return usedPercent_ >= v; }
  double GetUsedPercent() const { return usedPercent_; }
  uint32_t GetCount() const { return count_; }
  double GetTopKUsedPercentage() const { return topKUsedPercentage_; }

 private:
  std::string method_name_;    // Method name.
  uint32_t count_;             // Number of times it has been called.
  uint32_t method_size_;       // Size of the method on dex instructions.
  double usedPercent_;         // Percentage of how many times this method was called.
  double topKUsedPercentage_;  // The percentage of the group that comprise K% of the total used
                               // methods this methods belongs to.
};

// Profile data is stored in a map, indexed by the full method name.
typedef std::map<std::string, ProfileData> ProfileMap;

class ProfileHelper {
 private:
  ProfileHelper();

 public:
  // Read the profile data from the given file.  Calculates the percentage for each method.
  // Returns false if there was no profile file or it was malformed.
  static bool LoadProfileMap(ProfileMap& profileMap, const std::string& fileName);

  // Read the profile data from the given file and computes the group that comprise
  // topKPercentage of the total used methods.
  static bool LoadTopKSamples(std::set<std::string>& topKMethods, const std::string& fileName,
                              double topKPercentage);
};

}  // namespace art

#endif  // ART_RUNTIME_PROFILER_H_

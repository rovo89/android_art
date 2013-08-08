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


#define ATRACE_TAG ATRACE_TAG_DALVIK
#include <stdio.h>
#include <cutils/trace.h>

#include "timing_logger.h"

#include "base/logging.h"
#include "thread.h"
#include "base/stl_util.h"
#include "base/histogram-inl.h"

#include <cmath>
#include <iomanip>

namespace art {

CumulativeLogger::CumulativeLogger(const std::string& name)
    : name_(name),
      lock_name_("CumulativeLoggerLock" + name),
      lock_(lock_name_.c_str(), kDefaultMutexLevel, true) {
  Reset();
}

CumulativeLogger::~CumulativeLogger() {
  STLDeleteElements(&histograms_);
}

void CumulativeLogger::SetName(const std::string& name) {
  name_.assign(name);
}

void CumulativeLogger::Start() {
  MutexLock mu(Thread::Current(), lock_);
  index_ = 0;
}

void CumulativeLogger::End() {
  MutexLock mu(Thread::Current(), lock_);
  iterations_++;
}
void CumulativeLogger::Reset() {
  MutexLock mu(Thread::Current(), lock_);
  iterations_ = 0;
  STLDeleteElements(&histograms_);
}

uint64_t CumulativeLogger::GetTotalNs() const {
  return GetTotalTime() * kAdjust;
}

uint64_t CumulativeLogger::GetTotalTime() const {
  MutexLock mu(Thread::Current(), lock_);
  uint64_t total = 0;
  for (size_t i = 0; i < histograms_.size(); ++i) {
    total += histograms_[i]->Sum();
  }
  return total;
}


void CumulativeLogger::AddLogger(const base::TimingLogger &logger) {
  MutexLock mu(Thread::Current(), lock_);
  const std::vector<std::pair<uint64_t, const char*> >& splits = logger.GetSplits();
  typedef std::vector<std::pair<uint64_t, const char*> >::const_iterator It;
  // The first time this is run, the histograms array will be empty.
  if (kIsDebugBuild && !histograms_.empty() && splits.size() != histograms_.size()) {
    LOG(ERROR) << "Mismatch in splits.";
    typedef std::vector<Histogram<uint64_t> *>::const_iterator It2;
    It it = splits.begin();
    It2 it2 = histograms_.begin();
    while ((it != splits.end()) && (it2 != histograms_.end())) {
      if (it != splits.end()) {
        LOG(ERROR) << "\tsplit: " << it->second;
        ++it;
      }
      if (it2 != histograms_.end()) {
        LOG(ERROR) << "\tpreviously record: " << (*it2)->Name();
        ++it2;
      }
    }
  }
  for (It it = splits.begin(), end = splits.end(); it != end; ++it) {
    std::pair<uint64_t, const char*> split = *it;
    uint64_t split_time = split.first;
    const char* split_name = split.second;
    AddPair(split_name, split_time);
  }
}

void CumulativeLogger::Dump(std::ostream &os) {
  MutexLock mu(Thread::Current(), lock_);
  DumpHistogram(os);
}

void CumulativeLogger::AddPair(const std::string &label, uint64_t delta_time) {
  // Convert delta time to microseconds so that we don't overflow our counters.
  delta_time /= kAdjust;
  if (histograms_.size() <= index_) {
    const size_t max_buckets = Runtime::Current()->GetHeap()->IsLowMemoryMode() ? 16 : 100;
    histograms_.push_back(new Histogram<uint64_t>(label.c_str(), 50, max_buckets));
    DCHECK_GT(histograms_.size(), index_);
  }
  histograms_[index_]->AddValue(delta_time);
  DCHECK_EQ(label, histograms_[index_]->Name());
  ++index_;
}

void CumulativeLogger::DumpHistogram(std::ostream &os) {
  os << "Start Dumping histograms for " << iterations_ << " iterations"
     << " for " << name_ << "\n";
  for (size_t Idx = 0; Idx < histograms_.size(); Idx++) {
    Histogram<uint64_t>::CumulativeData cumulative_data;
    histograms_[Idx]->CreateHistogram(cumulative_data);
    histograms_[Idx]->PrintConfidenceIntervals(os, 0.99, cumulative_data);
    // Reset cumulative values to save memory. We don't expect DumpHistogram to be called often, so
    // it is not performance critical.
  }
  os << "Done Dumping histograms \n";
}


namespace base {

TimingLogger::TimingLogger(const char* name, bool precise, bool verbose)
    : name_(name), precise_(precise), verbose_(verbose),
      current_split_(NULL), current_split_start_ns_(0) {
}

void TimingLogger::Reset() {
  current_split_ = NULL;
  current_split_start_ns_ = 0;
  splits_.clear();
}

void TimingLogger::StartSplit(const char* new_split_label) {
  DCHECK(current_split_ == NULL);
  if (verbose_) {
    LOG(INFO) << "Begin: " << new_split_label;
  }
  current_split_ = new_split_label;
  ATRACE_BEGIN(current_split_);
  current_split_start_ns_ = NanoTime();
}

// Ends the current split and starts the one given by the label.
void TimingLogger::NewSplit(const char* new_split_label) {
  DCHECK(current_split_ != NULL);
  uint64_t current_time = NanoTime();
  uint64_t split_time = current_time - current_split_start_ns_;
  ATRACE_END();
  splits_.push_back(std::pair<uint64_t, const char*>(split_time, current_split_));
  if (verbose_) {
    LOG(INFO) << "End: " << current_split_ << " " << PrettyDuration(split_time) << "\n"
        << "Begin: " << new_split_label;
  }
  current_split_ = new_split_label;
  ATRACE_BEGIN(current_split_);
  current_split_start_ns_ = current_time;
}

void TimingLogger::EndSplit() {
  DCHECK(current_split_ != NULL);
  uint64_t current_time = NanoTime();
  uint64_t split_time = current_time - current_split_start_ns_;
  ATRACE_END();
  if (verbose_) {
    LOG(INFO) << "End: " << current_split_ << " " << PrettyDuration(split_time);
  }
  splits_.push_back(std::pair<uint64_t, const char*>(split_time, current_split_));
}

uint64_t TimingLogger::GetTotalNs() const {
  uint64_t total_ns = 0;
  typedef std::vector<std::pair<uint64_t, const char*> >::const_iterator It;
  for (It it = splits_.begin(), end = splits_.end(); it != end; ++it) {
    std::pair<uint64_t, const char*> split = *it;
    total_ns += split.first;
  }
  return total_ns;
}

void TimingLogger::Dump(std::ostream &os) const {
  uint64_t longest_split = 0;
  uint64_t total_ns = 0;
  typedef std::vector<std::pair<uint64_t, const char*> >::const_iterator It;
  for (It it = splits_.begin(), end = splits_.end(); it != end; ++it) {
    std::pair<uint64_t, const char*> split = *it;
    uint64_t split_time = split.first;
    longest_split = std::max(longest_split, split_time);
    total_ns += split_time;
  }
  // Compute which type of unit we will use for printing the timings.
  TimeUnit tu = GetAppropriateTimeUnit(longest_split);
  uint64_t divisor = GetNsToTimeUnitDivisor(tu);
  // Print formatted splits.
  for (It it = splits_.begin(), end = splits_.end(); it != end; ++it) {
    std::pair<uint64_t, const char*> split = *it;
    uint64_t split_time = split.first;
    if (!precise_ && divisor >= 1000) {
      // Make the fractional part 0.
      split_time -= split_time % (divisor / 1000);
    }
    os << name_ << ": " << std::setw(8) << FormatDuration(split_time, tu) << " "
       << split.second << "\n";
  }
  os << name_ << ": end, " << NsToMs(total_ns) << " ms\n";
}

}  // namespace base
}  // namespace art

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

#include "timing_logger.h"

#include "base/logging.h"
#include "thread.h"
#include "base/stl_util.h"
#include "base/histogram-inl.h"

#include <cmath>
#include <iomanip>

namespace art {

void TimingLogger::Reset() {
  times_.clear();
  labels_.clear();
  AddSplit("");
}

TimingLogger::TimingLogger(const std::string &name, bool precise)
    : name_(name),
      precise_(precise) {
  AddSplit("");
}

void TimingLogger::AddSplit(const std::string &label) {
  times_.push_back(NanoTime());
  labels_.push_back(label);
}

uint64_t TimingLogger::GetTotalNs() const {
  return times_.back() - times_.front();
}
;

void TimingLogger::Dump(std::ostream &os) const {
  uint64_t largest_time = 0;
  os << name_ << ": begin\n";
  for (size_t i = 1; i < times_.size(); ++i) {
    uint64_t delta_time = times_[i] - times_[i - 1];
    largest_time = std::max(largest_time, delta_time);
  }
  // Compute which type of unit we will use for printing the timings.
  TimeUnit tu = GetAppropriateTimeUnit(largest_time);
  uint64_t divisor = GetNsToTimeUnitDivisor(tu);
  for (size_t i = 1; i < times_.size(); ++i) {
    uint64_t delta_time = times_[i] - times_[i - 1];
    if (!precise_ && divisor >= 1000) {
      // Make the fraction 0.
      delta_time -= delta_time % (divisor / 1000);
    }
    os << name_ << ": " << std::setw(8) << FormatDuration(delta_time, tu) << " "
       << labels_[i] << "\n";
  }
  os << name_ << ": end, " << NsToMs(GetTotalNs()) << " ms\n";
}

CumulativeLogger::CumulativeLogger(const std::string &name)
    : name_(name),
      lock_(("CumulativeLoggerLock" + name).c_str(), kDefaultMutexLevel, true) {
  Reset();
}

CumulativeLogger::~CumulativeLogger() { STLDeleteElements(&histograms_); }

void CumulativeLogger::SetName(const std::string &name) { name_ = name; }

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

void CumulativeLogger::AddLogger(const TimingLogger &logger) {
  MutexLock mu(Thread::Current(), lock_);
  DCHECK_EQ(logger.times_.size(), logger.labels_.size());
  for (size_t i = 1; i < logger.times_.size(); ++i) {
    const uint64_t delta_time = logger.times_[i] - logger.times_[i - 1];
    const std::string &label = logger.labels_[i];
    AddPair(label, delta_time);
  }
}

void CumulativeLogger::Dump(std::ostream &os) {
  MutexLock mu(Thread::Current(), lock_);
  DumpHistogram(os);
}

void CumulativeLogger::AddPair(const std::string &label, uint64_t delta_time) {

  // Convert delta time to microseconds so that we don't overflow our counters.
  delta_time /= kAdjust;
  if (index_ >= histograms_.size()) {
    Histogram<uint64_t> *tmp_hist = new Histogram<uint64_t>(label);
    tmp_hist->AddValue(delta_time);
    histograms_.push_back(tmp_hist);
  } else {
    histograms_[index_]->AddValue(delta_time);
    DCHECK_EQ(label, histograms_[index_]->Name());
  }
  index_++;
}

void CumulativeLogger::DumpHistogram(std::ostream &os) {
  os << "Start Dumping histograms for " << iterations_ << " iterations"
     << " for " << name_ << "\n";
  for (size_t Idx = 0; Idx < histograms_.size(); Idx++) {
    Histogram<uint64_t> &hist = *(histograms_[Idx]);
    hist.CreateHistogram();
    hist.PrintConfidenceIntervals(os, 0.99);
  }
  os << "Done Dumping histograms \n";
}

}  // namespace art

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

#ifndef ART_SRC_TIMING_LOGGER_H_
#define ART_SRC_TIMING_LOGGER_H_

#include "logging.h"
#include "utils.h"

#include <stdint.h>

#include <string>
#include <vector>

namespace art {

class TimingLogger {
 public:
  explicit TimingLogger(const char* name, bool precise = false)
      : name_(name), precise_(precise) {
    AddSplit("");
  }

  void AddSplit(const std::string& label) {
    times_.push_back(NanoTime());
    labels_.push_back(label);
  }

  void Dump() const {
    Dump(LOG(INFO));
  }

  void Dump(std::ostream& os) const {
    os << name_ << ": begin\n";
    for (size_t i = 1; i < times_.size(); ++i) {
      if (precise_) {
        os << name_ << ": " << std::setw(12) << PrettyDuration(times_[i] - times_[i-1]) << " "
           << labels_[i] << "\n";
      } else {
        os << name_ << StringPrintf(": %8lld ms, ", NsToMs(times_[i] - times_[i-1])) << labels_[i]
           <<  "\n";
      }
    }
    os << name_ << ": end, " << NsToMs(GetTotalNs()) << " ms\n";
  }

  uint64_t GetTotalNs() const {
    return times_.back() - times_.front();
  }

 private:
  std::string name_;
  bool precise_;
  std::vector<uint64_t> times_;
  std::vector<std::string> labels_;
};

}  // namespace art

#endif  // ART_SRC_TIMING_LOGGER_H_

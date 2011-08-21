// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MONITOR_H_
#define ART_SRC_MONITOR_H_

#include "logging.h"
#include "macros.h"

namespace art {

class Monitor {
 public:

  // Lock constants used by compiler
  static const uint32_t kLwLockOwnerShift = 3;
  static const uint32_t kLwHashStateShift = 1;
  static const uint32_t kLwHashStateMask = 0x3;
  static const uint32_t kLwShapeThin = 0;

  void Enter() {
  }

  void Exit() {
  }

  void Notify() {
  }

  void NotifyAll() {
  }

  void Wait() {
  }

  void Wait(int64_t timeout) {
    Wait(timeout, 0);
  }

  void Wait(int64_t timeout, int32_t nanos) {
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(Monitor);

};

class MonitorLock {
 public:

  MonitorLock(Monitor* monitor) : monitor_(monitor) {
    CHECK(monitor != NULL);
    monitor_->Enter();
  }

  ~MonitorLock() {
    monitor_->Exit();
  }

  void Wait(int64_t millis = 0) {
    monitor_->Wait(millis);
  }

  void Notify() {
    monitor_->Notify();
  }

  void NotifyAll() {
    monitor_->NotifyAll();
  }

 private:
  Monitor* const monitor_;
  DISALLOW_COPY_AND_ASSIGN(MonitorLock);
};

}  // namespace art

#endif  // ART_SRC_MONITOR_H_

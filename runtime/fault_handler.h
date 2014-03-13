/*
 * Copyright (C) 2008 The Android Open Source Project
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


#ifndef ART_RUNTIME_FAULT_HANDLER_H_
#define ART_RUNTIME_FAULT_HANDLER_H_

#include <signal.h>
#include <vector>
#include <setjmp.h>
#include <stdint.h>

#include "base/mutex.h"   // For annotalysis.

namespace art {
class FaultHandler;

class FaultManager {
 public:
  FaultManager();
  ~FaultManager();

  void Init();

  void HandleFault(int sig, siginfo_t* info, void* context);
  void AddHandler(FaultHandler* handler);
  void RemoveHandler(FaultHandler* handler);

 private:
  bool IsInGeneratedCode(void *context) NO_THREAD_SAFETY_ANALYSIS;
  void GetMethodAndReturnPC(void* context, uintptr_t& method, uintptr_t& return_pc);

  typedef std::vector<FaultHandler*> Handlers;
  Handlers handlers_;
  struct sigaction oldaction_;
};

class FaultHandler {
 public:
  FaultHandler() : manager_(nullptr) {}
  explicit FaultHandler(FaultManager* manager) : manager_(manager) {}
  virtual ~FaultHandler() {}

  virtual bool Action(int sig, siginfo_t* siginfo, void* context) = 0;
 protected:
  FaultManager* const manager_;
};

class NullPointerHandler FINAL : public FaultHandler {
 public:
  NullPointerHandler() {}
  explicit NullPointerHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) OVERRIDE;
};

class SuspensionHandler FINAL : public FaultHandler {
 public:
  SuspensionHandler() {}
  explicit SuspensionHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) OVERRIDE;
};

class StackOverflowHandler FINAL : public FaultHandler {
 public:
  StackOverflowHandler() {}
  explicit StackOverflowHandler(FaultManager* manager);

  bool Action(int sig, siginfo_t* siginfo, void* context) OVERRIDE;
};

// Statically allocated so the the signal handler can get access to it.
extern FaultManager fault_manager;

}       // namespace art
#endif  // ART_RUNTIME_FAULT_HANDLER_H_


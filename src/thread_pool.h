/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_THREAD_POOL_H_
#define ART_SRC_THREAD_POOL_H_

#include <deque>
#include <vector>

#include "locks.h"
#include "../src/mutex.h"

namespace art {

class Closure;
class ThreadPool;

class ThreadPoolWorker {
 public:
  static const size_t kDefaultStackSize = 1 * MB;

  size_t GetStackSize() const {
    return stack_size_;
  }

  virtual ~ThreadPoolWorker();

 private:
  ThreadPoolWorker(ThreadPool* thread_pool, const std::string& name, size_t stack_size);
  static void* Callback(void* arg) LOCKS_EXCLUDED(Locks::mutator_lock_);
  void Run();

  ThreadPool* thread_pool_;
  const std::string name_;
  const size_t stack_size_;
  pthread_t pthread_;

  friend class ThreadPool;
  DISALLOW_COPY_AND_ASSIGN(ThreadPoolWorker);
};

class ThreadPool {
 public:
  // Returns the number of threads in the thread pool.
  size_t GetThreadCount() const {
    return threads_.size();
  }

  // Broadcast to the workers and tell them to empty out the work queue.
  void StartWorkers(Thread* self);

  // Do not allow workers to grab any new tasks.
  void StopWorkers(Thread* self);

  // Add a new task, the first available started worker will process it. Does not delete the task
  // after running it, it is the caller's responsibility.
  void AddTask(Thread* self, Closure* task);

  ThreadPool(size_t num_threads);
  virtual ~ThreadPool();

  // Wait for all tasks currently on queue to get completed.
  void Wait(Thread* self);

 private:
  // Add a new task.
  void AddThread(size_t stack_size);

  // Get a task to run, blocks if there are no tasks left
  Closure* GetTask(Thread* self);

  Mutex task_queue_lock_;
  ConditionVariable task_queue_condition_ GUARDED_BY(task_queue_lock_);
  ConditionVariable completion_condition_ GUARDED_BY(task_queue_lock_);
  volatile bool started_ GUARDED_BY(task_queue_lock_);
  volatile bool shutting_down_ GUARDED_BY(task_queue_lock_);
  // How many worker threads are waiting on the condition.
  volatile size_t waiting_count_ GUARDED_BY(task_queue_lock_);
  std::deque<Closure*> tasks_ GUARDED_BY(task_queue_lock_);
  // TODO: make this immutable/const?
  std::vector<ThreadPoolWorker*> threads_;

  friend class ThreadPoolWorker;
  DISALLOW_COPY_AND_ASSIGN(ThreadPool);
};

}  // namespace art

#endif  // ART_SRC_THREAD_POOL_H_

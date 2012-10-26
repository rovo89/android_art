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


#include <string>

#include "atomic_integer.h"
#include "common_test.h"
#include "thread_pool.h"

namespace art {

class CountClosure : public Closure {
 public:
  CountClosure(AtomicInteger* count) : count_(count) {

  }

  void Run(Thread* /* self */) {
    // Simulate doing some work.
    usleep(100);
    // Increment the counter which keeps track of work completed.
    ++*count_;
    delete this;
  }

 private:
  AtomicInteger* const count_;
};

class ThreadPoolTest : public CommonTest {
 public:
  static int32_t num_threads;
};

int32_t ThreadPoolTest::num_threads = 4;

// Check that the thread pool actually runs tasks that you assign it.
TEST_F(ThreadPoolTest, CheckRun) {
  Thread* self = Thread::Current();
  ThreadPool thread_pool(num_threads);
  AtomicInteger count = 0;
  static const int32_t num_tasks = num_threads * 4;
  for (int32_t i = 0; i < num_tasks; ++i) {
    thread_pool.AddTask(self, new CountClosure(&count));
  }
  thread_pool.StartWorkers(self);
  // Wait for tasks to complete.
  thread_pool.Wait(self);
  // Make sure that we finished all the work.
  EXPECT_EQ(num_tasks, count);
}

TEST_F(ThreadPoolTest, StopStart) {
  Thread* self = Thread::Current();
  ThreadPool thread_pool(num_threads);
  AtomicInteger count = 0;
  static const int32_t num_tasks = num_threads * 4;
  for (int32_t i = 0; i < num_tasks; ++i) {
    thread_pool.AddTask(self, new CountClosure(&count));
  }
  usleep(200);
  // Check that no threads started prematurely.
  EXPECT_EQ(0, count);
  // Signal the threads to start processing tasks.
  thread_pool.StartWorkers(self);
  usleep(200);
  thread_pool.StopWorkers(self);
  AtomicInteger bad_count = 0;
  thread_pool.AddTask(self, new CountClosure(&bad_count));
  usleep(200);
  // Ensure that the task added after the workers were stopped doesn't get run.
  EXPECT_EQ(0, bad_count);
}

class TreeClosure : public Closure {
 public:
  TreeClosure(ThreadPool* const thread_pool, AtomicInteger* count, int depth)
      : thread_pool_(thread_pool),
        count_(count),
        depth_(depth) {

  }

  void Run(Thread* self) {
    if (depth_ > 1) {
      thread_pool_->AddTask(self, new TreeClosure(thread_pool_, count_, depth_ - 1));
      thread_pool_->AddTask(self, new TreeClosure(thread_pool_, count_, depth_ - 1));
    }
    // Increment the counter which keeps track of work completed.
    ++*count_;
    delete this;
  }

 private:
  ThreadPool* const thread_pool_;
  AtomicInteger* const count_;
  const int depth_;
};

// Test that adding new tasks from within a task works.
TEST_F(ThreadPoolTest, RecursiveTest) {
  Thread* self = Thread::Current();
  ThreadPool thread_pool(num_threads);
  AtomicInteger count = 0;
  static const int depth = 8;
  thread_pool.AddTask(self, new TreeClosure(&thread_pool, &count, depth));
  thread_pool.StartWorkers(self);
  thread_pool.Wait(self);
  EXPECT_EQ((1 << depth) - 1, count);
}

}  // namespace art

#include "runtime.h"
#include "stl_util.h"
#include "thread.h"
#include "thread_pool.h"

namespace art {

ThreadPoolWorker::ThreadPoolWorker(ThreadPool* thread_pool, const std::string& name,
                                   size_t stack_size)
    : thread_pool_(thread_pool),
      name_(name),
      stack_size_(stack_size) {
  const char* reason = "new thread pool worker thread";
  pthread_attr_t attr;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attr), reason);
  CHECK_PTHREAD_CALL(pthread_attr_setstacksize, (&attr, stack_size), reason);
  CHECK_PTHREAD_CALL(pthread_create, (&pthread_, &attr, &Callback, this), reason);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attr), reason);
}

ThreadPoolWorker::~ThreadPoolWorker() {
  CHECK_PTHREAD_CALL(pthread_join, (pthread_, NULL), "thread pool worker shutdown");
}

void ThreadPoolWorker::Run() {
  Thread* self = Thread::Current();
  Closure* task = NULL;
  while ((task = thread_pool_->GetTask(self)) != NULL) {
    task->Run(self);
  }
}

void* ThreadPoolWorker::Callback(void* arg) {
  ThreadPoolWorker* worker = reinterpret_cast<ThreadPoolWorker*>(arg);
  Runtime* runtime = Runtime::Current();
  CHECK(runtime->AttachCurrentThread(worker->name_.c_str(), true, NULL));
  // Do work until its time to shut down.
  worker->Run();
  runtime->DetachCurrentThread();
  return NULL;
}

void ThreadPool::AddTask(Thread* self, Closure* task){
  MutexLock mu(self, task_queue_lock_);
  tasks_.push_back(task);
  // If we have any waiters, signal one.
  if (waiting_count_ != 0) {
    task_queue_condition_.Signal(self);
  }
}

void ThreadPool::AddThread(size_t stack_size) {
  threads_.push_back(
      new ThreadPoolWorker(
          this,
          StringPrintf("Thread pool worker %d", static_cast<int>(GetThreadCount())),
          stack_size));
}

ThreadPool::ThreadPool(size_t num_threads)
  : task_queue_lock_("task queue lock"),
    task_queue_condition_("task queue condition", task_queue_lock_),
    completion_condition_("task completion condition", task_queue_lock_),
    started_(false),
    shutting_down_(false),
    waiting_count_(0) {
  while (GetThreadCount() < num_threads) {
    AddThread(ThreadPoolWorker::kDefaultStackSize);
  }
}

ThreadPool::~ThreadPool() {
  {
    Thread* self = Thread::Current();
    MutexLock mu(self, task_queue_lock_);
    // Tell any remaining workers to shut down.
    shutting_down_ = true;
    android_memory_barrier();
    // Broadcast to everyone waiting.
    task_queue_condition_.Broadcast(self);
  }
  // Wait for the threads to finish.
  STLDeleteElements(&threads_);
}

void ThreadPool::StartWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = true;
  android_memory_barrier();
  task_queue_condition_.Broadcast(self);
}

void ThreadPool::StopWorkers(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  started_ = false;
  android_memory_barrier();
}

Closure* ThreadPool::GetTask(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  while (!shutting_down_) {
    if (started_ && !tasks_.empty()) {
      Closure* task = tasks_.front();
      tasks_.pop_front();
      return task;
    }

    waiting_count_++;
    if (waiting_count_ == GetThreadCount() && tasks_.empty()) {
      // We may be done, lets broadcast to the completion condition.
      completion_condition_.Broadcast(self);
    }
    task_queue_condition_.Wait(self);
    waiting_count_--;
  }

  // We are shutting down, return NULL to tell the worker thread to stop looping.
  return NULL;
}

void ThreadPool::Wait(Thread* self) {
  MutexLock mu(self, task_queue_lock_);
  // Wait until each thread is waiting and the task list is empty.
  while (waiting_count_ != GetThreadCount() || !tasks_.empty()) {
    completion_condition_.Wait(self);
  }
}

}  // namespace art

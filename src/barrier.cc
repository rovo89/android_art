#include "barrier.h"
#include "../src/mutex.h"
#include "thread.h"

namespace art {

Barrier::Barrier(int count)
    : count_(count),
      lock_("GC barrier lock"),
      condition_("GC barrier condition", lock_) {
}

void Barrier::Pass(Thread* self) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count_ - 1);
}

void Barrier::Wait(Thread* self) {
  Increment(self, -1);
}

void Barrier::Init(Thread* self, int count) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count);
}

void Barrier::Increment(Thread* self, int delta) {
  MutexLock mu(self, lock_);
  SetCountLocked(self, count_ + delta);
  if (count_ != 0) {
    condition_.Wait(self);
  }
}

void Barrier::SetCountLocked(Thread* self, int count) {
  count_ = count;
  if (count_ == 0) {
    condition_.Broadcast(self);
  }
}

Barrier::~Barrier() {
  CHECK(!count_) << "Attempted to destroy barrier with non zero count";
}

}

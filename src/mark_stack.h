// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_MARK_STACK_H_
#define ART_SRC_MARK_STACK_H_

#include "UniquePtr.h"
#include "logging.h"
#include "macros.h"
#include "mem_map.h"

namespace art {

class Object;

class MarkStack {
 public:
  static MarkStack* Create();

  ~MarkStack();

  void Push(const Object* obj) {
    DCHECK(obj != NULL);
    DCHECK_NE(ptr_, limit_);
    *ptr_ = obj;
    ++ptr_;
  }

  const Object* Pop() {
    DCHECK_NE(ptr_, base_);
    --ptr_;
    DCHECK(*ptr_ != NULL);
    return *ptr_;
  }

  bool IsEmpty() const {
    return ptr_ == base_;
  }

 private:
  MarkStack() :
      base_(NULL), limit_(NULL), ptr_(NULL) {
  }

  bool Init();

  // Memory mapping of the mark stack.
  UniquePtr<MemMap> mem_map_;

  // Base of the mark stack.
  const Object* const* base_;

  // Exclusive limit of the mark stack.
  const Object* const* limit_;

  // Pointer to the top of the mark stack.
  Object const**  ptr_;

  DISALLOW_COPY_AND_ASSIGN(MarkStack);
};

}  // namespace art

#endif  // ART_SRC_MARK_STACK_H_

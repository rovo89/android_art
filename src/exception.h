// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_EXCEPTION_H_
#define ART_SRC_EXCEPTION_H_

#include "class_linker.h"
#include "dex_file.h"
#include "object.h"
#include "thread.h"

namespace art {

struct InternalStackTrace {
  const Method* method;
  const uint16_t* pc;
};

extern InternalStackTrace *GetStackTrace(uint16_t stack_depth);

}

#endif  // ART_SRC_EXCEPTION_H_

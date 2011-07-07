// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_VERIFY_H_
#define ART_SRC_DEX_VERIFY_H_

#include "src/macros.h"
#include "src/object.h"

namespace art {

class DexVerify {
 public:
  static bool VerifyClass(Class* klass);

 private:
  static bool VerifyMethod(Method* method);

  DISALLOW_COPY_AND_ASSIGN(DexVerify);
};

}  // namespace art

#endif  // ART_SRC_DEX_VERIFY_H_

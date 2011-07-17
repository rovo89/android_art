// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/dex_verifier.h"

#include <iostream>

#include "logging.h"
#include "stringpiece.h"

namespace art {

bool DexVerify::VerifyClass(Class* klass) {
  if (klass->IsVerified()) {
    return true;
  }
  for (size_t i = 0; i < klass->NumDirectMethods(); ++i) {
    Method* method = klass->GetDirectMethod(i);
    if (!VerifyMethod(method)) {
        LOG(ERROR) << "Verifier rejected class " << klass->GetDescriptor();
      return false;
    }
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (!VerifyMethod(method)) {
        LOG(ERROR) << "Verifier rejected class " << klass->GetDescriptor();
      return false;
    }
  }
  return true;
}

bool DexVerify::VerifyMethod(Method* method) {
  return true;  // TODO
}

}  // namespace art

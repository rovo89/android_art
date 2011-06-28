// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_H_
#define ART_SRC_RUNTIME_H_

namespace art {

class Runtime {
 public:
  static bool Startup();
  static void Shutdown();

  static void Compile(const char* filename);
};

}  // namespace art

namespace r = art;

#endif  // ART_SRC_RUNTIME_H_

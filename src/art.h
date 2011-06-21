// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_ART_H_
#define ART_SRC_ART_H_

namespace art {

class Art {
 public:
  static bool Startup();
  static void Shutdown();
};

}  // namespace art

namespace r = art;

#endif  // ART_SRC_ART_H_

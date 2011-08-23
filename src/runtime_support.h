// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_SUPPORT_H_
#define ART_SRC_RUNTIME_SUPPORT_H_

#if defined(__arm__)
  extern "C" uint64_t art_shl_long(uint64_t, uint32_t);
  extern "C" uint64_t art_shr_long(uint64_t, uint32_t);
  extern "C" uint64_t art_ushr_long(uint64_t, uint32_t);
#endif

#endif  // ART_SRC_RUNTIME_SUPPORT_H_

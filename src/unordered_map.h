// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_CLASS_UNORDERED_MAP_H_
#define ART_SRC_CLASS_UNORDERED_MAP_H_

#include "stringpiece.h"

#ifdef __ANDROID__
#include <unordered_map>
#else
#include <tr1/unordered_map>
#endif

//TODO: move out to stringpiece.h?
namespace std {
#ifndef __ANDROID__
namespace tr1 {
#endif
template<>
struct hash<art::StringPiece> {
 public:
  size_t operator()(const art::StringPiece& string_piece) const {
    size_t string_size = string_piece.size();
    const char* string_data = string_piece.data();
    // this is the java.lang.String hashcode for convenience, not interoperability
    size_t hash = 0;
    while (string_size--) {
      hash = hash * 31 + *string_data++;
    }
    return hash;
  }
};
#ifndef __ANDROID__
}  // namespace tr1
#endif
}  // namespace std

#endif  // ART_SRC_CLASS_UNORDERED_MAP_H_

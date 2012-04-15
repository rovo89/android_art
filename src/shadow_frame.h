/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_SRC_SHADOW_FRAME_H_
#define ART_SRC_SHADOW_FRAME_H_

#include "logging.h"
#include "macros.h"

namespace art {

class Object;
class Method;

class ShadowFrame {
 public:
  // Number of references contained within this shadow frame
  uint32_t NumberOfReferences() const {
    return number_of_references_;
  }

  // Caller line number
  uint32_t GetLineNumber() const {
    return line_num_;
  }

  // Link to previous shadow frame or NULL
  ShadowFrame* GetLink() const {
    return link_;
  }

  void SetLink(ShadowFrame* frame) {
    DCHECK_NE(this, frame);
    link_ = frame;
  }

  Object* GetReference(size_t i) const {
    DCHECK_LT(i, number_of_references_);
    return references_[i];
  }

  void SetReference(size_t i, Object* object) {
    DCHECK_LT(i, number_of_references_);
    references_[i] = object;
  }

  Method* GetMethod() const {
    DCHECK_NE(method_, static_cast<void*>(NULL));
    return method_;
  }

  bool Contains(Object** shadow_frame_entry) const {
    // A ShadowFrame should at least contain a reference. Even if a
    // native method has no argument, we put jobject or jclass as a
    // reference. The former is "this", while the latter is for static
    // method.
    DCHECK_GT(number_of_references_, 0U);
    return ((&references_[0] <= shadow_frame_entry)
            && (shadow_frame_entry <= (&references_[number_of_references_ - 1])));
  }

  // Offset of link within shadow frame
  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, link_);
  }

  // Offset of method within shadow frame
  static size_t MethodOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, method_);
  }

  // Offset of line number within shadow frame
  static size_t LineNumOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, line_num_);
  }

  // Offset of length within shadow frame
  static size_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, number_of_references_);
  }

  // Offset of references within shadow frame
  static size_t ReferencesOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, references_);
  }

 private:
  // ShadowFrame should be allocated by the generated code directly.
  // We should not create new shadow stack in the runtime support function.
  ~ShadowFrame() {}

  uint32_t number_of_references_;
  ShadowFrame* link_;
  Method* method_;
  uint32_t line_num_;
  Object* references_[];

  DISALLOW_IMPLICIT_CONSTRUCTORS(ShadowFrame);
};

}  // namespace art

#endif  // ART_SRC_SHADOW_FRAME_H_

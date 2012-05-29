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

#include "lir_function.h"

#include "lir_frame_info.h"

#include "logging.h"

namespace llvm {

using art::greenland::LIR;
using art::greenland::LIRFunction;

void ilist_traits<LIR>::addNodeToList(LIR* lir) {
  DCHECK(lir->GetParent() == 0) << "LIR already in a function";
  // Update the LIR::parent_
  lir->SetParent(parent_);
  return;
}

void ilist_traits<LIR>::removeNodeFromList(LIR* lir) {
  DCHECK(lir->GetParent() == 0) << "LIR is not in a function";
  // Update the LIR::parent_
  lir->SetParent(NULL);
  return;
}

void ilist_traits<LIR>::transferNodesFromList(ilist_traits &src_traits,
                                              ilist_iterator<LIR> first,
                                              ilist_iterator<LIR> last) {
  UNIMPLEMENTED(FATAL);
  return;
}

void ilist_traits<LIR>::deleteNode(LIR* lir) {
  CHECK(!lir->GetParent()) << "LIR is still in a block!";
  parent_->DeleteLIR(lir);
  return;
}

} // namespace llvm

namespace art {
namespace greenland {

LIRFunction::LIRFunction() : frame_info_(NULL) {
  lirs_.parent_ = this;
  frame_info_ = new (allocator_) LIRFrameInfo();
  return;
}

LIRFunction::~LIRFunction() {
  lirs_.clear();
  lir_recycler_.clear(allocator_);

  frame_info_->~LIRFrameInfo();
  allocator_.Deallocate(frame_info_);

  return;
}

LIR* LIRFunction::CreateLIR(const LIRDesc& desc) {
  return new (lir_recycler_.Allocate<LIR>(allocator_)) LIR(desc);
}

void LIRFunction::DeleteLIR(LIR* lir) {
  lir->~LIR();
  lir_recycler_.Deallocate(allocator_, lir);
}

} // namespace greenland
} // namespace art

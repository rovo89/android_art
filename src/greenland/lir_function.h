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

#ifndef ART_SRC_GREENLAND_LIR_FUNCTION_H_
#define ART_SRC_GREENLAND_LIR_FUNCTION_H_

#include "lir.h"

#include <llvm/ADT/ilist.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/Recycler.h>

namespace llvm {

template <>
struct ilist_traits<art::greenland::LIR> :
    public ilist_default_traits<art::greenland::LIR> {
 private:
  mutable ilist_half_node<art::greenland::LIR> sentinel_;

  friend class art::greenland::LIRFunction;
  art::greenland::LIRFunction* parent_;

 public:
  art::greenland::LIR *createSentinel() const {
    return static_cast<art::greenland::LIR*>(&sentinel_);
  }
  void destroySentinel(art::greenland::LIR *) const {}

  art::greenland::LIR *provideInitialHead() const {
    return createSentinel();
  }
  art::greenland::LIR *ensureHead(art::greenland::LIR*) const {
    return createSentinel();
  }
  static void noteHead(art::greenland::LIR*, art::greenland::LIR*) {}

  void addNodeToList(art::greenland::LIR* N);
  void removeNodeFromList(art::greenland::LIR* N);
  void transferNodesFromList(ilist_traits &src_traits,
                             ilist_iterator<art::greenland::LIR> first,
                             ilist_iterator<art::greenland::LIR> last);
  void deleteNode(art::greenland::LIR *N);
private:
  void createNode(const art::greenland::LIR &);
};

} // namespace llvm

namespace art {
namespace greenland {

class LIRFrameInfo;

class LIRFunction {
 private:
  llvm::ilist<LIR> lirs_;

  // Pool-allocate the objects reside in this instance
  llvm::BumpPtrAllocator allocator_;

  // Allocation management for instructions in function.
  llvm::Recycler<LIR> lir_recycler_;

  // The stack information
  LIRFrameInfo* frame_info_;

 public:
  LIRFunction();
  ~LIRFunction();

 public:
  //----------------------------------------------------------------------------
  // LIR Accessor Functions
  //----------------------------------------------------------------------------
  typedef llvm::ilist<LIR>::iterator                          iterator;
  typedef llvm::ilist<LIR>::const_iterator              const_iterator;
  typedef std::reverse_iterator<iterator>             reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

  unsigned GetNumLIRs() const {
    return static_cast<unsigned>(lirs_.size());
  }
  bool IsEmpty() const {
    return lirs_.empty();
  }

  LIR& front() { return lirs_.front(); }
  LIR& back()  { return lirs_.back(); }
  const LIR& front() const { return lirs_.front(); }
  const LIR& back()  const { return lirs_.back(); }

  iterator                begin()       { return lirs_.begin();  }
  const_iterator          begin() const { return lirs_.begin();  }
  iterator                  end()       { return lirs_.end();    }
  const_iterator            end() const { return lirs_.end();    }
  reverse_iterator       rbegin()       { return lirs_.rbegin(); }
  const_reverse_iterator rbegin() const { return lirs_.rbegin(); }
  reverse_iterator       rend  ()       { return lirs_.rend();   }
  const_reverse_iterator rend  () const { return lirs_.rend();   }

  void pop_front() {
    lirs_.pop_front();
  }

  void push_back(LIR *lir) {
    lirs_.push_back(lir);
  }

  iterator insert(iterator i, LIR* lir) {
    return lirs_.insert(i, lir);
  }

  iterator erase(iterator i) {
    return lirs_.erase(i);
  }

  LIR* remove(iterator i) {
    return lirs_.remove(i);
  }

 public:
  //----------------------------------------------------------------------------
  // LIR Memory Allocation
  //----------------------------------------------------------------------------
  LIR* CreateLIR(const LIRDesc& desc);

  void DeleteLIR(LIR* lir);

 public:
  const LIRFrameInfo& GetFrameInfo() const {
    return *frame_info_;
  }
  LIRFrameInfo& GetFrameInfo() {
    return *frame_info_;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(LIRFunction);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_LIR_FUNCTION_H_

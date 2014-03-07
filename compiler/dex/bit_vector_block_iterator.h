/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_BIT_VECTOR_BLOCK_ITERATOR_H_
#define ART_COMPILER_DEX_BIT_VECTOR_BLOCK_ITERATOR_H_

#include "base/bit_vector.h"
#include "compiler_enums.h"
#include "utils/arena_bit_vector.h"
#include "utils/arena_allocator.h"
#include "compiler_ir.h"

namespace art {

class MIRGraph;

/**
 * @class BasicBlockIterator
 * @brief Helper class to get the BasicBlocks when iterating through the ArenaBitVector.
 */
class BitVectorBlockIterator {
  public:
    explicit BitVectorBlockIterator(BitVector* bv, MIRGraph* mir_graph)
      : mir_graph_(mir_graph),
        internal_iterator_(bv) {}

    explicit BitVectorBlockIterator(BitVector* bv, CompilationUnit* c_unit)
      : mir_graph_(c_unit->mir_graph.get()),
        internal_iterator_(bv) {}

    BasicBlock* Next();

    void* operator new(size_t size, ArenaAllocator* arena) {
      return arena->Alloc(size, kArenaAllocGrowableArray);
    };
    void operator delete(void* p) {}  // Nop.

  private:
    MIRGraph* const mir_graph_;
    BitVector::Iterator internal_iterator_;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_BIT_VECTOR_BLOCK_ITERATOR_H_

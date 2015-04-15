/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_PARALLEL_MOVE_RESOLVER_H_
#define ART_COMPILER_OPTIMIZING_PARALLEL_MOVE_RESOLVER_H_

#include "base/value_object.h"
#include "utils/growable_array.h"

namespace art {

class HParallelMove;
class Location;
class MoveOperands;

/**
 * Helper class to resolve a set of parallel moves. Architecture dependent code
 * generator must have their own subclass that implements the `EmitMove` and `EmitSwap`
 * operations.
 */
class ParallelMoveResolver : public ValueObject {
 public:
  explicit ParallelMoveResolver(ArenaAllocator* allocator) : moves_(allocator, 32) {}
  virtual ~ParallelMoveResolver() {}

  // Resolve a set of parallel moves, emitting assembler instructions.
  void EmitNativeCode(HParallelMove* parallel_move);

 protected:
  class ScratchRegisterScope : public ValueObject {
   public:
    // Spill a scratch register if no regs are free.
    ScratchRegisterScope(ParallelMoveResolver* resolver,
                         int blocked,
                         int if_scratch,
                         int number_of_registers);
    // Grab a scratch register only if available.
    ScratchRegisterScope(ParallelMoveResolver* resolver,
                         int blocked,
                         int number_of_registers);
    ~ScratchRegisterScope();

    int GetRegister() const { return reg_; }
    bool IsSpilled() const { return spilled_; }

   private:
    ParallelMoveResolver* resolver_;
    int reg_;
    bool spilled_;
  };

  bool IsScratchLocation(Location loc);

  // Allocate a scratch register for performing a move. The method will try to use
  // a register that is the destination of a move, but that move has not been emitted yet.
  int AllocateScratchRegister(int blocked, int if_scratch, int register_count, bool* spilled);
  // As above, but return -1 if no free register.
  int AllocateScratchRegister(int blocked, int register_count);

  // Emit a move.
  virtual void EmitMove(size_t index) = 0;

  // Execute a move by emitting a swap of two operands.
  virtual void EmitSwap(size_t index) = 0;

  virtual void SpillScratch(int reg) = 0;
  virtual void RestoreScratch(int reg) = 0;

  // List of moves not yet resolved.
  GrowableArray<MoveOperands*> moves_;

  static constexpr int kNoRegister = -1;

 private:
  // Build the initial list of moves.
  void BuildInitialMoveList(HParallelMove* parallel_move);

  // Perform the move at the moves_ index in question (possibly requiring
  // other moves to satisfy dependencies).
  //
  // Return whether another move in the dependency cycle needs to swap. This
  // is to handle 64bits swaps:
  // 1) In the case of register pairs, where we want the pair to swap first to avoid
  //    building pairs that are unexpected by the code generator. For example, if
  //    we were to swap R1 with R2, we would need to update all locations using
  //    R2 to R1. So a (R2,R3) pair register could become (R1,R3). We could make
  //    the code generator understand such pairs, but it's easier and cleaner to
  //    just not create such pairs and exchange pairs in priority.
  // 2) Even when the architecture does not have pairs, we must handle 64bits swaps
  //    first. Consider the case: (R0->R1) (R1->S) (S->R0), where 'S' is a single
  //    stack slot. If we end up swapping S and R0, S will only contain the low bits
  //    of R0. If R0->R1 is for a 64bits instruction, R1 will therefore not contain
  //    the right value.
  MoveOperands* PerformMove(size_t index);

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolver);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_PARALLEL_MOVE_RESOLVER_H_

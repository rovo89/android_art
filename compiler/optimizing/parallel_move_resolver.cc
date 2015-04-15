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
#include <iostream>

#include "parallel_move_resolver.h"
#include "nodes.h"
#include "locations.h"

namespace art {

void ParallelMoveResolver::EmitNativeCode(HParallelMove* parallel_move) {
  DCHECK(moves_.IsEmpty());
  // Build up a worklist of moves.
  BuildInitialMoveList(parallel_move);

  for (size_t i = 0; i < moves_.Size(); ++i) {
    const MoveOperands& move = *moves_.Get(i);
    // Skip constants to perform them last.  They don't block other moves
    // and skipping such moves with register destinations keeps those
    // registers free for the whole algorithm.
    if (!move.IsEliminated() && !move.GetSource().IsConstant()) {
      PerformMove(i);
    }
  }

  // Perform the moves with constant sources.
  for (size_t i = 0; i < moves_.Size(); ++i) {
    MoveOperands* move = moves_.Get(i);
    if (!move->IsEliminated()) {
      DCHECK(move->GetSource().IsConstant());
      EmitMove(i);
      // Eliminate the move, in case following moves need a scratch register.
      move->Eliminate();
    }
  }

  moves_.Reset();
}


void ParallelMoveResolver::BuildInitialMoveList(HParallelMove* parallel_move) {
  // Perform a linear sweep of the moves to add them to the initial list of
  // moves to perform, ignoring any move that is redundant (the source is
  // the same as the destination, the destination is ignored and
  // unallocated, or the move was already eliminated).
  for (size_t i = 0; i < parallel_move->NumMoves(); ++i) {
    MoveOperands* move = parallel_move->MoveOperandsAt(i);
    if (!move->IsRedundant()) {
      moves_.Add(move);
    }
  }
}

Location LowOf(Location location) {
  if (location.IsRegisterPair()) {
    return Location::RegisterLocation(location.low());
  } else if (location.IsFpuRegisterPair()) {
    return Location::FpuRegisterLocation(location.low());
  } else if (location.IsDoubleStackSlot()) {
    return Location::StackSlot(location.GetStackIndex());
  } else {
    return Location::NoLocation();
  }
}

Location HighOf(Location location) {
  if (location.IsRegisterPair()) {
    return Location::RegisterLocation(location.high());
  } else if (location.IsFpuRegisterPair()) {
    return Location::FpuRegisterLocation(location.high());
  } else if (location.IsDoubleStackSlot()) {
    return Location::StackSlot(location.GetHighStackIndex(4));
  } else {
    return Location::NoLocation();
  }
}

// Update the source of `move`, knowing that `updated_location` has been swapped
// with `new_source`. Note that `updated_location` can be a pair, therefore if
// `move` is non-pair, we need to extract which register to use.
static void UpdateSourceOf(MoveOperands* move, Location updated_location, Location new_source) {
  Location source = move->GetSource();
  if (LowOf(updated_location).Equals(source)) {
    move->SetSource(LowOf(new_source));
  } else if (HighOf(updated_location).Equals(source)) {
    move->SetSource(HighOf(new_source));
  } else {
    DCHECK(updated_location.Equals(source)) << updated_location << " " << source;
    move->SetSource(new_source);
  }
}

MoveOperands* ParallelMoveResolver::PerformMove(size_t index) {
  // Each call to this function performs a move and deletes it from the move
  // graph.  We first recursively perform any move blocking this one.  We
  // mark a move as "pending" on entry to PerformMove in order to detect
  // cycles in the move graph.  We use operand swaps to resolve cycles,
  // which means that a call to PerformMove could change any source operand
  // in the move graph.

  MoveOperands* move = moves_.Get(index);
  DCHECK(!move->IsPending());
  if (move->IsRedundant()) {
    // Because we swap register pairs first, following, un-pending
    // moves may become redundant.
    move->Eliminate();
    return nullptr;
  }

  // Clear this move's destination to indicate a pending move.  The actual
  // destination is saved in a stack-allocated local.  Recursion may allow
  // multiple moves to be pending.
  DCHECK(!move->GetSource().IsInvalid());
  Location destination = move->MarkPending();

  // Perform a depth-first traversal of the move graph to resolve
  // dependencies.  Any unperformed, unpending move with a source the same
  // as this one's destination blocks this one so recursively perform all
  // such moves.
  MoveOperands* required_swap = nullptr;
  for (size_t i = 0; i < moves_.Size(); ++i) {
    const MoveOperands& other_move = *moves_.Get(i);
    if (other_move.Blocks(destination) && !other_move.IsPending()) {
      // Though PerformMove can change any source operand in the move graph,
      // calling `PerformMove` cannot create a blocking move via a swap
      // (this loop does not miss any).
      // For example, assume there is a non-blocking move with source A
      // and this move is blocked on source B and there is a swap of A and
      // B.  Then A and B must be involved in the same cycle (or they would
      // not be swapped).  Since this move's destination is B and there is
      // only a single incoming edge to an operand, this move must also be
      // involved in the same cycle.  In that case, the blocking move will
      // be created but will be "pending" when we return from PerformMove.
      required_swap = PerformMove(i);

      if (required_swap == move) {
        // If this move is required to swap, we do so without looking
        // at the next moves. Swapping is not blocked by anything, it just
        // updates other moves's source.
        break;
      } else if (required_swap == moves_.Get(i)) {
        // If `other_move` was swapped, we iterate again to find a new
        // potential cycle.
        required_swap = nullptr;
        i = 0;
      } else if (required_swap != nullptr) {
        // A move is required to swap. We walk back the cycle to find the
        // move by just returning from this `PerforrmMove`.
        moves_.Get(index)->ClearPending(destination);
        return required_swap;
      }
    }
  }

  // We are about to resolve this move and don't need it marked as
  // pending, so restore its destination.
  move->ClearPending(destination);

  // This move's source may have changed due to swaps to resolve cycles and
  // so it may now be the last move in the cycle.  If so remove it.
  if (move->GetSource().Equals(destination)) {
    move->Eliminate();
    DCHECK(required_swap == nullptr);
    return nullptr;
  }

  // The move may be blocked on a (at most one) pending move, in which case
  // we have a cycle.  Search for such a blocking move and perform a swap to
  // resolve it.
  bool do_swap = false;
  if (required_swap != nullptr) {
    DCHECK_EQ(required_swap, move);
    do_swap = true;
  } else {
    for (size_t i = 0; i < moves_.Size(); ++i) {
      const MoveOperands& other_move = *moves_.Get(i);
      if (other_move.Blocks(destination)) {
        DCHECK(other_move.IsPending());
        if (!move->Is64BitMove() && other_move.Is64BitMove()) {
          // We swap 64bits moves before swapping 32bits moves. Go back from the
          // cycle by returning the move that must be swapped.
          return moves_.Get(i);
        }
        do_swap = true;
        break;
      }
    }
  }

  if (do_swap) {
    EmitSwap(index);
    // Any unperformed (including pending) move with a source of either
    // this move's source or destination needs to have their source
    // changed to reflect the state of affairs after the swap.
    Location source = move->GetSource();
    Location swap_destination = move->GetDestination();
    move->Eliminate();
    for (size_t i = 0; i < moves_.Size(); ++i) {
      const MoveOperands& other_move = *moves_.Get(i);
      if (other_move.Blocks(source)) {
        UpdateSourceOf(moves_.Get(i), source, swap_destination);
      } else if (other_move.Blocks(swap_destination)) {
        UpdateSourceOf(moves_.Get(i), swap_destination, source);
      }
    }
    // If the swap was required because of a 64bits move in the middle of a cycle,
    // we return the swapped move, so that the caller knows it needs to re-iterate
    // its dependency loop.
    return required_swap;
  } else {
    // This move is not blocked.
    EmitMove(index);
    move->Eliminate();
    DCHECK(required_swap == nullptr);
    return nullptr;
  }
}

bool ParallelMoveResolver::IsScratchLocation(Location loc) {
  for (size_t i = 0; i < moves_.Size(); ++i) {
    if (moves_.Get(i)->Blocks(loc)) {
      return false;
    }
  }

  for (size_t i = 0; i < moves_.Size(); ++i) {
    if (moves_.Get(i)->GetDestination().Equals(loc)) {
      return true;
    }
  }

  return false;
}

int ParallelMoveResolver::AllocateScratchRegister(int blocked,
                                                  int register_count,
                                                  int if_scratch,
                                                  bool* spilled) {
  DCHECK_NE(blocked, if_scratch);
  int scratch = -1;
  for (int reg = 0; reg < register_count; ++reg) {
    if ((blocked != reg) && IsScratchLocation(Location::RegisterLocation(reg))) {
      scratch = reg;
      break;
    }
  }

  if (scratch == -1) {
    *spilled = true;
    scratch = if_scratch;
  } else {
    *spilled = false;
  }

  return scratch;
}


int ParallelMoveResolver::AllocateScratchRegister(int blocked,
                                                  int register_count) {
  int scratch = -1;
  for (int reg = 0; reg < register_count; ++reg) {
    if ((blocked != reg) && IsScratchLocation(Location::RegisterLocation(reg))) {
      scratch = reg;
      break;
    }
  }

  return scratch;
}


ParallelMoveResolver::ScratchRegisterScope::ScratchRegisterScope(
    ParallelMoveResolver* resolver, int blocked, int if_scratch, int number_of_registers)
    : resolver_(resolver),
      reg_(kNoRegister),
      spilled_(false) {
  reg_ = resolver_->AllocateScratchRegister(blocked, number_of_registers, if_scratch, &spilled_);

  if (spilled_) {
    resolver->SpillScratch(reg_);
  }
}


ParallelMoveResolver::ScratchRegisterScope::ScratchRegisterScope(
    ParallelMoveResolver* resolver, int blocked, int number_of_registers)
    : resolver_(resolver),
      reg_(kNoRegister),
      spilled_(false) {
  // We don't want to spill a register if none are free.
  reg_ = resolver_->AllocateScratchRegister(blocked, number_of_registers);
}


ParallelMoveResolver::ScratchRegisterScope::~ScratchRegisterScope() {
  if (spilled_) {
    resolver_->RestoreScratch(reg_);
  }
}

}  // namespace art

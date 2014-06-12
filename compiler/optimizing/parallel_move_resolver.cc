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
    const MoveOperands& move = *moves_.Get(i);
    if (!move.IsEliminated()) {
      DCHECK(move.GetSource().IsConstant());
      EmitMove(i);
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


void ParallelMoveResolver::PerformMove(size_t index) {
  // Each call to this function performs a move and deletes it from the move
  // graph.  We first recursively perform any move blocking this one.  We
  // mark a move as "pending" on entry to PerformMove in order to detect
  // cycles in the move graph.  We use operand swaps to resolve cycles,
  // which means that a call to PerformMove could change any source operand
  // in the move graph.

  DCHECK(!moves_.Get(index)->IsPending());
  DCHECK(!moves_.Get(index)->IsRedundant());

  // Clear this move's destination to indicate a pending move.  The actual
  // destination is saved in a stack-allocated local.  Recursion may allow
  // multiple moves to be pending.
  DCHECK(!moves_.Get(index)->GetSource().IsInvalid());
  Location destination = moves_.Get(index)->MarkPending();

  // Perform a depth-first traversal of the move graph to resolve
  // dependencies.  Any unperformed, unpending move with a source the same
  // as this one's destination blocks this one so recursively perform all
  // such moves.
  for (size_t i = 0; i < moves_.Size(); ++i) {
    const MoveOperands& other_move = *moves_.Get(i);
    if (other_move.Blocks(destination) && !other_move.IsPending()) {
      // Though PerformMove can change any source operand in the move graph,
      // this call cannot create a blocking move via a swap (this loop does
      // not miss any).  Assume there is a non-blocking move with source A
      // and this move is blocked on source B and there is a swap of A and
      // B.  Then A and B must be involved in the same cycle (or they would
      // not be swapped).  Since this move's destination is B and there is
      // only a single incoming edge to an operand, this move must also be
      // involved in the same cycle.  In that case, the blocking move will
      // be created but will be "pending" when we return from PerformMove.
      PerformMove(i);
    }
  }
  MoveOperands* move = moves_.Get(index);

  // We are about to resolve this move and don't need it marked as
  // pending, so restore its destination.
  move->ClearPending(destination);

  // This move's source may have changed due to swaps to resolve cycles and
  // so it may now be the last move in the cycle.  If so remove it.
  if (move->GetSource().Equals(destination)) {
    move->Eliminate();
    return;
  }

  // The move may be blocked on a (at most one) pending move, in which case
  // we have a cycle.  Search for such a blocking move and perform a swap to
  // resolve it.
  bool do_swap = false;
  for (size_t i = 0; i < moves_.Size(); ++i) {
    const MoveOperands& other_move = *moves_.Get(i);
    if (other_move.Blocks(destination)) {
      DCHECK(other_move.IsPending());
      do_swap = true;
      break;
    }
  }

  if (do_swap) {
    EmitSwap(index);
    // Any unperformed (including pending) move with a source of either
    // this move's source or destination needs to have their source
    // changed to reflect the state of affairs after the swap.
    Location source = move->GetSource();
    Location destination = move->GetDestination();
    move->Eliminate();
    for (size_t i = 0; i < moves_.Size(); ++i) {
      const MoveOperands& other_move = *moves_.Get(i);
      if (other_move.Blocks(source)) {
        moves_.Get(i)->SetSource(destination);
      } else if (other_move.Blocks(destination)) {
        moves_.Get(i)->SetSource(source);
      }
    }
  } else {
    // This move is not blocked.
    EmitMove(index);
    move->Eliminate();
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
    if ((blocked != reg) &&
        IsScratchLocation(Location::RegisterLocation(ManagedRegister(reg)))) {
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


ParallelMoveResolver::ScratchRegisterScope::~ScratchRegisterScope() {
  if (spilled_) {
    resolver_->RestoreScratch(reg_);
  }
}

}  // namespace art

//===--- DeadCodeElimination.cpp - Promote alloc_box to alloc_stack ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "dead-code-elimination"
#include "swift/Subsystems.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
using namespace swift;

STATISTIC(NumBlocksRemoved, "Number of unreachable basic blocks removed");
STATISTIC(NumInstructionsRemoved, "Number of unreachable instructions removed");

/// \brief Perform a fast local check to see if the instruction is dead.
///
/// This rutine only examines the state of the instruction at hand.
static bool isInstructionTriviallyDead(SILInstruction *I) {
  if (!I->use_empty() || isa<TermInst>(I))
    return false;

  if (!I->mayHaveSideEffects())
    return true;

  return false;
}

/// \brief If the given instruction is dead, delete it along with its dead
/// operands.
///
/// \return Returns true if any instructions were deleted.
static bool recursivelyDeleteTriviallyDeadInstructions(SILInstruction *I) {
  // If the instruction is not dead, there is nothing to do.
  if (!I || !isInstructionTriviallyDead(I))
    return false;

  // Delete this instruction and others that become dead after it's deleted.
  SmallVector<SILInstruction*, 16> DeadInsts;
  DeadInsts.push_back(I);
  do {
    I = DeadInsts.pop_back_val();

    // Check if any of the operands will become dead as well.
    MutableArrayRef<Operand> Ops = I->getAllOperands();
    for (auto OpI = Ops.begin(), OpE = Ops.end(); OpI != OpE; ++OpI) {
      Operand &Op = *OpI;
      SILValue OpVal = Op.get();

      // Remove the reference from the instruction being deleted to this
      // operand.
      Op.drop();

      // If the operand is an instruction that is only used by the instruction
      // being deleted, delete it.
      if (SILInstruction *OpValInst = dyn_cast<SILInstruction>(OpVal))
        if (isInstructionTriviallyDead(OpValInst))
          DeadInsts.push_back(OpValInst);
    }

    // This will remove this instruction and all its uses.
    I->eraseFromParent();
  } while (!DeadInsts.empty());
  
  return true;
}

/// \brief Deletes the instrcutions in the set and any instructions that could
/// become dead after their removal.
///
/// Returns true if more instructions were determined to be dead and deleted.
static bool eraseAndCleanup(const llvm::DenseSet<SILInstruction*> &ToBeDeleted){
  bool AdditionalChanged = false;

  // First, drop references that keep other instructions live.
  llvm::DenseSet<SILInstruction*> PossiblyDead;
  for (auto II = ToBeDeleted.begin(), EI = ToBeDeleted.end(); II != EI; ++II) {

    // Deleting instructions might make their operands dead, let's collect them.
    SILInstruction* DI = *II;
    ArrayRef<Operand> Ops = DI->getAllOperands();
    for (auto OpI = Ops.begin(), OpE = Ops.end(); OpI != OpE; ++OpI) {
      SILInstruction *V = dyn_cast_or_null<SILInstruction>(OpI->get().getDef());
      // If the instruction will be deleted, no need to check if it is dead.
      if (V && !ToBeDeleted.count(V))
        PossiblyDead.insert(V);
    }

    // Drop references for all the instrcutions that will be deleted.
    DI->dropAllReferences();
  }

  // Delete the "possibly dead" instructions if they are dead.
  for (auto II = PossiblyDead.begin(),
       EI = PossiblyDead.end(); II != EI; ++II)
    AdditionalChanged &= recursivelyDeleteTriviallyDeadInstructions(*II);

  // Delete the unreachable instructions.
  for (auto II = ToBeDeleted.begin(),
            EI = ToBeDeleted.end(); II != EI; ++II) {
    (*II)->eraseFromParent();
  }

  return AdditionalChanged;
}

/// \brief Deletes the instrcution and any instructions that could become dead
/// after its removal.
static bool eraseAndCleanup(SILInstruction *I) {
  llvm::DenseSet<SILInstruction*> Set;
  Set.insert(I);
  return eraseAndCleanup(Set);
}

static bool constantFoldTerminator(SILBasicBlock &BB) {
  TermInst *TI = BB.getTerminator();

  // Process conditional branches with constant conditions.
  if (CondBranchInst *CBI = dyn_cast<CondBranchInst>(TI)) {
    SILValue V = CBI->getCondition();
    SILInstruction *CondI = dyn_cast<SILInstruction>(V.getDef());

    if (IntegerLiteralInst *ConstCond =
          dyn_cast_or_null<IntegerLiteralInst>(CondI)) {
      SILBuilder B(&BB);

      // Determine which of the successors is unreachable and create a new
      // terminator that only branches to the reachable sucessor.
      BranchInst *BI = 0;
      if (ConstCond->getValue() == APInt(1, /*value*/ 0, false)) {
        BI = B.createBranch(CBI->getLoc(),
                            CBI->getFalseBB(), CBI->getFalseArgs());
      } else {
        assert(ConstCond->getValue() == APInt(1, /*value*/ 1, false) &&
               "Our representation of true/false does not match.");
        BI = B.createBranch(CBI->getLoc(),
                            CBI->getTrueBB(), CBI->getTrueArgs());
      }

      // TODO: Produce an unreachable code warning here if the basic block
      // contains user code.

      eraseAndCleanup(CBI);

      return true;
    }
  }
  return false;
}

static bool isCallToNoReturn(const ApplyInst *AI, SILBasicBlock &BB) {
  SILType Ty = AI->getCallee().getType();
  SILModule *M = BB.getParent()->getParent();
  CanType CalleeTy = Ty.getFunctionTypeInfo(*M)->getSwiftType();
  if (const AnyFunctionType *FT = CalleeTy->getAs<FunctionType>())
    return FT->isNoReturn();

  assert(false);
  return false;
}

static bool simplifyBlocksWithCallsToNoReturn(SILBasicBlock &BB) {
  auto I = BB.begin(), E = BB.end();
  bool FoundNoReturnCall = false;

  // Collection of all instructions that should be deleted.
  llvm::DenseSet<SILInstruction*> ToBeDeleted;

  // Does this block conatin a call to a noreturn function?
  while (I != E) {
    auto CurrentInst = I;
    // Move the iterator before we remove instructions to avoid iterator
    // invalidation issues.
    ++I;

    // Remove all instructions following the noreturn call.
    if (FoundNoReturnCall) {

      // We will need to delete the instruction later on.
      ToBeDeleted.insert(CurrentInst);

      NumInstructionsRemoved++;
      continue;
    }

    if (ApplyInst *AI = dyn_cast<ApplyInst>(CurrentInst)) {
      if (isCallToNoReturn(AI, BB)) {
        FoundNoReturnCall = true;
        // FIXME: Diagnose unreachable code if the call is followed by anything
        // but implicit return.
      }
    }
  }

  if (!FoundNoReturnCall)
    return false;

  eraseAndCleanup(ToBeDeleted);

  // Add an unreachable terminator. The terminator has an invalid source
  // location to signal to the DataflowDiagnostic pass that this code does
  // not correspond to user code.
  SILBuilder B(&BB);
  B.createUnreachable(SILLocation());

  return true;
}

static bool removeUnreachableBlocks(SILFunction &F) {
  if (F.empty())
    return false;

  llvm::SmallPtrSet<SILBasicBlock*, 16> Reachable;
  SmallVector<SILBasicBlock*, 128> Worklist;
  Worklist.push_back(&F.front());
  Reachable.insert(&F.front());

  // Collect all reachable blocks by walking the successors.
  do {
    SILBasicBlock *BB = Worklist.pop_back_val();
    for (auto SI = BB->succ_begin(), SE = BB->succ_end(); SI != SE; ++SI) {
      if (Reachable.insert(*SI))
        Worklist.push_back(*SI);
    }
  } while (!Worklist.empty());
  assert(Reachable.size() <= F.size());

  // If everything is reachable, we are done.
  if (Reachable.size() == F.size())
    return false;

  // Remove references from the dead blocks.
  for (auto I = F.begin(), E = F.end(); I != E; ++I) {
    SILBasicBlock *BB = I;
    if (Reachable.count(BB))
      continue;

    // Drop references to other blocks.
    eraseAndCleanup(BB->getTerminator());
  }

  // Delete dead instrcutions and everything that could become dead after
  // their deletion.
  llvm::DenseSet<SILInstruction*> ToBeDeleted;
  for (auto BI = F.begin(), BE = F.end(); BI != BE; ++BI)
    if (!Reachable.count(BI))
      for (auto I = BI->begin(), E = BI->end(); I != E; ++I)
        ToBeDeleted.insert(&*I);
  eraseAndCleanup(ToBeDeleted);

  // Delete the dead blocks.
  for (auto I = F.begin(), E = F.end(); I != E;)
    if (!Reachable.count(I)) {
      I = F.getBlocks().erase(I);
      NumBlocksRemoved++;
    } else
      ++I;

  return true;
}

//===----------------------------------------------------------------------===//
//                          Top Level Driver
//===----------------------------------------------------------------------===//

void swift::performSILDeadCodeElimination(SILModule *M) {
  for (auto &Fn : *M) {
    for (auto &BB : Fn) {
      // Simplify the blocks with terminators that rely on constant conditions.
      if (constantFoldTerminator(BB))
        continue;

      // Remove instructions from the basic block after a call to a noreturn
      // function.
      if (simplifyBlocksWithCallsToNoReturn(BB))
        continue;
    }

    // Remove unreachable blocks.
    removeUnreachableBlocks(Fn);
  }
}

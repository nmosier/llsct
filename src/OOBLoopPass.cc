#include <vector>
#include <set>

#include <llvm/Pass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

#include "util.h"
#include "Transmitter.h"
#include "Mitigation.h"

namespace clou {
  namespace {

    struct OOBLoopPass final : public llvm::FunctionPass {
      static inline char ID = 0;
      OOBLoopPass(): llvm::FunctionPass(ID) {}

      /// Loops that are Spectre-v1.1-safe according to our analysis and only require one fence at all exits.      
      std::set<llvm::Loop *> loops;

      static bool ignoreCall(const llvm::CallBase *C) {
	if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(C)) {
	  if (llvm::isa<MitigationInst, llvm::DbgInfoIntrinsic>(II) || II->isAssumeLikeIntrinsic()) {
	    return true;
	  } else {
	    switch (II->getIntrinsicID()) {
	    case llvm::Intrinsic::fshr:
	    case llvm::Intrinsic::fshl:
	      return true;
	    default:
	      warn_unhandled_intrinsic(II);
	      return false;
	    }
	  }
	} else {
	  return false;
	}
      }

      static bool isDedicatedExit(const llvm::Loop *L, const llvm::BasicBlock *exit) {
	return std::all_of(llvm::pred_begin(exit),
			   llvm::pred_end(exit),
			   [L] (const llvm::BasicBlock *pred) {
			     return L->contains(pred);
			   });
      }

      /// Check if we can avoid mitigating Spectre v1.1 in this loop.
      static bool checkLoop(const llvm::Loop& L) {
	// We can't handle any calls in the loop, since OOB stores will violate the callee's assumptions.
	for (const llvm::BasicBlock *B : L.blocks()) {
	  for (const llvm::Instruction& I : *B) {
	    if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
	      if (!ignoreCall(CB)) {
		return false;
	      }
	    }
	  }
	}

	// TODO: Might need to add new assumptions here.
	
	return true;
      }

      bool propagateTaint(const llvm::Loop& L) {
	std::set<llvm::Instruction *> taints, taints_bak; /*!< Tainted registers */
	// NOTE: Don't need to track tainted memory, since we're already assuming that all loads are tainted.

	// Speculative taint propogation, assuming all loads return secrets
	do {
	  taints_bak = taints;

	  for (llvm::BasicBlock *B : L.blocks()) {
	    for (llvm::Instruction& I : *B) {
	      if (I.getType()->isVoidTy()) {
		// ignore
	      } else if (llvm::LoadInst *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
		/* We assume that all loads are returning secrets, since we're checking if we can avoid mitigating Spectre v1.1
		 * in this loop. */
		taints.insert(LI);
	      } else {
		/* We purposefully assume that all values defined outside the loop are public. Consider two cases:
		 * (i)  Nonspeculative secret: Any of these will never be leaked anyways, by our CT-Programming assumption.
		 * (ii) Speculative secret: We will eliminate these by inserting a fence before the loop (as well as after the loop).
		 */
		const bool tainted = std::any_of(I.op_begin(), I.op_end(), [&] (llvm::Value *V) -> bool {
		  if (llvm::Instruction *I = llvm::dyn_cast<llvm::Instruction>(V)) {
		    return taints.contains(I);
		  } else {
		    return false;
		  }
		});
		if (tainted) {
		  taints.insert(&I);
		}
	      }
	    }
	  }
	} while (taints != taints_bak);

	// Check if any speculative loop secrets are leaked via transmitters
	for (llvm::BasicBlock *B : L.blocks()) {
	  for (llvm::Instruction& I : *B) {
	    for (const TransmitterOperand& op : get_transmitter_sensitive_operands(&I)) {
	      if (llvm::Instruction *op_I = llvm::dyn_cast<llvm::Instruction>(op.V)) {
		if (!(llvm::isa<llvm::StoreInst>(&I) && op.kind == TransmitterOperand::PSEUDO)) {
		  if (taints.contains(op_I)) {
		    // A speculative secret is leaked, so we need to mitigate Spectre v1.1 in this loop.
#if 1
		    llvm::errs() << getPassName() << ": " << I.getFunction()->getName() << ": speculative secret is leaked:\n"
				 << "Transmitter: " << I << "\n"
				 << "Transmitted: " << *op_I << "\n";
#endif
		    return false;
		  }
		}
	      }
	    }
	  }
	}

	// We don't need to mitigate Spectre v1.1 in this loop.
	// A mitgiation pass using this analysis should insert lfences before and after this loop.
	return true;
      }

      bool runOnLoop(llvm::Loop *L, llvm::DominatorTree *DT, llvm::LoopInfo *LI) {
	if (checkLoop(*L) && propagateTaint(*L)) {
	  llvm::LLVMContext& ctx = L->getBlocks().front()->getContext();
	  llvm::MDNode *extra = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "clou.loop.secure")});
	  llvm::MDNode *newLoopID = llvm::MDNode::concatenate(L->getLoopID(), extra);
	  newLoopID->replaceOperandWith(0, newLoopID);
	  L->setLoopID(newLoopID);

	  // Mark all potential transmitter operands as "nospill".
	  for (llvm::BasicBlock *B : L->blocks()) {
	    for (llvm::Instruction& I : *B) {
	      for (const TransmitterOperand& transop : get_transmitter_sensitive_operands(&I)) {
		if (llvm::Instruction *transop_I = llvm::dyn_cast<llvm::Instruction>(transop.V)) {
		  if (L->contains(transop_I)) {
		    transop_I->setMetadata("clou.nospill", llvm::MDNode::get(ctx, {}));
		  }
		}
	      }
	    }
	  }

	  // Add loop preheader, if necessary
	  if (!L->getLoopPreheader()) {
	    llvm::InsertPreheaderForLoop(L, DT, LI, nullptr, false);
	  }

	  // Create dedicated exit block, if necessary
	  llvm::formDedicatedExitBlocks(L, DT, LI, nullptr, false);
	  
	  // Fence before and after loop
	  CreateMitigation(L->getLoopPreheader()->getTerminator(), "loop-entry");
	  llvm::SmallVector<llvm::BasicBlock *, 4> exits;
	  L->getExitBlocks(exits);
	  for (llvm::BasicBlock *exit : exits) {
	    CreateMitigation(&exit->front(), "loop-exit");
	  }

	  return true;
	} else {
	  return false;
	}
      }

      bool runOnLoopRec(llvm::Loop *L, llvm::DominatorTree *DT, llvm::LoopInfo *LI) {
	if (runOnLoop(L, DT, LI)) {
	  return true;
	} else {
	  bool changed = false;
	  for (llvm::Loop *L : L->getSubLoops()) {
	    changed |= runOnLoopRec(L, DT, LI);
	  }
	  return changed;
	}
      }

      bool runOnFunction(llvm::Function& F) override {
	llvm::DominatorTree DT(F);
	llvm::LoopInfo LI(DT);
	bool changed = false;
	for (llvm::Loop *L : LI) {
	  changed |= runOnLoopRec(L, &DT, &LI);
	}
	return changed;
      }

    };
    
  }

  llvm::RegisterPass<OOBLoopPass> X {"oob-loop-pass", "Spectre v1.1 Optimization Pass"};
  util::RegisterClangPass<OOBLoopPass> Y;
  
}

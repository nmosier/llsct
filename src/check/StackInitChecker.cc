#include <queue>
#include <set>


#include <llvm/Pass.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Support/WithColor.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Analysis/PostDominators.h>

#include "clou/util.h"

namespace clou {
  namespace {

    struct StackInitChecker final : public llvm::FunctionPass {
      static inline char ID = 0;
      StackInitChecker(): llvm::FunctionPass(ID) {}

      void getAnalysisUsage(llvm::AnalysisUsage& AU) const override {
	AU.addRequired<llvm::AAResultsWrapperPass>();
	AU.setPreservesAll();
      }
      
      static const llvm::DbgVariableIntrinsic *getAllocaDebug(llvm::AllocaInst& AI) {
	llvm::Function *F = AI.getFunction();
	for (auto& DVI : util::instructions<llvm::DbgVariableIntrinsic>(*F)) {
	  if (llvm::Value *V = DVI.getVariableLocationOp(0)) {
	    if (V == &AI) {
	      return &DVI;
	    }
	  }
	}
	return nullptr;
      }

      static bool isAllocaInit(const llvm::AllocaInst& AI, const llvm::Instruction *I, llvm::AliasAnalysis& AA, llvm::DataLayout& DL) {
	if (!I->mayWriteToMemory()) {
	  return false;
	}
	if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(I)) {
	  return AA.isMustAlias(&AI, SI->getPointerOperand());
	}
	if (const auto *MI = llvm::dyn_cast<llvm::MemIntrinsic>(I)) {
	  if (AA.isMustAlias(MI->getDest(), &AI)) {
	    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(MI->getLength())) {
	      if (C->getValue().getLimitedValue() >= DL.getTypeStoreSize(AI.getAllocatedType()).getFixedSize()) {
		return true;
	      }
	    }
	  }
	  return false;
	}
	if (llvm::isa<llvm::CallBase>(I)) {
	  // TODO: Might want to do inter-procedural analysis to see which functions *must* initialize their arguments.
	  return false;
	}
	unhandled_instruction(*I);
      }

      static bool isConstantRead(const llvm::AllocaInst& AI, const llvm::Instruction *I, llvm::AliasAnalysis& AA, llvm::DataLayout& DL) {
	llvm::LocationSize locsize = DL.getTypeStoreSize(AI.getAllocatedType()).getFixedSize();
	const auto modref = AA.getModRefInfo(I, &AI, locsize);
	if (modref == llvm::ModRefInfo::Ref || modref == llvm::ModRefInfo::ModRef) {
	  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(I)) {
	    return !has_incoming_addr(LI->getPointerOperand());
	  }
	  if (const auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(I)) {
	    return true;
	  }
	  if (const auto *C = llvm::dyn_cast<llvm::CallInst>(I)) {
	    return false;
	  }
	  unhandled_instruction(*I);
	} else {
	  return false;
	}
      }

      bool runOnFunction(llvm::Function& F) override {
	llvm::AliasAnalysis& AA = getAnalysis<llvm::AAResultsWrapperPass>().getAAResults();
	llvm::DataLayout DataLayout(F.getParent());
	llvm::PostDominatorTree PDT (F);

	for (llvm::AllocaInst& AI : util::instructions<llvm::AllocaInst>(F)) {
	  // Try to find use before def.
	  std::queue<llvm::Instruction *> todo;
	  std::set<llvm::Instruction *> seen;
	  todo.push(&F.getEntryBlock().front());
	  while (!todo.empty()) {
	    llvm::Instruction *I = todo.front();
	    todo.pop();
	    if (seen.insert(I).second) {
	      if (isConstantRead(AI, I, AA, DataLayout) && !PDT.dominates(I, &AI)) {
		const auto *dbg = getAllocaDebug(AI);
		if (dbg == nullptr) {
		  llvm::errs() << *AI.getParent() << "\n";
		}
		llvm::raw_ostream& os = llvm::errs();
		{
		  std::string prefix;
		  {
		    llvm::raw_string_ostream os(prefix);
		    llvm::DebugLoc DL;
		    if (dbg) {
		      DL = dbg->getDebugLoc();
		    }
		    if (DL) {
		      DL.print(os);
		    } else {
		      os << "?:?:?";
		    }
		  }
		  llvm::WithColor::error(os, prefix);
		  os << "stack allocation may not be defined before use along all control-flow paths";
		  if (dbg) {
		    os << " for variable '" << dbg->getVariable()->getName() << "'";
		  }
		  os << "\n";
		}
		if (const auto& DL = I->getDebugLoc()) {
		  llvm::WithColor::note(os) << "use: ";
		  DL.print(os);
		  os << "\n";
		}
	      }
	      
	      // Check if current instruction is an initialization
	      if (isAllocaInit(AI, I, AA, DataLayout)) {
		continue;
	      }

	      for (auto *succ : llvm::successors_inst(I)) {
		todo.push(succ);
	      }
	    }
	  }
	
	}

	return false;
      }

      void print(llvm::raw_ostream& os, const llvm::Module *) const override {
	(void) os;
	std::abort();
      }
    };

    llvm::RegisterPass<StackInitChecker> X {"clou-check-stack-init", "Clou's Stack Initialization Checker"};
    util::RegisterClangPass<StackInitChecker> Y {
      llvm::PassManagerBuilder::EP_EarlyAsPossible,
      llvm::PassManagerBuilder::EP_EnabledOnOptLevel0,
    };

  }
}

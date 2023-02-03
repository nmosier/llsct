#pragma once

#include <set>
#include <map>

#include <llvm/Pass.h>
#include <llvm/Analysis/AliasAnalysis.h>

namespace clou {

  class SpeculativeTaint final : public llvm::FunctionPass {
  public:
    static char ID;
    SpeculativeTaint();

    using TaintMap = std::map<llvm::Instruction *, std::set<llvm::Instruction *>>;

    TaintMap taints;

    void getAnalysisUsage(llvm::AnalysisUsage& AU) const override;
    bool runOnFunction(llvm::Function& F) override;
    void print(llvm::raw_ostream& os, const llvm::Module *M) const override;

    bool secret(llvm::Value *V);
  };
  
}

#pragma once

#include <set>
#include <map>

#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "my_scc_pass.h"

namespace clou {

class NonspeculativeTaint final: public MySCCPass {
public:
    static char ID;
    NonspeculativeTaint();
    
private:
    using ValueSet = std::set<llvm::Value *>;
    ValueSet ret_vals;
    ValueSet pub_vals;
    
    virtual void getAnalysisUsage(llvm::AnalysisUsage& AU) const override;
    
    virtual bool runOnSCC(const MySCC& SCC) override;
    
    virtual void print(llvm::raw_ostream& os, const llvm::Module *M) const override;
    
public:
    bool secret(llvm::Value *V) const;
};

}
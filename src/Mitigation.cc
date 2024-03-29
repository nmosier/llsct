#include "clou/Mitigation.h"

#include <cassert>

#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Clou/Clou.h>

#include "clou/Metadata.h"

namespace clou {

  bool MitigationInst::classof(const llvm::IntrinsicInst *I) {
#if 0
    return I->getIntrinsicID() == llvm::Intrinsic::x86_sse2_lfence && md::getMetadataFlag(I, mitigation_flag);
#else
    return I->getIntrinsicID() == llvm::Intrinsic::x86_sse2_lfence;
    // FIXME: find out why commented out code doesn't work.
#endif
  }

  bool MitigationInst::classof(const llvm::Value *V) {
    if (const auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(V)) {
      return classof(II);
    } else {
      return false;
    }
  }

#if 0
  void MitigationInst::setIdentifier(uint64_t id) {
    auto *CI = llvm::ConstantInt::get(llvm::Type::getInt64Ty(getContext()), id);
    auto *CAM = llvm::ConstantAsMetadata::get(CI);
    getMetadata(mitigation_flag)->getOperand(0)->
    llvm::cast<llvm::MDTuple>(getMetadata(mitigation_flag))->setOperand(0, CAM);
  }
#endif

  llvm::ConstantInt *MitigationInst::getIdentifier() const {
    return llvm::cast<llvm::ConstantInt>(llvm::cast<llvm::ConstantAsMetadata>(getMetadata(mitigation_flag)->getOperand(0))->getValue());
  }

  llvm::StringRef MitigationInst::getDescription() const {
    return llvm::cast<llvm::MDString>(getMetadata(mitigation_flag)->getOperand(1))->getString();
  }

  MitigationInst *CreateMitigation(llvm::IRBuilder<>& IRB, const char *lfencestr) {
    llvm::CallInst *I = IRB.CreateIntrinsic(llvm::Intrinsic::x86_sse2_lfence, {}, {});
    auto *invalid_id = llvm::ConstantInt::getSigned(IRB.getInt64Ty(), -1);
    auto *desc = llvm::MDString::get(IRB.getContext(), lfencestr);
    llvm::MDNode *MDN = llvm::MDNode::get(IRB.getContext(), {llvm::ConstantAsMetadata::get(invalid_id), desc});
    I->setMetadata(MitigationInst::mitigation_flag, MDN);
    assert(llvm::isa<MitigationInst>(I));

    if (clou::InsertTrapAfterMitigations)
      IRB.CreateIntrinsic(llvm::Intrinsic::x86_sse2_mfence, {}, {});
    
    return llvm::cast<MitigationInst>(I);
  }

  MitigationInst *CreateMitigation(llvm::Instruction *I, const char *lfencestr) {
    while (llvm::isa<llvm::PHINode>(I)) {
      I = I->getNextNode();
    }
    llvm::IRBuilder<> IRB (I);
    IRB.SetCurrentDebugLocation(I->getDebugLoc());
    return CreateMitigation(IRB, lfencestr);
  }


}

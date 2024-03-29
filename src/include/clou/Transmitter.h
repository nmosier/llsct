#pragma once

#include <set>
#include <tuple>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>

#include "util.h"

namespace clou {

  struct TransmitterOperand {
    enum Kind {
      TRUE,
      PSEUDO,
    } kind;
    llvm::Value *V;
    
    TransmitterOperand(Kind kind, llvm::Value *V): kind(kind), V(V) {}

    auto tuple() const {
      return std::make_tuple(kind, V);
    }

    bool operator<(const TransmitterOperand& o) const {
      return tuple() < o.tuple();
    }
    
    llvm::Instruction *I() const {
      return llvm::dyn_cast_or_null<llvm::Instruction>(V);
    }
  };

  bool callDoesNotTransmit(const llvm::CallBase *C);

  template <class OutputIt>
  OutputIt get_transmitter_sensitive_operands(llvm::Instruction *I, OutputIt out) {
    if (I->getNumOperands() == 0) {
      // ignore
    } else if (llvm::isa<llvm::LoadInst, llvm::StoreInst, llvm::AtomicCmpXchgInst, llvm::AtomicRMWInst>(I)) {
      *out++ = TransmitterOperand(TransmitterOperand::TRUE, util::getPointerOperand(I));
    } else if (llvm::isa<llvm::BranchInst, llvm::SwitchInst>(I)) {
      if (llvm::Value *cond = util::getConditionOperand(I))
	*out++ = TransmitterOperand(TransmitterOperand::TRUE, cond);
    } else if (llvm::CallBase *C = llvm::dyn_cast<llvm::CallBase>(I)) {
      if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(C)) {
	if (!II->isAssumeLikeIntrinsic() && !II->getType()->isVoidTy() && II->arg_size() > 0
	    && II->getIntrinsicID() != llvm::Intrinsic::annotation
	    ) {
	  std::vector<unsigned> none;
	  std::vector<unsigned> all;
	  for (unsigned i = 0; i < II->arg_size(); ++i) {
	    all.push_back(i);
	  }
	  std::vector<unsigned> leaked_args;
	  
	  switch (II->getIntrinsicID()) {
	  case llvm::Intrinsic::memset:
	    leaked_args = {0, 2, 3};
	    break;
	  case llvm::Intrinsic::memcpy:
	    leaked_args = all;
	    break;
	  case llvm::Intrinsic::experimental_constrained_fdiv:
	    leaked_args = all;
	    break;
	  case llvm::Intrinsic::masked_load:
	    leaked_args = {0, 1};
	    break;
	  case llvm::Intrinsic::masked_gather:
	    leaked_args = {0, 1};
	    break;
	  case llvm::Intrinsic::eh_typeid_for:
	    leaked_args = {0};
	    break;
	  case llvm::Intrinsic::vector_reduce_add:
	  case llvm::Intrinsic::vector_reduce_and:
	  case llvm::Intrinsic::vector_reduce_or:
	  case llvm::Intrinsic::fshl:
	  case llvm::Intrinsic::ctpop:
	  case llvm::Intrinsic::x86_aesni_aeskeygenassist:
	  case llvm::Intrinsic::x86_aesni_aesenc:
	  case llvm::Intrinsic::x86_aesni_aesenclast:
	  case llvm::Intrinsic::bswap:
	  case llvm::Intrinsic::x86_pclmulqdq:
	  case llvm::Intrinsic::umin:
	  case llvm::Intrinsic::umax:
	  case llvm::Intrinsic::smax:
	  case llvm::Intrinsic::smin:
	  case llvm::Intrinsic::abs:
	  case llvm::Intrinsic::umul_with_overflow:
	  case llvm::Intrinsic::bitreverse:
	  case llvm::Intrinsic::cttz:
	  case llvm::Intrinsic::usub_sat:
	  case llvm::Intrinsic::fmuladd:
	  case llvm::Intrinsic::fabs:
	  case llvm::Intrinsic::ctlz:
	  case llvm::Intrinsic::experimental_constrained_fcmp:
	  case llvm::Intrinsic::experimental_constrained_fsub:
	  case llvm::Intrinsic::experimental_constrained_fmul:
	  case llvm::Intrinsic::experimental_constrained_sitofp:
	  case llvm::Intrinsic::experimental_constrained_uitofp:
	  case llvm::Intrinsic::experimental_constrained_fptoui:
	  case llvm::Intrinsic::experimental_constrained_fcmps:
	  case llvm::Intrinsic::experimental_constrained_fadd:	
	  case llvm::Intrinsic::experimental_constrained_fptosi:
	  case llvm::Intrinsic::experimental_constrained_fpext:
	  case llvm::Intrinsic::experimental_constrained_floor:
	  case llvm::Intrinsic::experimental_constrained_ceil:
	  case llvm::Intrinsic::experimental_constrained_fptrunc:
	  case llvm::Intrinsic::experimental_constrained_fmuladd:
	  case llvm::Intrinsic::fshr:
	  case llvm::Intrinsic::vector_reduce_mul:
	  case llvm::Intrinsic::vector_reduce_umax:	    
	  case llvm::Intrinsic::vector_reduce_umin:
	  case llvm::Intrinsic::vector_reduce_smax:	    
	  case llvm::Intrinsic::vector_reduce_smin:
	  case llvm::Intrinsic::vector_reduce_xor:
	  case llvm::Intrinsic::uadd_with_overflow:
	  case llvm::Intrinsic::experimental_constrained_powi:
	  case llvm::Intrinsic::experimental_constrained_trunc:
	  case llvm::Intrinsic::experimental_constrained_round:
	  case llvm::Intrinsic::uadd_sat:
	    leaked_args = none;
	    break;
	  default:
	    warn_unhandled_intrinsic(II);
	  }
	  
	  for (unsigned leaked_arg : leaked_args)
	    *out++ = TransmitterOperand(TransmitterOperand::PSEUDO, II->getArgOperand(leaked_arg));
	}
	
      } else {
	*out++ = TransmitterOperand(TransmitterOperand::TRUE, C->getCalledOperand());
	for (llvm::Value *op : C->args())
	  *out++ = TransmitterOperand(TransmitterOperand::PSEUDO, op);
      }
    } else if (llvm::ReturnInst *RI = llvm::dyn_cast<llvm::ReturnInst>(I)) {
      if (llvm::Value *RV = RI->getReturnValue())
	*out++ = TransmitterOperand(TransmitterOperand::PSEUDO, RV);
    } else if (llvm::ResumeInst *RI = llvm::dyn_cast<llvm::ResumeInst>(I)) {
      if (llvm::Value *RV = RI->getValue())
	*out++ = TransmitterOperand(TransmitterOperand::PSEUDO, RV);
    } else if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(I)) {
      switch (BO->getOpcode()) {
      case llvm::BinaryOperator::BinaryOps::UDiv:
      case llvm::BinaryOperator::BinaryOps::SDiv:
	for (llvm::Value *op : BO->operands())
	  *out++ = TransmitterOperand(TransmitterOperand::TRUE, op);
	break;
      default: break;
      }
    } else if (llvm::isa<llvm::CmpInst, llvm::CastInst, llvm::PHINode, llvm::AllocaInst,
	       llvm::GetElementPtrInst, llvm::ShuffleVectorInst, llvm::InsertElementInst, llvm::SelectInst,
	       llvm::ExtractElementInst, llvm::ExtractValueInst, llvm::FreezeInst, llvm::UnaryOperator,
	       llvm::LandingPadInst, llvm::InsertValueInst>(I)) {
      // no leaked operands
    } else {
      unhandled_instruction(*I);
    }
    
    return out;
  }

  std::set<TransmitterOperand> get_transmitter_sensitive_operands(llvm::Instruction *I);

}

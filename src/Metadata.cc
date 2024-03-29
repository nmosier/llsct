#include "clou/Metadata.h"

namespace clou::md {

  void setMetadataFlag(llvm::Instruction *I, llvm::StringRef flag) {
    I->setMetadata(flag, llvm::MDNode::get(I->getContext(), {}));
  }
  
  bool getMetadataFlag(const llvm::Instruction *I, llvm::StringRef flag) {
    return I->getMetadata(flag) != nullptr;
  }
  
}

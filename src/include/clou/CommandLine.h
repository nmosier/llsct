#pragma once

#include <string>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

namespace clou {

  extern int verbose;

  bool enable_tests(void);
  llvm::raw_ostream& tests(void);
  
}


#include <ctime>

#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>
#include <variant>
#include <iomanip>
#include <csignal>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Clou/Clou.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>

#include <err.h>

#include "clou/MinCutSMT.h"
#include "clou/MinCutGreedy.h"
#include "clou/util.h"
#include "clou/Transmitter.h"
#include "clou/CommandLine.h"
#include "clou/Mitigation.h"
#include "clou/Log.h"
#include "clou/analysis/NonspeculativeTaintAnalysis.h"
#include "clou/analysis/SpeculativeTaintAnalysis.h"
#include "clou/analysis/LeakAnalysis.h"
#include "clou/Stat.h"
#include "clou/containers.h"
#include "clou/CFG.h"

namespace clou {
  namespace {

    extern "C" void ProfilerStart(const char *);
    extern "C" void ProfilerStop(void);
    __attribute__((constructor)) static void profile_start(void) {
      ProfilerStart("tmp.prof");
    }

    __attribute__((destructor)) static void profile_stop(void) {
      ProfilerStop();
    }
    

    llvm::cl::opt<bool> log_times {
      "clou-times",
      llvm::cl::desc("Log execution times of Mitigate Pass"),
    };

    static void handle_timeout(int sig) {
      (void) sig;
      assert(sig == SIGALRM);
      dprintf(STDERR_FILENO, "COMPILATION TIME EXCEEDED\n");
      std::exit(EXIT_FAILURE);
    }

    llvm::cl::opt<unsigned> Timeout {
      "clou-mitigation-pass-timeout",
      llvm::cl::Hidden,
      llvm::cl::init(0),
      llvm::cl::callback([] (const unsigned& t) {
	if (t > 0) {
	  signal(SIGALRM, handle_timeout);
	  alarm(t);
	}
      }),
    };

    void print_debugloc(llvm::raw_ostream& os, const llvm::Value *V) {
      if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
	if (const auto& DL = I->getDebugLoc()) {
	  DL.print(os);
	  return;
	}
      }
      os << "(none)";
    }

    std::string str_debugloc(const llvm::Value *V) {
      std::string s;
      llvm::raw_string_ostream ss(s);
      print_debugloc(ss, V);
      return s;
    }

    struct Node {
      llvm::Value *V;
      Node() {}
      Node(llvm::Instruction *V): V(V) {}
      auto operator<=>(const Node&) const = default;
    };

    struct NodeHash {
      auto operator()(const Node& n) const {
	return llvm::hash_value(n.V);
      }
    };

    llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Node& node) {
      return os << *node.V;
    }
    std::ostream& operator<<(std::ostream& os_, const Node& node) {
      llvm::raw_os_ostream os(os_);
      os << node;
      return os_;
    }

    using Alg = MinCutGreedy<Node>;
    using Edge = Alg::Edge;
    using ST = Alg::ST;
    
    static void checkCutST(llvm::ArrayRef<std::set<Node>> st, const std::set<Edge>& cutset, llvm::Function& F) {
      assert(st.size() >= 2);
      const auto succs = [&cutset] (const Node& u) {
	std::set<Node> succs;
	for (llvm::Instruction *I : llvm::successors_inst(llvm::cast<llvm::Instruction>(u.V))) {
	  const Node v(I);
	  const Edge e = {.src = u, .dst = v};
	  if (!cutset.contains(e))
	    succs.insert(v);
	}
	return succs;
      };
      std::vector<std::set<Node>> particles;
      std::vector<std::map<Node, Node>> parents;
      std::set<Node> S = st.front();
      for (const std::set<Node>& T : st.drop_front()) {
	auto& parent = parents.emplace_back();
	particles.push_back(S);
	
	// Find all nodes reachable from S.
	std::set<Node> reach;
	std::stack<Node> todo;

	// Add all the successors of sources but not sources themselves in order to be able to capture cases where s = t.
	for (const Node& u : S)
	  todo.push(u);

	while (!todo.empty()) {
	  const Node u = todo.top();
	  todo.pop();
	  for (const Node v : succs(u)) {
	    if (reach.insert(v).second) {
	      todo.push(v);
	      parent[v] = u;
	    }
	  }
	}

	S.clear();
	std::set_intersection(T.begin(), T.end(), reach.begin(), reach.end(), std::inserter(S, S.end()));
      }
      particles.push_back(S);

      if (!S.empty()) {
	llvm::WithColor::error() << "found s-t path!\n";

	// Try to find path.
	std::vector<Node> path;
	Node t = *particles.back().begin();
	for (const auto& [parent, S] : llvm::reverse(llvm::zip(parents, llvm::ArrayRef(particles).drop_back()))) {
	  Node v = t;
	  while (true) {
	    path.push_back(v);
	    v = parent.at(v);
	    if (S.contains(v))
	      break;
	  }
	  t = v;
	}
	path.push_back(t);

	/*
	 * 1, 2, 3, ... -- indicates index into st group.
	 * s -- source of cut
	 * d -- destination of cut
	 * * -- along path.
	 */

	auto& os = llvm::errs();
	for (llvm::BasicBlock& B : F) {
	  B.printAsOperand(os);
	  os << ":\n";
	  for (llvm::Instruction& I : B) {
	    for (char c = '0'; const std::set<Node>& group : st) {
	      os << (group.contains(Node(&I)) ? c : ' ');
	      ++c;
	    }
	    {
	      const auto it = llvm::find(path, Node(&I));
	      os << (it != path.end() ? '*' : ' ');
	    }
	    {
	      const auto it = llvm::find_if(cutset, [&I] (const auto& p) {
		return p.src == Node(&I);
	      });
	      os << (it != cutset.end() ? 's' : ' ');
	    }
	    {
	      const auto it = llvm::find_if(cutset, [&I] (const auto& p) {
		return p.dst == Node(&I);
	      });
	      os << (it != cutset.end() ? 'd' : ' ');
	    }
	    os << "  " << I << "\n";
	  }
	  os << "\n";
	}
	
	llvm::errs() << "Path:\n";
	for (const Node& v : llvm::reverse(path)) {
	  llvm::errs() << "    " << *v.V << "\n";
	}
	
	std::abort();
      }
    }
    
    static void checkCut(llvm::ArrayRef<ST> sts, const std::set<Edge>& cutset, llvm::Function& F) {
      for (const ST& st : sts)
	checkCutST(st.waypoints, cutset, F);
    }

    struct MitigatePass final : public llvm::FunctionPass {
      static inline char ID = 0;
    
      MitigatePass() : llvm::FunctionPass(ID) {}

      void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
	AU.addRequired<NonspeculativeTaint>();
	AU.addRequired<SpeculativeTaint>();
	AU.addRequired<LeakAnalysis>();
      }

      static bool ignoreCall(const llvm::CallBase *C) {
	if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(C)) {
	  if (llvm::isa<MitigationInst, llvm::DbgInfoIntrinsic>(II) ||  II->isAssumeLikeIntrinsic()) {
	    return true;
	  } else {
	    switch (II->getIntrinsicID()) {
	    case llvm::Intrinsic::fshr:
	    case llvm::Intrinsic::fshl:
	    case llvm::Intrinsic::x86_aesni_aesenc:
	    case llvm::Intrinsic::x86_aesni_aeskeygenassist:
	    case llvm::Intrinsic::x86_aesni_aesenclast:
	    case llvm::Intrinsic::vector_reduce_and:
	    case llvm::Intrinsic::vector_reduce_or:
	    case llvm::Intrinsic::umax:
	    case llvm::Intrinsic::umin:
	    case llvm::Intrinsic::smax:
	    case llvm::Intrinsic::smin:
	    case llvm::Intrinsic::ctpop:
	    case llvm::Intrinsic::bswap:
	    case llvm::Intrinsic::x86_pclmulqdq:
	    case llvm::Intrinsic::x86_rdrand_32:
	    case llvm::Intrinsic::vastart:
	    case llvm::Intrinsic::vaend:
	    case llvm::Intrinsic::vector_reduce_add:
	    case llvm::Intrinsic::abs:
	    case llvm::Intrinsic::umul_with_overflow:
	    case llvm::Intrinsic::bitreverse:
	    case llvm::Intrinsic::cttz:
	    case llvm::Intrinsic::usub_sat:
	    case llvm::Intrinsic::fmuladd:
	    case llvm::Intrinsic::annotation:
	    case llvm::Intrinsic::x86_sse2_mfence:
	    case llvm::Intrinsic::fabs:
	    case llvm::Intrinsic::floor:
	      return true;

	    case llvm::Intrinsic::memset:
	    case llvm::Intrinsic::memcpy:
	    case llvm::Intrinsic::memmove:
	      return false;

	    default:
	      warn_unhandled_intrinsic(II);
	      return false;
	    }
	  }
	} else {
	  return false;
	}	
      }

      static unsigned compute_edge_weight(llvm::Instruction *src, llvm::Instruction *dst, const llvm::DominatorTree& DT, const llvm::LoopInfo& LI) {
	if (WeightGraph) {
	  float score = 1.;
	  const unsigned LoopDepth = std::min(instruction_loop_nest_depth(src, LI), instruction_loop_nest_depth(dst, LI));
	  const unsigned DomDepth = std::max(instruction_dominator_depth(src, DT), instruction_dominator_depth(dst, DT));
	  score *= pow(LoopDepth + 1, LoopWeight);
	  score *= 1. / pow(DomDepth + 1, DominatorWeight);
	  return score * 1000;
	} else {
	  return 1;
	}
      }

      template <class OutputIt>
      OutputIt getPublicLoads(llvm::Function& F, OutputIt out) {
	LeakAnalysis& LA = getAnalysis<LeakAnalysis>();
	SpeculativeTaint& ST = getAnalysis<SpeculativeTaint>();
	for (auto& I : llvm::instructions(F)) {
	  if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
	    if (LA.mayLeak(LI) && (!ST.secret(LI) || util::isConstantAddress(LI->getPointerOperand()))) {
	      *out++ = LI;
	    }
	  }
	}
	return out;
      }

      static void getNonConstantAddressSecretStores(llvm::Function& F, NonspeculativeTaint& NST, SpeculativeTaint& ST,
						    std::set<llvm::StoreInst *>& nca_nt_sec_stores, std::set<llvm::StoreInst *>& nca_t_sec_stores,
						    std::set<llvm::StoreInst *>& nca_pub_stores) {
	for (llvm::Instruction& I : llvm::instructions(F)) {
	  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
	    if (!util::isConstantAddressStore(SI)) {
	      if (llvm::Instruction *V = llvm::dyn_cast<llvm::Instruction>(SI->getValueOperand())) {
		bool nt_sec = false;
		bool t_sec = false;
		for (auto *op_V : get_incoming_loads(V)) {
		  if (auto *op_I = llvm::dyn_cast<llvm::Instruction>(op_V))
		    if (auto *op_LI = llvm::dyn_cast<llvm::LoadInst>(op_I))
		      if (op_LI->getPointerOperand() == SI->getPointerOperand())
			continue;
		  if (NST.secret(op_V)) {
		    nt_sec = true;
		    break;
		  }
		  if (ST.secret(op_V))
		    t_sec = true;
		}

		if (nt_sec && !NST.secret(V)) {
		  llvm::WithColor::warning() << "mismatch in nt-secret labels\n";
		  continue;
		}
		if (t_sec && !ST.secret(V)) {
		  llvm::WithColor::warning() << "mismatch in t-secret labels\n";
		  continue;
		}

		if (nt_sec)
		  nca_nt_sec_stores.insert(SI);
		else if (t_sec)
		  nca_t_sec_stores.insert(SI);
		else
		  nca_pub_stores.insert(SI);
	      }
	    }
	  }
	}
      }

      static std::set<llvm::Instruction *> getSourcesForNCAAccess(llvm::Instruction *I,
								  [[maybe_unused]] const std::set<llvm::StoreInst *>& nca_pub_stores) {
	assert(I != &I->getFunction()->front().front() && "I cannot be the entrypoint instruction of the function");
	// compute reach set
	// NOTE: We specifically don't count the first instruction `I` as reaching itself in step 0.
	// For `I` to be reached, there must be a cycle I -> I in the CFG.
	std::set<llvm::Instruction *> reach;
	{
	  std::stack<llvm::Instruction *> todo;
	  todo.push(I);
	  while (!todo.empty()) {
	    auto *I = todo.top();
	    todo.pop();
	    for (auto *pred : llvm::predecessors(I))
	      if (reach.insert(I).second)
		todo.push(pred);
	  }
	}

	// compute candidate sources
	std::set<llvm::Instruction *> sources;

	// Type 1: values used to compute address.
	for (llvm::Value *op_V : get_incoming_loads(util::getPointerOperand(I))) {
	  if (auto *op_I = llvm::dyn_cast<llvm::Instruction>(op_V)) {
	    sources.insert(op_I);
	  } else if (llvm::isa<llvm::Argument>(op_V)) {
	    auto *EntryInst = &I->getFunction()->front().front();
	    assert(I != EntryInst);
	    sources.insert(EntryInst);
	  } else {
	    unhandled_value(*op_V);
	  }
	}

	// Type 2: Control-flow.
	// TODO: Can use more optimal analysis of control-equivalent uses of base pointer.
	for (auto *T : reach) {
	  if (T->isTerminator()) {
	    const bool unreachable_succ = llvm::any_of(llvm::successors_inst(T), [&reach] (auto *succ) {
	      return !reach.contains(succ);
	    });
	    if (unreachable_succ)
	      sources.insert(T);
	  }
	}

	std::erase_if(sources, [&reach] (llvm::Instruction *I) {
	  return !reach.contains(I);
	});

	// Also do backward frontier
	std::set<llvm::Instruction *> actual_sources;
#if 0
	util::getFrontierBwd(I, sources, actual_sources);
#else
	actual_sources = std::move(sources);
#endif
	
	return actual_sources;
      }

      template <class OutputIt>
      static OutputIt getCtrls(llvm::Function& F, OutputIt out) {
	for (auto& I : llvm::instructions(F)) {
	  if (llvm::CallBase *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
	    if (!ignoreCall(CB)) {
	      *out++ = CB;
	    }
	  } else if (llvm::isa<llvm::ReturnInst>(&I)) {
	    *out++ = &I;
	  }
	}
	return out;
      }

      static void getTransmitters(llvm::Function& F, SpeculativeTaint& ST, std::map<llvm::Instruction *, ISet>& out) {
	for (auto& I : llvm::instructions(F)) {
	  for (const TransmitterOperand& op : get_transmitter_sensitive_operands(&I)) {
	    if (ST.secret(op.V)) {
	      auto *op_I = llvm::cast<llvm::Instruction>(op.V);
	      out[&I].insert(op_I);
	    }
	  }
	}
      }

      template <class OutputIt>
      static void checkArgs(llvm::Function& F, OutputIt out) {
	llvm::DataLayout DL(F.getParent());
	unsigned counter = 0;
	for (llvm::Argument& A : F.args()) {
	  if (counter >= 6) {
	    *out++ = std::make_pair(&A, false);
	  } else {
	    llvm::Type *T = A.getType();
	    const auto bits = DL.getTypeSizeInBits(T);
	    constexpr auto regbits = 64;
	    counter += ((bits + regbits - 1) / regbits);
	    *out++ = std::make_pair(&A, counter <= 6);
	  }
	}
      }

      void staticStats(llvm::json::Object& j, llvm::Function& F) {
	// List callees
	auto& callees = j["callees"] = llvm::json::Array();
	auto& indirect_calls = j["indirect_calls"] = false;
	std::set<const llvm::Function *> seen;
	for (const llvm::CallBase& CB : util::instructions<llvm::CallBase>(F)) {
	  if (const llvm::Function *F = CB.getCalledFunction()) {
	    if (seen.insert(F).second) {
	      callees.getAsArray()->push_back(llvm::json::Value(F->getName()));
	    }
	  } else {
	    indirect_calls = true;
	  }
	}

	// Get all transmitters
	CountStat stat_naive_loads(j, "naive_loads");
	CountStat stat_naive_xmits(j, "naive_xmits");
	for (llvm::Instruction& I : llvm::instructions(F)) {
	  bool is_xmit = false;
	  for (const auto& op : get_transmitter_sensitive_operands(&I)) {
	    if (op.kind == TransmitterOperand::TRUE) {
	      for ([[maybe_unused]] const auto& V : get_incoming_loads(op.V)) {
		is_xmit = true;
		++stat_naive_loads;
	      }
	    }
	  }
	  if (is_xmit)
	    ++stat_naive_xmits;
	}
      }

      void saveLog(llvm::json::Object&& j, llvm::Function& F) {
	if (!ClouLog)
	  return;
	std::string s;
	llvm::raw_string_ostream ss(s);
	ss << ClouLogDir << "/" << F.getName() << ".json";
	std::ofstream os_cxx(s);
	llvm::raw_os_ostream os_llvm(os_cxx);
	os_llvm << llvm::json::Value(std::move(j));
      }

      bool runOnFunction(llvm::Function &F) override {
		
	if (whitelisted(F))
	  return false;

	clock_t t_start = clock();
	
	auto& NST = getAnalysis<NonspeculativeTaint>();
	auto& ST = getAnalysis<SpeculativeTaint>();
	auto& LA = getAnalysis<LeakAnalysis>();

	
	llvm::DominatorTree DT(F);
	llvm::LoopInfo LI(DT);

	// Set of speculatively public loads	
	std::set<llvm::LoadInst *> spec_pub_loads;
	getPublicLoads(F, std::inserter(spec_pub_loads, spec_pub_loads.end()));
	
	// Set of secret, speculatively out-of-bounds stores (speculative or nonspeculative)
	std::set<llvm::StoreInst *> nca_nt_sec_stores, nca_t_sec_stores, nca_pub_stores;
	getNonConstantAddressSecretStores(F, NST, ST, nca_nt_sec_stores, nca_t_sec_stores,
					  nca_pub_stores);

	// Set of control-transfer instructions that require all previous OOB stores to have resolved
	ISet ctrls;
	getCtrls(F, std::inserter(ctrls, ctrls.end()));

	// Set of transmitters
	std::map<llvm::Instruction *, ISet> transmitters;
	getTransmitters(F, ST, transmitters);

	using Alg = MinCutGreedy<Node>;
	Alg A;
	Alg::Graph& G = A.G;

#if 0
	const auto cull_sts = [] (std::vector<Alg::ST>& stvec) {
	  return;
	  unsigned culled = 0;

	  std::set<Alg::ST> sts(stvec.begin(), stvec.end());
	  
	  std::map<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>, std::set<Alg::ST>> groups;
	  for (const auto& st : sts)
	    groups[std::make_pair(st.s.V->getParent(), st.t.V->getParent())].insert(st);

	  // Approach: replace s-t pairs. 
	  for (const auto& [bbs, set] : groups) {
	    if (bbs.first != bbs.second) {
	      for (const Alg::ST& st : set) {
		sts.erase(st);
		++culled;
	      }
	      const Alg::ST newst = {
		.s = Node(&bbs.first->back()),
		.t = Node(&bbs.second->front())
	      };
	      sts.insert(newst);
	      --culled;
	    }
	  }

	  // Now find (s1, t1), (s2, t2) such that s2 postdominates s1 and t2 dominates t1.
	  {
	    std::set<Node> sources, sinks;
	    for (const Alg::ST& st : sts) {
	      sources.insert(st.s);
	      sinks.insert(st.t); 
	    }
	      
	  }
	  
	  llvm::errs() << "culled " << culled << "\n";

	  stvec.clear();
	  llvm::copy(sts, std::back_inserter(stvec));
	};
#endif
	
	/* Stats */
	llvm::json::Object log;

	CountStat stat_ncas_load(log, "sts_ncas_cal");
	CountStat stat_ncas_ctrl(log, "sts_ncas_ctrl");
	CountStat stat_st_udts(log, "sts_udt");
	CountStat stat_instructions(log, "instructions", std::distance(llvm::inst_begin(F), llvm::inst_end(F)));
	CountStat stat_nonspec_secrets(log, "maybe_nonspeculative_secrets",
				       llvm::count_if(llvm::instructions(F), [&] (auto& I) { return NST.secret(&I); }));
	CountStat stat_nonspec_publics(log, "definitely_nonspeculative_public",
				       llvm::count_if(util::nonvoid_instructions(F), [&] (auto& I) { return !NST.secret(&I); }));
	CountStat stat_spec_secrets(log, "maybe_speculative_secrets",
				    llvm::count_if(llvm::instructions(F), [&] (auto& I) { return ST.secret(&I); }));
	CountStat stat_spec_publics(log, "definitely_speculative_publics",
				    llvm::count_if(util::nonvoid_instructions(F), [&] (auto& I) { return !ST.secret(&I); }));
	CountStat stat_leaks(log, "maybe_leaked_instructions",
			     llvm::count_if(llvm::instructions(F), [&] (auto& I) { return LA.mayLeak(&I); }));
	CountStat stat_nonleaks(log, "definitely_not_leaked_instructions",
				llvm::count_if(util::nonvoid_instructions(F), [&] (auto& I) { return !LA.mayLeak(&I); }));
	CountStat stat_leaked_spec_secrets(log, "maybe_leaked_speculative_secret_loads",
					   llvm::count_if(util::instructions<llvm::LoadInst>(F), [&] (auto& I) {
					     return LA.mayLeak(&I) && ST.secret(&I);
					   }));


	{
	  // Transmitter stats
	  CountStat stat_xmit_pub(log, "xmit_safe");
	  CountStat stat_xmit_pseudo(log, "xmit_pseudo");
	  CountStat stat_xmit_true(log, "xmit_true");
	  for (auto& I : llvm::instructions(F)) {
	    enum Kind {
	      NONE = 0,
	      SAFE,
	      PSEUDO,
	      TRUE,
	    };
	    Kind kind = NONE;
	    for (const auto& op : get_transmitter_sensitive_operands(&I)) {
	      if (ST.secret(op.V)) {
		switch (op.kind) {
		case TransmitterOperand::TRUE:
		  kind = std::max(kind, TRUE);
		  break;
		case TransmitterOperand::PSEUDO:
		  kind = std::max(kind, PSEUDO);
		  break;
		default: std::abort();
		}
	      } else {
		kind = std::max(kind, SAFE);
	      }
	    }

	    switch (kind) {
	    case NONE: break;
	    case SAFE: ++stat_xmit_pub; break;
	    case PSEUDO: ++stat_xmit_pseudo; break;
	    case TRUE: ++stat_xmit_true; break;
	    default: std::abort();
	    }
	    
	  }
	}
	
	
	{
	  CountStat stat_cas(log, "cas");
	  CountStat stat_ncas_pub(log, "ncas_pub");
	  CountStat stat_ncas_sec(log, "ncas_sec");
	  
	  for (auto& SI : util::instructions<llvm::StoreInst>(F)) {
	    if (util::isConstantAddress(SI.getPointerOperand())) {
	      ++stat_cas;
	    } else {
	      auto *V = SI.getValueOperand();
	      if (NST.secret(V) || ST.secret(V)) {
		++stat_ncas_sec;
	      } else {
		++stat_ncas_pub;
	      }
	    }
	  }

	  CountStat stat_cal(log, "cal");
	  CountStat stat_ncal_leak(log, "ncal_leak");
	  CountStat stat_ncal_safe(log, "ncal_safe");
	  CountStat stat_cal_specsec(log, "cal_specsec");
	  CountStat stat_cal_leak(log, "cal_leak");

	  for (auto& LI : util::instructions<llvm::LoadInst>(F)) {
	    if (util::isConstantAddress(LI.getPointerOperand())) {
	      ++stat_cal;
	      if (LA.mayLeak(&LI)) {
		++stat_cal_leak;
	      }
	      if (ST.secret(&LI)) {
		++stat_cal_specsec;
	      }
	    } else {
	      assert(ST.secret(&LI));
	      if (LA.mayLeak(&LI)) {
		++stat_ncal_leak;
	      } else {
		++stat_ncal_safe;
	      }
	    }
	  }
	}

	const auto make_node_range = [] (const auto& iset) {
	  return llvm::map_range(iset, [] (llvm::Instruction *I) -> Node {
	    return {I};
	  });
	};
	const auto make_node_set = [&make_node_range] (const auto& irange) {
	  std::set<Node> nodes;
	  llvm::transform(irange, std::inserter(nodes, nodes.end()), [] (llvm::Instruction *I) -> Node {
	    return {I};
	  });
	  return nodes;
	};
	
	if (enabled.ncas_xmit) {

	  // Create ST-pairs for {oob_sec_stores X spec_pub_loads}
	  CountStat stat_spec_pub_loads(log, "spec_pub_loads", spec_pub_loads.size());

	  for (auto *SI : llvm::concat<llvm::StoreInst * const>(nca_nt_sec_stores, nca_t_sec_stores)) {
#if 0
	    // Compute source(s)
	    std::set<llvm::Instruction *> sources;
	    {
	      // initial sources: deps + nca pub stores
	      std::set<llvm::Value *> all_sources = get_incoming_loads(SI->getPointerOperand());
	      llvm::copy(nca_pub_stores, std::inserter(sources, sources.end()));
	      util::getFrontierBwd(SI, all_sources, sources);
	    }
#elif 0
	    const auto sources = getSourcesForNCAAccess(SI, nca_pub_stores);
#endif
	    
	    // Find instructions that the store may reach.
	    std::stack<llvm::Instruction *> todo;
	    todo.push(SI);
	    std::set<llvm::Instruction *> seen;
	    while (!todo.empty()) {
	      llvm::Instruction *I = todo.top();
	      todo.pop();
	      if (!seen.insert(I).second)
		continue;
	      for (llvm::Instruction *succ : llvm::successors_inst(I))
		todo.push(succ);
	    }

	    // Get relevant transmitter instructions.
	    std::set<Node> xmits;
	    for (llvm::Instruction *T : seen) {
	      const auto sensitive_operands = get_transmitter_sensitive_operands(T);
	      const bool vulnerable = llvm::any_of(sensitive_operands, [&] (const TransmitterOperand& TO) -> bool {
		const auto loads = get_incoming_loads(TO.V);
		return llvm::any_of(loads, [&] (auto *V) {
		  if (llvm::LoadInst *LI = llvm::dyn_cast<llvm::LoadInst>(V))
		    return seen.contains(LI);
		  else
		    return false;
		});
	      });
	      if (vulnerable)
		xmits.insert(T);
	    }

	    A.add_st(std::set<Node>{SI}, xmits);
	  }

	}


	if (enabled.ncas_ctrl) {
	  
	  // Create ST-pairs for {oob_sec_stores X ctrls}
#if 0
	  for (auto *ctrl : ctrls) {
	    for (auto *SI : llvm::concat<llvm::StoreInst * const>(nca_nt_sec_stores, nca_t_sec_stores)) {
#if 1
	      A.add_st({{SI}, {ctrl}});
	      ++stat_ncas_ctrl;
#else
	      for (auto *source : getSourcesForNCAAccess(SI, nca_pub_stores))
		sts.push_back({.s = source, .t = ctrl});
#endif
	    }
	  }
#else
	  const auto ncas = llvm::concat<llvm::StoreInst * const>(nca_nt_sec_stores, nca_t_sec_stores);
	  A.add_st(make_node_set(ncas), make_node_set(ctrls));
#endif
	}

	if (enabled.ncal_xmit) {

	  std::map<llvm::Instruction *, std::set<llvm::Instruction *>> sources_map;
	  const auto get_sources = [&] (llvm::Instruction *ncal) -> const std::set<llvm::Instruction *>& {
	    auto it = sources_map.find(ncal);
	    if (it == sources_map.end()) {
	      auto sources = getSourcesForNCAAccess(ncal, nca_pub_stores);
	      it = sources_map.emplace(ncal, std::move(sources)).first;
	    }
	    return it->second; 
	  };

	  // Create ST-pairs for {source X transmitter}
	  for (const auto& [xmit, xmit_ops] : transmitters) {
	    std::set<llvm::Instruction *> ncals;
	    for (llvm::Instruction *xmit_op : xmit_ops)
	      llvm::copy(ST.taints.at(xmit_op), std::inserter(ncals, ncals.end()));
#if 0
	    A.add_st(make_node_set(ncals), std::set<Node>{xmit});
#else
	    for (llvm::Instruction *ncal : ncals) {
	      if (ncal == &ncal->getFunction()->front().front()) {
		A.add_st(std::set<Node>{ncal}, std::set<Node>{xmit});
	      } else {
		const auto& sources = get_sources(ncal);
#if 1
		A.add_st(make_node_set(sources), std::set<Node>{ncal}, std::set<Node>{xmit});
#elif 0
		for (llvm::Instruction *source : sources)
		  A.add_st({{source}, {ncal}, {xmit}});
#else
		auto it = sources.begin();
		while (it != sources.end()) {
		  std::set<Node> s;
		  s.insert(*it++);
		  if (it != sources.end())
		    s.insert(*it++);
		  A.add_st(s, std::set<Node>{ncal}, std::set<Node>{xmit});
		}
#endif
	      }
	    }
#endif
	  }
	  
	}

	// Add CFG to graph
	for (auto& B : F) {
	  for (auto& src : B) {
	    auto& dsts = G[&src];
	    for (auto *dst : llvm::successors_inst(&src))
	      dsts[dst] = compute_edge_weight(&src, dst, DT, LI);
	  }
	}

	// Run algorithm to obtain min-cut
	const clock_t solve_start = clock();
#if 0
	cull_sts(sts);
#endif
	llvm::errs() << F.getName() << "\n";
	auto G_ = G;
	A.run();
	const clock_t solve_stop = clock();
	const float solve_duration = (static_cast<float>(solve_stop) - static_cast<float>(solve_start)) / CLOCKS_PER_SEC;
	auto& cut_edges = A.cut_edges;
	llvm::errs() << "num sts: " << A.get_sts().size() << "\n";
	llvm::errs() << "cut edges: " << cut_edges.size() << "\n";

	// double-check cut: make sure that no source can reach its sink
	{
	  std::set<Edge> cutset;
	  llvm::copy(cut_edges, std::inserter(cutset, cutset.end()));
	  checkCut(A.get_sts(), cutset, F);
	}

	// Output DOT graph, color cut edges
	if (ClouLog) {
	  
	  // emit dot
#if 0
	  {
	    std::stringstream path;
	    path << ClouLogDir << "/" << F.getName().str() << ".dot";
	    std::ofstream f(path.str());
	    f << "digraph {\n";

	    // collect stores + sinks
	    std::map<Node, std::size_t> nodes;
	    for (llvm::BasicBlock& B : F) {
	      for (llvm::Instruction& I : B) {
		nodes.emplace(&I, nodes.size());
	      }
	    }

	    std::map<Node, std::string> special;
	    for (const Alg::ST& st : A.sts) {
	      special[st.t] = "blue"; // TODO: used to be green, but sometimes nodes are both sources and sinks.
	      special[st.s] = "blue";
	    }
	    
	    for (const auto& [node, i] : nodes) {
	      f << "node" << i << " [label=\"" << node << "\"";
	      if (special.contains(node)) {
		f << ", style=filled, fontcolor=white, fillcolor=" << special[node] << "";
	      }
	      f << "];\n";
	    }

	    // Add ST-pairs as dotted gray edges
	    for (const auto& st : A.sts) {
	      f << "node" << nodes.at(st.s) << " -> node" << nodes.at(st.t) << " [style=\"dashed\", color=\"blue\", penwidth=3]\n";
	    }
	  
            
	    for (const auto& [u, usucc] : G_) {
	      for (const auto& [v, weight] : usucc) {
		if (weight > 0) {
		  f << "node" << nodes.at(u) << " -> " << "node" << nodes.at(v) << " [label=\"" << weight << "\", penwidth=3";
		  if (std::find(cut_edges.begin(),
				cut_edges.end(),
				Alg::Edge({.src = u, .dst = v})) != cut_edges.end()) {
		    f << ", color=\"red\"";
		  }
		  f << "];\n";
		}
	      }
	    }
            
	    f << "}\n";
	  }
#endif

	  // emit stats
	  {
	    log["function_name"] = F.getName();
	    log["solution_time"] = util::make_string_std(std::setprecision(3), solve_duration) + "s";
	    log["num_st_pairs"] = A.get_sts().size();
#if 0
	    VSet sources, sinks;
	    for (const auto& st : A.sts) {
	      sources.insert(st.s.V);
	      sinks.insert(st.t.V);
	    }
	    log["distinct_sources"] = sources.size();
	    log["distinct_sinks"] = sinks.size();
#endif

#if 0
	    // TODO: Re-add support for this.
	    auto& j_sts = log["st_pairs"] = llvm::json::Array();
	    for (const auto& st : A.sts) {
	      std::string source, sink;
	      llvm::raw_string_ostream source_os(source), sink_os(sink);
	      print_debugloc(source_os, st.s.V);
	      print_debugloc(sink_os, st.t.V);
	      source_os << "   (" << *st.s.V << ")";
	      sink_os   << "   (" << *st.t.V << ")";
	      j_sts.getAsArray()->push_back(llvm::json::Object({
		    {.K = "source", .V = source},
		    {.K = "sink", .V = sink},
		  }));
	    }
#endif

	    // TODO: emit this to separate file?
	    auto& j_cuts = log["cut_edges"] = llvm::json::Array();
	    for (const auto& cut : cut_edges) {
	      std::string src, dst;
	      llvm::raw_string_ostream src_os(src), dst_os(dst);
	      print_debugloc(src_os, cut.src.V);
	      print_debugloc(dst_os, cut.dst.V);
	      src_os << "   (" << *cut.src.V << ")";
	      dst_os << "   (" << *cut.dst.V << ")";
	      j_cuts.getAsArray()->push_back(llvm::json::Object({
		    {.K = "src", .V = src},
		    {.K = "dst", .V = dst},
		  }));
	    }

	    
	    log["lfences"] = cut_edges.size();

	    auto& j_ncas_nt_sec = log["ncas_nt_sec"] = llvm::json::Array();
	    for (auto *SI : nca_nt_sec_stores)
	      j_ncas_nt_sec.getAsArray()->push_back(util::make_string_llvm(*SI));

	    auto& j_ncas_t_sec = log["ncas_t_sec"] = llvm::json::Array();
	    for (auto * SI : nca_t_sec_stores)
	      j_ncas_t_sec.getAsArray()->push_back(util::make_string_llvm(*SI));
	  }
	}

	// Mitigations
	auto& lfence_srclocs = log["lfence_srclocs"] = llvm::json::Array();
	for (const auto& [src, dst] : cut_edges) {
	  if (llvm::Instruction *mitigation_point = getMitigationPoint(llvm::cast<llvm::Instruction>(src.V), llvm::cast<llvm::Instruction>(dst.V))) {
	    CreateMitigation(mitigation_point, "clou-mitigate-pass");
	    
	    // Print out mitigation info
	    if (ClouLog) {
	      lfence_srclocs.getAsArray()->push_back(llvm::json::Object({
		    {.K = "src", .V = str_debugloc(src.V)},
		    {.K = "dst", .V = str_debugloc(dst.V)},
		  }));
	    }
	  }
	}

	if (ClouLog) {
	  std::stringstream path;
	  path << ClouLogDir << "/" << F.getName().str() << ".ll";
	  std::ofstream f(path.str());
	  llvm::raw_os_ostream os(f);
	  F.print(os);
	}

#if 0
	// Mark all public values as nospill
	for (auto& B : F) {
	  for (auto& I : B) {
	    if (!(ST.secret(&I) || I.getType()->isVoidTy())) {
	      I.setMetadata("clou.nospill", llvm::MDNode::get(F.getContext(), {}));
	    }
	  }
	}
#endif

	
	
	const clock_t t_stop = clock();
	if (log_times) {
	  trace("time %.3f %s", static_cast<float>(t_stop - t_start) / CLOCKS_PER_SEC, F.getName().str().c_str());
	}

	staticStats(log, F);
	saveLog(std::move(log), F);

        return true;
      }

#if 1
      static bool shouldCutEdge([[maybe_unused]] llvm::Instruction *src, llvm::Instruction *dst) {
	// return llvm::predecessors(dst).size() > 1 || llvm::successors_inst(src).size() > 1;
	return llvm::predecessors(dst).size() > 1;
      }

      static llvm::Instruction *getMitigationPoint(llvm::Instruction *src, llvm::Instruction *dst) {
	if (shouldCutEdge(src, dst)) {
	  assert(src->isTerminator());
	  for (const llvm::Instruction *I = dst->getPrevNode(); I != nullptr; I = I->getPrevNode())
	    assert(llvm::isa<MitigationInst>(I));
	  if (dst != &dst->getParent()->front())
	    return nullptr;
	  llvm::BasicBlock *B = llvm::SplitEdge(src->getParent(), dst->getParent());
	  return &B->front();
	} else {
	  return dst;
	}
      }
#else
      static llvm::Instruction *getMitigationPoint(llvm::Value *src, llvm::Value *dst) {
	if (auto *dst_B = llvm::dyn_cast<llvm::BasicBlock>(dst)) {
	  // Check if we should cut the edge.
	  if (llvm::pred_size(dst_B) > 1) {
	    // Split theedge.
	    auto *src_I = llvm::cast<llvm::Instruction>(src);
	    llvm::BasicBlock *B = llvm::SplitEdge(src_I->getParent(), dst_B);
	    return &B->front();
	  } else {
	    // We can just insert at the front of the existing block.
	    return &dst_B->front();
	  }
	} else if (auto *I = llvm::dyn_cast<llvm::Instruction>(dst)) {
	  return I;
	} else {
	  llvm_unreachable("this is impossible");
	}
      }
#endif

      template <class STs>
      static void collapseOptimization(const STs& sts, VSetSet& src_out, VSetSet& sink_out) {
	VSet src_in, sink_in;
	for (const auto& st : sts) {
	  src_in.insert(st.s.V);
	  sink_in.insert(st.t.V);
	}
  
	VMap s_ts, t_ss;
	for (const auto& st : sts) {
	  s_ts[st.s.V].insert(st.t.V);
	  t_ss[st.t.V].insert(st.s.V);
	}

	// Flip maps
	std::map<std::set<llvm::Value *>, std::set<llvm::Value *>> ts_to_ss, ss_to_ts;
	for (const auto& [s, ts] : s_ts) {
	  ts_to_ss[ts].insert(s);
	}
	for (const auto& [t, ss] : t_ss) {
	  ss_to_ts[ss].insert(t);
	}

	// Copy out sets
	for (const auto& [_, ss] : ts_to_ss) {
	  src_out.insert(ss);
	}
	for (const auto& [_, ts] : ss_to_ts) {
	  sink_out.insert(ts);
	}

	// Validate results
	{
	  VSet src_out_flat, sink_out_flat;
	  for (const auto& gsrc : src_out) {
	    src_out_flat.insert(gsrc.begin(), gsrc.end());
	  }
	  for (const auto& gsink : sink_out) {
	    sink_out_flat.insert(gsink.begin(), gsink.end());
	  }
#if 0
	  if (src_out_flat != src_in || sink_out_flat != sink_in) {
	    auto& os = llvm::errs();
	    os << "sts:\n";
	    for (const auto& st : sts) {
	      os << "  (" << st.s.V << ", " << st.t.V << ")\n";
	    }
	    os << "src_in: " << src_in << "\n";
	    os << "sink_in: " << sink_in << "\n";
	    os << "s_ts: " << s_ts << "\n";
	    os << "t_ss: " << t_ss << "\n";
	    os << "src_out: " << src_out << "\n";
	    os << "sink_out: " << sink_out << "\n";
	    
	  }
#endif
	  assert(src_out_flat == src_in);
	  assert(sink_out_flat == sink_in);
	}
      }
    };

    llvm::RegisterPass<MitigatePass> X{"clou-mitigate",
					   "Clou Mitigation Pass"};
    util::RegisterClangPass<MitigatePass> Y;
  }
}


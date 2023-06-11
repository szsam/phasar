/******************************************************************************
 * Copyright (c) 2019 Philipp Schubert, Richard Leer, and Florian Sattler.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#ifndef PHASAR_PHASARLLVM_IFDSIDE_PROBLEMS_IDEINSTINTERACTIONALYSIS_H
#define PHASAR_PHASARLLVM_IFDSIDE_PROBLEMS_IDEINSTINTERACTIONALYSIS_H

#include "phasar/DataFlow/IfdsIde/EdgeFunctionComposer.h"
#include "phasar/DataFlow/IfdsIde/EdgeFunctionSingletonFactory.h"
#include "phasar/DataFlow/IfdsIde/EdgeFunctionUtils.h"
#include "phasar/DataFlow/IfdsIde/EdgeFunctions.h"
#include "phasar/DataFlow/IfdsIde/FlowFunctions.h"
#include "phasar/DataFlow/IfdsIde/IDETabulationProblem.h"
#include "phasar/DataFlow/IfdsIde/SolverResults.h"
#include "phasar/Domain/LatticeDomain.h"
#include "phasar/PhasarLLVM/DB/LLVMProjectIRDB.h"
#include "phasar/PhasarLLVM/DataFlow/IfdsIde/LLVMFlowFunctions.h"
#include "phasar/PhasarLLVM/DataFlow/IfdsIde/LLVMKFieldSensFlowFact.h"
#include "phasar/PhasarLLVM/DataFlow/IfdsIde/LLVMZeroValue.h"
#include "phasar/PhasarLLVM/Domain/LLVMAnalysisDomain.h"
#include "phasar/PhasarLLVM/Pointer/LLVMAliasInfo.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToUtils.h"
#include "phasar/PhasarLLVM/Utils/LLVMIRToSrc.h"
#include "phasar/PhasarLLVM/Utils/LLVMShorthands.h"
#include "phasar/Utils/BitVectorSet.h"
#include "phasar/Utils/ByRef.h"
#include "phasar/Utils/Logger.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace vara {
class Taint;
} // namespace vara

namespace psr {

template <typename EdgeFactType>
struct IDEInstInteractionAnalysisDomain : public LLVMAnalysisDomainDefault {
  // type of the element contained in the sets of edge functions
  using d_t = LLVMKFieldSensFlowFact<>;
  using e_t = EdgeFactType;
  using l_t = LatticeDomain<BitVectorSet<e_t>>;
};

///
/// SyntacticAnalysisOnly: Can be set if a syntactic-only analysis is desired
/// (without using points-to information)
///
/// IndirectTaints: Can be set to ensure non-interference
///
template <typename EdgeFactType = std::string,
          bool EnableIndirectTaints = false>
class IDEInstInteractionAnalysisT
    : public IDETabulationProblem<
          IDEInstInteractionAnalysisDomain<EdgeFactType>> {
  using IDETabulationProblem<
      IDEInstInteractionAnalysisDomain<EdgeFactType>>::generateFromZero;

public:
  using AnalysisDomainTy = IDEInstInteractionAnalysisDomain<EdgeFactType>;

  using IDETabProblemType = IDETabulationProblem<AnalysisDomainTy>;
  using typename IDETabProblemType::container_type;
  using typename IDETabProblemType::EdgeFunctionPtrType;
  using typename IDETabProblemType::FlowFunctionPtrType;

  using d_t = typename AnalysisDomainTy::d_t;
  using n_t = typename AnalysisDomainTy::n_t;
  using f_t = typename AnalysisDomainTy::f_t;
  using t_t = typename AnalysisDomainTy::t_t;
  using v_t = typename AnalysisDomainTy::v_t;
  // type of the element contained in the sets of edge functions
  using e_t = typename AnalysisDomainTy::e_t;
  using l_t = typename AnalysisDomainTy::l_t;
  using i_t = typename AnalysisDomainTy::i_t;

  using EdgeFactGeneratorTy = std::set<e_t>(
      std::variant<n_t, const llvm::GlobalVariable *> InstOrGlobal);

  IDEInstInteractionAnalysisT(
      const LLVMProjectIRDB *IRDB, const LLVMBasedICFG *ICF,
      LLVMAliasInfoRef PT, std::vector<std::string> EntryPoints = {"main"},
      std::function<EdgeFactGeneratorTy> EdgeFactGenerator = nullptr)
      : IDETabulationProblem<AnalysisDomainTy, container_type>(
            IRDB, std::move(EntryPoints), createZeroValue()),
        ICF(ICF), PT(PT), EdgeFactGen(std::move(EdgeFactGenerator)) {
    assert(ICF != nullptr);
    assert(PT);
    IIAAAddLabelsEF::initEdgeFunctionCleaner();
    IIAAKillOrReplaceEF::initEdgeFunctionCleaner();
  }

  ~IDEInstInteractionAnalysisT() override = default;

  /// Offer a special hook to the user that allows to generate additional
  /// edge facts on-the-fly. Above the generator function, the ordinary
  /// edge facts are generated according to the usual edge functions.
  inline void registerEdgeFactGenerator(
      std::function<EdgeFactGeneratorTy> EdgeFactGenerator) {
    EdgeFactGen = std::move(EdgeFactGenerator);
  }

  template <typename Container = std::set<d_t>>
  auto generateFlow(const llvm::Value *FactToGenerate, d_t From) {
    struct GenFrom final : public FlowFunction<d_t, Container> {
      GenFrom(const llvm::Value *GenValue, d_t FromValue)
          : GenValue(GenValue), FromValue(std::move(FromValue)) {}

      Container computeTargets(d_t Source) override {
        if (Source == FromValue) {
          return {std::move(Source), FromValue.getSameAP(GenValue)};
        }
        return {std::move(Source)};
      }

      const llvm::Value *GenValue;
      d_t FromValue;
    };

    return std::make_shared<GenFrom>(std::move(FactToGenerate),
                                     std::move(From));
  }

  template <typename Container = std::set<d_t>>
  auto generateFlowAndKillAllOthers(const llvm::Value *FactToGenerate,
                                    d_t From) {
    struct GenFlowAndKillAllOthers final : public FlowFunction<d_t, Container> {
      GenFlowAndKillAllOthers(const llvm::Value *GenValueBase, d_t FromValue)
          : GenValueBase(std::move(GenValueBase)),
            FromValue(std::move(FromValue)) {}

      Container computeTargets(d_t Source) override {
        if (Source == FromValue) {
          return {std::move(Source), FromValue.getSameAP(GenValueBase)};
        }
        return {};
      }

      const llvm::Value *GenValueBase;
      d_t FromValue;
    };

    return std::make_shared<GenFlowAndKillAllOthers>(std::move(FactToGenerate),
                                                     std::move(From));
  }

  // start formulating our analysis by specifying the parts required for IFDS

  FlowFunctionPtrType getNormalFlowFunction(n_t Curr, n_t /* Succ */) override {
    // Generate all local variables
    //
    // Flow function:
    //
    //                0
    //                |\
    // x = alloca y   | \
    //                v  v
    //                0  x
    //
    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Curr)) {
      PHASAR_LOG_LEVEL(DFADEBUG, "AllocaInst");
      return lambdaFlow<d_t>([Alloca](d_t Src) -> container_type {
        container_type Facts = {Src};
        if (IDEInstInteractionAnalysisT::isZeroValueImpl(Src)) {
          Facts.insert(Src.getStored(Alloca));
        }
        IF_LOG_ENABLED({
          for (const auto Fact : Facts) {
            PHASAR_LOG_LEVEL(DFADEBUG, "Create edge: "
                                           << llvmIRToShortString(Src) << " --"
                                           << llvmIRToShortString(Alloca)
                                           << "--> " << Fact);
          }
        });
        return Facts;
      });
    }

    // Handle indirect taints, i. e., propagate values that depend on branch
    // conditions whose operands are tainted.
    if constexpr (EnableIndirectTaints) {
      if (const auto *Br = llvm::dyn_cast<llvm::BranchInst>(Curr);
          Br && Br->isConditional()) {
        // If the branch is conditional and its condition is tainted, then we
        // need to propagates the instructions that are depending on this
        // branch, too.
        //
        // Flow function:
        //
        // Let I be the set of instructions of the branch instruction's
        // successors.
        //
        //                                          0  c  x
        //                                          |  |\
        // x = br C, label if.then, label if.else   |  | \--\
        //                                          v  v  v  v
        //                                          0  c  x  I
        //
        return lambdaFlow<d_t>([Br](d_t Src) {
          container_type Facts;
          Facts.insert(Src);
          if (Src == Br->getCondition()) {
            Facts.insert(Src.getOverapproximated(Br));
            for (const auto *Succs : Br->successors()) {
              for (const auto &Inst : Succs->instructionsWithoutDebug()) {
                Facts.insert(Src.getOverapproximated(&Inst));
              }
            }
          }
          return Facts;
        });
      }
    }

    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Curr)) {
      // If one of the potentially many loaded values holds, the load itself
      // (dereferenced value) must also be generated and populated.
      //
      // Flow function:
      //
      // Let Y = pts(y), be the points-to set of y.
      //
      //              0  Y  x
      //              |  |\
      // x = load y   |  | \
      //              v  v  v
      //              0  Y  x
      //
      return lambdaFlow<d_t>([Load](d_t Source) -> container_type {
        if (const auto *Gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                Load->getPointerOperand())) {
          const auto [Alloca, Offset] = getAllocaInstAndConstantOffset(Gep);
          // auto TargetedLoad = Source.getStored(Alloca,
          // Offset.value_or(9002));
          auto TargetedLoad =
              LLVMKFieldSensFlowFact<>::getCustomOffsetDerefValue(Alloca, {0});
          if (Source == TargetedLoad) {
            auto LoadedVal = Source.getLoaded(Load);
            return {Source, LoadedVal.value()};
          }
        }
        if (Source.getBaseValue() == Load->getPointerOperand()) {
          auto LoadedVal = Source.getLoaded(Load);
          if (LoadedVal == std::nullopt) {
            return {Source};
          }
          return {Source, LoadedVal.value()};
        }
        return {Source};
      });
    }

    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Curr)) {
      // If the value to be stored holds, the potential memory location(s)
      // that it is stored to must be generated and populated, too.
      //
      // Flow function:
      //
      // Let X be
      //    - pts(x), the points-to set of x, if x is an intersting pointer.
      //    - a singleton set containing x, otherwise.
      //
      // Let Y be pts(y), the points-to set of y.
      //
      //             0  X  y
      //             |  |\ |
      // store x y   |  | \|
      //             v  v  v
      //             0  x  Y
      //
      return lambdaFlow<d_t>([Store](d_t Src) -> container_type {
        if (Store->getPointerOperand() == Src) {
          return {};
        }
        container_type Facts;
        Facts.insert(Src);
        // y/Y now obtains its new value(s) from x/X
        // If a value is stored that holds we must generate all potential
        // memory locations the store might write to.
        if (Store->getValueOperand() == Src) {
          // Store to field or array.
          if (const auto &Gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                  Store->getPointerOperand())) {
            Facts.insert(LLVMKFieldSensFlowFact<>::getDerefValueFromGep(Gep));
          } else {
            // Ordinary store.
            Facts.insert(LLVMKFieldSensFlowFact<>::getNonIndirectionValue(
                Store->getPointerOperand()));
          }
        }
        // ... or from zero, if a constant literal is stored to y
        if (llvm::isa<llvm::ConstantData>(Store->getValueOperand()) &&
            IDEInstInteractionAnalysisT::isZeroValueImpl(Src)) {
          // Store to field or array.
          if (const auto &Gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                  Store->getPointerOperand())) {
            const auto [Alloca, Offset] = getAllocaInstAndConstantOffset(Gep);
            Facts.insert(Src.getStored(Alloca, Offset.value_or(9001)));
          } else {
            // Ordinary store.
            Facts.insert(Src.getStored(Store->getPointerOperand()));
          }
        }
        return Facts;
      });
    }

    // FIXME: handle gep here.
    if (llvm::isa<llvm::GetElementPtrInst>(Curr)) {
      return lambdaFlow<d_t>([](d_t Src) -> container_type { return {Src}; });
    }

    // At last, we can handle all other (unary/binary) instructions.
    //
    // Flow function:
    //
    //                       0  x  o  p
    //                       |  | /| /|
    // x = instruction o p   |  |/ |/ |
    //                       |  | /|  |
    //                       |  |/ |  |
    //                       v  v  v  v
    //                       0  x  o  p
    //
    return lambdaFlow<d_t>([Inst = Curr](d_t Src) {
      container_type Facts;
      if (IDEInstInteractionAnalysisT::isZeroValueImpl(Src)) {
        // keep the zero flow fact
        Facts.insert(Src);
        return Facts;
      }

      // populate and propagate other existing facts
      for (auto &Op : Inst->operands()) {
        // if one of the operands holds, also generate the instruction using
        // it
        if (Op == Src) {
          Facts.insert(Src.getSameAP(Inst)); // FIXMEMM
        }
      }
      // pass everything that already holds as identity
      Facts.insert(Src);
      IF_LOG_ENABLED({
        for (const auto Fact : Facts) {
          PHASAR_LOG_LEVEL(DFADEBUG,
                           "Create edge: " << llvmIRToShortString(Src) << " --"
                                           << llvmIRToShortString(Inst)
                                           << "--> " << Fact);
        }
      });
      return Facts;
    });
  }

  inline FlowFunctionPtrType getCallFlowFunction(n_t CallSite,
                                                 f_t DestFun) override {
    if (this->ICF->isHeapAllocatingFunction(DestFun)) {
      // Kill add facts and model the effects in getCallToRetFlowFunction().
      return killAllFlows<d_t>();
    }
    if (DestFun->isDeclaration()) {
      // We don't have anything that we could analyze, kill all facts.
      return killAllFlows<d_t>();
    }
    const auto *CS = llvm::cast<llvm::CallBase>(CallSite);

    // Map actual to formal parameters.
    auto MapFactsToCalleeFF = mapFactsToCallee<d_t>(
        CS, DestFun,
        [CS](const llvm::Value *ActualArg, ByConstRef<d_t> Src) {
          if (ActualArg != Src.getBaseValue()) { // FIXMEMM
            return false;
          }

          if (CS->hasStructRetAttr() && ActualArg == CS->getArgOperand(0)) {
            return false;
          }

          return true;
        },
        {}, true, true,
        [](const llvm::Value *Lhs, d_t Rhs) { return Rhs.getSameAP(Lhs); });

    // Generate the artificially introduced RVO parameters from zero value.
    auto SRetFormal = CS->hasStructRetAttr() ? DestFun->getArg(0) : nullptr;

    if (SRetFormal) {
      return unionFlows(std::move(MapFactsToCalleeFF),
                        generateFlowAndKillAllOthers(
                            SRetFormal, this->getZeroValue())); // FIXMEMM
    }

    return MapFactsToCalleeFF;
  }

  inline FlowFunctionPtrType getRetFlowFunction(n_t CallSite, f_t /*CalleeFun*/,
                                                n_t ExitInst,
                                                n_t /* RetSite */) override {
    // Map return value back to the caller. If pointer parameters hold at the
    // end of a callee function generate all of those in the caller context.

    auto MapFactsToCallerFF = mapFactsToCaller<d_t>(
        llvm::cast<llvm::CallBase>(CallSite), ExitInst,
        [](const llvm::Value *Param, d_t Src) {
          return Src.getBaseValue() == Param;
        },
        [](const llvm::Value *RetVal, d_t Src) {
          if (Src.getBaseValue() == RetVal) {
            return true;
          }
          if (isZeroValueImpl(Src)) {
            if (llvm::isa<llvm::ConstantData>(RetVal)) {
              return true;
            }
          }
          return false;
        },
        {}, true, true,
        [](const llvm::Value *Lhs, d_t Rhs) { return Rhs.getSameAP(Lhs); });

    return MapFactsToCallerFF;
  }

  inline FlowFunctionPtrType
  getCallToRetFlowFunction(n_t CallSite, n_t /* RetSite */,
                           llvm::ArrayRef<f_t> Callees) override {
    // Model call to heap allocating functions (new, new[], malloc, etc.) --
    // only model direct calls, though.
    if (Callees.size() == 1) {
      const auto *Callee = Callees.front();
      if (this->ICF->isHeapAllocatingFunction(Callee)) {
        // In case a heap allocating function is called, generate the pointer
        // that is returned.
        //
        // Flow function:
        //
        // Let H be a heap allocating function.
        //
        //              0
        //              |\
        // x = call H   | \
        //              v  v
        //              0  x
        //
        return generateFlow(CallSite, this->getZeroValue()); // FIXMEMM
      }
    }
    // Just use the auto mapping for values; pointer parameters and global
    // variables are killed and handled by getCallFlowfunction() and
    // getRetFlowFunction().
    // However, if only declarations are available as callee targets we would
    // lose the data-flow facts involved in the call which is usually not the
    // behavior that is intended. In that case, we must propagate all data-flow
    // facts alongside the call site.
    bool OnlyDecls = true;
    bool AllVoidRetTys = true;
    for (auto Callee : Callees) {
      if (!Callee->isDeclaration()) {
        OnlyDecls = false;
      }
      if (!Callee->getReturnType()->isVoidTy()) {
        AllVoidRetTys = false;
      }
    }

    return lambdaFlow<d_t>([CallSite = llvm::cast<llvm::CallBase>(CallSite),
                            OnlyDecls,
                            AllVoidRetTys](d_t Source) -> container_type {
      // There are a few things to consider, in case only declarations of
      // callee targets are available.
      if (OnlyDecls) {
        if (!AllVoidRetTys) {
          // If one or more of the declaration-only targets return a value, it
          // must be generated from zero!
          if (isZeroValueImpl(Source)) {
            return {Source, Source.getSameAP(CallSite)}; // FIXMEMM
          }
        }
        // If all declaration-only callee targets return void, just pass
        // everything as identity.
        return {Source};
      }
      // Do not pass global variables if definitions of the callee
      // function(s) are available, since the effect of the callee on these
      // values will be modelled using combined getCallFlowFunction and
      // getReturnFlowFunction.
      if (llvm::isa<llvm::Constant>(Source.getBaseValue())) {
        return {};
      }
      // Pass everything else as identity. In particular, also do not kill
      // pointer or reference parameters since this then also captures usages
      // oft the parameters, which we wish to compute using this analysis.
      return {Source};
    });
  }

  inline FlowFunctionPtrType
  getSummaryFlowFunction(n_t /* CallSite */, f_t /* DestFun */) override {
    // Do not use user-crafted summaries.
    return nullptr;
  }

  inline InitialSeeds<n_t, d_t, l_t> initialSeeds() override {
    InitialSeeds<n_t, d_t, l_t> Seeds;
    std::set<const llvm::Function *> EntryPointFuns;
    for (const auto &EntryPoint : this->EntryPoints) {
      EntryPointFuns.insert(this->IRDB->getFunctionDefinition(EntryPoint));
    }
    // Set initial seeds at the required entry points and generate the global
    // variables using generalized initial seeds
    for (const auto *EntryPointFun : EntryPointFuns) {
      // Generate zero value at the entry points
      Seeds.addSeed(&EntryPointFun->front().front(), this->getZeroValue(),
                    bottomElement());
      // Generate formal parameters of entry points, e.g. main(). Formal
      // parameters will otherwise cause trouble by overriding alloca
      // instructions without being valid data-flow facts themselves.
      for (const auto &Arg : EntryPointFun->args()) {
        Seeds.addSeed(&EntryPointFun->front().front(),
                      d_t::getNonIndirectionValue(&Arg), Bottom{});
      }
      // Generate all global variables using generalized initial seeds

      for (const auto &G : this->IRDB->getModule()->globals()) {
        if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(&G)) {
          l_t InitialValues = BitVectorSet<e_t>();
          std::set<e_t> EdgeFacts;
          if (EdgeFactGen) {
            EdgeFacts = EdgeFactGen(GV);
            // fill BitVectorSet
            InitialValues =
                BitVectorSet<e_t>(EdgeFacts.begin(), EdgeFacts.end());
          }
          Seeds.addSeed(&EntryPointFun->front().front(),
                        d_t::getZeroOffsetDerefValue(GV), InitialValues);
        }
      }
    }
    return Seeds;
  }

  [[nodiscard]] inline d_t createZeroValue() const {
    // Create a special value to represent the zero value!
    return d_t::getNonIndirectionValue(LLVMZeroValue::getInstance()); // FIXMEMM
  }

  inline bool isZeroValue(d_t d) const override { return isZeroValueImpl(d); }

  // In addition provide specifications for the IDE parts.

  inline EdgeFunctionPtrType getNormalEdgeFunction(n_t Curr, d_t CurrNode,
                                                   n_t /* Succ */,
                                                   d_t SuccNode) override {
    PHASAR_LOG_LEVEL(DFADEBUG,
                     "Process edge: " << llvmIRToShortString(CurrNode) << " --"
                                      << llvmIRToString(Curr) << "--> "
                                      << llvmIRToShortString(SuccNode));
    //
    // Zero --> Zero edges
    //
    // Edge function:
    //
    //                     0
    //                     |
    // %i = instruction    | \x.BOT
    //                     v
    //                     0
    //
    if (isZeroValue(CurrNode) && isZeroValue(SuccNode)) {
      return EdgeIdentity<l_t>::getInstance();
    }
    // check if the user has registered a fact generator function
    l_t UserEdgeFacts = BitVectorSet<e_t>();
    std::set<e_t> EdgeFacts;
    if (EdgeFactGen) {
      EdgeFacts = EdgeFactGen(Curr);
      // fill BitVectorSet
      UserEdgeFacts = BitVectorSet<e_t>(EdgeFacts.begin(), EdgeFacts.end());
    }
    //
    // Zero --> Alloca edges
    //
    // Edge function:
    //
    //                0
    //                 \
    // %a = alloca      \ \x.x \cup { commit of('%a = alloca') }
    //                   v
    //                   a
    //
    if (isZeroValue(CurrNode) && Curr == SuccNode) {
      if (llvm::isa<llvm::AllocaInst>(Curr)) {
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
    }
    //
    // i --> i edges
    //
    // Edge function:
    //
    //                    i
    //                    |
    // %i = instruction   | \x.x \cup { commit of('%i = instruction') }
    //                    v
    //                    i
    //
    if (Curr == CurrNode && CurrNode == SuccNode) {
      return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
    }
    // Handle loads instruction
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Curr)) {
      //
      // y --> x
      //
      // Edge function:
      //
      //             y
      //              \
      // x = load y    \ \x.{ commit of('x = load y') } \cup { commits of y }
      //                v
      //                x
      //
      if (CurrNode.getBaseValue() == Load->getPointerOperand() &&
          Load == SuccNode) {
        IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      } else {
        //
        // y --> y
        //
        // Edge function:
        //
        //             y
        //             |
        // x = load y  | \x.x (loads do not modify the value that is loaded
        // from)
        //             v
        //             y
        //
        return EdgeIdentity<l_t>::getInstance();
      }
    }
    // Overrides at store instructions
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Curr)) {
      // Overriding edge with literal: kill all labels that are propagated
      // along the edge of the value that is overridden.
      //
      // x --> y
      //
      // Edge function:
      //
      // Let x be a literal.
      //
      //               y
      //               |
      // store x y     | \x.{} \cup { commit of('store x y') }
      //               v
      //               y
      //
      if (llvm::isa<llvm::ConstantData>(Store->getValueOperand()) &&
          CurrNode == SuccNode &&
          Store->getPointerOperand() == CurrNode.getBaseValue()) {
        // Add the original variable, i.e., memory location.
        return IIAAKillOrReplaceEF::createEdgeFunction(UserEdgeFacts);
      }
      // Kill all labels that are propagated along the edge of the
      // value/values that is/are overridden.
      //
      // y --> y
      //
      // Edge function:
      //
      //            y
      //            |
      // store x y  | \x.{}
      //            v
      //            y
      //
      if (CurrNode == SuccNode) {
        llvm::errs() << "Found store with 'CurrNode == SuccNode' at: " << *Store
                     << '\n';
        LLVMKFieldSensFlowFact StoreTarget;
        if (const auto *Gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                Store->getPointerOperand())) {
          // Store to array/struct.
          StoreTarget = LLVMKFieldSensFlowFact<>::getDerefValueFromGep(Gep);
        } else {
          // Store to plain alloca.
          StoreTarget = LLVMKFieldSensFlowFact<>::getNonIndirectionValue(
              Store->getPointerOperand());
        }
        llvm::errs() << "Compare: " << CurrNode << " with " << StoreTarget
                     << '\n';
        if (CurrNode == StoreTarget) {
          llvm::errs() << "Jessas!\n";
          return IIAAKillOrReplaceEF::createEdgeFunction(BitVectorSet<e_t>());
        }
        llvm::errs() << "Error Jessas not found!\n";
      }
      // Overriding edge: obtain labels from value to be stored (and may add
      // UserEdgeFacts, if any).
      //
      // x --> y
      //
      // Edge function:
      //
      //            x
      //             \
      // store x y    \ \x.x \cup { commit of('store x y') }
      //               v
      //               y
      //
      if (CurrNode.getBaseValue() == Store->getValueOperand() &&
          SuccNode.getBaseValue() == Store->getPointerOperand()) {
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
    }

    // FIXME: handle GEP here.
    if (llvm::isa<llvm::GetElementPtrInst>(Curr)) {
      return EdgeIdentity<l_t>::getInstance();
    }

    // Handle edge functions for general instructions.
    for (const auto &Op : Curr->operands()) {
      //
      // 0 --> o_i
      //
      // Edge function:
      //
      //                        0
      //                         \
      // %i = instruction o_i     \ \x.x \cup { commit of('%i = instruction') }
      //                           v
      //                           o_i
      //
      if (isZeroValue(CurrNode) && Op == SuccNode) {
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
      //
      // o_i --> o_i
      //
      // Edge function:
      //
      //                        o_i
      //                        |
      // %i = instruction o_i   | \x.x \cup { commit of('%i = instruction') }
      //                        v
      //                        o_i
      //
      if (Op == CurrNode && CurrNode == SuccNode) {
        IF_LOG_ENABLED({
          PHASAR_LOG_LEVEL(DFADEBUG, "this is 'i'\n");
          for (auto &EdgeFact : EdgeFacts) {
            PHASAR_LOG_LEVEL(DFADEBUG, EdgeFact << ", ");
          }
          PHASAR_LOG_LEVEL(DFADEBUG, '\n');
        });
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
      //
      // o_i --> i
      //
      // Edge function:
      //
      //                        o_i
      //                         \
      // %i = instruction o_i     \ \x.x \cup { commit of('%i = instruction') }
      //                           v
      //                           i
      //
      if (Op == CurrNode && Curr == SuccNode) {
        IF_LOG_ENABLED({
          PHASAR_LOG_LEVEL(DFADEBUG, "this is '0'\n");
          for (auto &EdgeFact : EdgeFacts) {
            PHASAR_LOG_LEVEL(DFADEBUG, EdgeFact << ", ");
          }
          PHASAR_LOG_LEVEL(DFADEBUG, '\n');
        });
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
    }
    // Otherwise stick to identity.
    return EdgeIdentity<l_t>::getInstance();
  }

  inline EdgeFunctionPtrType getCallEdgeFunction(n_t CallSite, d_t SrcNode,
                                                 f_t /* DestinationMethod */,
                                                 d_t DestNode) override {
    // Handle the case in which a parameter that has been artificially
    // introduced by the compiler is passed. Such a value must be generated from
    // the zero value, to reflact the fact that the data flows from the callee
    // to the caller (at least) according to the source code.
    //
    // Let a_i be an argument that is annotated by the sret attribute.
    //
    // 0 --> a_i
    //
    // Edge function:
    //
    //                      0
    //                       \
    // call/invoke f(a_i)     \ \x.{}
    //                         v
    //                         a_i
    //
    std::set<const llvm::Value *> SRetParams;
    const auto *CS = llvm::cast<llvm::CallBase>(CallSite);
    for (unsigned Idx = 0; Idx < CS->arg_size(); ++Idx) {
      if (CS->paramHasAttr(Idx, llvm::Attribute::StructRet)) {
        SRetParams.insert(CS->getArgOperand(Idx)); // FIXMEMM
      }
    }
    if (isZeroValue(SrcNode) && SRetParams.count(DestNode.getBaseValue())) {
      return IIAAAddLabelsEF::createEdgeFunction(BitVectorSet<e_t>());
    }
    // Everything else can be passed as identity.
    return EdgeIdentity<l_t>::getInstance();
  }

  inline EdgeFunctionPtrType
  getReturnEdgeFunction(n_t CallSite, f_t /* CalleeMethod */, n_t ExitInst,
                        d_t ExitNode, n_t /* RetSite */, d_t RetNode) override {
    // Handle the case in which constant data is returned, e.g. ret i32 42.
    //
    // Let c be the return instruction's corresponding call site.
    //
    // 0 --> c
    //
    // Edge function:
    //
    //               0
    //                \
    // ret x           \ \x.x \cup { commit of('ret x') }
    //                  v
    //                  c
    //
    if (isZeroValue(ExitNode) && RetNode.getBaseValue() == CallSite) {
      const auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(ExitInst);
      if (const auto *CD =
              llvm::dyn_cast<llvm::ConstantData>(Ret->getReturnValue())) {
        // Check if the user has registered a fact generator function
        l_t UserEdgeFacts = BitVectorSet<e_t>();
        std::set<e_t> EdgeFacts;
        if (EdgeFactGen) {
          EdgeFacts = EdgeFactGen(ExitInst);
          // fill BitVectorSet
          UserEdgeFacts = BitVectorSet<e_t>(EdgeFacts.begin(), EdgeFacts.end());
        }
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
    }
    // Everything else can be passed as identity.
    return EdgeIdentity<l_t>::getInstance();
  }

  inline EdgeFunctionPtrType
  getCallToRetEdgeFunction(n_t CallSite, d_t CallNode, n_t /* RetSite */,
                           d_t RetSiteNode,
                           llvm::ArrayRef<f_t> Callees) override {
    // Check if the user has registered a fact generator function
    l_t UserEdgeFacts = BitVectorSet<e_t>();
    std::set<e_t> EdgeFacts;
    if (EdgeFactGen) {
      EdgeFacts = EdgeFactGen(CallSite);
      // fill BitVectorSet
      UserEdgeFacts = BitVectorSet<e_t>(EdgeFacts.begin(), EdgeFacts.end());
    }
    // Model call to heap allocating functions (new, new[], malloc, etc.) --
    // only model direct calls, though.
    if (Callees.size() == 1) {
      for (const auto *Callee : Callees) {
        if (this->ICF->isHeapAllocatingFunction(Callee)) {
          // Let H be a heap allocating function.
          //
          // 0 --> x
          //
          // Edge function:
          //
          //               0
          //                \
          // %i = call H     \ \x.x \cup { commit of('%i = call H') }
          //                  v
          //                  i
          //
          if (isZeroValue(CallNode) && RetSiteNode.getBaseValue() == CallSite) {
            return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
          }
        }
      }
    }
    // Capture interactions of the call instruction and its arguments.
    const auto *CS = llvm::dyn_cast<llvm::CallBase>(CallSite);
    for (const auto &Arg : CS->args()) {
      //
      // o_i --> o_i
      //
      // Edge function:
      //
      //                 o_i
      //                 |
      // %i = call o_i   | \ \x.x \cup { commit of('%i = call H') }
      //                 v
      //                 o_i
      //
      if (CallNode == Arg && CallNode == RetSiteNode) {
        return IIAAAddLabelsEF::createEdgeFunction(UserEdgeFacts);
      }
    }
    // Otherwise stick to identity
    return EdgeIdentity<l_t>::getInstance();
  }

  inline EdgeFunctionPtrType
  getSummaryEdgeFunction(n_t /* CallSite */, d_t /* CallNode */,
                         n_t /* RetSite */, d_t /* RetSiteNode */) override {
    // Do not use user-crafted summaries.
    return nullptr;
  }

  inline l_t topElement() override { return Top{}; }

  inline l_t bottomElement() override { return Bottom{}; }

  inline l_t join(l_t Lhs, l_t Rhs) override { return joinImpl(Lhs, Rhs); }

  inline EdgeFunctionPtrType allTopFunction() override {
    return std::make_shared<AllTop<l_t>>();
  }

  // Provide some handy helper edge functions to improve reuse.

  // Edge function that kills all labels in a set (and may replaces them with
  // others).
  class IIAAKillOrReplaceEF
      : public EdgeFunction<l_t>,
        public std::enable_shared_from_this<IIAAKillOrReplaceEF>,
        public EdgeFunctionSingletonFactory<IIAAKillOrReplaceEF, l_t> {
  public:
    const l_t Replacement;

    explicit IIAAKillOrReplaceEF() noexcept : Replacement(BitVectorSet<e_t>()) {
      // PHASAR_LOG_LEVEL(DFADEBUG,
      //               << "IIAAKillOrReplaceEF");
    }

    explicit IIAAKillOrReplaceEF(l_t Replacement) noexcept
        : Replacement(std::move(Replacement)) {
      // PHASAR_LOG_LEVEL(DFADEBUG,
      //               << "IIAAKillOrReplaceEF");
    }

    ~IIAAKillOrReplaceEF() override = default;

    l_t computeTarget(l_t /* Src */) override { return Replacement; }

    EdgeFunctionPtrType
    composeWith(EdgeFunctionPtrType SecondFunction) override {
      // PHASAR_LOG_LEVEL(DFADEBUG,
      //               << "IIAAKillOrReplaceEF::composeWith(): " << this->str()
      //               << " * " << SecondFunction->str());
      if (dynamic_cast<AllTop<l_t> *>(SecondFunction.get())) {
        llvm::report_fatal_error("AllTop should not be composed with");
      }
      if (dynamic_cast<AllBottom<l_t> *>(SecondFunction.get())) {
        return SecondFunction;
      }
      if (dynamic_cast<EdgeIdentity<l_t> *>(SecondFunction.get())) {
        return this->shared_from_this();
      }
      if (auto *AD = dynamic_cast<IIAAAddLabelsEF *>(SecondFunction.get())) {
        if (isKillAll()) {
          return IIAAAddLabelsEF::createEdgeFunction(AD->Data);
        }
        auto Union = joinImpl(Replacement, AD->Data);
        return IIAAAddLabelsEF::createEdgeFunction(std::move(Union));
      }
      if (dynamic_cast<IIAAKillOrReplaceEF *>(SecondFunction.get())) {
        // IIAAKillOrReplaceEF ignores its input
        return SecondFunction;
      }
      llvm::report_fatal_error(
          "found unexpected edge function in 'IIAAKillOrReplaceEF'");
    }

    EdgeFunctionPtrType joinWith(EdgeFunctionPtrType OtherFunction) override {
      // PHASAR_LOG_LEVEL(DFADEBUG,  <<
      // "IIAAKillOrReplaceEF::joinWith");
      if (dynamic_cast<AllTop<l_t> *>(OtherFunction.get())) {
        return this->shared_from_this();
      }
      if (dynamic_cast<AllBottom<l_t> *>(OtherFunction.get())) {
        return OtherFunction;
      }
      if (dynamic_cast<EdgeIdentity<l_t> *>(OtherFunction.get())) {
        /// XXX: Check for plausability
        return this->shared_from_this();
      }
      if (auto *AD = dynamic_cast<IIAAAddLabelsEF *>(OtherFunction.get())) {
        auto Union = joinImpl(Replacement, AD->Data);
        return IIAAAddLabelsEF::createEdgeFunction(std::move(Union));
      }
      if (auto *KR = dynamic_cast<IIAAKillOrReplaceEF *>(OtherFunction.get())) {
        auto Union = joinImpl(Replacement, KR->Replacement);
        return IIAAKillOrReplaceEF::createEdgeFunction(std::move(Union));
      }
      llvm::report_fatal_error(
          "found unexpected edge function in 'IIAAKillOrReplaceEF'");
    }

    bool equal_to(EdgeFunctionPtrType Other) const override {
      if (auto *KR = dynamic_cast<IIAAKillOrReplaceEF *>(Other.get())) {
        return Replacement == KR->Replacement;
      }
      return false;
    }

    void print(llvm::raw_ostream &OS,
               bool /* IsForDebug */ = false) const override {
      OS << "EF: (IIAAKillOrReplaceEF)<->";
      if (isKillAll()) {
        OS << "(KillAll";
      } else {
        IDEInstInteractionAnalysisT::printEdgeFactImpl(OS, Replacement);
      }
      OS << ")";
    }

    [[nodiscard]] bool isKillAll() const {
      if (auto *RSet = Replacement.getValueOrNull()) {
        return RSet->empty();
      }
      return false;
    }
  };

  // Edge function that adds the given labels to existing labels
  // add all labels provided by Data.
  class IIAAAddLabelsEF
      : public EdgeFunction<l_t>,
        public std::enable_shared_from_this<IIAAAddLabelsEF>,
        public EdgeFunctionSingletonFactory<IIAAAddLabelsEF, l_t> {
  public:
    const l_t Data;

    explicit IIAAAddLabelsEF(l_t Data) noexcept : Data(std::move(Data)) {
      // PHASAR_LOG_LEVEL(DFADEBUG,  << "IIAAAddLabelsEF");
    }

    ~IIAAAddLabelsEF() override = default;

    l_t computeTarget(l_t Src) override { return joinImpl(Src, Data); }

    EdgeFunctionPtrType
    composeWith(EdgeFunctionPtrType SecondFunction) override {
      // PHASAR_LOG_LEVEL(DFADEBUG,
      //               << "IIAAAddLabelEF::composeWith(): " << this->str() << "
      //               * "
      //               << SecondFunction->str());
      if (dynamic_cast<AllTop<l_t> *>(SecondFunction.get())) {
        llvm::report_fatal_error("AllTop should not be composed with");
      }
      if (dynamic_cast<AllBottom<l_t> *>(SecondFunction.get())) {
        return SecondFunction;
      }
      if (dynamic_cast<EdgeIdentity<l_t> *>(SecondFunction.get())) {
        return this->shared_from_this();
      }
      if (auto *AD = dynamic_cast<IIAAAddLabelsEF *>(SecondFunction.get())) {
        auto Union = joinImpl(Data, AD->Data);
        return IIAAAddLabelsEF::createEdgeFunction(std::move(Union));
      }
      if (dynamic_cast<IIAAKillOrReplaceEF *>(SecondFunction.get())) {
        return SecondFunction;
      }
      llvm::report_fatal_error(
          "found unexpected edge function in 'IIAAAddLabelsEF'");
    }

    EdgeFunctionPtrType joinWith(EdgeFunctionPtrType OtherFunction) override {
      // PHASAR_LOG_LEVEL(DFADEBUG,  <<
      // "IIAAAddLabelsEF::joinWith");
      if (dynamic_cast<AllTop<l_t> *>(OtherFunction.get())) {
        return this->shared_from_this();
      }
      if (dynamic_cast<AllBottom<l_t> *>(OtherFunction.get())) {
        return OtherFunction;
      }
      if (dynamic_cast<EdgeIdentity<l_t> *>(OtherFunction.get())) {
        /// XXX: Check for plausability
        return this->shared_from_this();
      }
      if (auto *AD = dynamic_cast<IIAAAddLabelsEF *>(OtherFunction.get())) {
        auto Union = joinImpl(Data, AD->Data);
        return IIAAAddLabelsEF::createEdgeFunction(std::move(Union));
      }
      if (auto *KR = dynamic_cast<IIAAKillOrReplaceEF *>(OtherFunction.get())) {
        auto Union = joinImpl(Data, KR->Replacement);
        return IIAAAddLabelsEF::createEdgeFunction(std::move(Union));
      }
      llvm::report_fatal_error(
          "found unexpected edge function in 'IIAAAddLabelsEF'");
    }

    [[nodiscard]] bool equal_to(EdgeFunctionPtrType Other) const override {
      if (auto *AS = dynamic_cast<IIAAAddLabelsEF *>(Other.get())) {
        return (Data == AS->Data);
      }
      return false;
    }

    void print(llvm::raw_ostream &OS,
               bool /* IsForDebug */ = false) const override {
      OS << "EF: (IIAAAddLabelsEF: ";
      IDEInstInteractionAnalysisT::printEdgeFactImpl(OS, Data);
      OS << ")";
    }
  };

  // Provide functionalities for printing things and emitting text reports.

  void printNode(llvm::raw_ostream &OS, n_t n) const override {
    OS << llvmIRToString(n);
  }

  void printDataFlowFact(llvm::raw_ostream &OS, d_t FlowFact) const override {
    FlowFact.print(OS);
  }

  void printFunction(llvm::raw_ostream &OS, f_t Fun) const override {
    OS << Fun->getName();
  }

  inline void printEdgeFact(llvm::raw_ostream &OS,
                            l_t EdgeFact) const override {
    printEdgeFactImpl(OS, EdgeFact);
  }

  static void stripBottomResults(std::unordered_map<d_t, l_t> &Res) {
    for (auto It = Res.begin(); It != Res.end();) {
      if (It->second.isBottom()) {
        It = Res.erase(It);
      } else {
        ++It;
      }
    }
  }

  void emitTextReport(const SolverResults<n_t, d_t, l_t> &SR,
                      llvm::raw_ostream &OS = llvm::outs()) override {
    OS << "\n====================== IDE-Inst-Interaction-Analysis Report "
          "======================\n";
    // if (!IRDB->debugInfoAvailable()) {
    //   // Emit only IR code, function name and module info
    //   OS << "\nWARNING: No Debug Info available - emiting results without "
    //         "source code mapping!\n";
    for (const auto *f : this->ICF->getAllFunctions()) {
      std::string FunName = getFunctionNameFromIR(f);
      OS << "\nFunction: " << FunName << "\n----------"
         << std::string(FunName.size(), '-') << '\n';
      for (const auto *Inst : this->ICF->getAllInstructionsOf(f)) {
        auto Results = SR.resultsAt(Inst, true);
        stripBottomResults(Results);
        if (!Results.empty()) {
          OS << "At IR statement: " << this->NtoString(Inst) << '\n';
          for (auto Result : Results) {
            if (!Result.second.isBottom()) {
              OS << "   Fact: " << this->DtoString(Result.first)
                 << "\n  Value: " << this->LtoString(Result.second) << '\n';
            }
          }
          OS << '\n';
        }
      }
      OS << '\n';
    }
    // } else {
    // TODO: implement better report in case debug information are available
    //   }
  }

  /// Computes all variables where a result set has been computed using the
  /// edge functions (and respective value domain).
  inline std::unordered_set<d_t>
  getAllVariables(const SolverResults<n_t, d_t, l_t> & /* Solution */) const {
    std::unordered_set<d_t> Variables;
    // collect all variables that are available
    const llvm::Module *M = this->IRDB->getModule();
    for (const auto &G : M->globals()) {
      Variables.insert(&G);
    }
    for (const auto *I : this->IRDB->getAllInstructions()) {
      if (const auto *A = llvm::dyn_cast<llvm::AllocaInst>(I)) {
        Variables.insert(A);
      }
      if (const auto *H = llvm::dyn_cast<llvm::CallBase>(I)) {
        if (!H->isIndirectCall() && H->getCalledFunction() &&
            this->ICF->isHeapAllocatingFunction(H->getCalledFunction())) {
          Variables.insert(H);
        }
      }
    }

    return Variables;
  }

  /// Computes all variables for which an empty set has been computed using the
  /// edge functions (and respective value domain).
  inline std::unordered_set<d_t> getAllVariablesWithEmptySetValue(
      const SolverResults<n_t, d_t, l_t> &Solution) const {
    return removeVariablesWithoutEmptySetValue(Solution,
                                               getAllVariables(Solution));
  }

protected:
  static inline bool isZeroValueImpl(d_t d) {
    return LLVMZeroValue::isLLVMZeroValue(d);
  }

  static void printEdgeFactImpl(llvm::raw_ostream &OS, const l_t &EdgeFact) {
    if (std::holds_alternative<Top>(EdgeFact)) {
      OS << std::get<Top>(EdgeFact);
    } else if (std::holds_alternative<Bottom>(EdgeFact)) {
      OS << std::get<Bottom>(EdgeFact);
    } else {
      auto LSet = std::get<BitVectorSet<e_t>>(EdgeFact);
      OS << "(set size: " << LSet.size() << ") values: ";
      if constexpr (std::is_same_v<e_t, vara::Taint *>) {
        for (const auto &LElem : LSet) {
          std::string IRBuffer;
          llvm::raw_string_ostream RSO(IRBuffer);
          LElem->print(RSO);
          RSO.flush();
          OS << IRBuffer << ", ";
        }
      } else {
        for (const auto &LElem : LSet) {
          OS << LElem << ", ";
        }
      }
    }
  }

  static inline l_t joinImpl(const l_t &Lhs, const l_t &Rhs) {
    /// XXX: Check for plausability
    if (Lhs.isTop() || Lhs.isBottom()) {
      return Rhs;
    }
    if (Rhs.isTop() || Rhs.isBottom()) {
      return Lhs;
    }
    const auto &LhsSet = std::get<BitVectorSet<e_t>>(Lhs);
    const auto &RhsSet = std::get<BitVectorSet<e_t>>(Rhs);
    return LhsSet.setUnion(RhsSet);
    LLVMKFieldSensFlowFact<> foo;
    foo.print(llvm::outs());
    foo.getLoaded(nullptr);
    foo.getWithOffset(nullptr);
  }

private:
  // KFieldSensFlowFact<const llvm::Value *> KFSFF,
  //     KFSFF2 = KFSFF.getStored(nullptr), KFSFF3 = KFSFF.getWithOffset(42),
  //     KFSFF4 = KFSFF.getFirstOverapproximated();
  // // LLVMKFieldSensFlowFact<0> foo;
  // // LLVMKFieldSensFlowFact<0, 0, const llvm::Value*> foo2 = foo;
  // std::optional<KFieldSensFlowFact<const llvm::Value *>> KFSFF5 =
  //     KFSFF.getLoaded(nullptr, 64); // FIXME just make it compile for now.
  /// Filters out all variables that had a non-empty set during edge functions
  /// computations.
  inline std::unordered_set<d_t> removeVariablesWithoutEmptySetValue(
      const SolverResults<n_t, d_t, l_t> &Solution,
      std::unordered_set<d_t> Variables) const {
    // Check the solver results and remove all variables for which a
    // non-empty set has been computed
    auto Results = Solution.getAllResultEntries();
    for (const auto &Result : Results) {
      // We do not care for the concrete instruction at which data-flow facts
      // hold, instead we just wish to find out if a variable has been generated
      // at some point. Therefore, we only care for the variables and their
      // associated values and ignore at which point a variable may holds as a
      // data-flow fact.
      const auto Variable = Result.getColumnKey();
      const auto &Value = Result.getValue();
      // skip result entry if variable is not in the set of all variables
      if (Variables.find(Variable) == Variables.end()) {
        continue;
      }
      // skip result entry if the computed value is not of type BitVectorSet
      if (!std::holds_alternative<BitVectorSet<e_t>>(Value)) {
        continue;
      }
      // remove variable from result set if a non-empty that has been computed
      auto &Values = std::get<BitVectorSet<e_t>>(Value);
      if (!Values.empty()) {
        Variables.erase(Variable);
      }
    }
    return Variables;
  }

  const LLVMBasedICFG *ICF{};
  LLVMAliasInfoRef PT{};
  std::function<EdgeFactGeneratorTy> EdgeFactGen;
  static inline const bool OnlyConsiderLocalAliases = true;

  inline BitVectorSet<e_t> edgeFactGenForInstToBitVectorSet(n_t CurrInst) {
    if (EdgeFactGen) {
      auto Results = EdgeFactGen(CurrInst);
      BitVectorSet<e_t> BVS(Results.begin(), Results.end());
      return BVS;
    }
    return {};
  }

  inline BitVectorSet<e_t>
  edgeFactGenForGlobalVarToBitVectorSet(const llvm::GlobalVariable *GlobalVar) {
    if (EdgeFactGen) {
      auto Results = EdgeFactGen(GlobalVar);
      BitVectorSet<e_t> BVS(Results.begin(), Results.end());
      return BVS;
    }
    return {};
  }
}; // namespace psr

using IDEInstInteractionAnalysis = IDEInstInteractionAnalysisT<>;

} // namespace psr

#endif

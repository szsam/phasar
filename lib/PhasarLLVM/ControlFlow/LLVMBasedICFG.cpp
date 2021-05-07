/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

/*
 * LLVMBasedInterproceduralICFG.cpp
 *
 *  Created on: 09.09.2016
 *      Author: pdschbrt
 */

#include <cassert>
#include <memory>

#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

#include "boost/graph/copy.hpp"
#include "boost/graph/depth_first_search.hpp"
#include "boost/graph/graph_utility.hpp"
#include "boost/graph/graphviz.hpp"

#include "phasar/PhasarLLVM/ControlFlow/LLVMBasedICFG.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/CHAResolver.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/DTAResolver.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/NOResolver.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/OTFResolver.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/RTAResolver.h"
#include "phasar/PhasarLLVM/ControlFlow/Resolver/Resolver.h"

#include "phasar/Utils/LLVMShorthands.h"
#include "phasar/Utils/Logger.h"
#include "phasar/Utils/PAMMMacros.h"
#include "phasar/Utils/Utilities.h"

#include "phasar/DB/ProjectIRDB.h"

#include "phasar/PhasarLLVM/Pointer/LLVMPointsToGraph.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToInfo.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToSet.h"

#include "phasar/PhasarLLVM/TypeHierarchy/LLVMTypeHierarchy.h"
#include "phasar/PhasarLLVM/TypeHierarchy/LLVMVFTable.h"

using namespace psr;
using namespace std;

// Define some handy helper functionalities
namespace {
template <class graphType> class VertexWriter {
public:
  VertexWriter(const graphType &CGraph) : CGraph(CGraph) {}
  template <class VertexOrEdge>
  void operator()(std::ostream &Out, const VertexOrEdge &V) const {
    Out << "[label=\"" << CGraph[V].getFunctionName() << "\"]";
  }

private:
  const graphType &CGraph;
};

template <class graphType> class EdgeLabelWriter {
public:
  EdgeLabelWriter(const graphType &CGraph) : CGraph(CGraph) {}
  template <class VertexOrEdge>
  void operator()(std::ostream &Out, const VertexOrEdge &V) const {
    Out << "[label=\"" << CGraph[V].getCallSiteAsString() << "\"]";
  }

private:
  const graphType &CGraph;
};

} // anonymous namespace

namespace psr {

struct LLVMBasedICFG::dependency_visitor : boost::default_dfs_visitor {
  std::vector<vertex_t> &Vertices;
  dependency_visitor(std::vector<vertex_t> &V) : Vertices(V) {}
  template <typename Vertex, typename Graph>
  void finish_vertex(Vertex U, const Graph &G) {
    Vertices.push_back(U);
  }
};

LLVMBasedICFG::VertexProperties::VertexProperties(const llvm::Function *F)
    : F(F) {}

std::string LLVMBasedICFG::VertexProperties::getFunctionName() const {
  return F->getName().str();
}

LLVMBasedICFG::EdgeProperties::EdgeProperties(const llvm::Instruction *I)
    : CS(I), ID(stoull(getMetaDataID(I))) {}

std::string LLVMBasedICFG::EdgeProperties::getCallSiteAsString() const {
  return llvmIRToString(CS);
}

// Need to provide copy constructor explicitly to avoid multiple frees of TH and
// PT in case any of them is allocated within the constructor. To this end, we
// set UserTHInfos and UserPTInfos to true here.
LLVMBasedICFG::LLVMBasedICFG(const LLVMBasedICFG &ICF)
    : IRDB(ICF.IRDB), CGType(ICF.CGType), S(ICF.S), TH(ICF.TH), PT(ICF.PT),
      // TODO copy resolver
      Res(nullptr), VisitedFunctions(ICF.VisitedFunctions),
      CallGraph(ICF.CallGraph), FunctionVertexMap(ICF.FunctionVertexMap) {}

LLVMBasedICFG::LLVMBasedICFG(ProjectIRDB &IRDB, CallGraphAnalysisType CGType,
                             const std::set<std::string> &EntryPoints,
                             LLVMTypeHierarchy *TH, LLVMPointsToInfo *PT,
                             Soundness S, bool IncludeGlobals)
    : IRDB(IRDB), CGType(CGType), S(S), TH(TH), PT(PT) {
  PAMM_GET_INSTANCE;
  // check for faults in the logic
  if (!TH && (CGType != CallGraphAnalysisType::NORESOLVE)) {
    // no type hierarchy information provided by the user,
    // we need to construct a type hierarchy ourselfes
    this->TH = new LLVMTypeHierarchy(IRDB);
    UserTHInfos = false;
  }
  if (!PT && (CGType == CallGraphAnalysisType::OTF)) {
    // no pointer information provided by the user,
    // we need to construct a points-to infos ourselfes
    this->PT = new LLVMPointsToSet(IRDB);
    UserPTInfos = false;
  }
  // instantiate the respective resolver type
  Res = makeResolver(IRDB, CGType, *this->TH, *this->PT);
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), INFO)
                << "Starting CallGraphAnalysisType: " << CGType);
  VisitedFunctions.reserve(IRDB.getAllFunctions().size());

  if (IncludeGlobals) {
    collectGlobalCtors();
    collectGlobalDtors();
    collectGlobalInitializers();
    collectRegisteredDtors();
    // push registered destructors to function worklist
    for (const auto &RegisteredDtor : getRegisteredDtors()) {
      FunctionWL.push_back(RegisteredDtor);
    }
    // push global destructors to function worklist
    // for (const auto &GlobalDtor : getGlobalDtors()) {
    //   FunctionWL.push(GlobalDtor);
    // }
    appendGlobalDtors(FunctionWL);
  }

  for (const auto &EntryPoint : EntryPoints) {
    const llvm::Function *F = IRDB.getFunctionDefinition(EntryPoint);
    UserEntryPoints.insert(F);
    if (F == nullptr) {
      llvm::report_fatal_error("Could not retrieve function for entry point");
    }
    FunctionWL.push_back(F);
  }

  if (IncludeGlobals) {
    // push global constructors to function worklist
    // for (const auto &GlobalCtor : getGlobalCtors()) {
    //   FunctionWL.push(GlobalCtor);
    // }
    appendGlobalCtors(FunctionWL);
  }

  bool FixpointReached;
  do {
    FixpointReached = true;
    while (!FunctionWL.empty()) {
      const llvm::Function *F = FunctionWL.back();
      FunctionWL.pop_back();
      processFunction(F, *Res, FixpointReached);
    }
    for (const auto &[Callsite, _] : IndirectCalls) {
      FixpointReached &= !constructDynamicCall(Callsite, *Res);
    }
  } while (!FixpointReached);
  for (const auto &[IndirectCall, Targets] : IndirectCalls) {
    if (Targets == 0) {
      LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), WARNING)
                    << "No callees found for callsite "
                    << llvmIRToString(IndirectCall));
    }
  }
  REG_COUNTER("CG Vertices", getNumOfVertices(), PAMM_SEVERITY_LEVEL::Full);
  REG_COUNTER("CG Edges", getNumOfEdges(), PAMM_SEVERITY_LEVEL::Full);
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), INFO)
                << "Call graph has been constructed");
}

LLVMBasedICFG::~LLVMBasedICFG() {
  // if we had to compute type hierarchy or points-to information ourselfs,
  // we need to clean up
  if (!UserTHInfos) {
    delete TH;
  }
  if (!UserPTInfos) {
    delete PT;
  }
}

void LLVMBasedICFG::processFunction(const llvm::Function *F, Resolver &Resolver,
                                    bool &FixpointReached) {
  PAMM_GET_INSTANCE;
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                << "Walking in function: " << F->getName().str());
  if (F->isDeclaration() || !VisitedFunctions.insert(F).second) {
    LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                  << "Function already visited or only declaration: "
                  << F->getName().str());
    return;
  }

  // add a node for function F to the call graph (if not present already)
  vertex_t ThisFunctionVertexDescriptor;
  auto FvmItr = FunctionVertexMap.find(F);
  if (FvmItr != FunctionVertexMap.end()) {
    ThisFunctionVertexDescriptor = FvmItr->second;
  } else {
    ThisFunctionVertexDescriptor =
        boost::add_vertex(VertexProperties(F), CallGraph);
    FunctionVertexMap[F] = ThisFunctionVertexDescriptor;
  }

  // iterate all instructions of the current function
  for (const auto &BB : *F) {
    for (const auto &I : BB) {
      if (llvm::isa<llvm::CallInst>(I) || llvm::isa<llvm::InvokeInst>(I)) {
        Resolver.preCall(&I);

        llvm::ImmutableCallSite CS(&I);
        set<const llvm::Function *> PossibleTargets;
        // check if function call can be resolved statically
        if (CS.getCalledFunction() != nullptr) {
          PossibleTargets.insert(CS.getCalledFunction());
          LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                        << "Found static call-site: ");
          LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                        << "  " << llvmIRToString(CS.getInstruction()));
        } else {
          // still try to resolve the called function statically
          const llvm::Value *SV = CS.getCalledValue()->stripPointerCasts();
          const llvm::Function *ValueFunction =
              !SV->hasName() ? nullptr : IRDB.getFunction(SV->getName());
          if (ValueFunction) {
            PossibleTargets.insert(ValueFunction);
            LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                          << "Found static call-site: "
                          << llvmIRToString(CS.getInstruction()));
          } else {
            // the function call must be resolved dynamically
            LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                          << "Found dynamic call-site: ");
            LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                          << "  " << llvmIRToString(CS.getInstruction()));
            IndirectCalls[&I] = 0;
            FixpointReached = false;
            continue;
          }
        }

        LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                      << "Found " << PossibleTargets.size()
                      << " possible target(s)");

        Resolver.handlePossibleTargets(CS, PossibleTargets);
        // Insert possible target inside the graph and add the link with
        // the current function
        for (const auto &PossibleTarget : PossibleTargets) {
          vertex_t TargetVertex;
          auto TargetFvmItr = FunctionVertexMap.find(PossibleTarget);
          if (TargetFvmItr != FunctionVertexMap.end()) {
            TargetVertex = TargetFvmItr->second;
          } else {
            TargetVertex =
                boost::add_vertex(VertexProperties(PossibleTarget), CallGraph);
            FunctionVertexMap[PossibleTarget] = TargetVertex;
          }
          boost::add_edge(ThisFunctionVertexDescriptor, TargetVertex,
                          EdgeProperties(CS.getInstruction()), CallGraph);
        }

        // continue resolving
        for (const auto *PossibleTarget : PossibleTargets) {
          FunctionWL.push_back(PossibleTarget);
        }

        Resolver.postCall(&I);
      } else {
        Resolver.otherInst(&I);
      }
    }
  }
}

bool LLVMBasedICFG::constructDynamicCall(const llvm::Instruction *I,
                                         Resolver &Resolver) {
  PAMM_GET_INSTANCE;
  bool NewTargetsFound = false;
  // Find vertex of calling function.
  vertex_t ThisFunctionVertexDescriptor;
  auto FvmItr = FunctionVertexMap.find(I->getFunction());
  if (FvmItr != FunctionVertexMap.end()) {
    ThisFunctionVertexDescriptor = FvmItr->second;
  } else {
    LOG_IF_ENABLE(
        BOOST_LOG_SEV(lg::get(), ERROR)
        << "constructDynamicCall: Did not find vertex of calling function "
        << I->getFunction()->getName().str() << " at callsite "
        << llvmIRToString(I));
    std::terminate();
  }

  if (llvm::isa<llvm::CallInst>(I) || llvm::isa<llvm::InvokeInst>(I)) {
    Resolver.preCall(I);
    llvm::ImmutableCallSite CS(I);
    set<const llvm::Function *> PossibleTargets;
    // the function call must be resolved dynamically
    LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                  << "Looking into dynamic call-site: ");
    LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG) << "  " << llvmIRToString(I));
    // call the resolve routine
    if (LLVMBasedICFG::isVirtualFunctionCall(CS.getInstruction())) {
      PossibleTargets = Resolver.resolveVirtualCall(CS);
    } else {
      PossibleTargets = Resolver.resolveFunctionPointer(CS);
    }
    if (IndirectCalls.count(I) == 0 ||
        IndirectCalls[I] < PossibleTargets.size()) {
      LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                    << "Found " << PossibleTargets.size() - IndirectCalls[I]
                    << " new possible target(s)");
      IndirectCalls[I] = PossibleTargets.size();
      NewTargetsFound = true;
    }
    if (!NewTargetsFound) {
      return NewTargetsFound;
    }
    // Throw out already found targets
    for (const auto &OE : boost::make_iterator_range(
             boost::out_edges(ThisFunctionVertexDescriptor, CallGraph))) {
      if (CallGraph[OE].CS == I) {
        PossibleTargets.erase(CallGraph[boost::target(OE, CallGraph)].F);
      }
    }
    Resolver.handlePossibleTargets(CS, PossibleTargets);
    // Insert possible target inside the graph and add the link with
    // the current function
    for (const auto &PossibleTarget : PossibleTargets) {
      vertex_t TargetVertex;
      auto TargetFvmItr = FunctionVertexMap.find(PossibleTarget);
      if (TargetFvmItr != FunctionVertexMap.end()) {
        TargetVertex = TargetFvmItr->second;
      } else {
        TargetVertex =
            boost::add_vertex(VertexProperties(PossibleTarget), CallGraph);
        FunctionVertexMap[PossibleTarget] = TargetVertex;
      }
      boost::add_edge(ThisFunctionVertexDescriptor, TargetVertex,
                      EdgeProperties(I), CallGraph);
    }

    // continue resolving
    for (const auto *PossibleTarget : PossibleTargets) {
      FunctionWL.push_back(PossibleTarget);
    }

    Resolver.postCall(I);
  } else {
    Resolver.otherInst(I);
  }
  return NewTargetsFound;
}

std::unique_ptr<Resolver> LLVMBasedICFG::makeResolver(ProjectIRDB &IRDB,
                                                      CallGraphAnalysisType CGT,
                                                      LLVMTypeHierarchy &TH,
                                                      LLVMPointsToInfo &PT) {
  switch (CGType) {
  case (CallGraphAnalysisType::NORESOLVE):
    return make_unique<NOResolver>(IRDB);
    break;
  case (CallGraphAnalysisType::CHA):
    return make_unique<CHAResolver>(IRDB, TH);
    break;
  case (CallGraphAnalysisType::RTA):
    return make_unique<RTAResolver>(IRDB, TH);
    break;
  case (CallGraphAnalysisType::DTA):
    return make_unique<DTAResolver>(IRDB, TH);
    break;
  case (CallGraphAnalysisType::OTF):
    return make_unique<OTFResolver>(IRDB, TH, *this, PT);
    break;
  default:
    llvm::report_fatal_error("Resolver strategy not properly instantiated");
    break;
  }
}

bool LLVMBasedICFG::isIndirectFunctionCall(const llvm::Instruction *N) const {
  llvm::ImmutableCallSite CS(N);
  return CS.isIndirectCall();
}

bool LLVMBasedICFG::isVirtualFunctionCall(const llvm::Instruction *N) const {
  llvm::ImmutableCallSite CS(N);
  // check potential receiver type
  const auto *RecType = getReceiverType(CS);
  if (!RecType) {
    return false;
  }
  if (!TH->hasType(RecType)) {
    return false;
  }
  if (!TH->hasVFTable(RecType)) {
    return false;
  }
  return getVFTIndex(CS) >= 0;
}

const llvm::Function *LLVMBasedICFG::getFunction(const string &Fun) const {
  return IRDB.getFunction(Fun);
}

std::vector<const llvm::Instruction *>
LLVMBasedICFG::getPredsOf(const llvm::Instruction *Inst) const {
  if (!IgnoreDbgInstructions) {
    if (Inst->getPrevNode()) {
      return {Inst->getPrevNode()};
    }
  } else {
    if (Inst->getPrevNonDebugInstruction()) {
      return {Inst->getPrevNonDebugInstruction()};
    }
  }
  // If we do not have a predecessor yet, look for basic blocks which
  // lead to our instruction in question!
  {
    vector<const llvm::Instruction *> Preds;
    std::transform(llvm::pred_begin(Inst->getParent()),
                   llvm::pred_end(Inst->getParent()), back_inserter(Preds),
                   [](const llvm::BasicBlock *BB) {
                     assert(BB && "BB under analysis was not well formed.");
                     return BB->getTerminator();
                   });

    if (!Preds.empty())
      return Preds;
  }

  auto getAllUserExitPoints = [](const auto &UserEntryPoints, auto &IRDB) {
    // goto all exit points of the UserEntryPoints and all calls of
    // std::exit()
    std::vector<const llvm::Instruction *> ret;

    for (const llvm::Function *UserEntry : UserEntryPoints) {
      appendAllExitPoints(UserEntry, ret);
    }

    const llvm::Function *ExitFn = IRDB.getFunction("exit");
    if (!ExitFn)
      return ret;

    for (auto *ExitCall : ExitFn->users()) {
      if (auto ExitInst = llvm::dyn_cast<llvm::Instruction>(ExitCall)) {
        ret.push_back(ExitInst);
      }
    }

    return ret;
  };

  if (auto globCtorIt = GlobalCtorFn.find(Inst->getFunction());
      globCtorIt != GlobalCtorFn.end()) {
    auto it = globCtorIt->second;
    if (it != GlobalCtors.begin()) {
      return getAllExitPoints(std::prev(it)->second);
    } // else: all global ctors have been completed
  } else if (auto globDtorIt = GlobalDtorFn.find(Inst->getFunction());
             globDtorIt != GlobalDtorFn.end()) {
    auto it = globDtorIt->second;
    if (it != GlobalDtors.begin()) {
      return getAllExitPoints(std::prev(it)->second);
    } else {
      return getAllUserExitPoints(UserEntryPoints, IRDB);
    }
  } else if (auto registeredIt = RegisteredDtorIndex.find(Inst->getFunction());
             registeredIt != RegisteredDtorIndex.end()) {
    if (registeredIt->second > 0) {
      return getAllExitPoints(RegisteredDtors[registeredIt->second - 1]);
    } else if (!GlobalDtors.empty()) {
      return getAllExitPoints(GlobalDtors.rbegin()->second);
    } else {
      return getAllUserExitPoints(UserEntryPoints, IRDB);
    }
  }

  return {};
}

std::vector<const llvm::Instruction *>
LLVMBasedICFG::getSuccsOf(const llvm::Instruction *Inst) const {
  vector<const llvm::Instruction *> Successors;
  // case we wish to consider LLVM's debug instructions
  if (!IgnoreDbgInstructions) {
    if (Inst->getNextNode()) {
      Successors.push_back(Inst->getNextNode());
    }
  } else {
    if (Inst->getNextNonDebugInstruction()) {
      Successors.push_back(Inst->getNextNonDebugInstruction());
    }
  }

  if (!Inst->isTerminator())
    return Successors;

  // TODO: Do we have more (e.g. exceptional) flows apart from ret?
  if (llvm::isa<llvm::ReturnInst>(Inst)) {
    // Handle artificial intraprocedural flow between global(C|D)tors
    if (auto globCtorIt = GlobalCtorFn.find(Inst->getFunction());
        globCtorIt != GlobalCtorFn.end()) {
      auto it = std::next(globCtorIt->second);
      if (it != GlobalCtors.end()) {
        // Flow to next global ctor
        Successors.push_back(&it->second->front().front());
      } else {
        // The end of the global ctor chain has been reached.
        // => start with the user entrypoints
        Successors.reserve(UserEntryPoints.size());
        for (const auto *F : UserEntryPoints) {
          Successors.push_back(&F->front().front());
        }
      }
    } else if (auto globDtorIt = GlobalDtorFn.find(Inst->getFunction());
               globDtorIt != GlobalDtorFn.end()) {

      if (auto it = std::next(globDtorIt->second); it != GlobalDtors.end()) {
        // Flow to next global dtor
        Successors.push_back(&it->second->front().front());
      } else if (!RegisteredDtors.empty()) {
        // We have no further regular global dtor, but at least one that was
        // registered using __cxa_atexit
        Successors.push_back(&RegisteredDtors[0]->front().front());
      }
      // else: we have reached the end of the program
    } else if (auto registeredIt =
                   RegisteredDtorIndex.find(Inst->getFunction());
               registeredIt != RegisteredDtorIndex.end()) {
      if (registeredIt->second < RegisteredDtors.size() - 1) {
        // Flow to next registered dtor
        Successors.push_back(
            &RegisteredDtors[registeredIt->second + 1]->front().front());
      }
    } else if (UserEntryPoints.count(Inst->getFunction())) {
      if (auto it = GlobalDtors.begin(); it != GlobalDtors.end()) {
        // The end of the user-entrypoint has been reached
        // => start with first global dtor
        // Ignore unbalanced returns for now
        Successors.push_back(&it->second->front().front());
      } else if (!RegisteredDtors.empty()) {
        // We have no regular global dtor, but at least one that was registered
        // using __cxa_atexit
        Successors.push_back(&RegisteredDtors[0]->front().front());
      }
    }
  } else if (auto Call = llvm::dyn_cast<llvm::CallBase>(Inst);
             Call && Call->getCalledFunction() &&
             Call->getCalledFunction()->getName() == "exit") {
    if (auto it = GlobalDtors.begin(); it != GlobalDtors.end()) {
      Successors.push_back(&it->second->front().front());
    }
  }

  Successors.reserve(Inst->getNumSuccessors() + Successors.size());
  std::transform(llvm::succ_begin(Inst), llvm::succ_end(Inst),
                 back_inserter(Successors),
                 [](const llvm::BasicBlock *BB) { return &BB->front(); });

  return Successors;
}

const llvm::Function *LLVMBasedICFG::getFirstGlobalCtorOrNull() const {
  auto it = GlobalCtors.begin();
  if (it != GlobalCtors.end())
    return it->second;

  return nullptr;
}
const llvm::Function *LLVMBasedICFG::getLastGlobalDtorOrNull() const {
  if (!RegisteredDtors.empty())
    return RegisteredDtors.back();

  auto it = GlobalDtors.rbegin();
  if (it != GlobalDtors.rend())
    return it->second;

  return nullptr;
}

set<const llvm::Function *> LLVMBasedICFG::getAllFunctions() const {
  return IRDB.getAllFunctions();
}

boost::container::flat_set<const llvm::Function *>
LLVMBasedICFG::getAllVertexFunctions() const {
  boost::container::flat_set<const llvm::Function *> vertexFuncs;
  vertexFuncs.reserve(FunctionVertexMap.size());
  for (auto v : FunctionVertexMap) {
    vertexFuncs.insert(v.first);
  }
  return vertexFuncs;
}

std::vector<const llvm::Instruction *>
LLVMBasedICFG::getOutEdges(const llvm::Function *F) const {
  auto FunctionMapIt = FunctionVertexMap.find(F);
  if (FunctionMapIt == FunctionVertexMap.end()) {
    return {};
  }

  std::vector<const llvm::Instruction *> Edges;
  for (const auto EdgeIt : boost::make_iterator_range(
           boost::out_edges(FunctionMapIt->second, CallGraph))) {
    auto Edge = CallGraph[EdgeIt];
    Edges.push_back(Edge.CS);
  }

  return Edges;
}

LLVMBasedICFG::OutEdgesAndTargets
LLVMBasedICFG::getOutEdgeAndTarget(const llvm::Function *F) const {
  auto FunctionMapIt = FunctionVertexMap.find(F);
  if (FunctionMapIt == FunctionVertexMap.end()) {
    return {};
  }

  OutEdgesAndTargets Edges;
  for (const auto EdgeIt : boost::make_iterator_range(
           boost::out_edges(FunctionMapIt->second, CallGraph))) {
    auto Edge = CallGraph[EdgeIt];
    auto Target = CallGraph[boost::target(EdgeIt, CallGraph)];
    Edges.insert(std::make_pair(Edge.CS, Target.F));
  }

  return Edges;
}

size_t LLVMBasedICFG::removeEdges(const llvm::Function *F,
                                  const llvm::Instruction *I) {
  auto FunctionMapIt = FunctionVertexMap.find(F);
  if (FunctionMapIt == FunctionVertexMap.end()) {
    return 0;
  }

  size_t EdgesRemoved = 0;
  auto OutEdges = boost::out_edges(FunctionMapIt->second, CallGraph);
  for (auto EdgeIt : boost::make_iterator_range(OutEdges)) {
    auto Edge = CallGraph[EdgeIt];
    if (Edge.CS == I) {
      boost::remove_edge(EdgeIt, CallGraph);
      ++EdgesRemoved;
    }
  }
  return EdgesRemoved;
}

bool LLVMBasedICFG::removeVertex(const llvm::Function *F) {
  auto FunctionMapIt = FunctionVertexMap.find(F);
  if (FunctionMapIt == FunctionVertexMap.end()) {
    return false;
  }

  boost::remove_vertex(FunctionMapIt->second, CallGraph);
  FunctionVertexMap.erase(FunctionMapIt);
  return true;
}

size_t LLVMBasedICFG::getCallerCount(const llvm::Function *F) const {
  auto MapEntry = FunctionVertexMap.find(F);
  if (MapEntry == FunctionVertexMap.end()) {
    return 0;
  }

  auto EdgeIterators = boost::in_edges(MapEntry->second, CallGraph);
  return std::distance(EdgeIterators.first, EdgeIterators.second);
}

set<const llvm::Function *>
LLVMBasedICFG::getCalleesOfCallAt(const llvm::Instruction *N) const {
  if (llvm::isa<llvm::CallInst>(N) || llvm::isa<llvm::InvokeInst>(N)) {
    set<const llvm::Function *> Callees;
    auto MapEntry = FunctionVertexMap.find(N->getFunction());
    if (MapEntry == FunctionVertexMap.end()) {
      return Callees;
    }
    out_edge_iterator EI;

    out_edge_iterator EIEnd;
    for (boost::tie(EI, EIEnd) = boost::out_edges(MapEntry->second, CallGraph);
         EI != EIEnd; ++EI) {
      auto Edge = CallGraph[*EI];
      if (N == Edge.CS) {
        auto Target = boost::target(*EI, CallGraph);
        Callees.insert(CallGraph[Target].F);
      }
    }
    return Callees;
  } else {
    return {};
  }
}

set<const llvm::Instruction *>
LLVMBasedICFG::getCallersOf(const llvm::Function *F) const {
  set<const llvm::Instruction *> CallersOf;
  auto MapEntry = FunctionVertexMap.find(F);
  if (MapEntry == FunctionVertexMap.end()) {
    return CallersOf;
  }
  in_edge_iterator EI;

  in_edge_iterator EIEnd;
  for (boost::tie(EI, EIEnd) = boost::in_edges(MapEntry->second, CallGraph);
       EI != EIEnd; ++EI) {
    auto Edge = CallGraph[*EI];
    CallersOf.insert(Edge.CS);
  }
  return CallersOf;
}

set<const llvm::Instruction *>
LLVMBasedICFG::getCallsFromWithin(const llvm::Function *F) const {
  set<const llvm::Instruction *> CallSites;
  for (llvm::const_inst_iterator I = llvm::inst_begin(F), E = llvm::inst_end(F);
       I != E; ++I) {
    if (llvm::isa<llvm::CallInst>(*I) || llvm::isa<llvm::InvokeInst>(*I)) {
      CallSites.insert(&(*I));
    }
  }
  return CallSites;
}

/**
 * Returns all statements to which a call could return.
 * In the RHS paper, for every call there is just one return site.
 * We, however, use as return site the successor statements, of which
 * there can be many in case of exceptional flow.
 */
set<const llvm::Instruction *>
LLVMBasedICFG::getReturnSitesOfCallAt(const llvm::Instruction *N) const {
  set<const llvm::Instruction *> ReturnSites;
  if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(N)) {
    ReturnSites.insert(Call->getNextNode());
  }
  if (const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(N)) {
    ReturnSites.insert(&Invoke->getNormalDest()->front());
    ReturnSites.insert(&Invoke->getUnwindDest()->front());
  }
  return ReturnSites;
}

void LLVMBasedICFG::collectGlobalCtors() {
  for (const auto *Module : IRDB.getAllModules()) {
    insertGlobalCtorsDtorsImpl(GlobalCtors, Module, "llvm.global_ctors");
    // auto Part = getGlobalCtorsDtorsImpl(Module, "llvm.global_ctors");
    // GlobalCtors.insert(GlobalCtors.begin(), Part.begin(), Part.end());
  }

  for (auto it = GlobalCtors.cbegin(), end = GlobalCtors.cend(); it != end;
       ++it) {
    GlobalCtorFn.try_emplace(it->second, it);
  }
}

void LLVMBasedICFG::collectGlobalDtors() {
  for (const auto *Module : IRDB.getAllModules()) {
    insertGlobalCtorsDtorsImpl(GlobalDtors, Module, "llvm.global_dtors");
    // auto Part = getGlobalCtorsDtorsImpl(Module, "llvm.global_dtors");
    // GlobalDtors.insert(GlobalDtors.begin(), Part.begin(), Part.end());
  }

  for (auto it = GlobalDtors.cbegin(), end = GlobalDtors.cend(); it != end;
       ++it) {
    GlobalDtorFn.try_emplace(it->second, it);
  }
}

void LLVMBasedICFG::collectGlobalInitializers() {
  // get all functions used to initialize global variables
  forEachGlobalCtor([this](auto *GlobalCtor) {
    for (const auto &BB : *GlobalCtor) {
      for (const auto &I : BB) {
        if (auto Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
          GlobalInitializers.push_back(Call->getCalledFunction());
        }
      }
    }
  });
}

void LLVMBasedICFG::collectRegisteredDtors() {
  // get all destructors that are registered with __cxa_atexit
  for (const auto &F : getGlobalInitializers()) {
    for (const auto &BB : *F) {
      for (const auto &I : BB) {
        if (auto Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
          llvm::ImmutableCallSite CS(Call);
          if (CS.getCalledFunction()->getName() == "__cxa_atexit") {
            // dtor is operand in call to __cxa_atexit
            auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(CS.getArgOperand(0));
            if (CE == nullptr) {
              continue;
            }

            // auto *AsI = CE->getAsInstruction();
            assert(CE->isCast());
            const llvm::Function *Dtor = llvm::cast<llvm::Function>(
                // llvm::cast<llvm::BitCastInst>(AsI)->getOperand(0)
                CE->getOperand(0));
            RegisteredDtors.push_back(Dtor);
            // AsI->deleteValue();
          }
        }
      }
    }
  }

  if (RegisteredDtors.empty())
    return;

  // Dtors are called in reverse order of their registration
  std::reverse(RegisteredDtors.begin(), RegisteredDtors.end());
  for (size_t i = 0; i < RegisteredDtors.size(); ++i) {
    RegisteredDtorIndex.try_emplace(RegisteredDtors[i], i);
  }
}

/**
 * Returns the set of all nodes that are neither call nor start nodes.
 */
set<const llvm::Instruction *> LLVMBasedICFG::allNonCallStartNodes() const {
  set<const llvm::Instruction *> NonCallStartNodes;
  for (auto *M : IRDB.getAllModules()) {
    for (auto &F : *M) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          if ((!llvm::isa<llvm::CallInst>(&I)) &&
              (!llvm::isa<llvm::InvokeInst>(&I)) && (!isStartPoint(&I))) {
            NonCallStartNodes.insert(&I);
          }
        }
      }
    }
  }
  return NonCallStartNodes;
}

void LLVMBasedICFG::mergeWith(const LLVMBasedICFG &Other) {
  using vertex_t = bidigraph_t::vertex_descriptor;
  using vertex_map_t = std::map<vertex_t, vertex_t>;
  vertex_map_t OldToNewVertexMapping;
  boost::associative_property_map<vertex_map_t> VertexMapWrapper(
      OldToNewVertexMapping);
  boost::copy_graph(Other.CallGraph, CallGraph,
                    boost::orig_to_copy(VertexMapWrapper));

  // This vector holds the call-sites that are used to merge the whole-module
  // points-to graphs
  vector<pair<llvm::ImmutableCallSite, const llvm::Function *>> Calls;
  vertex_iterator VIv;

  vertex_iterator VIvEnd;

  vertex_iterator VIu;

  vertex_iterator VIuEnd;
  // Iterate the vertices of this graph 'v'
  for (boost::tie(VIv, VIvEnd) = boost::vertices(CallGraph); VIv != VIvEnd;
       ++VIv) {
    // Iterate the vertices of the other graph 'u'
    for (boost::tie(VIu, VIuEnd) = boost::vertices(CallGraph); VIu != VIuEnd;
         ++VIu) {
      // Check if we have a virtual node that can be replaced with the actual
      // node
      if (CallGraph[*VIv].F == CallGraph[*VIu].F &&
          CallGraph[*VIv].F->isDeclaration() &&
          !CallGraph[*VIu].F->isDeclaration()) {
        in_edge_iterator EI;

        in_edge_iterator EIEnd;
        for (boost::tie(EI, EIEnd) = boost::in_edges(*VIv, CallGraph);
             EI != EIEnd; ++EI) {
          auto Source = boost::source(*EI, CallGraph);
          auto Edge = CallGraph[*EI];
          // This becomes the new edge for this graph to the other graph
          boost::add_edge(Source, *VIu, Edge.CS, CallGraph);
          Calls.emplace_back(llvm::ImmutableCallSite(Edge.CS),
                             CallGraph[*VIu].F);
          // Remove the old edge flowing into the virtual node
          boost::remove_edge(Source, *VIv, CallGraph);
        }
        // Remove the virtual node
        boost::remove_vertex(*VIv, CallGraph);
      }
    }
  }

  // Update the FunctionVertexMap:
  for (const auto &OtherValues : Other.FunctionVertexMap) {
    auto MappingIter = OldToNewVertexMapping.find(OtherValues.second);
    if (MappingIter != OldToNewVertexMapping.end()) {
      FunctionVertexMap.insert(
          make_pair(OtherValues.first, MappingIter->second));
    }
  }

  // Merge the already visited functions
  VisitedFunctions.insert(Other.VisitedFunctions.begin(),
                          Other.VisitedFunctions.end());
  // Merge the points-to graphs
  // WholeModulePTG.mergeWith(Other.WholeModulePTG, Calls);
}

CallGraphAnalysisType LLVMBasedICFG::getCallGraphAnalysisType() const {
  return CGType;
}

void LLVMBasedICFG::print(ostream &OS) const {
  OS << "Call Graph:\n";
  vertex_iterator UI;

  vertex_iterator UIEnd;
  for (boost::tie(UI, UIEnd) = boost::vertices(CallGraph); UI != UIEnd; ++UI) {
    OS << CallGraph[*UI].getFunctionName() << " --> ";
    out_edge_iterator EI;

    out_edge_iterator EIEnd;
    for (boost::tie(EI, EIEnd) = boost::out_edges(*UI, CallGraph); EI != EIEnd;
         ++EI) {
      OS << CallGraph[target(*EI, CallGraph)].getFunctionName() << " ";
    }
    OS << '\n';
  }
}

void LLVMBasedICFG::printAsDot(std::ostream &OS, bool PrintEdgeLabels) const {
  if (PrintEdgeLabels) {
    boost::write_graphviz(OS, CallGraph, VertexWriter<bidigraph_t>(CallGraph),
                          EdgeLabelWriter<bidigraph_t>(CallGraph));
  } else {
    boost::write_graphviz(OS, CallGraph, VertexWriter<bidigraph_t>(CallGraph));
  }
}

nlohmann::json LLVMBasedICFG::getAsJson() const {
  nlohmann::json J;
  vertex_iterator VIv;

  vertex_iterator VIvEnd;
  out_edge_iterator EI;

  out_edge_iterator EIEnd;
  // iterate all graph vertices
  for (boost::tie(VIv, VIvEnd) = boost::vertices(CallGraph); VIv != VIvEnd;
       ++VIv) {
    J[PhasarConfig::JsonCallGraphID()][CallGraph[*VIv].getFunctionName()];
    // iterate all out edges of vertex vi_v
    for (boost::tie(EI, EIEnd) = boost::out_edges(*VIv, CallGraph); EI != EIEnd;
         ++EI) {
      J[PhasarConfig::JsonCallGraphID()][CallGraph[*VIv].getFunctionName()] +=
          CallGraph[boost::target(*EI, CallGraph)].getFunctionName();
    }
  }
  return J;
}

void LLVMBasedICFG::printAsJson(std::ostream &OS) const { OS << getAsJson(); }

vector<const llvm::Function *> LLVMBasedICFG::getDependencyOrderedFunctions() {
  vector<vertex_t> Vertices;
  vector<const llvm::Function *> Functions;
  dependency_visitor Deps(Vertices);
  boost::depth_first_search(CallGraph, visitor(Deps));
  for (auto V : Vertices) {
    if (!CallGraph[V].F->isDeclaration()) {
      Functions.push_back(CallGraph[V].F);
    }
  }
  return Functions;
}

unsigned LLVMBasedICFG::getNumOfVertices() {
  return boost::num_vertices(CallGraph);
}

unsigned LLVMBasedICFG::getNumOfEdges() { return boost::num_edges(CallGraph); }

} // namespace psr

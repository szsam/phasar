/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

/*
 * IFDSTabulationProblemPlugin.h
 *
 *  Created on: 14.06.2017
 *      Author: philipp
 */

#ifndef PHASAR_PHASARLLVM_PLUGINS_INTERFACES_IFDSIDE_IFDSTABULATIONPROBLEMPLUGIN_H_
#define PHASAR_PHASARLLVM_PLUGINS_INTERFACES_IFDSIDE_IFDSTABULATIONPROBLEMPLUGIN_H_

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include <phasar/PhasarLLVM/DataFlowSolver/IfdsIde/IFDSTabulationProblem.h>
#include <phasar/PhasarLLVM/DataFlowSolver/IfdsIde/LLVMZeroValue.h>
#include <phasar/Utils/LLVMShorthands.h>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace psr {

class ProjectIRDB;
class LLVMBasedICFG;
class TypeHierarchy;
class PointsToInfo;

class IFDSTabulationProblemPlugin
    : public IFDSTabulationProblem<
          const llvm::Instruction *, const llvm::Value *,
          const llvm::Function *, LLVMBasedICFG> {
protected:
  std::vector<std::string> EntryPoints;

public:
  IFDSTabulationProblemPlugin(const ProjectIRDB *IRDB, const TypeHierarchy *TH,
                const LLVMBasedICFG *ICF, const PointsToInfo *PT,
                std::set<std::string> EntryPoints = {"main"})
      : IFDSTabulationProblem<const llvm::Instruction *,
                                     const llvm::Value *,
                                     const llvm::Function *, LLVMBasedICFG>(
                                       IRDB, TH, ICF, PT, EntryPoints)
   {
    IFDSTabulationProblem::ZeroValue = createZeroValue();
  }
  ~IFDSTabulationProblemPlugin() override = default;

  const llvm::Value *createZeroValue() const override {
    // create a special value to represent the zero value!
    return LLVMZeroValue::getInstance();
  }

  bool isZeroValue(const llvm::Value *d) const override {
    return LLVMZeroValue::getInstance()->isLLVMZeroValue(d);
  }

  void printNode(std::ostream &os, const llvm::Instruction *n) const override {
    os << llvmIRToString(n);
  }

  void printDataFlowFact(std::ostream &os,
                         const llvm::Value *d) const override {
    os << llvmIRToString(d);
  }

  void printMethod(std::ostream &os, const llvm::Function *m) const override {
    os << m->getName().str();
  }
};

extern std::map<std::string,
                std::unique_ptr<IFDSTabulationProblemPlugin> (*)(
                    LLVMBasedICFG &I, std::vector<std::string> EntryPoints)>
    IFDSTabulationProblemPluginFactory;

} // namespace psr

#endif

/**
 * @author Sebastian Roland <seroland86@gmail.com>
 */

#ifndef PHASAR_PHASARLLVM_DATAFLOWSOLVER_IFDSIDE_IFDSFIELDSENSTAINTANALYSIS_UTILS_DATAFLOWUTILS_H
#define PHASAR_PHASARLLVM_DATAFLOWSOLVER_IFDSIDE_IFDSFIELDSENSTAINTANALYSIS_UTILS_DATAFLOWUTILS_H

#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "llvm/IR/Instructions.h"

#include "phasar/PhasarLLVM/Domain/ExtendedValue.h"

namespace psr {

class DataFlowUtils {
public:
  DataFlowUtils() = delete;

  static bool isValueTainted(const llvm::Value *currentInst,
                             const ExtendedValue &fact);

  static bool isMemoryLocationTainted(const llvm::Value *memLocationMatr,
                                      const ExtendedValue &fact);

  static std::vector<const llvm::Value *>
  getMemoryLocationSeqFromMatr(const llvm::Value *memLocationMatr);
  static std::vector<const llvm::Value *>
  getMemoryLocationSeqFromFact(const ExtendedValue &memLocationFact);
  static std::vector<const llvm::Value *>
  getVaListMemoryLocationSeqFromFact(const ExtendedValue &vaListFact);

  static bool isMemoryLocationSeqsEqual(
      const std::vector<const llvm::Value *> &memLocationSeq1,
      const std::vector<const llvm::Value *> &memLocationSeq2);

  static bool isSubsetMemoryLocationSeq(
      const std::vector<const llvm::Value *> &memLocationSeqInst,
      const std::vector<const llvm::Value *> &memLocationSeqFact);
  static std::vector<const llvm::Value *> getRelocatableMemoryLocationSeq(
      const std::vector<const llvm::Value *> &taintedMemLocationSeq,
      const std::vector<const llvm::Value *> &srcMemLocationSeq);
  static std::vector<const llvm::Value *> joinMemoryLocationSeqs(
      const std::vector<const llvm::Value *> &memLocationSeq1,
      const std::vector<const llvm::Value *> &memLocationSeq2);

  static bool isPatchableArgumentStore(const llvm::Value *srcValue,
                                       const ExtendedValue &fact);
  static bool isPatchableArgumentMemcpy(
      const llvm::Value *srcValue,
      const std::vector<const llvm::Value *> &srcMemLocationSeq,
      const ExtendedValue &fact);
  static bool isPatchableVaListArgument(const llvm::Value *srcValue,
                                        const ExtendedValue &fact);
  static bool isPatchableReturnValue(const llvm::Value *srcValue,
                                     const ExtendedValue &fact);
  static std::vector<const llvm::Value *> patchMemoryLocationFrame(
      const std::vector<const llvm::Value *> &patchableMemLocationSeq,
      const std::vector<const llvm::Value *> &patchMemLocationSeq);

  static std::vector<
      std::tuple<const llvm::Value *, const std::vector<const llvm::Value *>,
                 const llvm::Value *>>
  getSanitizedArgList(const llvm::CallInst *callInst,
                      const llvm::Function *destFun,
                      const llvm::Value *zeroValue);

  static const llvm::BasicBlock *
  getEndOfTaintedBlock(const llvm::BasicBlock *startBasicBlock);
  static bool removeTaintedBlockInst(const ExtendedValue &fact,
                                     const llvm::Instruction *currentInst);
  static bool isAutoGENInTaintedBlock(const llvm::Instruction *currentInst);

  static bool isMemoryLocationFact(const ExtendedValue &ev);
  static bool isKillAfterStoreFact(const ExtendedValue &ev);
  static bool isCheckOperandsInst(const llvm::Instruction *currentInst);
  static bool isAutoIdentity(const llvm::Instruction *currentInst,
                             const ExtendedValue &fact);
  static bool isVarArgParam(const llvm::Value *param,
                            const llvm::Value *zeroValue);
  static bool isVaListType(const llvm::Type *type);
  static bool isReturnValue(const llvm::Instruction *currentInst,
                            const llvm::Instruction *successorInst);
  static bool isArrayDecay(const llvm::Value *memLocationMatr);
  static bool isGlobalMemoryLocationSeq(
      const std::vector<const llvm::Value *> &memLocationSeq);

  static void dumpFact(const ExtendedValue &ev);

  static std::set<std::string> getTaintedFunctions();
  static std::set<std::string> getBlacklistedFunctions();

  static std::string getTraceFilenamePrefix(const std::string &entryPoint);
};

} // namespace psr

#endif

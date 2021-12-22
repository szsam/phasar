/**
 * @author Sebastian Roland <seroland86@gmail.com>
 */

#ifndef PHINODELOWFUNCTION_H
#define PHINODELOWFUNCTION_H

#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/IFDSFieldSensTaintAnalysis/FlowFunctions/FlowFunctionBase.h"

namespace psr {

class PHINodeFlowFunction : public FlowFunctionBase {
public:
  PHINodeFlowFunction(const llvm::Instruction *CurrentInst,
                      TraceStats &TraceStats, const ExtendedValue &ZV)
      : FlowFunctionBase(CurrentInst, TraceStats, ZV) {}
  ~PHINodeFlowFunction() override = default;

  std::set<ExtendedValue> computeTargetsExt(ExtendedValue &Fact) override;
};

} // namespace psr

#endif // PHINODELOWFUNCTION_H

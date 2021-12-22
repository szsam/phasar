/**
 * @author Sebastian Roland <seroland86@gmail.com>
 */

#ifndef CALLTORETFLOWFUNCTION_H
#define CALLTORETFLOWFUNCTION_H

#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/IFDSFieldSensTaintAnalysis/FlowFunctions/FlowFunctionBase.h"

namespace psr {

class CallToRetFlowFunction : public FlowFunctionBase {
public:
  CallToRetFlowFunction(const llvm::Instruction *CurrentInst,
                        TraceStats &TStats, const ExtendedValue &ZeroValue)
      : FlowFunctionBase(CurrentInst, TStats, ZeroValue) {}
  ~CallToRetFlowFunction() override = default;

  std::set<ExtendedValue> computeTargetsExt(ExtendedValue &Fact) override;
};

} // namespace psr

#endif // CALLTORETFLOWFUNCTION_H

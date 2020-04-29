/******************************************************************************
 * Copyright (c) 2020 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Fabian Schiebel and others
 *****************************************************************************/

#ifndef PHASAR_PHASARLLVM_IFDSIDE_PROBLEMS_IDEGENERALIZEDLCA_GENCONSTANT_H_
#define PHASAR_PHASARLLVM_IFDSIDE_PROBLEMS_IDEGENERALIZEDLCA_GENCONSTANT_H_

#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/EdgeFunction.h"
#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/Problems/IDEGeneralizedLCA/IDEGeneralizedLCA.h"

namespace psr {

class GenConstant : public EdgeFunction<IDEGeneralizedLCA::v_t>,
                    public std::enable_shared_from_this<GenConstant> {
  IDEGeneralizedLCA::v_t val;
  size_t maxSize;

public:
  GenConstant(const IDEGeneralizedLCA::v_t &val, size_t maxSize);
  IDEGeneralizedLCA::v_t computeTarget(IDEGeneralizedLCA::v_t source) override;

  std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> composeWith(
      std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> secondFunction)
      override;

  std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>>
  joinWith(std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> otherFunction)
      override;

  bool equal_to(std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> other)
      const override;
  void print(std::ostream &OS, bool isForDebug = false) const override;
};

} // namespace psr

#endif
// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (C) 2020 Parichay Kapoor <pk.kapoor@samsung.com>
 *
 * @file   sgd.h
 * @date   6 October 2020
 * @see    https://github.com/nnstreamer/nntrainer
 * @author Jijoong Moon <jijoong.moon@samsung.com>
 * @author Parichay Kapoor <pk.kapoor@samsung.com>
 * @bug    No known bugs except for NYI items
 * @brief  This is the SGD optimizer.
 */
#ifndef __SGD_H__
#define __SGD_H__
#ifdef __cplusplus

#include <optimizer_impl.h>

namespace nntrainer {

/**
 * @class   SGD optimizer class
 * @brief   Stochastic Gradient Descent optimizer class
 */
class SGD : public OptimizerImpl {
public:
  /**
   * @brief Construct a new SGD object
   *
   */
  SGD();

  /**
   * @copydoc applyGradient(RunOptimizerContext &context)
   */
  void applyGradient(RunOptimizerContext &context);

  /**
   * @copydoc Optimizer::getType()
   */
  const std::string getType() const { return SGD::type; }

  inline static const std::string type = "sgd";
};
} /* namespace nntrainer */

#endif /* __cplusplus */
#endif /* __SGD_H__ */

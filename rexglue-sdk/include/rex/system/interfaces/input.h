/**
 * @file        system/interfaces/input.h
 * @brief       Abstract input system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/system/xtypes.h>

namespace rex::system {

class IInputSystem {
 public:
  virtual ~IInputSystem() = default;
  virtual X_STATUS Setup() = 0;
  virtual void Shutdown() = 0;

  // Consume mouse movement before controller-stick conversion clamps it.
  // The default keeps custom input backends source-compatible.
  virtual bool ConsumeRawMouseDelta(int32_t& delta_x, int32_t& delta_y) {
    delta_x = 0;
    delta_y = 0;
    return false;
  }
};

}  // namespace rex::system

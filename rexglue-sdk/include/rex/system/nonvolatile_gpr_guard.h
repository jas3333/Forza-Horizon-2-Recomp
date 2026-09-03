/**
 * @file        system/nonvolatile_gpr_guard.h
 * @brief       Guest-call ABI diagnostics for nonvolatile GPRs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <array>
#include <cstdint>

#include <rex/ppc/context.h>

namespace rex::runtime {

struct NonvolatileGprDifference {
  uint32_t register_index = 0;
  uint64_t before = 0;
  uint64_t after = 0;

  explicit operator bool() const noexcept { return register_index != 0; }
};

/** Copy the process-wide GPR-guard configuration into a new guest context. */
void ConfigureNonvolatileGprGuardContext(PPCContext& context) noexcept;

/** A snapshot of the ABI-preserved guest GPRs r14 through r31. */
struct NonvolatileGprSnapshot {
  static constexpr uint32_t kFirstRegister = 14;
  static constexpr uint32_t kRegisterCount = 18;

  std::array<uint64_t, kRegisterCount> values{};

  void Capture(const PPCContext& context) noexcept;
  NonvolatileGprDifference FindFirstDifference(const PPCContext& context) const noexcept;
};

/** Start one normal guest-call ABI check. Zero means that the guard is disabled. */
uint32_t BeginNonvolatileGprCall(const PPCContext& context, uint32_t caller, uint32_t callsite,
                                 uint32_t target) noexcept;

/** Finish one normal guest-call ABI check. */
void EndNonvolatileGprCall(const PPCContext& context, uint32_t token) noexcept;

}  // namespace rex::runtime

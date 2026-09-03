/**
 * @file        system/nonvolatile_gpr_guard.cpp
 * @brief       Guest-call ABI diagnostics for nonvolatile GPRs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/nonvolatile_gpr_guard.h>

#include <vector>

REXCVAR_DEFINE_BOOL(nonvolatile_gpr_guard, false, "Debug",
                    "Stop when a normal guest call changes r14 through r31")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::runtime {
namespace {

struct NonvolatileGprCallFrame {
  uint32_t caller = 0;
  uint32_t callsite = 0;
  uint32_t target = 0;
  NonvolatileGprSnapshot snapshot{};
};

thread_local std::vector<NonvolatileGprCallFrame> g_call_frames;

}  // namespace

void ConfigureNonvolatileGprGuardContext(PPCContext& context) noexcept {
  context.nonvolatile_gpr_guard = REXCVAR_GET(nonvolatile_gpr_guard);
}

void NonvolatileGprSnapshot::Capture(const PPCContext& context) noexcept {
  const auto* first = &context.r14;
  for (uint32_t index = 0; index < kRegisterCount; ++index) {
    values[index] = first[index].u64;
  }
}

NonvolatileGprDifference NonvolatileGprSnapshot::FindFirstDifference(
    const PPCContext& context) const noexcept {
  const auto* first = &context.r14;
  for (uint32_t index = 0; index < kRegisterCount; ++index) {
    if (values[index] != first[index].u64) {
      return {
          .register_index = kFirstRegister + index,
          .before = values[index],
          .after = first[index].u64,
      };
    }
  }
  return {};
}

uint32_t BeginNonvolatileGprCall(const PPCContext& context, uint32_t caller, uint32_t callsite,
                                 uint32_t target) noexcept {
  if (!context.nonvolatile_gpr_guard) {
    return 0;
  }

  auto& frame = g_call_frames.emplace_back();
  frame.caller = caller;
  frame.callsite = callsite;
  frame.target = target;
  frame.snapshot.Capture(context);
  return static_cast<uint32_t>(g_call_frames.size());
}

void EndNonvolatileGprCall(const PPCContext& context, uint32_t token) noexcept {
  if (token == 0) {
    return;
  }

  if (token != g_call_frames.size()) {
    REX_FATAL("[AVDIAG] nonvolatile GPR guard stack mismatch: token=0x{:X} depth=0x{:X}", token,
              g_call_frames.size());
  }

  const auto& frame = g_call_frames.back();
  const auto difference = frame.snapshot.FindFirstDifference(context);
  if (!difference) {
    g_call_frames.pop_back();
    return;
  }

  REX_FATAL(
      "[AVDIAG] nonvolatile GPR mismatch: caller=0x{:08X} callsite=0x{:08X} "
      "target=0x{:08X} register=r{} before=0x{:016X} after=0x{:016X}",
      frame.caller, frame.callsite, frame.target, difference.register_index, difference.before,
      difference.after);
}

}  // namespace rex::runtime

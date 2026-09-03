/**
 * @file        tests/unit/system/nonvolatile_gpr_guard_test.cpp
 * @brief       Tests for guest-call nonvolatile GPR comparison
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/nonvolatile_gpr_guard.h>

namespace {

void SetNonvolatileGprs(PPCContext& context) {
  auto* first = &context.r14;
  for (uint32_t index = 0; index < 18; ++index) {
    first[index].u64 = 0x1400000000000000ull + index;
  }
}

}  // namespace

TEST_CASE("NonvolatileGprSnapshot accepts an unchanged call return", "[runtime][abi]") {
  PPCContext context{};
  SetNonvolatileGprs(context);

  rex::runtime::NonvolatileGprSnapshot snapshot;
  snapshot.Capture(context);

  CHECK_FALSE(snapshot.FindFirstDifference(context));
}

TEST_CASE("NonvolatileGprSnapshot identifies the first changed register", "[runtime][abi]") {
  PPCContext context{};
  SetNonvolatileGprs(context);

  rex::runtime::NonvolatileGprSnapshot snapshot;
  snapshot.Capture(context);
  const uint64_t before = context.r18.u64;
  context.r18.u64 = 0xDEADBEEF;

  const auto difference = snapshot.FindFirstDifference(context);
  REQUIRE(difference);
  CHECK(difference.register_index == 18);
  CHECK(difference.before == before);
  CHECK(difference.after == 0xDEADBEEF);
}

TEST_CASE("NonvolatileGprSnapshot ignores volatile guest registers", "[runtime][abi]") {
  PPCContext context{};
  SetNonvolatileGprs(context);

  rex::runtime::NonvolatileGprSnapshot snapshot;
  snapshot.Capture(context);
  context.r3.u64 = 1;
  context.r12.u64 = 2;

  CHECK_FALSE(snapshot.FindFirstDifference(context));
}

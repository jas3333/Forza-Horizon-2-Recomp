/**
 * @file        tests/unit/core/fiber_test.cpp
 * @brief       Unit tests for rex::thread::Fiber
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/thread/fiber.h>

#include <stdexcept>

using rex::thread::Fiber;

// Globals - fiber entry functions cannot capture closures.
static Fiber* s_main = nullptr;
static int s_count = 0;

static void counting_fiber(void*) {
  ++s_count;  // first resume
  Fiber::SwitchTo(s_main);
  ++s_count;  // second resume
  Fiber::SwitchTo(s_main);
}

static void throwing_fiber(void*) {
  throw std::runtime_error("guest fiber failure");
}

static void returning_fiber(void*) {
  ++s_count;
}

TEST_CASE("rex::thread::Fiber - basic context switch", "[fiber]") {
  s_count = 0;
  s_main = Fiber::ConvertCurrentThread();
  auto* f = Fiber::Create(256 * 1024, counting_fiber, nullptr);

  REQUIRE(s_count == 0);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 1);  // fiber ran, switched back
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 2);  // fiber resumed, ran again, switched back
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 2);  // fiber returned and resumed its caller
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 2);  // switching to a completed fiber is a no-op

  // NOTE: Cleanup is not exception-safe. If a REQUIRE above fires, Catch2
  // throws and these Destroy() calls are skipped, leaking the host fiber
  // handles. Acceptable for a test-only scenario; fix if spurious failures occur.
  f->Destroy();
  s_main->Destroy();
  s_main = nullptr;
}

TEST_CASE("rex::thread::Fiber - completed fiber cannot resume past its entry", "[fiber]") {
  s_count = 0;
  s_main = Fiber::ConvertCurrentThread();
  auto* f = Fiber::Create(256 * 1024, returning_fiber, nullptr);

  Fiber::SwitchTo(f);
  REQUIRE(s_count == 1);
  Fiber::SwitchTo(f);
  REQUIRE(s_count == 1);

  f->Destroy();
  s_main->Destroy();
  s_main = nullptr;
}

TEST_CASE("rex::thread::Fiber - carries exceptions to the caller", "[fiber]") {
  s_main = Fiber::ConvertCurrentThread();
  auto* f = Fiber::Create(256 * 1024, throwing_fiber, nullptr);

  REQUIRE_THROWS_AS(Fiber::SwitchTo(f), std::runtime_error);

  f->Destroy();
  s_main->Destroy();
  s_main = nullptr;
}

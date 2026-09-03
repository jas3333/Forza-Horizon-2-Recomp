/**
 * @file        rex/graphics/diag_counters.h
 * @brief       Reusable, cvar-gated GPU diagnostic counters and value-change
 *              loggers, usable by any game built on rexglue-sdk.
 *
 * @copyright   Copyright (c) 2026 rexglue-sdk contributors.
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/logging.h>

namespace rex::graphics::diag {

// A thread-safe "N per second" counter. Call Tick() once per event (e.g. once
// per draw call, once per Present()); it logs a total once every full second
// has elapsed since the last log, then resets. Zero overhead when not ticked.
//
// Usage (typically as a function-local static, one per call site):
//   static rex::graphics::diag::RateCounter counter("draw_packets");
//   if (REXCVAR_GET(diag_draw_rate)) counter.Tick();
class RateCounter {
 public:
  explicit RateCounter(const char* label) : label_(label) {}

  void Tick(uint64_t count = 1) {
    count_.fetch_add(count, std::memory_order_relaxed);
    auto now = std::chrono::steady_clock::now();
    // Relaxed load is fine: at worst two threads both decide to report in the
    // same tick, which just means one extra log line, not lost data (the
    // exchange below is still atomic per-thread).
    if (now - last_report_.load(std::memory_order_relaxed) >= std::chrono::seconds(1)) {
      last_report_.store(now, std::memory_order_relaxed);
      uint64_t total = count_.exchange(0, std::memory_order_relaxed);
      REXGPU_INFO("[DIAG] {}/sec={}", label_, total);
    }
  }

 private:
  const char* label_;
  std::atomic<uint64_t> count_{0};
  std::atomic<std::chrono::steady_clock::time_point> last_report_{
      std::chrono::steady_clock::now()};
};

// Logs a value only when it changes from the last-seen value, once per
// distinct value (not once per call) - for state that's set every frame but
// rarely actually changes (formats, blend constants, cvar-driven modes).
//
// Usage (function-local static, one per call site + value type):
//   static rex::graphics::diag::ChangeLogger<uint32_t> logger("color_format");
//   if (REXCVAR_GET(diag_rt_format)) logger.LogIfChanged(current_format);
template <typename T>
class ChangeLogger {
 public:
  explicit ChangeLogger(const char* label) : label_(label) {}

  void LogIfChanged(const T& value) {
    if (!has_value_ || !(value == last_value_)) {
      has_value_ = true;
      last_value_ = value;
      REXGPU_INFO("[DIAG] {} changed to {}", label_, value);
    }
  }

 private:
  const char* label_;
  T last_value_{};
  bool has_value_ = false;
};

}  // namespace rex::graphics::diag

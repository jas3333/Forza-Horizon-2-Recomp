/**
 * @file        rex/ui/frame_stats.h
 * @brief       Lightweight guest-frame timing for Release overlays.
 * @license     BSD 3-Clause License
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

class FrameStatsTracker {
 public:
  void RecordFrame() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    RecordFrameAt(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
  }

  // Public for deterministic tests and alternate monotonic clocks.
  void RecordFrameAt(uint64_t timestamp_ns) {
    frame_count_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t previous_ns =
        last_frame_timestamp_ns_.exchange(timestamp_ns, std::memory_order_relaxed);
    if (!previous_ns || timestamp_ns <= previous_ns) {
      return;
    }

    const uint64_t period_ns = timestamp_ns - previous_ns;
    if (period_ns > kPauseResetNs) {
      smoothed_period_ns_.store(0, std::memory_order_relaxed);
      return;
    }

    const uint64_t previous_period_ns = smoothed_period_ns_.load(std::memory_order_relaxed);
    const uint64_t smoothed_ns =
        previous_period_ns ? ((previous_period_ns * 7) + period_ns) / 8 : period_ns;
    smoothed_period_ns_.store(smoothed_ns, std::memory_order_relaxed);
  }

  FrameStats GetStats() const {
    FrameStats result;
    result.frame_count = frame_count_.load(std::memory_order_relaxed);
    const uint64_t period_ns = smoothed_period_ns_.load(std::memory_order_relaxed);
    if (period_ns) {
      result.frame_time_ms = double(period_ns) / 1'000'000.0;
      result.fps = 1'000'000'000.0 / double(period_ns);
    }
    return result;
  }

 private:
  static constexpr uint64_t kPauseResetNs = 1'000'000'000;

  std::atomic<uint64_t> frame_count_{0};
  std::atomic<uint64_t> last_frame_timestamp_ns_{0};
  std::atomic<uint64_t> smoothed_period_ns_{0};
};

}  // namespace rex::ui

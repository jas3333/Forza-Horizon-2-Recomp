#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <rex/ui/frame_stats.h>

TEST_CASE("Frame statistics report a stable guest rate", "[ui][frame_stats]") {
  rex::ui::FrameStatsTracker tracker;

  tracker.RecordFrameAt(1'000'000'000);
  tracker.RecordFrameAt(1'016'666'667);

  const auto stats = tracker.GetStats();
  REQUIRE(stats.frame_count == 2);
  REQUIRE(stats.frame_time_ms == Catch::Approx(16.666667).margin(0.000001));
  REQUIRE(stats.fps == Catch::Approx(60.0).margin(0.001));
}

TEST_CASE("Frame statistics reset after a pause", "[ui][frame_stats]") {
  rex::ui::FrameStatsTracker tracker;

  tracker.RecordFrameAt(1'000'000'000);
  tracker.RecordFrameAt(1'016'666'667);
  tracker.RecordFrameAt(3'000'000'000);

  auto stats = tracker.GetStats();
  REQUIRE(stats.frame_count == 3);
  REQUIRE(stats.frame_time_ms == 0.0);
  REQUIRE(stats.fps == 0.0);

  tracker.RecordFrameAt(3'033'333'333);
  stats = tracker.GetStats();
  REQUIRE(stats.frame_count == 4);
  REQUIRE(stats.fps == Catch::Approx(30.0).margin(0.001));
}

#include "fh2_rtlog.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace fh2::rtlog {

Counters g_counters;
bool g_active = false;

namespace {

constexpr uint32_t kFramesPerCapture = 2;
constexpr uint32_t kStatFrames = 300;

std::vector<double> ParseTimes() {
  std::vector<double> times;
  const char* env = std::getenv("FH2_RTLOG_SEC");
  if (!env) return times;
  std::string text(env);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string::npos) comma = text.size();
    times.push_back(std::atof(text.substr(pos, comma - pos).c_str()));
    pos = comma + 1;
  }
  return times;
}

std::vector<double> g_times = ParseTimes();
size_t g_next_time = 0;
uint32_t g_frames_left = 0;
uint64_t g_frame = 0;
std::chrono::steady_clock::time_point g_start;
Counters g_last_report;

// Resolve destinations: start -> end, kept for a few seconds of frames.
std::map<uint32_t, uint32_t> g_resolve_dests;

}  // namespace

void NoteResolveDest(uint32_t address, uint32_t length) {
  if (!length) return;
  uint32_t& end = g_resolve_dests[address];
  end = std::max(end, address + length);
}

bool OverlapsResolveDest(uint32_t address, uint32_t length) {
  uint32_t end = address + length;
  auto it = g_resolve_dests.upper_bound(address);
  if (it != g_resolve_dests.begin()) {
    auto prev = std::prev(it);
    if (prev->second > address) return true;
  }
  return it != g_resolve_dests.end() && it->first < end;
}

void OnSwap() {
  auto now = std::chrono::steady_clock::now();
  if (g_frame++ == 0) g_start = now;
  double seconds = std::chrono::duration<double>(now - g_start).count();

  if (g_active) {
    REXGPU_INFO("[RTLOG] ==== swap (end of frame {}) ====", g_frame - 1);
    if (--g_frames_left == 0) {
      g_active = false;
      REXGPU_INFO("[RTLOG] ==== capture end ====");
    }
  }
  if (!g_active && g_next_time < g_times.size() && seconds >= g_times[g_next_time]) {
    ++g_next_time;
    g_active = true;
    g_frames_left = kFramesPerCapture;
    REXGPU_INFO("[RTLOG] ==== capture start at {:.1f} s (frame {}) ====", seconds, g_frame);
  }
  // Forget old resolve destinations now and then; the game reuses them every
  // frame, so they come straight back.
  if (g_frame % 600 == 0) g_resolve_dests.clear();

  if (g_frame % kStatFrames == 0) {
    const Counters& c = g_counters;
    const Counters& l = g_last_report;
    auto per = [](uint64_t now_value, uint64_t last_value) {
      return double(now_value - last_value) / kStatFrames;
    };
    REXGPU_INFO(
        "[RTSTAT] per frame: draws {:.0f}, rt changes {:.1f}, transfers {:.1f} (into {:.1f} "
        "RTs), resolves {:.1f} ({:.2f} MB, {:.1f} clears), dumps {:.1f}, texture loads {:.1f} "
        "({:.2f} MB, {:.1f} of resolve output), GPU {:.2f} ms, predicated skips {:.1f}",
        per(c.draws, l.draws), per(c.rt_changes, l.rt_changes), per(c.transfers, l.transfers),
        per(c.transfer_rts, l.transfer_rts), per(c.resolves, l.resolves),
        per(c.resolve_bytes, l.resolve_bytes) / 1e6, per(c.resolve_clears, l.resolve_clears),
        per(c.dumps, l.dumps), per(c.tex_loads, l.tex_loads),
        per(c.tex_load_bytes, l.tex_load_bytes) / 1e6,
        per(c.tex_loads_resolved, l.tex_loads_resolved), per(c.gpu_ns, l.gpu_ns) / 1e6,
        per(c.predicated_skips, l.predicated_skips));
    std::string breakdown;
    for (uint32_t i = 0; i < uint32_t(GpuCategory::kCount); ++i) {
      double ms = per(c.gpu_category_ns[i], l.gpu_category_ns[i]) / 1e6;
      if (ms >= 0.005) {
        breakdown += fmt::format("{}{} {:.2f}", breakdown.empty() ? "" : ", ",
                                 GpuCategoryName(GpuCategory(i)), ms);
      }
    }
    REXGPU_INFO("[RTSTAT] GPU ms per frame by category: {}", breakdown);
    g_last_report = c;
  }
}

}  // namespace fh2::rtlog

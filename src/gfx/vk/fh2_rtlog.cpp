#include "fh2_rtlog.h"

#include <rex/graphics/command_processor.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rex::graphics {
extern std::atomic<uint64_t> g_shared_memory_upload_bytes;
}  // namespace rex::graphics

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

std::vector<std::pair<uint32_t, uint32_t>> g_memory_resolves[2];  // [current, previous]

void NoteMemoryResolve(uint32_t address, uint32_t length) {
  if (length) g_memory_resolves[0].emplace_back(address, address + length);
}

bool OverlapsRecentMemoryResolve(uint32_t address, uint32_t length) {
  uint32_t end = address + length;
  for (const auto& list : g_memory_resolves) {
    for (const auto& range : list) {
      if (range.first < end && range.second > address) return true;
    }
  }
  return false;
}

std::vector<std::pair<uint32_t, uint32_t>> g_non_texture_readers;

void NoteNonTextureReader(uint32_t address, uint32_t length) {
  uint32_t end = address + length;
  for (const auto& range : g_non_texture_readers) {
    if (range.first <= address && range.second >= end) return;
  }
  g_non_texture_readers.emplace_back(address, end);
  REXGPU_INFO("[RTSTAT] vertex fetch reads resolve output: {:08X} len {:X}", address, length);
}

bool OverlapsNonTextureReader(uint32_t address, uint32_t length) {
  uint32_t end = address + length;
  for (const auto& range : g_non_texture_readers) {
    if (range.first < end && range.second > address) return true;
  }
  return false;
}

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

double SecondsSinceStart() {
  if (!g_frame) return 0.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

namespace {
struct TransferModeStep {
  double from_seconds;
  TransferMode mode;
};
std::vector<TransferModeStep> ParseTransferModes() {
  std::vector<TransferModeStep> steps;
  const char* env = std::getenv("FH2_TRANSFER_MODES");
  if (!env) return steps;
  std::string text(env);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string::npos) comma = text.size();
    std::string item = text.substr(pos, comma - pos);
    size_t at = item.find('@');
    std::string name = item.substr(0, at);
    double seconds = at == std::string::npos ? 0.0 : std::atof(item.c_str() + at + 1);
    TransferMode mode = name == "none"      ? TransferMode::kNone
                        : name == "samefmt" ? TransferMode::kSameFormat
                                            : TransferMode::kAll;
    steps.push_back({seconds, mode});
    pos = comma + 1;
  }
  return steps;
}
const std::vector<TransferModeStep> g_transfer_steps = ParseTransferModes();
}  // namespace

int TimedEnvIntUncached(const char* name, int default_value);

int TimedEnvInt(const char* name, int default_value) {
  // Per-draw callers: answer from a per-frame cache keyed by the (literal)
  // name pointer; the parse and the clock read happen once per frame.
  struct CacheEntry {
    const char* name;
    int default_value;
    uint64_t frame;
    int value;
  };
  static CacheEntry cache[16];
  static uint32_t cache_size = 0;
  for (uint32_t i = 0; i < cache_size; ++i) {
    CacheEntry& entry = cache[i];
    if (entry.name == name && entry.default_value == default_value) {
      if (entry.frame != g_frame) {
        entry.frame = g_frame;
        entry.value = TimedEnvIntUncached(name, default_value);
      }
      return entry.value;
    }
  }
  int value = TimedEnvIntUncached(name, default_value);
  if (cache_size < 16) {
    cache[cache_size++] = {name, default_value, g_frame, value};
  }
  return value;
}

// Live overrides set through the game's debug channel (env <NAME> <value>).
namespace {
std::mutex g_live_env_mutex;
std::map<std::string, int> g_live_env;
}  // namespace

extern "C" __attribute__((visibility("default"))) void Fh2SetLiveEnv(const char* name, int value,
                                                                      int clear) {
  std::lock_guard<std::mutex> lock(g_live_env_mutex);
  if (clear) {
    g_live_env.erase(name);
  } else {
    g_live_env[name] = value;
  }
}

int TimedEnvIntUncached(const char* name, int default_value) {
  {
    std::lock_guard<std::mutex> lock(g_live_env_mutex);
    auto live = g_live_env.find(name);
    if (live != g_live_env.end()) return live->second;
  }
  struct Step {
    double from_seconds;
    int value;
  };
  static std::map<std::string, std::vector<Step>> parsed;
  auto it = parsed.find(name);
  if (it == parsed.end()) {
    std::vector<Step> steps;
    if (const char* env = std::getenv(name)) {
      std::string text(env);
      size_t pos = 0;
      while (pos < text.size()) {
        size_t comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        std::string item = text.substr(pos, comma - pos);
        size_t at = item.find('@');
        steps.push_back({at == std::string::npos ? 0.0 : std::atof(item.c_str() + at + 1),
                         std::atoi(item.substr(0, at).c_str())});
        pos = comma + 1;
      }
    }
    it = parsed.emplace(name, std::move(steps)).first;
  }
  int value = default_value;
  double seconds = SecondsSinceStart();
  for (const auto& step : it->second) {
    if (seconds >= step.from_seconds) value = step.value;
  }
  return value;
}

TransferMode CurrentTransferMode() {
  // Default: FH2 never reads EDRAM reinterpreted as an unrelated format (the
  // game clears after such rebinds), but its 4x-MSAA fast clears reach the 1x
  // targets through same-format transfers -- dropping those breaks shadows.
  TransferMode mode = TransferMode::kSameFormat;
  double seconds = SecondsSinceStart();
  for (const TransferModeStep& step : g_transfer_steps) {
    if (seconds >= step.from_seconds) mode = step.mode;
  }
  // Live override (debug channel: env FH2_TRANSFER_ALL 1).
  if (TimedEnvInt("FH2_TRANSFER_ALL", 0) == 1) mode = TransferMode::kAll;
  return mode;
}

const char* TransferModeName(TransferMode mode) {
  switch (mode) {
    case TransferMode::kNone: return "none";
    case TransferMode::kSameFormat: return "samefmt";
    default: return "all";
  }
}

namespace {
// Spike logger: any frame (swap to swap) over kSpikeMs is logged with what
// happened during it.
constexpr double kSpikeMs = 25.0;
Counters g_frame_start_counters;
uint64_t g_frame_start_idle_ns = 0, g_frame_start_wait_ns = 0, g_frame_start_upload = 0;
std::chrono::steady_clock::time_point g_last_swap;
}  // namespace

std::atomic<int> g_capture_request{0};

// Debug channel: log the next n whole frames ([RTLOG]).
extern "C" __attribute__((visibility("default"))) void Fh2CaptureFrames(int n) {
  g_capture_request = n > 0 ? n : 1;
}

void OnSwap() {
  auto now = std::chrono::steady_clock::now();
  {
    uint64_t idle_ns = rex::graphics::g_command_processor_idle_ns.load(std::memory_order_relaxed);
    uint64_t wait_ns =
        rex::graphics::g_command_processor_wait_reg_mem_ns.load(std::memory_order_relaxed);
    uint64_t upload = rex::graphics::g_shared_memory_upload_bytes.load(std::memory_order_relaxed);
    if (g_frame > 300) {
      double frame_ms = std::chrono::duration<double, std::milli>(now - g_last_swap).count();
      if (frame_ms > kSpikeMs) {
        const Counters& c = g_counters;
        const Counters& s = g_frame_start_counters;
        REXGPU_INFO(
            "[SPIKE] frame {}: {:.1f} ms -- command thread idle {:.1f} ms, WAIT_REG_MEM {:.1f} ms; "
            "draws {}, texture loads {} ({:.2f} MB), resolves {}, transfers {}, new shaders {}, "
            "new pipelines {}, GPU (completed) {:.1f} ms, blocking WAIT_REG_MEMs {}, fence waits "
            "{:.1f} ms, uploads {:.2f} MB",
            g_frame, frame_ms, double(idle_ns - g_frame_start_idle_ns) / 1e6,
            double(wait_ns - g_frame_start_wait_ns) / 1e6, c.draws - s.draws,
            c.tex_loads - s.tex_loads, double(c.tex_load_bytes - s.tex_load_bytes) / 1e6,
            c.resolves - s.resolves, c.transfers - s.transfers,
            c.shaders_translated - s.shaders_translated,
            c.pipelines_created - s.pipelines_created, double(c.gpu_ns - s.gpu_ns) / 1e6,
            c.wait_reg_mem_blocks - s.wait_reg_mem_blocks,
            double(c.fence_wait_ns - s.fence_wait_ns) / 1e6,
            double(upload - g_frame_start_upload) / 1e6);
      }
    }
    g_last_swap = now;
    g_frame_start_counters = g_counters;
    g_frame_start_idle_ns = idle_ns;
    g_frame_start_wait_ns = wait_ns;
    g_frame_start_upload = upload;
  }
  if (g_frame++ == 0) g_start = now;
  double seconds = std::chrono::duration<double>(now - g_start).count();

  if (g_active) {
    REXGPU_INFO("[RTLOG] ==== swap (end of frame {}) ====", g_frame - 1);
    if (--g_frames_left == 0) {
      g_active = false;
      REXGPU_INFO("[RTLOG] ==== capture end ====");
    }
  }
  if (!g_active && g_capture_request.load() > 0) {
    g_active = true;
    g_frames_left = g_capture_request.exchange(0);
    REXGPU_INFO("[RTLOG] ==== capture start (requested) at {:.1f} s (frame {}) ====", seconds,
                g_frame);
  }
  if (!g_active && g_next_time < g_times.size() && seconds >= g_times[g_next_time]) {
    ++g_next_time;
    g_active = true;
    g_frames_left = kFramesPerCapture;
    REXGPU_INFO("[RTLOG] ==== capture start at {:.1f} s (frame {}) ====", seconds, g_frame);
  }
  g_memory_resolves[1] = std::move(g_memory_resolves[0]);
  g_memory_resolves[0].clear();
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
        "[RTSTAT] per frame (transfers mode {}): draws {:.0f}, rt changes {:.1f}, transfers {:.1f} (into {:.1f} "
        "RTs), resolves {:.1f} ({:.2f} MB, {:.1f} clears), dumps {:.1f}, texture loads {:.1f} "
        "({:.2f} MB, {:.1f} of resolve output), GPU {:.2f} ms, predicated skips {:.1f}, "
        "direct resolves {:.1f} (misses {:.1f}, memory skipped {:.1f}, kept {:.1f}), transfers narrowed {:.1f} skipped {:.1f} "
        "deferred {:.1f} materialized {:.1f} cleared {:.1f}, deduped draws {:.1f}, vfetch skipped {:.1f}",
        TransferModeName(CurrentTransferMode()), per(c.draws, l.draws), per(c.rt_changes, l.rt_changes), per(c.transfers, l.transfers),
        per(c.transfer_rts, l.transfer_rts), per(c.resolves, l.resolves),
        per(c.resolve_bytes, l.resolve_bytes) / 1e6, per(c.resolve_clears, l.resolve_clears),
        per(c.dumps, l.dumps), per(c.tex_loads, l.tex_loads),
        per(c.tex_load_bytes, l.tex_load_bytes) / 1e6,
        per(c.tex_loads_resolved, l.tex_loads_resolved), per(c.gpu_ns, l.gpu_ns) / 1e6,
        per(c.predicated_skips, l.predicated_skips), per(c.direct_resolves, l.direct_resolves),
        per(c.direct_resolve_misses, l.direct_resolve_misses),
        per(c.resolves_memory_skipped, l.resolves_memory_skipped),
        per(c.resolves_memory_kept, l.resolves_memory_kept),
        per(c.transfers_narrowed, l.transfers_narrowed),
        per(c.transfers_skipped, l.transfers_skipped),
        per(c.transfers_deferred, l.transfers_deferred),
        per(c.transfers_materialized, l.transfers_materialized),
        per(c.transfers_cleared, l.transfers_cleared), per(c.draws_deduped, l.draws_deduped),
        per(c.vfetch_requests_skipped, l.vfetch_requests_skipped));
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

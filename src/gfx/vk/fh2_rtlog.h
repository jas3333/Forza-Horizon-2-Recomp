// FH2 renderer frame log and per-frame counters.
//
// FH2_RTLOG_SEC=20,45 logs every render-target change, resolve and texture
// load of two whole frames starting at each of those times (seconds since the
// first swap), as "[RTLOG]" lines. This is the frame graph the native
// render-target design is built from. The counters are always on and are
// reported as one "[RTSTAT]" line every 300 frames.
#pragma once

#include <cstdint>

#include <fmt/format.h>

#include <rex/logging.h>

namespace fh2::rtlog {

// GPU time categories, attributed between consecutive GPU timestamps.
enum class GpuCategory : uint8_t {
  kOther,
  kDraw,
  kTransfer,
  kDump,
  kResolveCopy,
  kResolveClear,
  kTextureLoad,
  kSwap,
  kCount,
};
inline const char* GpuCategoryName(GpuCategory category) {
  static const char* const kNames[] = {"other", "draws", "transfers", "dumps", "resolve copy",
                                       "resolve clear", "texture loads", "swap"};
  return kNames[uint32_t(category)];
}

struct Counters {
  uint64_t draws = 0;
  uint64_t rt_changes = 0;       // render target set changes (render pass breaks)
  uint64_t transfers = 0;        // EDRAM ownership transfers
  uint64_t transfer_rts = 0;     // destination render targets with transfers
  uint64_t resolves = 0;
  uint64_t resolve_bytes = 0;    // bytes written to guest memory by resolves
  uint64_t resolve_clears = 0;
  uint64_t dumps = 0;            // render target dumps to the EDRAM buffer
  uint64_t tex_loads = 0;
  uint64_t tex_load_bytes = 0;
  uint64_t tex_loads_resolved = 0;  // loads overlapping a resolve destination
  uint64_t gpu_ns = 0;              // GPU execution time of completed submissions
  uint64_t predicated_skips = 0;    // draws skipped by predicated tiling bin checks
  uint64_t gpu_category_ns[uint32_t(GpuCategory::kCount)] = {};
};

extern Counters g_counters;
extern bool g_active;

// Called by IssueSwap: advances the frame, arms or disarms logging, reports
// the counters.
void OnSwap();

// Resolve destinations of the logged and recent frames, to tag texture loads
// that read resolve output.
void NoteResolveDest(uint32_t address, uint32_t length);
bool OverlapsResolveDest(uint32_t address, uint32_t length);

template <typename... Args>
inline void Log(fmt::format_string<Args...> format, Args&&... args) {
  if (g_active) {
    REXGPU_INFO("[RTLOG] {}", fmt::format(format, std::forward<Args>(args)...));
  }
}

}  // namespace fh2::rtlog

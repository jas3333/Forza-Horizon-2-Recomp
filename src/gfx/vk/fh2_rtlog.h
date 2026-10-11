// FH2 renderer frame log and per-frame counters.
//
// FH2_RTLOG_SEC=20,45 logs every render-target change, resolve and texture
// load of two whole frames starting at each of those times (seconds since the
// first swap), as "[RTLOG]" lines. This is the frame graph the native
// render-target design is built from. The counters are always on and are
// reported as one "[RTSTAT]" line every 300 frames.
#pragma once

#include <cstdint>
#include <string>

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
  uint64_t direct_resolves = 0;     // resolves written straight into textures
  uint64_t resolves_memory_skipped = 0;  // of those, without the guest memory write
  uint64_t resolves_memory_kept = 0;     // skip refused: other textures read the range
  uint64_t transfers_narrowed = 0;  // transfers clipped to the rows written since
  uint64_t transfers_skipped = 0;   // transfers dropped (nothing written since)
  uint64_t transfers_deferred = 0;  // transfers skipped as nothing read the target yet
  uint64_t transfers_cleared = 0;   // copy-backs replaced by depth clears
  uint64_t full_frame_passes = 0;   // tiled passes drawn full-frame
  uint64_t draws_deduped = 0;       // strip draws skipped as already drawn full-frame
  uint64_t shaders_translated = 0;
  uint64_t wait_reg_mem_blocks = 0;  // WAIT_REG_MEMs that had to wait
  uint64_t fence_wait_ns = 0;        // time in submission fence checks / waits
  uint64_t pipelines_created = 0;
  uint64_t transfers_materialized = 0;  // deferred copies made real later
  uint64_t direct_resolve_misses = 0;  // color resolves that couldn't be
  uint64_t predicated_skips = 0;
  uint64_t vfetch_requests_skipped = 0;  // vertex ranges known valid, not re-requested    // draws skipped by predicated tiling bin checks
  uint64_t gpu_category_ns[uint32_t(GpuCategory::kCount)] = {};
};

extern Counters g_counters;
extern bool g_active;

// Called by IssueSwap: advances the frame, arms or disarms logging, reports
// the counters.
void OnSwap();

// Seconds since the first swap.
double SecondsSinceStart();

// Experiment: which EDRAM ownership transfers to perform.
// Default samefmt; FH2_TRANSFER_MODES="all@0,samefmt@45,none@60" switches
// by time (an entry @0 overrides the default).
enum class TransferMode { kAll, kSameFormat, kNone };
TransferMode CurrentTransferMode();

// Integer setting from an environment variable "value@seconds,value@seconds"
// (the last entry whose time has passed wins; default before any).
int TimedEnvInt(const char* name, int default_value);
const char* TransferModeName(TransferMode mode);

// Resolve destinations of the logged and recent frames, to tag texture loads
// that read resolve output.
void NoteResolveDest(uint32_t address, uint32_t length);
// Resolves that went through guest memory (not direct), this frame and the
// previous one: a texture over such a range may be reloaded from memory, so
// the memory under it must stay valid.
void NoteMemoryResolve(uint32_t address, uint32_t length);
bool OverlapsRecentMemoryResolve(uint32_t address, uint32_t length);
// Non-texture readers of resolve output (vertex fetch): their ranges must keep
// guest memory written. Logged once per range.
void NoteNonTextureReader(uint32_t address, uint32_t length);
bool OverlapsNonTextureReader(uint32_t address, uint32_t length);
bool OverlapsResolveDest(uint32_t address, uint32_t length);

// Per-pass GPU profile (ctl "passprof <frames>"): every render target change
// starts a pass, keyed by its description and its occurrence in the frame;
// GPU time between pass marks is summed per key and the top passes are
// logged as [PASSPROF] lines a few frames after the profiled ones.
extern bool g_pass_profile;
// Starts a pass (returns its id for the GPU mark), or UINT32_MAX when not
// profiling.
uint32_t PassBegin(const std::string& description);
void PassAddDraw();
// Events inside the current pass: 0 render pass begin, 1 barrier batch,
// 2 graphics pipeline bind.
void PassAddEvent(int kind);
void PassAddGpuTime(uint32_t pass_id, uint64_t ns);

template <typename... Args>
inline void Log(fmt::format_string<Args...> format, Args&&... args) {
  if (g_active) {
    REXGPU_INFO("[RTLOG] {}", fmt::format(format, std::forward<Args>(args)...));
  }
}

}  // namespace fh2::rtlog

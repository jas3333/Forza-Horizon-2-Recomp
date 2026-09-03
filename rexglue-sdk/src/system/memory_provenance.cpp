/**
 * @file        system/memory_provenance.cpp
 * @brief       Filtered guest-memory write provenance for crash diagnostics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 *
 * @remarks     The runtime-gated data-trace design follows Xenia Edge commit
 *              b975095fd. This static-recompiler implementation records only
 *              stores that overlap one selected guest-memory range.
 */

#include <algorithm>
#include <array>
#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/memory_provenance.h>
#include <rex/system/thread_state.h>

REXCVAR_DEFINE_UINT32(memory_trace_address, 0, "Debug",
                      "First guest address in the memory provenance range")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_UINT32(memory_trace_length, 0, "Debug",
                      "Length of the guest memory provenance range")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::runtime {
namespace {

struct MemoryProvenanceSlot {
  std::atomic<uint64_t> committed_sequence{0};
  std::atomic<uint32_t> thread_id{0};
  std::atomic<uint32_t> function{0};
  std::atomic<uint32_t> instruction{0};
  std::atomic<uint32_t> address{0};
  std::atomic<uint32_t> size{0};
  std::atomic<uint64_t> value{0};
};

constexpr uint64_t kMemoryProvenanceCapacity = 64;
static_assert((kMemoryProvenanceCapacity & (kMemoryProvenanceCapacity - 1)) == 0);

std::array<MemoryProvenanceSlot, kMemoryProvenanceCapacity> g_slots;
std::atomic<uint64_t> g_next_sequence{0};

}  // namespace

void ConfigureMemoryProvenanceContext(PPCContext& context) noexcept {
  context.memory_trace_address = REXCVAR_GET(memory_trace_address);
  context.memory_trace_length = REXCVAR_GET(memory_trace_length);
}

void RecordMemoryProvenanceStore(const PPCContext& context, uint32_t address,
                                 uint32_t size, uint64_t value) noexcept {
  const uint64_t sequence = g_next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  auto& slot = g_slots[(sequence - 1) & (kMemoryProvenanceCapacity - 1)];
  slot.committed_sequence.store(0, std::memory_order_relaxed);
  slot.thread_id.store(ThreadState::GetThreadID(), std::memory_order_relaxed);
  slot.function.store(context.current_function, std::memory_order_relaxed);
  slot.instruction.store(context.current_instruction, std::memory_order_relaxed);
  slot.address.store(address, std::memory_order_relaxed);
  slot.size.store(size, std::memory_order_relaxed);
  slot.value.store(value, std::memory_order_relaxed);
  slot.committed_sequence.store(sequence, std::memory_order_release);
}

void LogMemoryProvenance() noexcept {
  const uint64_t end = g_next_sequence.load(std::memory_order_acquire);
  const uint64_t begin = end > kMemoryProvenanceCapacity
                             ? end - kMemoryProvenanceCapacity + 1
                             : 1;
  for (uint64_t sequence = begin; sequence <= end; ++sequence) {
    const auto& slot = g_slots[(sequence - 1) & (kMemoryProvenanceCapacity - 1)];
    if (slot.committed_sequence.load(std::memory_order_acquire) != sequence) {
      continue;
    }
    REXLOG_ERROR(
        "[AVDIAG] memory write: sequence=0x{:X} thread=0x{:X} function=0x{:08X} "
        "pc=0x{:08X} address=0x{:08X} size=0x{:X} value=0x{:016X}",
        sequence, slot.thread_id.load(std::memory_order_relaxed),
        slot.function.load(std::memory_order_relaxed),
        slot.instruction.load(std::memory_order_relaxed),
        slot.address.load(std::memory_order_relaxed),
        slot.size.load(std::memory_order_relaxed),
        slot.value.load(std::memory_order_relaxed));
  }
}

}  // namespace rex::runtime

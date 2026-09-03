/**
 * @file        system/checkpoint.h
 * @brief       Cooperative guest checkpoint boundary probe
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace rex::system {

struct CheckpointProbeResult {
  // True as soon as a running main guest thread object was found, regardless
  // of whether it hit a checkpoint before the timeout. Distinguishes "no main
  // guest thread exists/is running" from "found it, but it never reached a
  // recompiled function boundary in time" -- the two collapsed to the same
  // "timed out" outcome before this field was added.
  bool thread_found = false;
  bool reached = false;
  uint32_t thread_id = 0;
  // The guest kernel handle (XObject::handle(), 0xF8xxxxxx-style), so this
  // result can be cross-referenced against other guest-handle-keyed logging
  // (e.g. [WAITDIAG] thread=0x...) to check whether the "stuck" main thread
  // is the same thread already visible elsewhere as a tight poller.
  uint32_t thread_handle = 0;
  uint32_t guest_address = 0;
  uint32_t guest_lr = 0;
  uint32_t guest_stack_pointer = 0;
  // Populated only when thread_found && !reached: an unsynchronized read of
  // the stuck thread's own PPCContext (current_function/current_instruction
  // are updated per basic block by the generated code), taken directly
  // instead of via the cooperative park -- the whole point is that this
  // thread never reaches a checkpoint to park at. Racy by nature (the target
  // thread may be mutating these concurrently); good enough to answer "what
  // guest function is it inside of right now", not to be trusted as atomic.
  uint32_t last_known_function = 0;
  uint32_t last_known_instruction = 0;
  // Same unsynchronized-peek rationale, extended to the most recent indirect
  // (bctr/bctrl) call this thread made -- lets us see who dispatched into a
  // function that has no static callers/xrefs (vtable/table-driven calls),
  // without needing an actual crash to trigger the existing AVDIAG dump of
  // this same data.
  uint32_t last_indirect_target = 0;
  uint32_t last_indirect_caller = 0;
  uint32_t last_indirect_callsite = 0;
  uint32_t last_indirect_r3 = 0;
  uint32_t last_indirect_r4 = 0;
  // Table-based ("interior-PC resume") dispatch is a separate mechanism from
  // a plain bctr/bctrl indirect call -- see PPCContext::dispatch_address.
  // A function with no static bl callers and no bctr xref may still be
  // reached this way, so capture it too.
  uint32_t last_dispatch_target = 0;
  uint32_t last_dispatch_kind = 0;
  uint32_t last_dispatch_arg_count = 0;
  uint32_t last_dispatch_r3 = 0;
  uint32_t last_dispatch_r4 = 0;
  // Same walk as [AVDIAG]'s "guest return-addr chain" in
  // exception_handler_win.cpp, applied live to a stuck-but-not-crashed
  // thread instead of a crash dump: scans the guest stack from r1 for words
  // that look like code addresses, to recover the call chain without
  // needing symbols or a crash. Space-separated hex addresses, outermost
  // (deepest/oldest) call first, same as the AVDIAG formatting.
  std::string guest_return_chain;
  // 2026-09-02, one-off investigation aid: the guest return-addr chain
  // walk (above) is a heuristic that can pick up stale words left over from
  // an unrelated earlier stack frame -- it pointed at a call site that
  // passes a fixed global address (0x8342B654, a sync-object instance whose
  // first field is a kernel handle, per Ghidra) into the same generic Wait
  // utility the main thread is frozen inside of. Reading this address live
  // confirms or refutes whether that lead is real or stale stack litter.
  uint32_t debug_probe_object_field0 = 0;
};

/** Called at the entry of each generated guest function. */
void PollCheckpointBoundary(PPCContext& context, uint32_t guest_address);

/** Park and resume the main guest thread at a generated function boundary. */
CheckpointProbeResult ProbeMainThreadCheckpoint(uint32_t timeout_ms = 2000);

// 2026-09-02: built while chasing a Semaphore (handle 0xf8000038) that the
// main guest thread waits on forever but that no observed
// KeReleaseSemaphore/NtReleaseSemaphore call ever targets. Working theory:
// whatever guest thread is supposed to release it never runs far enough to
// do so (or never starts at all). This is a cheap, non-cooperative snapshot
// of every guest thread's last-known PPCContext state (same unsynchronized
// peek as CheckpointProbeResult::last_known_*) to see at a glance which
// threads exist and which of them, if any, are themselves stuck.
struct GuestThreadSnapshot {
  uint32_t thread_id = 0;
  uint32_t handle = 0;
  bool is_main = false;
  bool is_running = false;
  uint32_t last_known_function = 0;
  uint32_t last_known_instruction = 0;
};
std::vector<GuestThreadSnapshot> SnapshotAllGuestThreads();

// Dumps the main guest thread's [CALLTRACE] ring buffer (PPCContext::
// call_trace, populated by REX_HOOK when the calltrace_kernel_calls cvar is
// on) in chronological order (oldest surviving entry first). Same
// unsynchronized live-peek approach as the rest of this file -- safe because
// entries are plain string-literal pointers (program lifetime), never freed.
// Empty if calltrace_kernel_calls was never enabled or no main thread is
// running.
std::vector<const char*> DumpMainThreadCallTrace();

// Same as DumpMainThreadCallTrace, but for any guest thread by its small
// integer thread_id (the "id=" field GuestThreadSnapshot/[THREADSNAP] report)
// instead of always the main one. Needed to compare a specific worker
// thread's kernel-call history against an equivalent thread's trace from a
// reference emulator (e.g. Xenia's own debug log) when that worker, not the
// main thread, is the one that does the real boot-sequence work.
std::vector<const char*> DumpThreadCallTraceById(uint32_t guest_thread_id);

}  // namespace rex::system

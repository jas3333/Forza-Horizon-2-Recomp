#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <rex/exception_handler.h>
#endif
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmodule.h>
#include <rex/system/xobject.h>
#include <rex/kernel/crt/heap.h>

#include "diagnostics/heap_tracker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {

// Reusable diagnostic (see D:\GAME RECOMP\tools\diagnostics\heap_tracker.h
// and the "Ferramentas de diagnóstico" section of the project's methodology
// doc): tracks real Alloc()/Free() traffic through our CRT heap plus every
// "leaked free" the sub_83131408 workaround below has to drop, ranked by
// guest call site (lr). Started lazily on first use; logs a summary every 5s
// only while there is something to report, so it stays silent on a healthy
// run instead of adding noise.
void EnsureHeapDiagnosticsThreadStarted() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      using namespace std::chrono_literals;
      for (;;) {
        std::this_thread::sleep_for(5s);
        auto totals = recomp_diag::HeapTracker::Instance().GetTotals();
        if (totals.leaked_free_total == 0 && totals.live_block_count == 0) {
          continue;
        }
        REXKRNL_WARN(
            "[HeapDiag] live_blocks={} live_bytes={} alloc_count={} free_count={} "
            "unknown_free_count={} leaked_free_total={}",
            totals.live_block_count, totals.live_bytes, totals.alloc_count, totals.free_count,
            totals.unknown_free_count, totals.leaked_free_total);
        for (auto& site : recomp_diag::HeapTracker::Instance().TopLeakedFreeSites(10)) {
          REXKRNL_WARN("[HeapDiag] leaked_free site lr={:08X} count={}", site.caller_pc,
                       site.count);
        }
        for (auto& site : recomp_diag::HeapTracker::Instance().TopLiveAllocationSites(10)) {
          REXKRNL_WARN("[HeapDiag] live_alloc site lr={:08X} tag={} count={} bytes={}",
                       site.caller_pc, site.tag, site.count, site.bytes);
        }
      }
    }).detach();
  });
}

static std::atomic<uint32_t> g_xmedia_call_count{0};

// Commits guest pages on demand so the title survives faults it would
// otherwise die on. Load-bearing, and measured as such: removing it turned a
// boot that reached the controller-tips screen into an immediate crash --
// 70 log lines, 0xC0000005 inside rexruntime.dll -- on the very first
// "write of guest 0x00000054" that appears in every session.
//
// It was removed once on the theory that VirtualAlloc cannot commit inside
// ReXGlue's file-mapped guest arena, so this could never have been doing
// anything. The theory was wrong for the regions the title actually faults
// on, and the run above is what settled it. Note the ReXGlue log line
// "Unhandled guest access violation" still appears for other faults, so
// this recovers some but not all of them.
//
// This is still a workaround, not a fix: pages the title expects to be
// committed are not, and something upstream should be committing them.
#ifdef _WIN32
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
    // Guest address space window (virtual base 0x1_0000_0000, physical base
    // 0x2_0000_0000, plus headroom).
    if (fault_addr >= 0x0000000100000000ull && fault_addr < 0x0000000600000000ull) {
      uintptr_t page_base = fault_addr & ~0xFFFFull;
      void* res = VirtualAlloc((void*)page_base, 0x10000, MEM_COMMIT, PAGE_READWRITE);
      if (res) {
        DWORD old_prot;
        VirtualProtect(res, 0x10000, PAGE_READWRITE, &old_prot);
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

struct CrashHandlerInit {
  CrashHandlerInit() {
    AddVectoredExceptionHandler(1, CrashFilter);
  }
} g_crashHandlerInit;
#else
// POSIX port of CrashFilter. Guest pages are reserved PROT_NONE in the SDK's
// shared-memory arena, so "committing" one is an mprotect. This goes through
// the SDK's exception chain rather than a raw sigaction so its own SIGSEGV
// users (MMIO, GPU write watches) see faults first; it is installed from
// OnPreLaunchModule(), after the runtime has registered those.
bool CrashFilter(rex::arch::Exception* ex, void* data) {
  (void)data;
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) {
    return false;
  }
  uint64_t fault_addr = ex->fault_address();
  if (fault_addr < 0x0000000100000000ull || fault_addr >= 0x0000000600000000ull) {
    return false;
  }
  uintptr_t page_base = fault_addr & ~0xFFFFull;
  return mprotect(reinterpret_cast<void*>(page_base), 0x10000, PROT_READ | PROT_WRITE) == 0;
}
#endif

static uint32_t GetXMediaExport(uint32_t ordinal) {
  switch (ordinal) {
    case 1:  return 0x88050388;
    case 2:  return 0x880503A0;
    case 3:  return 0x880503B8;
    case 4:  return 0x880503C0;
    case 5:  return 0x880503C8;
    case 6:  return 0x880503D0;
    case 7:  return 0x880503D8;
    case 8:  return 0x880503E0;
    case 9:  return 0x880503E8;
    case 10: return 0x88050448;
    case 11: return 0x88050450;
    case 12: return 0x88050458;
    case 13: return 0x880504F0;
    case 14: return 0x880504F8;
    default: return 0;
  }
}

static uint32_t GetSpeechExport(uint32_t ordinal) {
  switch (ordinal) {
    case 1:  return 0x890A00D8;
    case 2:  return 0x890A00F0;
    case 3:  return 0x890A0108;
    case 4:  return 0x890A0110;
    case 5:  return 0x890A0118;
    case 6:  return 0x890A0120;
    case 7:  return 0x890A0128;
    case 8:  return 0x890A0130;
    case 9:  return 0x890A0138;
    case 10: return 0x890A0140;
    case 11: return 0x890A0148;
    case 12: return 0x890A0150;
    case 13: return 0x890A0158;
    case 14: return 0x890A0160;
    default: return 0;
  }
}

}  // namespace

void InstallGuestPageFaultRecovery() {
#ifndef _WIN32
  static std::once_flag once;
  std::call_once(once, [] { rex::arch::ExceptionHandler::Install(CrashFilter, nullptr); });
#endif
}

extern "C" void __imp__XamGetLiveHiveValueA(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 0x80070490; // ERROR_NOT_FOUND
}

// The SDK's object reference counting is asymmetric, and the title notices.
// xboxkrnl_ob.cpp implements ObDereferenceObject (it resolves the guest
// pointer and calls ReleaseHandle(), which can destroy the object) but leaves
// ObReferenceObject as a bare REX_EXPORT_STUB that does nothing. Every retain
// is therefore lost while every release counts, so shared kernel objects die
// while the title still holds them.
//
// That is the shape of the corruption seen during boot: the listener walk at
// guest 0x82D97D60 iterates an MSVC std::set and calls vtable[0] on each
// entry, and entries there were reading back as null -- a destroyed object
// still registered. This mirrors the existing Dereference implementation so
// the pair balances.
// DISABLED pending isolation. Implementing the retain made a clean boot that
// previously reached the tips screen crash on START instead, at guest
// 0x824A61B8 -- the release-then-destroy path -- with r3=0x494E5400, which is
// the ASCII bytes "INT\0" rather than a pointer.
//
// The asymmetry is real and worth returning to, but the retain has to be
// narrower than this. ObReferenceObject is called with whatever pointer the
// title holds, not only kernel objects, and GetNativeObject() will happily
// interpret an arbitrary guest address as an object header. Retaining on a
// misidentified pointer is worse than not retaining at all, which is why
// Xenia leaves this one alone too.
#if 0
extern "C" void __imp__ObReferenceObject(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t native_ptr = ctx.r3.u32;
  if (native_ptr == 0 || native_ptr == 0xDEADF00D) {
    ctx.r3.u64 = 0;
    return;
  }
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    ctx.r3.u64 = 0;
    return;
  }
  auto object = rex::system::XObject::GetNativeObject<rex::system::XObject>(
      ks, ks->memory()->TranslateVirtual(native_ptr));
  if (object) {
    object->RetainHandle();
  }
  ctx.r3.u64 = 0;
}
#endif

// The SDK ships NetDll_getsockopt as a bare REX_EXPORT_STUB: it logs, leaves
// r3 holding whatever was there, and never touches the caller's optval. That
// is not harmless here. At guest 0x83229DC0 the title does:
//
//   socket(AF_INET, SOCK_DGRAM);            // r30 = handle
//   [r31+0x2C] = 0x2000;                    // default receive buffer, 8 KB
//   getsockopt(s, SOL_SOCKET, SO_RCVBUF, &optval, &optlen);
//   if (result != -1) [r31+0x2C] = optval;  // adopt the queried size
//
// With the stub, "result" is a leftover register and optval is uninitialised
// stack, so the title stores a garbage receive-buffer size over its own sane
// default and then sizes an allocation from it. That feeds the allocator
// retry loop at 0x8241A8E8 (allocate, wait, retry x100), which is the
// endless "LOADING... PLEASE WAIT".
//
// Argument order comes from the import thunk at 0x82BE53D8, which shifts the
// guest's five arguments up and prepends the caller id:
//   r3 caller, r4 socket, r5 level, r6 optname, r7 optval*, r8 optlen*
extern "C" void __imp__NetDll_getsockopt(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kSolSocket = 0xFFFF;
  constexpr uint32_t kSoSndBuf = 0x1001;
  constexpr uint32_t kSoRcvBuf = 0x1002;
  constexpr uint32_t kSoError = 0x1007;
  // Matches the title's own default at 0x83229DC0, so adopting it is a no-op.
  constexpr uint32_t kDefaultSocketBuffer = 0x2000;

  uint32_t level = ctx.r5.u32;
  uint32_t optname = ctx.r6.u32;
  uint32_t optval_ptr = ctx.r7.u32;
  uint32_t optlen_ptr = ctx.r8.u32;

  auto fail = [&ctx]() { ctx.r3.s64 = -1; };  // SOCKET_ERROR: caller keeps its default

  if (level != kSolSocket || optval_ptr == 0 || optlen_ptr == 0) {
    fail();
    return;
  }
  uint32_t optlen = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + optlen_ptr));
  if (optlen < sizeof(uint32_t)) {
    fail();
    return;
  }

  uint32_t value = 0;
  switch (optname) {
    case kSoRcvBuf:
    case kSoSndBuf:
      value = kDefaultSocketBuffer;
      break;
    case kSoError:
      value = 0;  // no pending error
      break;
    default:
      // Anything not answered here must report failure rather than leave the
      // caller reading uninitialised memory.
      fail();
      return;
  }

  *reinterpret_cast<uint32_t*>(base + optval_ptr) = __builtin_bswap32(value);
  *reinterpret_cast<uint32_t*>(base + optlen_ptr) = __builtin_bswap32(sizeof(uint32_t));
  ctx.r3.u64 = 0;
}

// Tried removing this override so app_manager()->DispatchMessageAsync()
// (XmpApp/XgiApp/XLiveBaseApp/XamApp, now registered by kernel_init.cpp)
// would actually run instead of being permanently faked as immediate
// success. REVERTED: with the real dispatch running, the very first
// Storefront query the title makes on this path --
//   NtCreateFile FAILED path='B13EBABEBABEBABE:\StorefrontPurchaseHistory'
//   -> 0xc000000f (no Xbox Live, so no such content package exists)
// -- surfaces a failure the title does not handle safely: immediately after,
// the log shows a burst of "Unhandled guest access violation" reads at
// garbage addresses (0x19E15FE9, 0x690A2961, ...), then a hard
// [FATAL] indirect.unregistered target=0x00000000 lr=0x8246D7AC. That FATAL
// does not occur with the fake-success override in place, so real dispatch
// trades "XAM async messages are no-oped" for "the title crashes the first
// time one of them legitimately fails" -- worse, not better, until whatever
// reads r3=0x2E1773D0 near lr=0x8246D7AC is made to tolerate a real
// X_ERROR_NOT_FOUND/failed-lookup result instead of assuming success.
extern "C" void __imp__XMsgStartIORequestEx(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t overlapped = ctx.r8.u32;
  auto* ks = rex::system::kernel_state();
  if (ks && overlapped != 0) {
    ks->CompleteOverlappedImmediate(overlapped, 0);
  }
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

extern "C" void __imp__XMsgStartIORequest(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t overlapped = ctx.r7.u32;
  auto* ks = rex::system::kernel_state();
  if (ks && overlapped != 0) {
    ks->CompleteOverlappedImmediate(overlapped, 0);
  }
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

extern "C" void __imp__XMsgCancelIORequest(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

extern "C" void __imp__XexGetProcedureAddress(PPCContext& ctx, uint8_t* base) {
  uint32_t module_handle = ctx.r3.u32;
  uint32_t ordinal = ctx.r4.u32;
  uint32_t out_proc_ptr = ctx.r5.u32;

  auto* ks = rex::system::kernel_state();
  if (!ks) {
    ctx.r3.u64 = 0xC0000001;
    return;
  }

  uint32_t resolved_addr = 0;
  const char* module_name = "Unknown";

  if (module_handle == 0x3000b000u && ordinal == 1164) {
    module_name = "xam.xex (VoiceSetMicArrayIdleUsers)";
    resolved_addr = 0x8245F9E8;
  } else if (module_handle == 0x3000b000u && (ordinal == 1800 || ordinal == 0x0708)) {
    module_name = "xam.xex (XamGetLiveHiveValueA)";
    resolved_addr = 0x8245F9E0;
  }

  if (resolved_addr == 0 && module_handle == 0x3000b000u) {
    module_name = "xam.xex";
    auto xam = ks->GetKernelModule("xam.xex");
    if (xam) {
      resolved_addr = xam->GetProcAddressByOrdinal(static_cast<uint16_t>(ordinal), ctx.r31.u32);
    }
  }

  if (resolved_addr == 0 && module_handle == 0x30001000u) {
    module_name = "xboxkrnl.exe";
    auto krnl = ks->GetKernelModule("xboxkrnl.exe");
    if (krnl) {
      resolved_addr = krnl->GetProcAddressByOrdinal(static_cast<uint16_t>(ordinal), ctx.r31.u32);
    }
  }

  if (resolved_addr == 0) {
    if (ordinal >= 1 && ordinal <= 14) {
      module_name = "XMediaFacade";
      resolved_addr = GetXMediaExport(ordinal);
    } else if (module_handle >= 0x89000000u && module_handle < 0x8A000000u) {
      module_name = "SpeechFacade";
      resolved_addr = GetSpeechExport(ordinal);
    }
  }

  if (resolved_addr != 0) {
    if (out_proc_ptr != 0) {
      *reinterpret_cast<uint32_t*>(base + out_proc_ptr) = __builtin_bswap32(resolved_addr);
    }
    // lr identifies the caller. XMediaFacade ordinal 6 is the one the title
    // polls while the loading screen is up (hundreds of resolves per session,
    // and it resolves the address again on every poll), so log where that
    // poll comes from -- that call site is what decides the movie is still
    // playing.
    if (ordinal != 6 || (g_xmedia_call_count.fetch_add(1) % 180 == 0)) {
      REXKRNL_INFO(
          "[TELEMETRY] XexGetProcedureAddress: [Module: {} (h={:#x})] Ordinal {} "
          "-> ProcAddr {:#010x} caller_lr={:#010x}",
          module_name, module_handle, ordinal, resolved_addr,
          static_cast<uint32_t>(ctx.lr));
    }
    ctx.r3.u64 = 0;
    return;
  }

  if (out_proc_ptr != 0) {
    *reinterpret_cast<uint32_t*>(base + out_proc_ptr) = 0;
  }
  REXKRNL_WARN("[TELEMETRY-UNRESOLVED] XexGetProcedureAddress: [Module: {} (h={:#x})] Ordinal {} NOT FOUND!", 
               module_name, module_handle, ordinal);
  ctx.r3.u64 = 0xC0000135;
}

// TESTING 2026-09-01 (see docs, Fase 1 "revisão pedida pelo utilizador"):
// these two used to be unconditional no-ops with no comment and no measured
// justification, unlike every other override in this file. Decompiled the
// real bodies: sub_831D5F58 is a plain tail-branch into sub_831D75A0, and
// sub_831D75A0 is the title's own PowerPC context-save routine -- it saves
// r14-r31, cr0-cr7, lr, f14-f31/fpscr, and the full VMX128 vector file into a
// per-thread slot reached via r13 (thread pointer) -> +256 -> +356. Callers
// (Function_82D92C40, Function_831F5168) call it at slot/index-transition
// points, not from a fault handler -- this looks like the title's own
// checkpoint/fiber-context mechanism, not crash telemetry. The recompiled
// body itself is pure register-to-memory stores, no calls, nothing that
// looks unsafe to execute. Removing the no-op override to let the real
// translated body run and measuring the effect, rather than assuming either
// direction is safe.

// CGame::GetPartsHelper -- the async career-parts count.
//
// This is the crash that shows up "only when the save and cache are kept".
// The recompiled body (verified 1:1 against Forza_uncompressed.xex) runs
//
//   SELECT Count(GarageId) AS NumParts FROM %sCareer_Garage
//
// via two chained virtual calls: a factory writes a statement object to
// [sp+0x54], then the code immediately calls that object's vtable[0xB8]. On
// a second launch the player database exists but holds no schema -- it is a
// 6 MB "cmss" container with ~68 bytes of payload, because the first boot
// never got far enough to create the tables -- so the factory never writes
// its out-param, [sp+0x54] stays 0, and the vtable read dispatches through
// guest address 0. That is the FATAL at lr=0x8255D388.
//
// The function already has a safe answer for "no parts": at 0x8255D390 it
// cleans up and returns 0, leaving the two result flags clear. That is what
// this returns. Zero is also the truthful answer while the schema is
// missing, so this is the title's own under-limit path rather than an
// invented result. Offsets 424/425 are the flags the prologue clears at
// 0x8255D2C0 and the >50000 / >55000 branches would set.
extern "C" REX_FUNC(sub_8255D2C0) {
  uint32_t self = ctx.r3.u32;
  if (self != 0) {
    *(base + self + 424) = 0;
    *(base + self + 425) = 0;
  }
  ctx.r3.u64 = 0;
}

// Xbox Live Cloud Save / Telemetry Sync offline no-op
extern "C" REX_FUNC(sub_82D9F3E0) {
  (void)base;
  ctx.r3.u64 = 0; // Return success / offline
}

// Festival Leaderboard / Cloud Session offline handler
extern "C" REX_FUNC(sub_827DC190) {
  (void)base;
  ctx.r3.s64 = -1; // No active online session
}

// Multiplayer / Matchmaking Session Manager safe offline no-op
extern "C" REX_FUNC(sub_827E1558) {
  (void)base;
  ctx.r3.u64 = 0; // Success
}

// ---------------------------------------------------------------------------
// Guest heap replacement.
//
// DEFINE_REX_FUNC emits each recompiled body as a weak symbol, so the strong
// definitions below replace the game's own memory routines:
//
//   sub_83131408  free()        - 74 call sites in the recompiled output
//   sub_8313ABE8  alloc(zeroed) -  2 call sites
//   sub_8313E968  arena reset   -  2 call sites
//
// These are load-bearing, not cosmetic. Ghidra shows the real free() as:
//
//   if (p) { if (DAT_83409a68) { n = (*DAT_83409a8c)(); used -= n; }
//            (*DAT_83409a84)(p); }
//
// i.e. the game dispatches allocation through function pointers installed
// during startup. In this recompilation those pointers are still null when
// the first allocations run, so the untouched code path calls guest address
// 0 and dies. Removing these overrides was measured, not guessed: the
// session dropped from ~8000 log lines (title screen reached) to ~965 with
// 17 fresh null-address calls, so they stay until the real fix lands --
// initialising the game's own allocator pointers so its native free() works.
//
// sub_8313AD40 and sub_8313FAFC were dead code (never emitted, never called
// by the recompiled output) and are gone.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Guest heap replacement. DEFINE_REX_FUNC emits each recompiled body as a
// weak symbol, so the strong definitions below replace the game's own memory
// routines: sub_83131408 free() (74 call sites), sub_8313ABE8 alloc(zeroed)
// (2 sites), sub_8313E968 arena reset (2 sites).
//
// Kept because three configurations were measured on a clean boot, not
// reasoned about:
//
//   overrides as below   ~8000 log lines, reaches the loading screen
//   no overrides         ~1200 lines, writes to junk guest addresses
//   delegate when ready  ~1200 lines, same corruption
//
// The delegating variant gated on the game's allocator dispatch table at
// guest 0x83409A68 (+0x1C free, +0x24 size), which a live debugger confirms
// the title does populate during startup (free() = 0x82D965D0). Handing
// frees back to the native allocator once that table was live still
// corrupted the heap, so blocks reaching free() are evidently not the ones
// the native allocator handed out -- the allocation side is more tangled
// than these three entry points suggest.
//
// This is therefore a known-imperfect workaround, not a fix: with only 2 of
// the allocation sites redirected, natively allocated blocks reaching this
// free() fail InHeap() and are silently dropped rather than returned to the
// game's free list. That leak is the most likely remaining cause of the
// corrupted database state behind the infinite loading hang. Fixing it
// properly means finding every allocation entry point the title uses and
// routing the whole family consistently.
// ---------------------------------------------------------------------------

// A fourth variant was tried and reverted: calling the title's own lazy
// installer (sub_83139BF0(4, 0x8226C168), the call the game makes from
// 0x83131220 when the table is empty) from inside these hooks, then
// delegating to the native routines. It froze the process outright -- the
// render thread stopped logging entirely -- because running guest code from
// a hook reuses the caller's guest stack pointer and can re-enter locks the
// caller already holds. Reinstalling the allocator has to happen on the
// guest's own terms, not from inside a hook.

// For reference when picking this up again: the title dispatches allocation
// through a table at guest 0x83409A68, whose slots the disassembly of its own
// free() at 0x83131408 identifies as
//   +0x00 tracking flag   +0x1C free()   +0x24 block size()
// Statically those are zero; the title fills them during startup (read live
// with a debugger: free() = 0x82D965D0, size() = 0x82D965D8, which are
// branches to 0x82419080 and 0x82E47B80). The table being populated is not
// enough on its own -- see the note on sub_83131408 below.

// MemoryArena::Reset()
extern "C" REX_FUNC(sub_8313E968) {
  uint32_t arena = ctx.r3.u32;
  if (arena != 0) {
    *reinterpret_cast<uint32_t*>(base + arena + 12) = 0;
    *reinterpret_cast<uint32_t*>(base + arena + 16) = 0;
  }
}

// REMOVED 2026-09-01 (see docs, Fase 1 "recomendação revista"): this used to
// silently drop every free() the title's own allocator sent here that wasn't
// already in our CRT heap -- a leak affecting 74 call sites, the single
// biggest known source of heap corruption chased all day (0xBEBEBEBE poison
// pattern confirmed via GDB, multiple corrupted-vtable crashes all tracing
// back to it). FH1's own hooks file (pinyon-shift, which this project's
// original author used as a base) never overrides its title's native
// allocator at all -- only this project added that override, most likely to
// paper over an FH2-specific boot crash without first finding out why the
// real free() wasn't safe to call. Four delegation attempts earlier today
// all failed differently because InvalidFunctionTrap used to hard-abort on
// target==0; now that it survives that case instead, letting the real
// translated body run outright (rather than re-adding a delegating wrapper)
// is the more direct test of whether the title's own allocator can simply
// run unmodified. Being measured now across multiple boots, fresh and
// kept-save, not assumed safe.


// TESTING 2026-09-01 (see docs, Fase 1 "recomendação revista"): only 2 call
// sites in the whole title use this entry point, and decompiling one of them
// (0x8316D840) shows it belongs to a distributed allocator self-test/warm-up
// sequence (alloc 10 bytes, confirm, free it, then probe the alloc()
// trampoline at 0x82D965E0). Overriding this unconditionally means that
// self-test never runs against the title's real allocator, which is a
// plausible contributor to the "only partly constructed" state chased all
// day. Removed the override to let the real translated body run -- with
// InvalidFunctionTrap now surviving target==0, a failure here is far less
// likely to hard-crash than when this override was first added. Measuring,
// not assuming.

// XMediaFacade allocation thunks.
//
// Worth knowing what these actually are: the recompiled sub_88050340 is an
// import thunk, not an allocator body --
//
//   r11 = [0x882844E0 + 236];  mtctr r11;  bctr
//
// so overriding it substitutes the routine XMediaFacade imports, across 190
// call sites (sub_8805C400 adds 4 more). The pairing is also lopsided: the
// matching free override, sub_8805C468, has zero call sites in the module,
// so frees leave through XMediaFacade's own untouched path.
//
// That asymmetry looks wrong on paper, but removing all three was measured
// and is worse: a boot that reached the loading screen with ~5000 log lines
// froze at ~1120 with the render thread stopped. Kept as-is until the
// import target is identified and the whole family can move together.
extern "C" REX_FUNC(sub_88050340) {
  uint32_t size = ctx.r3.u32;
  if (size == 0) {
    ctx.r3.u64 = 0;
    return;
  }
  uint32_t addr = rex::kernel::crt::GetHeap().Alloc(size, true);
  recomp_diag::HeapTracker::Instance().OnAlloc(addr, size, static_cast<uint32_t>(ctx.lr),
                                                "xmediafacade");
  ctx.r3.u64 = addr;
}

extern "C" REX_FUNC(sub_8805C400) {
  uint32_t size = ctx.r3.u32;
  if (size == 0) {
    ctx.r3.u64 = 0;
    return;
  }
  uint32_t addr = rex::kernel::crt::GetHeap().Alloc(size, true);
  recomp_diag::HeapTracker::Instance().OnAlloc(addr, size, static_cast<uint32_t>(ctx.lr),
                                                "xmediafacade");
  ctx.r3.u64 = addr;
}

extern "C" REX_FUNC(sub_8805C468) {
  uint32_t ptr = ctx.r3.u32;
  if (ptr != 0 && rex::kernel::crt::GetHeap().InHeap(ptr)) {
    recomp_diag::HeapTracker::Instance().OnFree(ptr);
    rex::kernel::crt::GetHeap().Free(ptr);
  }
}

// ---------------------------------------------------------------------------
// Listener-set notify guard for sub_82D97D60 -- TRIED AND REVERTED.
//
// sub_82D97D60 walks an inline std::set<Listener*> at param_1+0x2C (MSVC
// red-black tree) and calls listener->vtable[0](listener, param_1) per
// entry. It is the crash behind repeated "crashed on START" reports this
// session (lr=0x82D97D9C, a listener pointer whose "vtable" is garbage,
// landing on FunctionDispatcher's InvalidFunctionTrap).
//
// A native override that reimplemented the same loop -- validating each
// listener's vtable/method via GetFunction() before dispatching, and
// reserving its own 0x70-byte guest stack frame before delegating to the
// real tree-successor helper (sub_82FC77C8) -- was built and tested. It
// made things worse, not better: within seconds of boot, *before* reaching
// the point where START can even be pressed, the log filled with
// "Unhandled guest access violation: write of guest 0x6CxxFFxx" at a
// steady ~0x10000 stride -- a guest stack pointer descending without bound,
// each newly-touched 64K page recovered by CrashFilter only to fault again
// one page lower. That is a runaway recursion/loop signature, not the
// single clean abort the unpatched function produces. Something about
// re-entering this notification path (either genuine unbounded recursion in
// the title's own listener graph, surfaced only once bad entries stop being
// fatal, or a subtlety in how nested guest calls were driven from here) is
// unsafe. Reverted whole. The dangling-listener crash this was meant to
// guard against is real and still open; the fix needs to happen without
// hand-driving nested guest calls from a native override the way this did.


// ---------------------------------------------------------------------------
// Read-only diagnostic for the sub_83134738 lead documented above sub_83131408:
// this bucket/array object reads a pointer at +0x38 and calls through its
// vtable+0x1C when growing past its current capacity (the +0xE byte). The
// object never gets there in the current (leaking) configuration unless
// something else already routes through it, so this hook only logs -- it
// changes nothing about behavior, always tail-calling into the real
// translated body. Purpose: find out (a) whether this function is ever
// reached at all under normal play, and (b) what field_0x38 looks like on
// entry, without risking the regressions the two prior direct-delegation
// attempts caused.
extern "C" void __imp__sub_83134710(PPCContext& ctx, uint8_t* base);

// 2026-09-01: added an unconditional (not capped at 30) log for field_0x38
// == 0 specifically, plus a DebugBreak()-based live-capture experiment to
// watchpoint a bad instance under GDB. Reverted the DebugBreak() -- it would
// hard-crash any normal (non-debugged) run the moment it fired, which is
// worse than the leak workaround it was meant to help fix. The unconditional
// bad-instance log stays: it is harmless (just a log line) and means the
// next time this class does hit a null field_0x38 under any configuration,
// it will be on record with its exact guest address instead of needing to
// re-instrument to find it again.
extern "C" REX_FUNC(sub_83134710) {
  static std::atomic<uint32_t> call_count{0};
  uint32_t self = ctx.r3.u32;
  uint32_t index = ctx.r4.u32;
  uint32_t n = call_count.fetch_add(1);
  if (self != 0) {
    uint32_t field_0x38 = __builtin_bswap32(
        *reinterpret_cast<uint32_t*>(base + self + 0x38));
    if (n < 30) {
      uint8_t capacity_0xE = *reinterpret_cast<uint8_t*>(base + self + 0xE);
      REXKRNL_WARN(
          "[Diag_83134710] call#{} this={:08X} index={:08X} field_0x38={:08X} "
          "capacity_0xE={:02X} lr={:08X}",
          n, self, index, field_0x38, capacity_0xE,
          static_cast<uint32_t>(ctx.lr));
    }
    if (field_0x38 == 0) {
      REXKRNL_WARN(
          "[Diag_83134710] BAD instance: this={:08X} index={:08X} lr={:08X}",
          self, index, static_cast<uint32_t>(ctx.lr));
    }
  }
  __imp__sub_83134710(ctx, base);
}

// Fixed Signed-In profile (FabioAP, XUID: 0xB13EBABEBABEBABE)
extern "C" void __imp__XamUserGetSigninState(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t user_index = ctx.r3.u32;
  if (user_index == 0) {
    ctx.r3.u64 = 1; // eXUserSigninState_SignedInLocally
  } else {
    ctx.r3.u64 = 0; // eXUserSigninState_NotSignedIn
  }
}

extern "C" void __imp__XamUserGetName(PPCContext& ctx, uint8_t* base) {
  uint32_t user_index = ctx.r3.u32;
  uint32_t buffer_ptr = ctx.r4.u32;
  uint32_t buffer_len = ctx.r5.u32;
  if (user_index == 0 && buffer_ptr != 0 && buffer_len >= 8) {
    std::memcpy(base + buffer_ptr, "FabioAP\0", 8);
    ctx.r3.u64 = 0; // ERROR_SUCCESS
  } else {
    ctx.r3.u64 = 0x57;
  }
}

extern "C" void __imp__XamUserGetSigninInfo(PPCContext& ctx, uint8_t* base) {
  uint32_t user_index = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  uint32_t info_ptr = ctx.r5.u32;
  (void)flags;
  if (user_index == 0 && info_ptr != 0) {
    *reinterpret_cast<uint64_t*>(base + info_ptr) = __builtin_bswap64(0xB13EBABEBABEBABEu);
    *reinterpret_cast<uint32_t*>(base + info_ptr + 8) = __builtin_bswap32(1);
    *reinterpret_cast<uint32_t*>(base + info_ptr + 12) = __builtin_bswap32(1);
    *reinterpret_cast<uint32_t*>(base + info_ptr + 16) = 0;
    *reinterpret_cast<uint32_t*>(base + info_ptr + 20) = 0;
    std::memcpy(base + info_ptr + 24, "FabioAP\0", 8);
    ctx.r3.u64 = 0; // ERROR_SUCCESS
  } else {
    ctx.r3.u64 = 0x57;
  }
}

extern "C" void __imp__XamShowSigninUI(PPCContext& ctx, uint8_t* base) {
  (void)base;
  auto* ks = rex::system::kernel_state();
  if (ks) {
    ks->BroadcastNotification(0x0000000A, 1); // XN_SYS_SIGNINCHANGED (Xenia exact ID)
    ks->BroadcastNotification(0x00000002, 1); // XN_SYS_STORAGEDEVICESCHANGED
    ks->BroadcastNotification(0x00000009, 0); // XN_SYS_UI (0 = UI Closed)
  }
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

extern "C" void __imp__XamShowDeviceSelectorUI(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t out_device_id_ptr = ctx.r6.u32;
  uint32_t overlapped_ptr = ctx.r7.u32;
  if (out_device_id_ptr != 0) {
    *reinterpret_cast<uint32_t*>(base + out_device_id_ptr) = __builtin_bswap32(0x00000001); // Device ID 1 (Hard Drive)
  }
  auto* ks = rex::system::kernel_state();
  if (ks && overlapped_ptr) {
    ks->CompleteOverlappedImmediate(overlapped_ptr, 0);
  }
  if (ks) {
    ks->BroadcastNotification(0x00000002, 1); // XN_SYS_STORAGEDEVICESCHANGED
    ks->BroadcastNotification(0x00000009, 0); // XN_SYS_UI (0 = UI Closed)
  }
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

// X_CONTENT_DEVICE_DATA in Big-Endian UTF-16 format (Hard Drive with 3 GB Free)
static const uint8_t kDummyHddDeviceData[80] = {
  0x00, 0x00, 0x00, 0x01, // device_id = 1
  0x00, 0x00, 0x00, 0x01, // device_type = 1 (HDD)
  0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, // total_bytes = 20 GB
  0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, // free_bytes = 3 GB
  0x00, 0x48, 0x00, 0x61, 0x00, 0x72, 0x00, 0x64, // u"Hard "
  0x00, 0x20, 0x00, 0x44, 0x00, 0x72, 0x00, 0x69, // u" Dri"
  0x00, 0x76, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00, // u"ve\0\0"
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

extern "C" void __imp__XamContentGetDeviceData(PPCContext& ctx, uint8_t* base) {
  uint32_t device_id = ctx.r3.u32;
  uint32_t data_ptr = ctx.r4.u32;
  (void)device_id;
  if (data_ptr != 0) {
    std::memcpy(base + data_ptr, kDummyHddDeviceData, sizeof(kDummyHddDeviceData));
    ctx.r3.u64 = 0; // ERROR_SUCCESS
  } else {
    ctx.r3.u64 = 0x57;
  }
}

extern "C" void __imp__XamContentGetDeviceState(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 0; // Device ready / mounted
}

extern "C" void __imp__XamContentGetDeviceName(PPCContext& ctx, uint8_t* base) {
  uint32_t name_ptr = ctx.r4.u32;
  if (name_ptr != 0) {
    static const uint8_t kNameBe[22] = {
      0x00, 0x48, 0x00, 0x61, 0x00, 0x72, 0x00, 0x64,
      0x00, 0x20, 0x00, 0x44, 0x00, 0x72, 0x00, 0x69,
      0x00, 0x76, 0x00, 0x65, 0x00, 0x00
    };
    std::memcpy(base + name_ptr, kNameBe, sizeof(kNameBe));
    ctx.r3.u64 = 0;
  }
}

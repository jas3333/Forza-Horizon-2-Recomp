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
#include <rex/system/xthread.h>
#include <rex/kernel/crt/heap.h>
#include <rex/system/xmemory.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <cmath>
#include <vector>

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
// Signatures (as in Xenia's xam_msg.cc): XMsgStartIORequestEx(app r3,
// message r4, overlapped r5, buffer r6, buffer_length r7, task_args r8) and
// XMsgStartIORequest(app r3, message r4, overlapped r5, buffer r6,
// buffer_length r7). These used to read the overlapped pointer from r8 / r7
// (buffer_length), so the completion was written to whatever address those
// held -- e.g. the "write of guest 0x00000054" access violation on every boot.
extern "C" void __imp__XMsgStartIORequestEx(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t overlapped = ctx.r5.u32;
  auto* ks = rex::system::kernel_state();
  if (ks && overlapped != 0) {
    ks->CompleteOverlappedImmediate(overlapped, 0);
  }
  ctx.r3.u64 = 0; // ERROR_SUCCESS
}

extern "C" void __imp__XMsgStartIORequest(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t overlapped = ctx.r5.u32;
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

  // Ask the loaded module for its real export first. The hardcoded tables
  // below were guesses: the facade's ordinal 3 was mapped to 0x880503B8,
  // which made the title call the wrong export (sub_880561C8 with a stack
  // pointer and r4=5) and crash on the first movie.
  if (resolved_addr == 0 && module_handle != 0) {
    auto module = rex::system::XModule::GetFromHModule(ks, base + module_handle);
    if (module) {
      resolved_addr = module->GetProcAddressByOrdinal(static_cast<uint16_t>(ordinal),
                                                      static_cast<uint32_t>(ctx.lr));
      if (resolved_addr) {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1) < 64) {
          uint32_t guessed = ordinal <= 14 ? GetXMediaExport(ordinal) : 0;
          REXKRNL_INFO("[EXPORT] {} ordinal {} -> {:#010x} (old table {:#010x}) caller_lr={:#010x}",
                       module->name(), ordinal, resolved_addr, guessed,
                       static_cast<uint32_t>(ctx.lr));
        }
      }
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

// sub_82D9F3E0 is NOT a cloud-save/telemetry stub (as it was once labelled
// and no-op'd here): it is the player-database loader -- ATTACH
// 'memorydb:\N' AS filedb, list filedb's tables, then CREATE TABLE
// [Profile0_X] + INSERT ... SELECT * FROM filedb.[X] for each. With it
// stubbed every saved PlayerDatabase was ignored and the first Profile0_
// query crashed on a null statement (found by tracing Xenia, 2026-10-11).

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
// REMOVED 2026-10-10: the overrides of sub_88050340 / sub_8805C400 /
// sub_8805C468 that sent XMediaFacade allocations to the host CRT heap. They
// dropped the XMemAlloc attribute word (r4, e.g. 0x208C800A from
// sub_8805C788: alignment and memory type) and paired with frees that still
// went through the module's own path. With them in place the first movie
// (sub_8805C788) crashed calling a vtable slot holding string bytes. The
// "worse without them" measurement above predates the XAM argument-register
// fix (c388cd36), which removed the heap corruption that was confounding it.

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

// Signed-in profile 0: gamertag and XUID come from the SDK's user_name /
// user_xuid settings (UserProfile), default "User" / 0xB13EBABEBABEBABE.
// Saves are keyed by the XUID, so changing the name keeps them.
static void CopyGamertag(uint8_t* dst, uint32_t capacity) {
  std::string name = rex::system::kernel_state()->user_profile()->name();
  size_t n = std::min<size_t>(name.size(), capacity ? capacity - 1 : 0);
  std::memcpy(dst, name.data(), n);
  std::memset(dst + n, 0, capacity - n);
}

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
  if (user_index == 0 && buffer_ptr != 0 && buffer_len >= 1) {
    CopyGamertag(base + buffer_ptr, std::min<uint32_t>(buffer_len, 16));
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
    *reinterpret_cast<uint64_t*>(base + info_ptr) =
        __builtin_bswap64(rex::system::kernel_state()->user_profile()->xuid());
    *reinterpret_cast<uint32_t*>(base + info_ptr + 8) = __builtin_bswap32(1);
    *reinterpret_cast<uint32_t*>(base + info_ptr + 12) = __builtin_bswap32(1);
    *reinterpret_cast<uint32_t*>(base + info_ptr + 16) = 0;
    *reinterpret_cast<uint32_t*>(base + info_ptr + 20) = 0;
    CopyGamertag(base + info_ptr + 24, 16);  // XUSER_SIGNIN_INFO::szUserName[16]
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

// XamShowDeviceSelectorUI(user_index r3, content_type r4, content_flags r5,
// total_requested r6 (u64), device_id_ptr r7, overlapped r8). This used to
// take r6/r7: it wrote the device id over whatever address the requested
// size happened to be -- during saves that landed in the title's small-block
// allocator and zeroed a free-list link, which later handed out live blocks
// twice (SQLite then freed mismatched pointers, the main heap's free lists
// were corrupted and the allocator spun forever: the frozen title screen)
// -- and "completed" the real device id variable as if it were the
// overlapped structure.
extern "C" void __imp__XamShowDeviceSelectorUI(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t out_device_id_ptr = ctx.r7.u32;
  uint32_t overlapped_ptr = ctx.r8.u32;
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

// XamContentGetDeviceState(device_id r3, overlapped r4).
extern "C" void __imp__XamContentGetDeviceState(PPCContext& ctx, uint8_t* base) {
  (void)base;
  uint32_t overlapped_ptr = ctx.r4.u32;
  auto* ks = rex::system::kernel_state();
  if (ks && overlapped_ptr) {
    ks->CompleteOverlappedImmediate(overlapped_ptr, 0);
    ctx.r3.u64 = 0x3E5;  // ERROR_IO_PENDING, as for other overlapped XAM calls
    return;
  }
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

// Read-only diagnostic for the null call at lr=0x825272D8. sub_82527260
// formats a query from the format string at 0x82012550 and the string at
// [r3+36] (inline when its length field < 16, else a pointer -- MSVC SSO),
// asks a factory (vtable+32) for a statement object, then calls its
// vtable[184]. When the factory leaves the out-param at 0 that second call
// lands on guest address 0. Log what is being queried, then run the real
// body unchanged.
extern "C" void __imp__sub_82527260(PPCContext& ctx, uint8_t* base);

extern "C" REX_FUNC(sub_82527260) {
  static std::atomic<uint32_t> call_count{0};
  if (call_count.fetch_add(1) < 50) {
    auto guest_cstr = [base](uint32_t addr) -> std::string {
      if (addr == 0 || addr >= 0xC0000000u) {
        return "<null>";
      }
      const char* p = reinterpret_cast<const char*>(base + addr);
      return std::string(p, strnlen(p, 200));
    };
    uint32_t self = ctx.r3.u32;
    uint32_t len = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + self + 36 + 20));
    uint32_t arg = self + 36;
    if (len >= 16) {
      arg = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + self + 36));
    }
    REXKRNL_WARN("[Diag_82527260] fmt='{}' arg='{}' this={:08X} lr={:08X}",
                 guest_cstr(0x82012550), guest_cstr(arg), self,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82527260(ctx, base);
}

// FH2_SQLTRACE=1: log every SQL statement the title runs through its
// "execute and collect rows" helper (sub_82D9F808, statement text in r5),
// in the same "[SQL] r7=... <text>" format as the trace_guest_functions hook
// in the local Xenia comparison fork, so the two streams can be diffed.
extern "C" void __imp__sub_82D9F808(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(sub_82D9F808) {
  static const bool enabled = [] {
    const char* v = std::getenv("FH2_SQLTRACE");
    return v && *v && *v != '0';
  }();
  if (enabled && ctx.r5.u32 >= 0x10000u && ctx.r5.u32 < 0xC0000000u) {
    const char* c = reinterpret_cast<const char*>(base + ctx.r5.u32);
    std::string sql;
    for (size_t i = 0; i < 90 && c[i]; ++i) sql += (c[i] >= 32 && c[i] < 127) ? c[i] : '.';
    REXKRNL_INFO("[SQL] r7={:08X} {}", ctx.r7.u32, sql);
    __imp__sub_82D9F808(ctx, base);
    REXKRNL_INFO("[SQL]   -> r3={:08X}", ctx.r3.u32);
    return;
  }
  __imp__sub_82D9F808(ctx, base);
}

// FH2_CALLTRACE_DUMP_SEC=<n> (with --calltrace_kernel_calls): after n seconds,
// log the main guest thread's ring buffer of recent kernel/XAM export calls --
// the same dump the F8 bind produces, without needing a key press.
#include <rex/system/checkpoint.h>
namespace {
struct CallTraceAutoDump {
  CallTraceAutoDump() {
    const char* v = std::getenv("FH2_CALLTRACE_DUMP_SEC");
    if (!v || !*v) return;
    int seconds = std::atoi(v);
    std::thread([seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      const char* tid_env = std::getenv("FH2_CALLTRACE_TID");
      auto trace = tid_env ? rex::system::DumpThreadCallTraceById(uint32_t(std::atoi(tid_env)))
                           : rex::system::DumpMainThreadCallTrace();
      REXKRNL_WARN("[CALLTRACE-AUTO] {} call(s), oldest first:", trace.size());
      std::string line;
      for (size_t i = 0; i < trace.size(); ++i) {
        line += trace[i];
        line += ' ';
        if ((i + 1) % 12 == 0 || i + 1 == trace.size()) {
          REXKRNL_WARN("[CALLTRACE-AUTO] {}", line);
          line.clear();
        }
      }
    }).detach();
  }
} g_call_trace_auto_dump;
}  // namespace

// FH2_RDOC_CAPTURE_SEC=<n>[,<m>...]: when running under RenderDoc
// (renderdoccmd capture ...), trigger a frame capture after each listed
// number of seconds.
#include <rex/ui/renderdoc_api.h>
namespace {
struct RenderDocAutoCapture {
  RenderDocAutoCapture() {
    const char* v = std::getenv("FH2_RDOC_CAPTURE_SEC");
    if (!v || !*v) return;
    std::string spec = v;
    std::thread([spec] {
      auto rdoc = rex::ui::RenderDocAPI::CreateIfConnected();
      if (!rdoc || !rdoc->api_1_0_0()) {
        REXKRNL_WARN("[RDOC] RenderDoc not attached; no capture");
        return;
      }
      int elapsed = 0;
      size_t pos = 0;
      while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        int at = std::atoi(spec.substr(pos, comma - pos).c_str());
        if (at > elapsed) std::this_thread::sleep_for(std::chrono::seconds(at - elapsed));
        elapsed = std::max(elapsed, at);
        rdoc->api_1_0_0()->TriggerCapture();
        REXKRNL_WARN("[RDOC] capture triggered at {}s", at);
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      std::this_thread::sleep_for(std::chrono::hours(24));  // keep the API loaded
    }).detach();
  }
} g_renderdoc_auto_capture;
}  // namespace

// Profiling: the D3D ring-buffer space wait (spins on the CP read pointer).
// Shows up as "guest:ring_space_wait" in Tracy captures.
#include <rex/dbg.h>
extern "C" void __imp__sub_82B72A90(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(sub_82B72A90) {
#ifdef REXGLUE_ENABLE_PROFILING
  // The game calls this in a tight loop (~500k/s), far too often for a zone
  // per call. Accumulate the time and plot the share of each 100 ms spent
  // here ("guest_ring_wait_pct" in Tracy).
  static std::atomic<int64_t> spent_ns{0};
  static std::atomic<int64_t> window_start_ns{0};
  auto now = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  // Only time the wait while a profiler is attached: clock reads around every
  // spin iteration cost a quarter of this thread otherwise.
  if (!TracyIsConnected) {
    __imp__sub_82B72A90(ctx, base);
    return;
  }
  int64_t t0 = now();
  __imp__sub_82B72A90(ctx, base);
  int64_t t1 = now();
  spent_ns += t1 - t0;
  int64_t ws = window_start_ns.load();
  if (ws == 0) {
    window_start_ns.compare_exchange_strong(ws, t1);
  } else if (t1 - ws >= 100000000 && window_start_ns.compare_exchange_strong(ws, t1)) {
    TracyPlot("guest_ring_wait_pct", double(spent_ns.exchange(0)) * 100.0 / double(t1 - ws));
  }
#else
  __imp__sub_82B72A90(ctx, base);
#endif
}

// FH2_SCAN_SHADERS=<seconds>:<out_dir> -- after the given time, sweep the
// whole 4 GB guest address space for Xbox 360 shader containers (magic
// 0x102A1100 vertex / 0x102A1101 pixel) and save each one (header + virtual
// + physical part) as <out_dir>/<guest address>.bin. Reads go through
// /proc/self/mem, so unmapped or protected ranges just fail instead of
// faulting. Feeds tools/extract_shaders.py: most of FH2's engine shaders
// arrive from LZX-compressed archives and only exist decompressed in memory.
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
namespace {
struct ShaderMemoryScanner {
  ShaderMemoryScanner() {
    const char* spec = std::getenv("FH2_SCAN_SHADERS");
    if (!spec || !*spec) return;
    std::string s = spec;
    size_t colon = s.find(':');
    if (colon == std::string::npos) return;
    int seconds = std::atoi(s.substr(0, colon).c_str());
    std::string out_dir = s.substr(colon + 1);
    std::thread([seconds, out_dir] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      auto* mem = rex::system::kernel_state() ? rex::system::kernel_state()->memory() : nullptr;
      if (!mem) return;
      uint8_t* base = mem->virtual_membase();
      std::filesystem::create_directories(out_dir);
      int fd = open("/proc/self/mem", O_RDONLY);
      if (fd < 0) return;
      constexpr size_t kChunk = 1 << 20;
      std::vector<uint8_t> buf(kChunk + 64);
      uint32_t saved = 0, readable_mb = 0;
      for (uint64_t guest = 0x00010000; guest < 0x100000000ull; guest += kChunk) {
        ssize_t got = pread(fd, buf.data(), kChunk + 36, off_t(reinterpret_cast<uintptr_t>(base + guest)));
        if (got < 64) continue;
        ++readable_mb;
        for (ssize_t i = 0; i + 36 <= std::min<ssize_t>(got, kChunk); i += 4) {
          if (buf[i] != 0x10 || buf[i + 1] != 0x2A || buf[i + 2] != 0x11 || buf[i + 3] > 1) continue;
          auto be32 = [&](ssize_t o) {
            return (uint32_t(buf[o]) << 24) | (uint32_t(buf[o + 1]) << 16) |
                   (uint32_t(buf[o + 2]) << 8) | buf[o + 3];
          };
          uint32_t virt = be32(i + 4), phys = be32(i + 8);
          if (!virt || !phys || virt > (1u << 20) || phys > (1u << 20)) continue;
          std::vector<uint8_t> container(virt + phys);
          if (pread(fd, container.data(), container.size(),
                    off_t(reinterpret_cast<uintptr_t>(base + guest + i))) != ssize_t(container.size())) {
            continue;
          }
          FILE* f = std::fopen(fmt::format("{}/{:08X}.bin", out_dir, uint32_t(guest + i)).c_str(), "wb");
          if (f) {
            std::fwrite(container.data(), 1, container.size(), f);
            std::fclose(f);
            ++saved;
          }
        }
      }
      close(fd);
      REXKRNL_WARN("[SHADERSCAN] {} MB readable, {} shader containers saved to {}", readable_mb, saved,
                   out_dir);
    }).detach();
  }
} g_shader_memory_scanner;
}  // namespace

// ---------------------------------------------------------------------------
// Debug: find and move the game clock (time of day).
//
//   8  scan: snapshot every committed guest float in (0, 24], then every
//        10 s for 60 s keep those that rise steadily at a constant rate
//        (the time of day does; little else does). Logs [TODSCAN] candidates.
//   0  select the next candidate (logged).
//   9  move the selected value forward one hour (1.0 if it looks like
//        hours, 1/24 if it looks like a 0..1 fraction of the day).
//
// FH2_TOD_ADDR=<hex> preselects an address (e.g. one found earlier), so 9
// works immediately without a scan.
namespace {
struct TodCandidate {
  uint32_t addr;
  float first, last, delta0;
};
// The master clock found by the targeted search (key 7): a float or double
// holding the time of day in some unit; per_day = value of a full day.
struct TodMaster {
  uint32_t addr;
  bool is_double;
  double per_day;
  bool is_int = false;
};
double TodMasterRead(rex::memory::Memory* mem, const TodMaster& m);
std::vector<TodMaster> g_tod_masters;
size_t g_tod_master_index = 0;
bool g_tod_use_master = false;
std::mutex g_tod_mutex;
std::vector<TodCandidate> g_tod_candidates;
std::atomic<bool> g_tod_scanning{false};
uint32_t g_tod_selected = 0;
size_t g_tod_index = 0;

float TodRead(rex::memory::Memory* mem, uint32_t addr) {
  uint32_t v = __builtin_bswap32(*mem->TranslateVirtual<uint32_t*>(addr));
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
double TodReadD(rex::memory::Memory* mem, uint32_t addr) {
  uint64_t v = __builtin_bswap64(*mem->TranslateVirtual<uint64_t*>(addr));
  double d;
  std::memcpy(&d, &v, 8);
  return d;
}
void TodWriteD(rex::memory::Memory* mem, uint32_t addr, double d) {
  uint64_t v;
  std::memcpy(&v, &d, 8);
  *mem->TranslateVirtual<uint64_t*>(addr) = __builtin_bswap64(v);
}
void TodWrite(rex::memory::Memory* mem, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  *mem->TranslateVirtual<uint32_t*>(addr) = __builtin_bswap32(v);
}

template <typename Fn>
void TodForEachCommittedRegion(rex::memory::Memory* mem, Fn&& fn) {
  // Virtual guest space only (0xA0000000+ mirrors physical memory).
  for (uint64_t a = 0x00010000; a < 0xA0000000ull;) {
    auto* heap = mem->LookupHeap(uint32_t(a));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(a), &info) || !info.region_size) {
      a += 0x10000;
      continue;
    }
    if ((info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & rex::memory::kMemoryProtectRead)) {
      fn(info.base_address, info.region_size);
    }
    a = std::max<uint64_t>(a + 0x1000, uint64_t(info.base_address) + info.region_size);
  }
}

void TodScan() {
  auto* mem = rex::system::kernel_state()->memory();
  std::vector<TodCandidate> cands;
  TodForEachCommittedRegion(mem, [&](uint32_t base, uint32_t size) {
    for (uint32_t off = 0; off + 4 <= size; off += 4) {
      float f = TodRead(mem, base + off);
      if (std::isfinite(f) && f > 0.0001f && f <= 24.0f) {
        cands.push_back({base + off, f, f, 0.0f});
      }
    }
  });
  REXLOG_INFO("[TODSCAN] snapshot 0: {} values in (0, 24]", cands.size());
  for (int pass = 1; pass <= 6; ++pass) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::vector<TodCandidate> kept;
    for (TodCandidate c : cands) {
      // Region may have been freed since: re-check it is still readable.
      auto* heap = mem->LookupHeap(c.addr);
      rex::memory::HeapAllocationInfo info{};
      if (!heap || !heap->QueryRegionInfo(c.addr, &info) ||
          !(info.state & rex::memory::kMemoryAllocationCommit) ||
          !(info.protect & rex::memory::kMemoryProtectRead)) {
        continue;
      }
      float v = TodRead(mem, c.addr);
      float d = v - c.last;
      if (!std::isfinite(v) || d <= 0.0f || d > 1.0f) continue;
      if (pass == 1) {
        c.delta0 = d;
      } else if (d < c.delta0 * 0.7f || d > c.delta0 * 1.4f) {
        continue;
      }
      c.last = v;
      kept.push_back(c);
    }
    cands.swap(kept);
    REXLOG_INFO("[TODSCAN] pass {}: {} candidates rising steadily", pass, cands.size());
  }
  for (size_t i = 0; i < cands.size() && i < 60; ++i) {
    const TodCandidate& c = cands[i];
    REXLOG_INFO("[TODSCAN] #{} {:08X}: {:.4f} -> {:.4f} ({:.5f} per minute)", i, c.addr, c.first,
                c.last, (c.last - c.first) / 1.0f);
  }
  std::lock_guard<std::mutex> lock(g_tod_mutex);
  g_tod_candidates = std::move(cands);
  g_tod_index = 0;
  if (!g_tod_candidates.empty()) {
    // Preselect the fastest riser: the time of day (as a fraction of a day)
    // moved ~0.03 per minute, far more than the other survivors.
    // Only values that look like a fraction of a day (<= 1) qualify.
    size_t best = 0;
    float best_rise = -1.0f;
    for (size_t i = 0; i < g_tod_candidates.size(); ++i) {
      const TodCandidate& c = g_tod_candidates[i];
      if (c.last <= 1.0f && c.last - c.first > best_rise) {
        best = i;
        best_rise = c.last - c.first;
      }
    }
    g_tod_index = best;
    g_tod_selected = g_tod_candidates[best].addr;
    REXLOG_INFO("[TODSCAN] selected #{} {:08X} (7 = find its master clock, 0 next, 9 +1 hour)",
                best, g_tod_selected);
  }
  g_tod_scanning = false;
}
// Key 7: the selected candidate is a per-frame copy of the clock (fraction of
// a day). Search for the same time in other units -- hours, minutes,
// seconds, as float or double -- and keep values whose ratio to the copy
// still holds two seconds later.
double TodMasterRead(rex::memory::Memory* mem, const TodMaster& m) {
  if (m.is_int) return double(int32_t(__builtin_bswap32(*mem->TranslateVirtual<uint32_t*>(m.addr))));
  return m.is_double ? TodReadD(mem, m.addr) : TodRead(mem, m.addr);
}

void TodFindMaster() {
  auto* mem = rex::system::kernel_state()->memory();
  // Every scan candidate that looks like the time as a fraction of a day
  // (value <= 1, rising >= 0.01 per minute) is a possible per-frame copy.
  std::vector<uint32_t> refs;
  {
    std::lock_guard<std::mutex> lock(g_tod_mutex);
    for (const TodCandidate& c : g_tod_candidates) {
      if (c.last <= 1.0f && c.last - c.first >= 0.01f) refs.push_back(c.addr);
    }
  }
  if (refs.empty()) {
    REXLOG_INFO("[TODSCAN] 7: no clock-like candidates (press 8 first)");
    g_tod_scanning = false;
    return;
  }
  static const double kScales[] = {1.0, 24.0, 1440.0, 86400.0, 86400000.0};
  auto matches = [](double v, double ref, double scale) {
    double want = ref * scale;
    return std::isfinite(v) && want > 0 && std::fabs(v - want) <= want * 0.004 + 1e-6;
  };
  struct Pending {
    TodMaster m;
    uint32_t ref_addr;
    double v0;
  };
  std::vector<Pending> found;
  std::vector<double> ref_values;
  for (uint32_t r : refs) ref_values.push_back(TodRead(mem, r));
  TodForEachCommittedRegion(mem, [&](uint32_t base, uint32_t size) {
    for (uint32_t off = 0; off + 4 <= size; off += 4) {
      uint32_t a = base + off;
      float f = TodRead(mem, a);
      bool have_d = (off & 7) == 0 && off + 8 <= size;
      double d = have_d ? TodReadD(mem, a) : 0.0;
      int32_t iv = int32_t(__builtin_bswap32(*mem->TranslateVirtual<uint32_t*>(a)));
      for (size_t i = 0; i < refs.size(); ++i) {
        if (a == refs[i]) continue;
        for (double k : kScales) {
          if (k != 1.0 && matches(f, ref_values[i], k))
            found.push_back({{a, false, k}, refs[i], double(f)});
          if (have_d && matches(d, ref_values[i], k)) found.push_back({{a, true, k}, refs[i], d});
          if (k >= 1440.0 && matches(double(iv), ref_values[i], k))
            found.push_back({{a, false, k, true}, refs[i], double(iv)});
        }
      }
    }
  });
  REXLOG_INFO("[TODSCAN] master search over {} clock-like values: {} matches; rechecking in 2 s",
              refs.size(), found.size());
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::vector<TodMaster> kept;
  for (const Pending& p : found) {
    double ref2 = TodRead(mem, p.ref_addr);
    double v = TodMasterRead(mem, p.m);
    bool dup = false;
    for (const TodMaster& k : kept) dup |= k.addr == p.m.addr;
    // It must have moved along with the clock copy (within 25%), not just
    // still be close to it -- stored timestamps match once by coincidence.
    double ref_index = 0;
    for (size_t i = 0; i < refs.size(); ++i) if (refs[i] == p.ref_addr) ref_index = double(i);
    double expected = (ref2 - ref_values[size_t(ref_index)]) * p.m.per_day;
    double moved = v - p.v0;
    if (!dup && expected > 0 && moved > expected * 0.75 && moved < expected * 1.25) {
      kept.push_back(p.m);
    }
  }
  for (size_t i = 0; i < kept.size() && i < 40; ++i) {
    const TodMaster& m = kept[i];
    double v = TodMasterRead(mem, m);
    REXLOG_INFO("[TODSCAN] master #{} {:08X} {} {:.4f} ({} per day)", i, m.addr,
                m.is_int ? "int32" : m.is_double ? "double" : "float", v, m.per_day);
  }
  std::lock_guard<std::mutex> lock(g_tod_mutex);
  g_tod_masters = std::move(kept);
  g_tod_master_index = 0;
  g_tod_use_master = !g_tod_masters.empty();
  if (g_tod_use_master) {
    REXLOG_INFO("[TODSCAN] using master #0 {:08X} (9 = +1 hour, 0 = next master)",
                g_tod_masters[0].addr);
  } else {
    REXLOG_INFO("[TODSCAN] no master clock found");
  }
  g_tod_scanning = false;
}
}  // namespace

void InstallFh2ClockWatchKey();
void InstallFh2CommandChannel();
void InstallFh2DebugTools() {
  InstallFh2ClockWatchKey();
  InstallFh2CommandChannel();
  REXLOG_INFO("[TODSCAN] game-clock keys: 8 = find clock, 6 = find code writing it, 7 = find master clock, 9 = +1 hour, 0 = next candidate");
  if (const char* env = std::getenv("FH2_TOD_ADDR")) {
    g_tod_selected = uint32_t(std::strtoul(env, nullptr, 16));
  }
  rex::ui::RegisterBind("bind_fh2_tod_scan", "8", "FH2: find the game clock (60 s)", [] {
    if (g_tod_scanning.exchange(true)) {
      REXLOG_INFO("[TODSCAN] 8 pressed: scan already running");
      return;
    }
    REXLOG_INFO("[TODSCAN] scanning for the game clock: keep driving in free roam for 60 s");
    std::thread(TodScan).detach();
  });
  rex::ui::RegisterBind("bind_fh2_tod_master", "7", "FH2: find the master game clock", [] {
    if (g_tod_scanning.exchange(true)) {
      REXLOG_INFO("[TODSCAN] 7 pressed: a search is already running");
      return;
    }
    REXLOG_INFO("[TODSCAN] 7 pressed: searching for the master clock (3 s)");
    std::thread(TodFindMaster).detach();
  });
  rex::ui::RegisterBind("bind_fh2_tod_next", "0", "FH2: next game clock candidate", [] {
    std::lock_guard<std::mutex> lock(g_tod_mutex);
    if (g_tod_use_master) {
      g_tod_master_index = (g_tod_master_index + 1) % g_tod_masters.size();
      REXLOG_INFO("[TODSCAN] using master #{} {:08X}", g_tod_master_index,
                  g_tod_masters[g_tod_master_index].addr);
      return;
    }
    if (g_tod_candidates.empty()) {
      REXLOG_INFO("[TODSCAN] 0 pressed: no candidates yet (press 8 first)");
      return;
    }
    g_tod_index = (g_tod_index + 1) % g_tod_candidates.size();
    g_tod_selected = g_tod_candidates[g_tod_index].addr;
    REXLOG_INFO("[TODSCAN] selected #{} {:08X}", g_tod_index, g_tod_selected);
  });
  rex::ui::RegisterBind("bind_fh2_tod_forward", "9", "FH2: move the game clock +1 hour", [] {
    if (!g_tod_selected) {
      REXLOG_INFO("[TODSCAN] 9 pressed: no clock selected yet (press 8 first)");
      return;
    }
    auto* mem = rex::system::kernel_state()->memory();
    if (g_tod_use_master) {
      std::lock_guard<std::mutex> lock(g_tod_mutex);
      const TodMaster& m = g_tod_masters[g_tod_master_index];
      double v = TodMasterRead(mem, m);
      double nv = std::fmod(v + m.per_day / 24.0, m.per_day);
      if (m.is_int) {
        *mem->TranslateVirtual<uint32_t*>(m.addr) = __builtin_bswap32(uint32_t(int32_t(nv)));
      } else if (m.is_double) {
        TodWriteD(mem, m.addr, nv);
      } else {
        TodWrite(mem, m.addr, float(nv));
      }
      REXLOG_INFO("[TODSCAN] master {:08X}: {:.4f} -> {:.4f}", m.addr, v, nv);
      return;
    }
    float v = TodRead(mem, g_tod_selected);
    float step = v <= 1.0f ? 1.0f / 24.0f : 1.0f;
    float limit = v <= 1.0f ? 1.0f : 24.0f;
    float nv = v + step;
    if (nv >= limit) nv -= limit;
    TodWrite(mem, g_tod_selected, nv);
    REXLOG_INFO("[TODSCAN] {:08X}: {:.4f} -> {:.4f}", g_tod_selected, v, nv);
  });
}

// ---------------------------------------------------------------------------
// Debug: key 6 -- find the game code that writes the clock copy. After a
// clock scan (key 8), takes the candidate rising at the time-of-day rate
// (0.02-0.05 of a day per minute), puts a hardware write watchpoint on it in
// every existing thread, and logs the writing guest code ([HWWATCH], to
// stderr / play.stdout) for the first 16 writes, then removes them.
#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <rex/system/thread_state.h>
namespace {
uint32_t g_hw_guest = 0;
std::atomic<int> g_hw_hits{0};
std::vector<int> g_hw_fds;
void HwWatchTrap(int, siginfo_t*, void*) {
  int n = g_hw_hits.fetch_add(1);
  if (n >= 16) return;
  auto* ts = rex::runtime::ThreadState::Get();
  PPCContext* c = ts ? ts->context() : nullptr;
  char buf[400];
  int len = snprintf(buf, sizeof(buf),
                     "[HWWATCH] write %08X #%d fn=%08X pc=%08X lr=%08X r3=%08X r31=%08X tid=%d bt=",
                     g_hw_guest, n, c ? c->current_function : 0u, c ? c->current_instruction : 0u,
                     c ? uint32_t(c->lr) : 0u, c ? c->r3.u32 : 0u, c ? c->r31.u32 : 0u,
                     int(syscall(SYS_gettid)));
  // Guest call stack: walk the PPC back chain from r1 (saved LR at caller_sp - 8).
  if (c && len > 0) {
    uint8_t* membase = rex::system::kernel_state()->memory()->virtual_membase();
    uint32_t sp = c->r1.u32;
    for (int i = 0; i < 10 && sp >= 0x70000000u && sp < 0x80000000u && len < 380; ++i) {
      uint32_t caller_sp = __builtin_bswap32(*reinterpret_cast<uint32_t*>(membase + sp));
      if (caller_sp <= sp || caller_sp >= 0x80000000u) break;
      uint32_t ret = __builtin_bswap32(*reinterpret_cast<uint32_t*>(membase + caller_sp - 8));
      len += snprintf(buf + len, sizeof(buf) - size_t(len), " %08X", ret);
      sp = caller_sp;
    }
  }
  if (len > 0 && len < int(sizeof(buf)) - 1) buf[len++] = '\n';
  if (len > 0) (void)!write(2, buf, size_t(len));
}
void HwWatchArm(uint32_t guest) {
  auto* mem = rex::system::kernel_state()->memory();
  g_hw_guest = guest;
  g_hw_hits = 0;
  struct sigaction sa = {};
  sa.sa_sigaction = HwWatchTrap;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGTRAP, &sa, nullptr);
  perf_event_attr attr = {};
  attr.type = PERF_TYPE_BREAKPOINT;
  attr.size = sizeof(attr);
  attr.bp_type = HW_BREAKPOINT_W;
  attr.bp_addr = reinterpret_cast<uint64_t>(mem->virtual_membase() + guest);
  attr.bp_len = HW_BREAKPOINT_LEN_4;
  attr.sample_period = 1;
  attr.remove_on_exec = 1;
  attr.sigtrap = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  int ok = 0, failed = 0, last_errno = 0;
  if (DIR* dir = opendir("/proc/self/task")) {
    while (dirent* e = readdir(dir)) {
      int tid = std::atoi(e->d_name);
      if (tid <= 0) continue;
      long fd = syscall(SYS_perf_event_open, &attr, tid, -1, -1, 0);
      if (fd >= 0) {
        g_hw_fds.push_back(int(fd));
        ++ok;
      } else {
        ++failed;
        last_errno = errno;
      }
    }
    closedir(dir);
  }
  REXLOG_INFO("[HWWATCH] watching clock copy {:08X} in {} threads ({} failed, errno {})", guest,
              ok, failed, last_errno);
  // Remove after a few seconds (16 hits are plenty).
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    for (int fd : g_hw_fds) close(fd);
    g_hw_fds.clear();
    REXLOG_INFO("[HWWATCH] removed; {} writes seen (details in play.stdout)", g_hw_hits.load());
  }).detach();
}
}  // namespace

void InstallFh2ClockWatchKey() {
  rex::ui::RegisterBind("bind_fh2_tod_watch", "6", "FH2: find the code writing the game clock", [] {
    uint32_t addr = 0;
    {
      std::lock_guard<std::mutex> lock(g_tod_mutex);
      for (const TodCandidate& c : g_tod_candidates) {
        float rise = c.last - c.first;
        if (c.last <= 1.0f && rise >= 0.02f && rise <= 0.05f) {
          addr = c.addr;
          break;
        }
      }
    }
    if (!addr) {
      REXLOG_INFO("[HWWATCH] 6 pressed: no clock copy found yet (press 8 first)");
      return;
    }
    if (!g_hw_fds.empty()) return;
    HwWatchArm(addr);
  });
}

// ---------------------------------------------------------------------------
// Time of day. sub_82686CC8 is the time-of-day update: the clock is a float
// in seconds at this+10488 (wraps at 86400), advanced by rate(this+10492) x
// frame time, and crossing 72000 s (20:00) fires the "getting dark" switch.
// The hook remembers the clock object; 'tod <hours>' on the debug channel sets
// the clock (it keeps running from there), 'tod' alone logs it.
std::atomic<uint32_t> g_tod_object{0};
std::atomic<float> g_tod_set_seconds{-1.0f};
extern "C" void __imp__sub_82686CC8(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(sub_82686CC8) {
  uint32_t self = ctx.r3.u32;
  if (g_tod_object.exchange(self) != self) {
    REXLOG_INFO("[TOD] clock object {:08X}", self);
  }
  float set = g_tod_set_seconds.exchange(-1.0f);
  if (set >= 0.0f) {
    uint32_t v;
    std::memcpy(&v, &set, 4);
    *reinterpret_cast<uint32_t*>(base + self + 10488) = __builtin_bswap32(v);
  }
  __imp__sub_82686CC8(ctx, base);
}

// ---------------------------------------------------------------------------
// Debug command channel: a named pipe "fh2ctl" in the working directory.
// One command per line (e.g. `echo "key 8" > play/fh2ctl`); results go to the
// log as [CTL] lines.
//   key <name>          press a bound key (8, 6, 7, 9, 0, F12, F3, ...)
//   peek <hex> [n]      log n 32-bit words (hex and float) at a guest address
//   poke <hex> <float>  write a float to a guest address
//   watch <hex>         hardware write watchpoint on a guest address (logs writers)
//   tod [hours]         set the game clock (e.g. tod 19.9), or log it
//   env <NAME> <v|off>  live renderer switch (FH2_* settings)
//   capture [n]         renderer frame log of the next n frames
//   say <text>          log a marker
#include <fcntl.h>
#include <sys/stat.h>
#include <sstream>
#include <rex/ui/ui_event.h>
namespace {
void CtlCommand(const std::string& line) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  if (cmd.empty()) return;
  auto* mem = rex::system::kernel_state()->memory();
  if (cmd == "key") {
    std::string name;
    in >> name;
    rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(name);
    if (vk == rex::ui::VirtualKey::kNone) {
      REXLOG_INFO("[CTL] unknown key '{}'", name);
      return;
    }
    rex::ui::KeyEvent e(nullptr, vk, 1, false, false, false, false, false);
    bool handled = rex::ui::ProcessKeyEvent(e);
    REXLOG_INFO("[CTL] key {} -> {}", name, handled ? "handled" : "no bind");
  } else if (cmd == "peek") {
    std::string a;
    uint32_t n = 1;
    in >> a >> n;
    uint32_t addr = uint32_t(std::strtoul(a.c_str(), nullptr, 16)) & ~3u;
    n = std::min<uint32_t>(std::max<uint32_t>(n, 1), 64);
    auto* heap = mem->LookupHeap(addr);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(addr, &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit)) {
      REXLOG_INFO("[CTL] peek {:08X}: not committed", addr);
      return;
    }
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t w = __builtin_bswap32(*mem->TranslateVirtual<uint32_t*>(addr + i * 4));
      float f;
      std::memcpy(&f, &w, 4);
      REXLOG_INFO("[CTL] {:08X}: {:08X} {}", addr + i * 4, w, f);
    }
  } else if (cmd == "poke") {
    std::string a;
    float f = 0;
    in >> a >> f;
    uint32_t addr = uint32_t(std::strtoul(a.c_str(), nullptr, 16)) & ~3u;
    TodWrite(mem, addr, f);
    REXLOG_INFO("[CTL] poke {:08X} = {}", addr, f);
  } else if (cmd == "watch") {
    std::string a;
    in >> a;
    uint32_t addr = uint32_t(std::strtoul(a.c_str(), nullptr, 16)) & ~3u;
    if (!g_hw_fds.empty()) {
      REXLOG_INFO("[CTL] watch: a watchpoint is still active");
      return;
    }
    HwWatchArm(addr);
  } else if (cmd == "tod") {
    std::string v;
    in >> v;
    uint32_t obj = g_tod_object.load();
    if (!obj) {
      REXLOG_INFO("[CTL] tod: clock not running yet (load into free roam)");
      return;
    }
    if (v.empty()) {
      float now = TodRead(mem, obj + 10488);
      REXLOG_INFO("[CTL] tod: {:.0f} s = {:02d}:{:02d}", now, int(now / 3600) % 24,
                  int(now / 60) % 60);
      return;
    }
    float hours = std::fmod(std::max(std::strtof(v.c_str(), nullptr), 0.0f), 24.0f);
    g_tod_set_seconds = hours * 3600.0f;
    REXLOG_INFO("[CTL] tod: clock set to {:02d}:{:02d}", int(hours) % 24,
                int(hours * 60) % 60);
  } else if (cmd == "env") {
    // Live renderer switch: env FH2_DIRECT_RESOLVE 0 | env FH2_DIRECT_RESOLVE off
    std::string name, v;
    in >> name >> v;
    using SetFn = void (*)(const char*, int, int);
    // The GPU plugin is dlopen'ed (possibly RTLD_LOCAL): look it up by name.
    void* plugin = dlopen("librexgpu-fh2.so", RTLD_NOW | RTLD_NOLOAD);
    auto fn = reinterpret_cast<SetFn>(plugin ? dlsym(plugin, "Fh2SetLiveEnv")
                                             : dlsym(RTLD_DEFAULT, "Fh2SetLiveEnv"));
    if (!fn) {
      REXLOG_INFO("[CTL] env: the renderer has no live switches (not the fh2 GPU plugin?)");
      return;
    }
    fn(name.c_str(), std::atoi(v.c_str()), v == "off");
    REXLOG_INFO("[CTL] env {} = {}", name, v);
  } else if (cmd == "capture") {
    // Frame log of the next n frames (renderer [RTLOG] lines in play.log).
    int n = 1;
    in >> n;
    void* plugin = dlopen("librexgpu-fh2.so", RTLD_NOW | RTLD_NOLOAD);
    auto fn = reinterpret_cast<void (*)(int)>(plugin ? dlsym(plugin, "Fh2CaptureFrames") : nullptr);
    if (!fn) {
      REXLOG_INFO("[CTL] capture: not available (fh2 GPU plugin required)");
      return;
    }
    fn(n);
    REXLOG_INFO("[CTL] capture {} frame(s)", n);
  } else if (cmd == "say") {
    std::string rest;
    std::getline(in, rest);
    REXLOG_INFO("[CTL] say:{}", rest);
  } else {
    REXLOG_INFO("[CTL] unknown command '{}'", cmd);
  }
}

void CtlThreadMain() {
  const char* path = "fh2ctl";
  unlink(path);
  if (mkfifo(path, 0600) != 0) {
    REXLOG_INFO("[CTL] could not create pipe {} (errno {})", path, errno);
    return;
  }
  REXLOG_INFO("[CTL] listening on {}/{}", std::filesystem::current_path().string(), path);
  std::string pending;
  while (true) {
    int fd = open(path, O_RDONLY);  // blocks until a writer connects
    if (fd < 0) return;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      pending.append(buf, size_t(n));
      size_t nl;
      while ((nl = pending.find('\n')) != std::string::npos) {
        CtlCommand(pending.substr(0, nl));
        pending.erase(0, nl + 1);
      }
    }
    close(fd);
  }
}
}  // namespace

void InstallFh2CommandChannel() { std::thread(CtlThreadMain).detach(); }

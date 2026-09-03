/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/exception_handler.h>

#if REX_PLATFORM_WIN32

#include "platform_win.h"

#include <algorithm>

#include <rex/assert.h>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/stack.h>
#include <rex/system/thread_state.h>
#include <rex/system/memory_provenance.h>
#include <rex/system/xthread.h>

namespace rex::arch {

// Handle of the added VectoredExceptionHandler.
void* veh_handle_ = nullptr;
// Handle of the added VectoredContinueHandler.
void* vch_handle_ = nullptr;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  // Visual Studio SetThreadName.
  if (ex_info->ExceptionRecord->ExceptionCode == 0x406D1388) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  thread_context.rip = ex_info->ContextRecord->Rip;
  thread_context.eflags = ex_info->ContextRecord->EFlags;
  std::memcpy(thread_context.int_registers, &ex_info->ContextRecord->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ex_info->ContextRecord->Xmm0,
              sizeof(thread_context.xmm_registers));

  // https://msdn.microsoft.com/en-us/library/ms679331(v=vs.85).aspx
  // https://msdn.microsoft.com/en-us/library/aa363082(v=vs.85).aspx
  Exception ex;
  switch (ex_info->ExceptionRecord->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case STATUS_ACCESS_VIOLATION: {
      Exception::AccessViolationOperation access_violation_operation;
      switch (ex_info->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
          access_violation_operation = Exception::AccessViolationOperation::kRead;
          break;
        case 1:
          access_violation_operation = Exception::AccessViolationOperation::kWrite;
          break;
        default:
          access_violation_operation = Exception::AccessViolationOperation::kUnknown;
          break;
      }
      ex.InitializeAccessViolation(&thread_context,
                                   ex_info->ExceptionRecord->ExceptionInformation[1],
                                   access_violation_operation);
    } break;
    default:
      // Unknown/unhandled type.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
      ex_info->ContextRecord->Rip = thread_context.rip;
      ex_info->ContextRecord->EFlags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        (&ex_info->ContextRecord->Rax)[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&ex_info->ContextRecord->Xmm0 + modified_register_index,
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  // No handler took it: an unhandled guest access violation. Log the current
  // guest thread's PPC context so the faulting guest state is visible without
  // reverse-engineering a minidump (host->guest RIP mapping is unreliable at
  // -O3). [AVDIAG]
  if (ex_info->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION) {
    auto* ts = rex::runtime::ThreadState::Get();
    if (ts && ts->context()) {
      auto* c = ts->context();
      REXLOG_ERROR(
          "[AVDIAG] guest AV: fault=0x{:X} function=0x{:08X} pc=0x{:08X} lr=0x{:08X} "
          "r1(sp)=0x{:08X} r3=0x{:08X} "
          "r4=0x{:08X} r11=0x{:08X} r20=0x{:08X} r31=0x{:08X} "
          "indirect_target=0x{:08X} indirect_caller=0x{:08X} "
          "indirect_callsite=0x{:08X} indirect_r3=0x{:08X} indirect_r4=0x{:08X} "
          "dispatch_target=0x{:08X} dispatch_kind=0x{:X} dispatch_argc=0x{:X} "
          "dispatch_r3=0x{:08X} dispatch_r4=0x{:08X}",
          ex_info->ExceptionRecord->ExceptionInformation[1], (uint32_t)c->current_function,
          (uint32_t)c->current_instruction, (uint32_t)c->lr, c->r1.u32, c->r3.u32, c->r4.u32,
          c->r11.u32, c->r20.u32, c->r31.u32, c->last_indirect_target,
          c->last_indirect_caller, c->last_indirect_callsite, c->last_indirect_r3,
          c->last_indirect_r4, c->last_dispatch_target, c->last_dispatch_kind,
          c->last_dispatch_arg_count, c->last_dispatch_r3, c->last_dispatch_r4);
      rex::runtime::LogMemoryProvenance();
      const uint32_t trace_count =
          std::min(c->indirect_trace_index, PPCContext::kIndirectTraceCapacity);
      const uint32_t trace_start = c->indirect_trace_index - trace_count;
      for (uint32_t sequence = trace_start; sequence < c->indirect_trace_index; ++sequence) {
        const auto& trace =
            c->indirect_trace[sequence & (PPCContext::kIndirectTraceCapacity - 1)];
        REXLOG_ERROR(
            "[AVDIAG] indirect trace: sequence=0x{:X} target=0x{:08X} caller=0x{:08X} "
            "callsite=0x{:08X} r3=0x{:08X} r4=0x{:08X}",
            sequence, trace.target, trace.caller, trace.callsite, trace.r3, trace.r4);
      }
      if (rex::system::XThread::IsInThread()) {
        auto* thread = rex::system::XThread::GetCurrentThread();
        const auto* creation = thread->creation_params();
        REXLOG_ERROR(
            "[AVDIAG] guest thread: id=0x{:X} handle=0x{:08X} main=0x{:X} process=0x{:08X} "
            "start=0x{:08X} context=0x{:08X}",
            thread->thread_id(), thread->handle(), thread->main_thread() ? 1 : 0,
            creation->guest_process, creation->start_address, creation->start_context);
      }
      // Walk the guest stack for code-range return addresses (the call chain).
      auto* base = rex::system::kernel_state()->memory()->virtual_membase();
      uint32_t sp = c->r1.u32;
      std::string chain;
      for (int i = 0; i < 64 && sp >= 0x1000 && sp < 0xC0000000; ++i) {
        uint32_t w = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + sp + i * 4));
        if ((w >= 0x82000000 && w < 0x831F0000) || (w >= 0x88050000 && w < 0x88250000))
          chain += fmt::format(" 0x{:08X}", w);
      }
      REXLOG_ERROR("[AVDIAG] guest return-addr chain:{}", chain);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!veh_handle_) {
    veh_handle_ = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);

    if (IsDebuggerPresent()) {
      // TODO(benvanik): do we need a continue handler if a debugger is
      // attached?
      // vch_handle_ = AddVectoredContinueHandler(1, ExceptionHandlerCallback);
    }
  }

  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (veh_handle_) {
      RemoveVectoredExceptionHandler(veh_handle_);
      veh_handle_ = nullptr;
    }
    if (vch_handle_) {
      RemoveVectoredContinueHandler(vch_handle_);
      vch_handle_ = nullptr;
    }
  }
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_WIN32

/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdlib>
#include <ucontext.h>
#include <utility>
#ifdef REXGLUE_ENABLE_PROFILING
#include <atomic>
#include <string>
#include <tracy/Tracy.hpp>
#endif

namespace rex::thread {

namespace {
// Tells Tracy which fiber is now executing on this thread. Guest fibers
// swap host stacks under Tracy's per-thread zone stack; without this a zone
// opened on one fiber and closed after a switch corrupted it, and attaching
// tracy-capture killed the game.
void ProfilerOnFiberResumed(const char* profile_name) {
#ifdef REXGLUE_ENABLE_PROFILING
  if (!TracyIsStarted) return;
  if (profile_name) {
    TracyFiberEnter(profile_name);
  } else {
    TracyFiberLeave;
  }
#else
  (void)profile_name;
#endif
}

const char* MakeFiberProfileName() {
#ifdef REXGLUE_ENABLE_PROFILING
  static std::atomic<uint32_t> next{0};
  // Leaked on purpose: Tracy keeps the pointer for the whole session.
  return (new std::string("fiber " + std::to_string(next.fetch_add(1))))->c_str();
#else
  return nullptr;
#endif
}
}  // namespace

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->profile_name_ = MakeFiberProfileName();
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  ProfilerOnFiberResumed(f->profile_name_);
  try {
    f->entry_(f->arg_);
  } catch (...) {
    f->exception_ = std::current_exception();
  }

  f->finished_ = true;
  if (f->return_fiber_) {
    SwitchTo(f->return_fiber_);
  }
  std::terminate();
}

void Fiber::SwitchTo(Fiber* target) {
  if (!target || target->finished_) {
    return;
  }
  Fiber* from = tls_current_;
  target->return_fiber_ = from;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
  // Back on `from` (someone switched to it).
  ProfilerOnFiberResumed(from->is_thread_fiber_ ? nullptr : from->profile_name_);

  if (target->exception_) {
    auto exception = std::exchange(target->exception_, {});
    std::rethrow_exception(exception);
  }
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC

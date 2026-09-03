/**
 * @file        rex/core/fiber_win32.cpp
 * @brief       Win32 backend for rex::thread::Fiber
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WIN32
#include <Windows.h>
#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdlib>
#include <utility>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->handle_ = ::ConvertThreadToFiber(nullptr);
  if (!f->handle_) {
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
  f->handle_ = ::CreateFiber(stack_size, &Fiber::Trampoline, nullptr);
  if (!f->handle_) {
    delete f;
    return nullptr;
  }
  return f;
}

/*static*/ void __stdcall Fiber::Trampoline(void*) {
  Fiber* f = tls_current_;
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
  ::SwitchToFiber(target->handle_);

  if (target->exception_) {
    auto exception = std::exchange(target->exception_, {});
    std::rethrow_exception(exception);
  }
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    ::ConvertFiberToThread();
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
    ::DeleteFiber(handle_);
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WIN32

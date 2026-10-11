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

#include <rex/dbg.h>
#include <rex/thread/mutex.h>

#if REX_PLATFORM_LINUX
#include <immintrin.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rex::thread {

global_mutex_type& global_critical_region::mutex() {
  static global_mutex_type global_mutex;
  return global_mutex;
}

#if REX_PLATFORM_LINUX
namespace {
constexpr int kGlobalMutexSpinCount = 128;

inline void futex_wait(std::atomic<uint32_t>* addr, uint32_t expected) {
  syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}
inline void futex_wake(std::atomic<uint32_t>* addr, int count) {
  syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}
// Only compared for equality, to detect the owner re-entering.
inline uint64_t current_thread_token() {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pthread_self()));
}
}  // namespace

void global_mutex::lock() {
  uint64_t self = current_thread_token();
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return;
  }
  uint32_t expected = 0;
  if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                     std::memory_order_relaxed)) [[likely]] {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return;
  }
  lock_slow();
}

void global_mutex::lock_slow() {
  uint64_t self = current_thread_token();
  for (int i = 0; i < kGlobalMutexSpinCount; ++i) {
    _mm_pause();
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
  }
  while (true) {
    // Mark contended; if it was free we now own it.
    if (state_.exchange(2, std::memory_order_acquire) == 0) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
    futex_wait(&state_, 2);
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
  }
}

void global_mutex::unlock() {
  if (--recursion_count_ > 0) {
    return;
  }
  owner_.store(0, std::memory_order_relaxed);
  if (state_.exchange(0, std::memory_order_release) == 2) {
    futex_wake(&state_, 1);
  }
}

bool global_mutex::try_lock() {
  uint64_t self = current_thread_token();
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return true;
  }
  uint32_t expected = 0;
  if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return true;
  }
  return false;
}
#endif

}  // namespace rex::thread

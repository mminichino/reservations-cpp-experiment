#pragma once

#include <atomic>

namespace reservations::platform {

/// Process-wide cooperative cancel (Ctrl+C / SIGTERM). Checked by KvWorkerPool waits,
/// getAndLock retries, and exercise loops so shutdown does not hang on join.
inline std::atomic<bool>& ShutdownFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}

inline void RequestShutdown() { ShutdownFlag().store(true, std::memory_order_relaxed); }

inline bool ShutdownRequested() { return ShutdownFlag().load(std::memory_order_relaxed); }

}  // namespace reservations::platform

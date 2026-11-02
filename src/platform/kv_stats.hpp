#pragma once

#include <atomic>
#include <cstdint>

namespace reservations::platform {

/// Process-wide KV Get size telemetry (payload bytes of found documents).
class KvGetStats {
 public:
  static void RecordFound(std::size_t bytes) {
    gets_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(bytes, std::memory_order_relaxed);
  }

  struct Snapshot {
    std::uint64_t gets{0};
    std::uint64_t bytes{0};
    [[nodiscard]] double AvgBytes() const {
      return gets == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(gets);
    }
  };

  /// Atomically read and zero counters (for interval reporting).
  static Snapshot TakeAndReset() {
    Snapshot snap;
    snap.gets = gets_.exchange(0, std::memory_order_relaxed);
    snap.bytes = bytes_.exchange(0, std::memory_order_relaxed);
    return snap;
  }

 private:
  static inline std::atomic<std::uint64_t> gets_{0};
  static inline std::atomic<std::uint64_t> bytes_{0};
};

}  // namespace reservations::platform

#pragma once

#include <atomic>
#include <cstdint>

namespace reservations::platform {

/// Process-wide KV Get telemetry scoped to shop/book operations.
/// RecordFound adds immediately to a shared counter so Gets from GetMany worker
/// threads are included. EndOperation counts the completed op; avg_get is
/// interval_bytes / interval_ops (mean found payload per shop/book, not per Get).
class KvGetStats {
 public:
  static void RecordFound(std::size_t bytes) {
    bytes_.fetch_add(bytes, std::memory_order_relaxed);
  }

  static void BeginOperation() {}

  static void EndOperation() { ops_.fetch_add(1, std::memory_order_relaxed); }

  struct Snapshot {
    std::uint64_t ops{0};
    std::uint64_t bytes{0};
    /// Mean found-document payload bytes across completed shop/book operations.
    [[nodiscard]] double AvgBytesPerOp() const {
      return ops == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(ops);
    }
  };

  static Snapshot TakeAndReset() {
    Snapshot snap;
    snap.ops = ops_.exchange(0, std::memory_order_relaxed);
    snap.bytes = bytes_.exchange(0, std::memory_order_relaxed);
    return snap;
  }

 private:
  static inline std::atomic<std::uint64_t> ops_{0};
  static inline std::atomic<std::uint64_t> bytes_{0};
};

}  // namespace reservations::platform

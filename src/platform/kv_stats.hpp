#pragma once

#include <atomic>
#include <cstdint>

namespace reservations::platform {

/// Per-operation get/put body-byte accumulator (shared with GetMany workers via BindOp).
struct KvOpAccum {
  std::atomic<std::uint64_t> get_bytes{0};
  std::atomic<std::uint64_t> put_bytes{0};

  KvOpAccum() : get_bytes(0), put_bytes(0) {}
};

/// Process-wide KV telemetry for shop/book operations.
/// avg_get/avg_put = interval body bytes / ops; max_* = max per-op total in the interval.
/// Exists probes are not counted (no body).
class KvGetStats {
 public:
  using OpState = KvOpAccum;

  class BindOp {
   public:
    explicit BindOp(OpState* op) noexcept : prev_(tls_op_) { tls_op_ = op; }
    BindOp(const BindOp&) = delete;
    BindOp& operator=(const BindOp&) = delete;
    ~BindOp() { tls_op_ = prev_; }

   private:
    OpState* prev_{nullptr};
  };

  [[nodiscard]] static OpState* CurrentOp() noexcept { return tls_op_; }

  static void RecordFound(std::size_t bytes) {
    if (tls_op_ != nullptr) {
      tls_op_->get_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  static void RecordPut(std::size_t bytes) {
    if (tls_op_ != nullptr) {
      tls_op_->put_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  static void BeginOperation() {
    tls_storage_.get_bytes.store(0, std::memory_order_relaxed);
    tls_storage_.put_bytes.store(0, std::memory_order_relaxed);
    tls_op_ = &tls_storage_;
  }

  static void EndOperation() {
    if (tls_op_ != nullptr) {
      const std::uint64_t g = tls_op_->get_bytes.load(std::memory_order_relaxed);
      const std::uint64_t p = tls_op_->put_bytes.load(std::memory_order_relaxed);
      get_bytes_.fetch_add(g, std::memory_order_relaxed);
      put_bytes_.fetch_add(p, std::memory_order_relaxed);
      AtomicMax(max_get_, g);
      AtomicMax(max_put_, p);
      tls_op_ = nullptr;
    }
    ops_.fetch_add(1, std::memory_order_relaxed);
  }

  struct Snapshot {
    std::uint64_t ops{0};
    std::uint64_t get_bytes{0};
    std::uint64_t put_bytes{0};
    std::uint64_t max_get{0};
    std::uint64_t max_put{0};

    [[nodiscard]] double AvgGetBytesPerOp() const {
      return ops == 0 ? 0.0 : static_cast<double>(get_bytes) / static_cast<double>(ops);
    }
    [[nodiscard]] double AvgPutBytesPerOp() const {
      return ops == 0 ? 0.0 : static_cast<double>(put_bytes) / static_cast<double>(ops);
    }
  };

  static Snapshot TakeAndReset() {
    Snapshot snap;
    snap.ops = ops_.exchange(0, std::memory_order_relaxed);
    snap.get_bytes = get_bytes_.exchange(0, std::memory_order_relaxed);
    snap.put_bytes = put_bytes_.exchange(0, std::memory_order_relaxed);
    snap.max_get = max_get_.exchange(0, std::memory_order_relaxed);
    snap.max_put = max_put_.exchange(0, std::memory_order_relaxed);
    return snap;
  }

 private:
  static void AtomicMax(std::atomic<std::uint64_t>& slot, std::uint64_t value) {
    std::uint64_t cur = slot.load(std::memory_order_relaxed);
    while (value > cur &&
           !slot.compare_exchange_weak(cur, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
  }

  static inline thread_local OpState tls_storage_{};
  static inline thread_local OpState* tls_op_{nullptr};
  static inline std::atomic<std::uint64_t> ops_{0};
  static inline std::atomic<std::uint64_t> get_bytes_{0};
  static inline std::atomic<std::uint64_t> put_bytes_{0};
  static inline std::atomic<std::uint64_t> max_get_{0};
  static inline std::atomic<std::uint64_t> max_put_{0};
};

}  // namespace reservations::platform

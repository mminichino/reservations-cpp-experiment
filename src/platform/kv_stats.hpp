#pragma once

#include <atomic>
#include <cstdint>

namespace reservations::platform {

/// Process-wide KV Get telemetry for shop/book operations.
///
/// BeginOperation/EndOperation run on the exercise worker thread. RecordFound
/// attributes body bytes to the current op (atomic so GetMany pool threads can
/// BindOp to the parent). avg_get = interval body bytes / completed ops.
/// Exists probes do not call RecordFound (no document body).
class KvGetStats {
 public:
  using OpState = std::atomic<std::uint64_t>;

  /// RAII: bind RecordFound on this thread to an existing op (GetMany workers).
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
      tls_op_->fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  static void BeginOperation() {
    tls_storage_.store(0, std::memory_order_relaxed);
    tls_op_ = &tls_storage_;
  }

  static void EndOperation() {
    if (tls_op_ != nullptr) {
      bytes_.fetch_add(tls_op_->load(std::memory_order_relaxed), std::memory_order_relaxed);
      tls_op_ = nullptr;
    }
    ops_.fetch_add(1, std::memory_order_relaxed);
  }

  struct Snapshot {
    std::uint64_t ops{0};
    std::uint64_t bytes{0};
    /// Mean found-document body bytes per completed shop/book operation.
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
  static inline thread_local OpState tls_storage_{0};
  static inline thread_local OpState* tls_op_{nullptr};
  static inline std::atomic<std::uint64_t> ops_{0};
  static inline std::atomic<std::uint64_t> bytes_{0};
};

}  // namespace reservations::platform

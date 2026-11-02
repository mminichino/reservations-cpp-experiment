#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace reservations::cache {

/// Per-flight write fence (seqlock-style). Odd generation => write in progress.
/// Readers wait until the generation is even (or timeout).
class FlightWriteFence {
 public:
  class Guard {
   public:
    Guard(FlightWriteFence& fence, std::string flight_id) : fence_(&fence), flight_id_(std::move(flight_id)) {
      fence_->BeginWrite(flight_id_);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept : fence_(other.fence_), flight_id_(std::move(other.flight_id_)) {
      other.fence_ = nullptr;
    }
    ~Guard() {
      if (fence_ != nullptr) {
        fence_->EndWrite(flight_id_);
      }
    }

   private:
    FlightWriteFence* fence_{nullptr};
    std::string flight_id_;
  };

  [[nodiscard]] Guard Acquire(std::string flight_id) { return Guard(*this, std::move(flight_id)); }

  void BeginWrite(std::string_view flight_id) {
    auto& slot = SlotFor(flight_id);
    std::unique_lock lock(slot.mu);
    slot.cv.wait(lock, [&] { return slot.writers == 0; });
    ++slot.writers;
    ++slot.generation;  // odd => writing
    if ((slot.generation & 1ULL) == 0ULL) {
      ++slot.generation;
    }
  }

  void EndWrite(std::string_view flight_id) {
    auto& slot = SlotFor(flight_id);
    std::lock_guard lock(slot.mu);
    if (slot.writers > 0) {
      --slot.writers;
    }
    if ((slot.generation & 1ULL) != 0ULL) {
      ++slot.generation;  // even => stable
    }
    slot.cv.notify_all();
  }

  /// Blocks while a write fence is held. Returns false on timeout (caller should KV-fetch).
  [[nodiscard]] bool WaitUntilClear(std::string_view flight_id,
                                    std::chrono::milliseconds timeout = std::chrono::milliseconds{100}) {
    auto& slot = SlotFor(flight_id);
    std::unique_lock lock(slot.mu);
    return slot.cv.wait_for(lock, timeout, [&] { return slot.writers == 0 && (slot.generation & 1ULL) == 0ULL; });
  }

  [[nodiscard]] bool IsWriting(std::string_view flight_id) {
    auto& slot = SlotFor(flight_id);
    std::lock_guard lock(slot.mu);
    return slot.writers > 0 || (slot.generation & 1ULL) != 0ULL;
  }

  [[nodiscard]] std::uint64_t Generation(std::string_view flight_id) {
    auto& slot = SlotFor(flight_id);
    std::lock_guard lock(slot.mu);
    return slot.generation;
  }

 private:
  struct Slot {
    std::mutex mu;
    std::condition_variable cv;
    std::uint64_t generation{0};
    int writers{0};
  };

  Slot& SlotFor(std::string_view flight_id) {
    std::lock_guard lock(map_mu_);
    auto& ptr = slots_[std::string(flight_id)];
    if (!ptr) {
      ptr = std::make_unique<Slot>();
    }
    return *ptr;
  }

  std::mutex map_mu_;
  // unique_ptr so map rehash does not invalidate Slot references held across waits.
  std::unordered_map<std::string, std::unique_ptr<Slot>> slots_;
};

}  // namespace reservations::cache

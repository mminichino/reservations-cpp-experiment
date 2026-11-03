#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/result.hpp"
#include "platform/couchbase_client.hpp"

namespace reservations::cache {

inline constexpr const char* kInventoryFenceCollection = "inventory";
inline constexpr std::uint32_t kInventoryFenceTtlSeconds = 5;

[[nodiscard]] inline std::string InventoryFenceKey(std::string_view flight_id) {
  return "fence::inv::" + std::string(flight_id);
}

struct InventoryFenceDocument {
  std::string type{"inventory_fence"};
  std::string flightId;
  std::uint64_t generation{0};
  std::string owner;
};

struct FenceWaitResult {
  /// True if no fence key was present when the wait finished (or never present).
  bool clear{true};
  /// True if a fence key was observed at least once during the wait.
  bool saw_fence{false};
};

/// Couchbase-backed per-flight write fence visible to all processes.
class DistributedInventoryFence {
 public:
  class Guard {
   public:
    Guard() = default;
    Guard(platform::CouchbaseClient* client, std::string flight_id)
        : client_(client), flight_id_(std::move(flight_id)) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept : client_(other.client_), flight_id_(std::move(other.flight_id_)) {
      other.client_ = nullptr;
    }
    Guard& operator=(Guard&& other) noexcept {
      if (this != &other) {
        Release();
        client_ = other.client_;
        flight_id_ = std::move(other.flight_id_);
        other.client_ = nullptr;
      }
      return *this;
    }
    ~Guard() { Release(); }

    void Release();

    [[nodiscard]] bool active() const noexcept { return client_ != nullptr; }

   private:
    platform::CouchbaseClient* client_{nullptr};
    std::string flight_id_;
  };

  /// Wait until the KV fence is absent (or timeout). Polls with short sleeps.
  [[nodiscard]] static FenceWaitResult WaitUntilClear(
      platform::CouchbaseClient& client, std::string_view flight_id,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{100});

  /// Upsert fence key with TTL after waiting for a clear window. RAII Guard Removes on destroy.
  [[nodiscard]] static Result<Guard> Acquire(
      platform::CouchbaseClient& client, std::string flight_id,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{100});

  [[nodiscard]] static Result<void> Release(platform::CouchbaseClient& client, std::string_view flight_id);
};

}  // namespace reservations::cache

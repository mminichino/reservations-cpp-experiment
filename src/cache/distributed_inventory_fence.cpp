#include "cache/distributed_inventory_fence.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "platform/document_store.hpp"

namespace reservations::cache {
namespace {

[[nodiscard]] bool FencePresent(platform::CouchbaseClient& client, std::string_view flight_id) {
  auto kv = client.Get(kInventoryFenceCollection, InventoryFenceKey(flight_id));
  return kv && kv.value().found;
}

[[nodiscard]] std::string OwnerId() {
#if defined(_WIN32)
  return "pid-" + std::to_string(_getpid());
#else
  return "pid-" + std::to_string(::getpid());
#endif
}

}  // namespace

void DistributedInventoryFence::Guard::Release() {
  if (client_ == nullptr) {
    return;
  }
  (void)DistributedInventoryFence::Release(*client_, flight_id_);
  client_ = nullptr;
}

FenceWaitResult DistributedInventoryFence::WaitUntilClear(platform::CouchbaseClient& client,
                                                          std::string_view flight_id,
                                                          std::chrono::milliseconds timeout) {
  FenceWaitResult out;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (!FencePresent(client, flight_id)) {
      out.clear = true;
      return out;
    }
    out.saw_fence = true;
    if (std::chrono::steady_clock::now() >= deadline) {
      out.clear = false;
      return out;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
}

Result<DistributedInventoryFence::Guard> DistributedInventoryFence::Acquire(
    platform::CouchbaseClient& client, std::string flight_id, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  platform::DocumentStore<InventoryFenceDocument> store(client, kInventoryFenceCollection);
  const std::string owner = OwnerId();

  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    (void)WaitUntilClear(client, flight_id, std::min(remaining, std::chrono::milliseconds{20}));

    InventoryFenceDocument doc;
    doc.flightId = flight_id;
    doc.generation = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    doc.owner = owner;

    auto upserted = store.Upsert(InventoryFenceKey(flight_id), doc, kInventoryFenceTtlSeconds);
    if (upserted) {
      return Result<Guard>::Success(Guard(&client, std::move(flight_id)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return Result<Guard>::Failure(
      Error::Make(ErrorCode::RateLimited, "Timed out acquiring inventory fence for " + flight_id));
}

Result<void> DistributedInventoryFence::Release(platform::CouchbaseClient& client,
                                                std::string_view flight_id) {
  return client.Remove(kInventoryFenceCollection, InventoryFenceKey(flight_id));
}

}  // namespace reservations::cache

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <rfl/json.hpp>

#include "common/result.hpp"
#include "platform/couchbase_client.hpp"
#include "platform/shutdown.hpp"

namespace reservations::platform {

inline constexpr std::chrono::seconds kKvLockTtl{5};
inline constexpr std::chrono::milliseconds kKvLockWait{2000};
inline constexpr std::chrono::milliseconds kKvLockRetrySleep{5};

/// Object-database-style persistence: C++ class instances are serialized with reflect-cpp
/// and stored as Couchbase JSON documents. Thread-safety: externally synchronized
/// through the underlying CouchbaseClient.
template <typename T>
class DocumentStore {
 public:
  DocumentStore(CouchbaseClient& client, std::string collection)
      : client_(client), collection_(std::move(collection)) {}

  struct Loaded {
    T document;
    std::uint64_t cas{0};
  };

  [[nodiscard]] Result<std::optional<Loaded>> Get(std::string_view key) {
    auto kv = client_.Get(collection_, key);
    if (!kv) {
      return Result<std::optional<Loaded>>::Failure(kv.error());
    }
    if (!kv.value().found) {
      return Result<std::optional<Loaded>>::Success(std::nullopt);
    }
    return ParseLoaded(key, kv.value());
  }

  /// getAndLock; retries while document_locked until kKvLockWait.
  [[nodiscard]] Result<std::pair<Loaded, KvLockGuard>> GetAndLock(
      std::string_view key, std::chrono::seconds lock_ttl = kKvLockTtl,
      std::chrono::milliseconds wait = kKvLockWait) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (true) {
      auto kv = client_.GetAndLock(collection_, key, lock_ttl);
      if (kv) {
        auto parsed = ParseLoaded(key, kv.value());
        if (!parsed) {
          (void)client_.Unlock(collection_, key, kv.value().cas);
          return Result<std::pair<Loaded, KvLockGuard>>::Failure(parsed.error());
        }
        if (!parsed.value().has_value()) {
          (void)client_.Unlock(collection_, key, kv.value().cas);
          return Result<std::pair<Loaded, KvLockGuard>>::Failure(
              Error::Make(ErrorCode::NotFound, "Document missing for " + std::string(key)));
        }
        KvLockGuard guard(&client_, collection_, std::string(key), kv.value().cas);
        return Result<std::pair<Loaded, KvLockGuard>>::Success(
            std::pair<Loaded, KvLockGuard>{std::move(*parsed.value()), std::move(guard)});
      }
      if (kv.error().code != ErrorCode::DocumentLocked) {
        return Result<std::pair<Loaded, KvLockGuard>>::Failure(kv.error());
      }
      if (ShutdownRequested() || std::chrono::steady_clock::now() >= deadline) {
        return Result<std::pair<Loaded, KvLockGuard>>::Failure(
            Error::Make(ErrorCode::RateLimited, ShutdownRequested()
                                                    ? "interrupted waiting for lock on " + std::string(key)
                                                    : "Timed out waiting for lock on " + std::string(key)));
      }
      std::this_thread::sleep_for(kKvLockRetrySleep);
    }
  }

  [[nodiscard]] Result<MutationResult> Upsert(std::string_view key, const T& document,
                                              std::optional<std::uint32_t> expiry_seconds = std::nullopt) {
    const std::string json = rfl::json::write(document);
    return client_.Upsert(collection_, key, json, expiry_seconds);
  }

  [[nodiscard]] Result<MutationResult> Insert(std::string_view key, const T& document,
                                              std::optional<std::uint32_t> expiry_seconds = std::nullopt) {
    const std::string json = rfl::json::write(document);
    return client_.Insert(collection_, key, json, expiry_seconds);
  }

  [[nodiscard]] Result<MutationResult> Replace(std::string_view key, const T& document, std::uint64_t cas,
                                               std::optional<std::uint32_t> expiry_seconds = std::nullopt) {
    const std::string json = rfl::json::write(document);
    return client_.Replace(collection_, key, json, cas, expiry_seconds);
  }

  [[nodiscard]] const std::string& collection() const noexcept { return collection_; }
  [[nodiscard]] CouchbaseClient& client() noexcept { return client_; }

 private:
  [[nodiscard]] Result<std::optional<Loaded>> ParseLoaded(std::string_view key, const KvResult& kv) {
    if (!kv.found) {
      return Result<std::optional<Loaded>>::Success(std::nullopt);
    }
    auto parsed = rfl::json::read<T>(kv.value);
    if (!parsed) {
      return Result<std::optional<Loaded>>::Failure(
          Error::Make(ErrorCode::InternalError, "reflect-cpp deserialize failed for key " + std::string(key) +
                                                    ": " + parsed.error().what()));
    }
    return Result<std::optional<Loaded>>::Success(
        Loaded{.document = std::move(parsed.value()), .cas = kv.cas});
  }

  CouchbaseClient& client_;
  std::string collection_;
};

}  // namespace reservations::platform

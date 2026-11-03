#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/env.hpp"
#include "common/result.hpp"

namespace reservations::platform {

struct KvResult {
  std::string value;
  std::uint64_t cas{0};
  bool found{false};
};

struct MutationResult {
  std::uint64_t cas{0};
};

/// Result of KV exists — CAS without loading the document body.
struct ExistsResult {
  bool found{false};
  std::uint64_t cas{0};
};

/// Thin Couchbase C++ SDK wrapper used by reflect-cpp document stores.
/// Thread-safety: externally synchronized — one instance per thread, or protect with a mutex.
class CouchbaseClient {
 public:
  explicit CouchbaseClient(CouchbaseConfig config);
  ~CouchbaseClient();

  CouchbaseClient(const CouchbaseClient&) = delete;
  CouchbaseClient& operator=(const CouchbaseClient&) = delete;

  [[nodiscard]] Result<void> Connect();
  void Close();

  [[nodiscard]] const CouchbaseConfig& config() const noexcept { return config_; }

  /// Creates bucket / scope / collections via the management REST API when missing.
  [[nodiscard]] Result<void> EnsureTopology(const std::vector<std::string>& collections);

  /// Deletes the configured bucket via management REST (no-op if absent). Caller should
  /// Close() before drop and Connect()+EnsureTopology() afterward to recreate.
  [[nodiscard]] Result<void> DropBucket();

  /// Opens the configured data bucket for KV (after topology exists).
  [[nodiscard]] Result<void> OpenBucket();

  [[nodiscard]] Result<KvResult> Get(std::string_view collection, std::string_view key);

  /// KV exists — returns whether the key is present and its CAS (no body).
  [[nodiscard]] Result<ExistsResult> Exists(std::string_view collection, std::string_view key);

  /// Pessimistic lock for mutation. On document_locked, caller should retry/wait.
  [[nodiscard]] Result<KvResult> GetAndLock(std::string_view collection, std::string_view key,
                                            std::chrono::seconds lock_duration);

  [[nodiscard]] Result<void> Unlock(std::string_view collection, std::string_view key, std::uint64_t cas);

  [[nodiscard]] Result<MutationResult> Upsert(std::string_view collection, std::string_view key,
                                              std::string_view value,
                                              std::optional<std::uint32_t> expiry_seconds = std::nullopt);
  [[nodiscard]] Result<MutationResult> Insert(std::string_view collection, std::string_view key,
                                              std::string_view value,
                                              std::optional<std::uint32_t> expiry_seconds = std::nullopt);
  [[nodiscard]] Result<MutationResult> Replace(std::string_view collection, std::string_view key,
                                               std::string_view value, std::uint64_t cas,
                                               std::optional<std::uint32_t> expiry_seconds = std::nullopt);
  [[nodiscard]] Result<void> Remove(std::string_view collection, std::string_view key);

 private:
  struct Impl;
  CouchbaseConfig config_;
  std::unique_ptr<Impl> impl_;
};

/// RAII unlock unless Release() after a successful Replace (which clears the lock).
class KvLockGuard {
 public:
  KvLockGuard() = default;
  KvLockGuard(CouchbaseClient* client, std::string collection, std::string key, std::uint64_t cas)
      : client_(client), collection_(std::move(collection)), key_(std::move(key)), cas_(cas) {}

  KvLockGuard(const KvLockGuard&) = delete;
  KvLockGuard& operator=(const KvLockGuard&) = delete;

  KvLockGuard(KvLockGuard&& other) noexcept
      : client_(other.client_),
        collection_(std::move(other.collection_)),
        key_(std::move(other.key_)),
        cas_(other.cas_) {
    other.client_ = nullptr;
  }

  KvLockGuard& operator=(KvLockGuard&& other) noexcept {
    if (this != &other) {
      Unlock();
      client_ = other.client_;
      collection_ = std::move(other.collection_);
      key_ = std::move(other.key_);
      cas_ = other.cas_;
      other.client_ = nullptr;
    }
    return *this;
  }

  ~KvLockGuard() { Unlock(); }

  /// Call after successful Replace so destructor does not Unlock.
  void Release() { client_ = nullptr; }

  [[nodiscard]] std::uint64_t cas() const noexcept { return cas_; }
  [[nodiscard]] explicit operator bool() const noexcept { return client_ != nullptr; }

 private:
  void Unlock() {
    if (client_ != nullptr) {
      (void)client_->Unlock(collection_, key_, cas_);
      client_ = nullptr;
    }
  }

  CouchbaseClient* client_{nullptr};
  std::string collection_;
  std::string key_;
  std::uint64_t cas_{0};
};

}  // namespace reservations::platform

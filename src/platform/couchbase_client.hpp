#pragma once

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

}  // namespace reservations::platform

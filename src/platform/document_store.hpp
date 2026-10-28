#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rfl/json.hpp>

#include "common/result.hpp"
#include "platform/couchbase_client.hpp"

namespace reservations::platform {

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
    auto parsed = rfl::json::read<T>(kv.value().value);
    if (!parsed) {
      return Result<std::optional<Loaded>>::Failure(
          Error::Make(ErrorCode::InternalError, "reflect-cpp deserialize failed for key " + std::string(key) +
                                                    ": " + parsed.error().what()));
    }
    return Result<std::optional<Loaded>>::Success(
        Loaded{.document = std::move(parsed.value()), .cas = kv.value().cas});
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

 private:
  CouchbaseClient& client_;
  std::string collection_;
};

}  // namespace reservations::platform

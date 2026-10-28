#include "platform/couchbase_client.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include <couchbase/cluster.hxx>
#include <couchbase/codec/codec_flags.hxx>
#include <couchbase/codec/encoded_value.hxx>
#include <couchbase/codec/raw_json_transcoder.hxx>
#include <couchbase/codec/transcoder_traits.hxx>
#include <curl/curl.h>
#include <fmt/format.h>
#include <system_error>

namespace reservations::platform {
namespace {

size_t CurlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

struct HttpResponse {
  long status{0};
  std::string body;
};

Result<HttpResponse> HttpRequest(const std::string& method, const std::string& url,
                                 const std::string& username, const std::string& password,
                                 const std::string& body, const std::string& content_type) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return Result<HttpResponse>::Failure(Error::Make(ErrorCode::InternalError, "curl_easy_init failed"));
  }

  HttpResponse response;
  struct curl_slist* headers = nullptr;
  if (!content_type.empty()) {
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
  curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  if (headers != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    const std::string msg = std::string("HTTP request failed: ") + curl_easy_strerror(rc);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return Result<HttpResponse>::Failure(Error::Make(ErrorCode::ConnectionError, msg));
  }
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return Result<HttpResponse>::Success(std::move(response));
}

std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      out.push_back('&');
    }
    char* key = curl_easy_escape(nullptr, fields[i].first.c_str(), static_cast<int>(fields[i].first.size()));
    char* val = curl_easy_escape(nullptr, fields[i].second.c_str(), static_cast<int>(fields[i].second.size()));
    out.append(key ? key : "");
    out.push_back('=');
    out.append(val ? val : "");
    curl_free(key);
    curl_free(val);
  }
  return out;
}

Result<int> ParseDataMemoryQuotaMb(const std::string& pools_json) {
  const std::string key = "\"memoryQuota\"";
  const auto pos = pools_json.find(key);
  if (pos == std::string::npos) {
    return Result<int>::Failure(
        Error::Make(ErrorCode::InternalError, "memoryQuota not found in /pools/default response"));
  }
  const auto colon = pools_json.find(':', pos + key.size());
  if (colon == std::string::npos) {
    return Result<int>::Failure(Error::Make(ErrorCode::InternalError, "malformed memoryQuota field"));
  }
  std::size_t i = colon + 1;
  while (i < pools_json.size() && (pools_json[i] == ' ' || pools_json[i] == '\t')) {
    ++i;
  }
  if (i >= pools_json.size() || pools_json[i] < '0' || pools_json[i] > '9') {
    return Result<int>::Failure(Error::Make(ErrorCode::InternalError, "memoryQuota is not an integer"));
  }
  int value = 0;
  while (i < pools_json.size() && pools_json[i] >= '0' && pools_json[i] <= '9') {
    value = value * 10 + (pools_json[i] - '0');
    ++i;
  }
  if (value < 256) {
    return Result<int>::Failure(Error::Make(ErrorCode::InternalError,
                                            "memoryQuota " + std::to_string(value) + " MB is below minimum 256"));
  }
  return Result<int>::Success(value);
}

std::string FormatSdkError(const couchbase::error& err) {
  // Avoid couchbase/fmt/error.hxx — on Linux it collides with std::format_to vs fmt::format_to.
  if (err.message().empty()) {
    return std::string(err.ec().message());
  }
  return fmt::format("{} - {}", err.ec().message(), err.message());
}

Error FromSdkError(const couchbase::error& err, const char* what) {
  if (err.ec() == couchbase::errc::key_value::document_exists) {
    return Error::Make(ErrorCode::AlreadyExists, std::string(what) + ": document exists");
  }
  if (err.ec() == couchbase::errc::common::cas_mismatch) {
    return Error::Make(ErrorCode::VersionConflict, std::string(what) + ": CAS mismatch");
  }
  if (err.ec() == couchbase::errc::key_value::document_not_found) {
    return Error::Make(ErrorCode::NotFound, std::string(what) + ": document not found");
  }
  return Error::Make(ErrorCode::InternalError, fmt::format("{}: {}", what, FormatSdkError(err)));
}

}  // namespace

struct CouchbaseClient::Impl {
  std::optional<couchbase::cluster> cluster;
  std::mutex mutex;
};

CouchbaseClient::CouchbaseClient(CouchbaseConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

CouchbaseClient::~CouchbaseClient() { Close(); }

Result<void> CouchbaseClient::Connect() {
  couchbase::cluster_options options(config_.username, config_.password);
  auto [err, cluster] = couchbase::cluster::connect(config_.connection_string, options).get();
  if (err) {
    return Result<void>::Failure(Error::Make(ErrorCode::ConnectionError,
                                             fmt::format("cluster connect: {}", FormatSdkError(err))));
  }
  impl_->cluster = std::move(cluster);
  return Result<void>::Success();
}

void CouchbaseClient::Close() {
  if (impl_ && impl_->cluster.has_value()) {
    impl_->cluster->close().get();
    impl_->cluster.reset();
  }
}

Result<void> CouchbaseClient::DropBucket() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const std::string base = "http://" + config_.management_host + ":" + std::to_string(config_.management_port);

  const auto list =
      HttpRequest("GET", base + "/pools/default/buckets", config_.username, config_.password, "", "");
  if (!list) {
    return Result<void>::Failure(list.error());
  }
  const bool exists = list.value().body.find("\"name\":\"" + config_.bucket + "\"") != std::string::npos ||
                      list.value().body.find("\"name\": \"" + config_.bucket + "\"") != std::string::npos;
  if (!exists) {
    return Result<void>::Success();
  }

  const auto deleted = HttpRequest("DELETE", base + "/pools/default/buckets/" + config_.bucket,
                                   config_.username, config_.password, "", "");
  if (!deleted) {
    return Result<void>::Failure(deleted.error());
  }
  if (deleted.value().status >= 300 && deleted.value().status != 404) {
    return Result<void>::Failure(Error::Make(
        ErrorCode::InternalError,
        "Delete bucket failed (" + std::to_string(deleted.value().status) + "): " + deleted.value().body));
  }

  for (int i = 0; i < 60; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto check =
        HttpRequest("GET", base + "/pools/default/buckets", config_.username, config_.password, "", "");
    if (!check) {
      continue;
    }
    const bool still =
        check.value().body.find("\"name\":\"" + config_.bucket + "\"") != std::string::npos ||
        check.value().body.find("\"name\": \"" + config_.bucket + "\"") != std::string::npos;
    if (!still) {
      return Result<void>::Success();
    }
  }
  return Result<void>::Failure(
      Error::Make(ErrorCode::InternalError, "Timed out waiting for bucket '" + config_.bucket + "' to disappear"));
}

Result<void> CouchbaseClient::EnsureTopology(const std::vector<std::string>& collections) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const std::string base = "http://" + config_.management_host + ":" + std::to_string(config_.management_port);

  const auto pools = HttpRequest("GET", base + "/pools/default", config_.username, config_.password, "", "");
  if (!pools) {
    return Result<void>::Failure(pools.error());
  }
  if (pools.value().status != 200) {
    return Result<void>::Failure(Error::Make(
        ErrorCode::InternalError,
        "GET /pools/default failed (" + std::to_string(pools.value().status) + "): " + pools.value().body));
  }
  auto quota = ParseDataMemoryQuotaMb(pools.value().body);
  if (!quota) {
    return Result<void>::Failure(quota.error());
  }
  const std::string ram_quota_mb = std::to_string(quota.value());
  std::cerr << "Using Data Service memoryQuota=" << ram_quota_mb << " MB for bucket '" << config_.bucket
            << "'\n";

  {
    const auto list = HttpRequest("GET", base + "/pools/default/buckets", config_.username, config_.password, "",
                                  "");
    if (!list) {
      return Result<void>::Failure(list.error());
    }
    const bool exists = list.value().body.find("\"name\":\"" + config_.bucket + "\"") != std::string::npos ||
                        list.value().body.find("\"name\": \"" + config_.bucket + "\"") != std::string::npos;
    if (!exists) {
      const auto body = FormEncode({{"name", config_.bucket},
                                    {"ramQuotaMB", ram_quota_mb},
                                    {"bucketType", "couchbase"},
                                    {"flushEnabled", "1"},
                                    {"replicaNumber", "0"}});
      const auto created =
          HttpRequest("POST", base + "/pools/default/buckets", config_.username, config_.password, body,
                      "application/x-www-form-urlencoded");
      if (!created) {
        return Result<void>::Failure(created.error());
      }
      if (created.value().status >= 300 && created.value().status != 400) {
        return Result<void>::Failure(Error::Make(
            ErrorCode::InternalError,
            "Create bucket failed (" + std::to_string(created.value().status) + "): " + created.value().body));
      }
      for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto check = HttpRequest("GET", base + "/pools/default/buckets/" + config_.bucket, config_.username,
                                       config_.password, "", "");
        if (check && check.value().status == 200) {
          break;
        }
      }
    } else {
      const auto body = FormEncode({{"ramQuotaMB", ram_quota_mb}});
      const auto updated =
          HttpRequest("POST", base + "/pools/default/buckets/" + config_.bucket, config_.username,
                      config_.password, body, "application/x-www-form-urlencoded");
      if (!updated) {
        return Result<void>::Failure(updated.error());
      }
      if (updated.value().status >= 300) {
        return Result<void>::Failure(Error::Make(
            ErrorCode::InternalError, "Update bucket quota failed (" + std::to_string(updated.value().status) +
                                          "): " + updated.value().body));
      }
    }
  }

  {
    const auto scopes = HttpRequest("GET", base + "/pools/default/buckets/" + config_.bucket + "/scopes",
                                    config_.username, config_.password, "", "");
    if (!scopes) {
      return Result<void>::Failure(scopes.error());
    }
    const bool scope_exists = scopes.value().body.find("\"name\":\"" + config_.scope + "\"") != std::string::npos ||
                              scopes.value().body.find("\"name\": \"" + config_.scope + "\"") != std::string::npos;
    if (!scope_exists) {
      const auto body = FormEncode({{"name", config_.scope}});
      const auto created = HttpRequest("POST", base + "/pools/default/buckets/" + config_.bucket + "/scopes",
                                       config_.username, config_.password, body,
                                       "application/x-www-form-urlencoded");
      if (!created) {
        return Result<void>::Failure(created.error());
      }
      if (created.value().status >= 300 && created.value().status != 400) {
        return Result<void>::Failure(Error::Make(
            ErrorCode::InternalError,
            "Create scope failed (" + std::to_string(created.value().status) + "): " + created.value().body));
      }
    }
  }

  for (const auto& collection : collections) {
    const auto scopes = HttpRequest("GET", base + "/pools/default/buckets/" + config_.bucket + "/scopes",
                                    config_.username, config_.password, "", "");
    if (!scopes) {
      return Result<void>::Failure(scopes.error());
    }
    const bool collection_exists =
        scopes.value().body.find("\"name\":\"" + collection + "\"") != std::string::npos ||
        scopes.value().body.find("\"name\": \"" + collection + "\"") != std::string::npos;
    if (!collection_exists) {
      const auto body = FormEncode({{"name", collection}});
      const auto created = HttpRequest(
          "POST", base + "/pools/default/buckets/" + config_.bucket + "/scopes/" + config_.scope + "/collections",
          config_.username, config_.password, body, "application/x-www-form-urlencoded");
      if (!created) {
        return Result<void>::Failure(created.error());
      }
      if (created.value().status >= 300 && created.value().status != 400) {
        return Result<void>::Failure(Error::Make(ErrorCode::InternalError,
                                                 "Create collection '" + collection + "' failed (" +
                                                     std::to_string(created.value().status) +
                                                     "): " + created.value().body));
      }
    }
  }

  auto opened = OpenBucket();
  if (!opened) {
    return opened;
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  return Result<void>::Success();
}

Result<void> CouchbaseClient::OpenBucket() {
  if (!impl_->cluster.has_value()) {
    return Result<void>::Failure(Error::Make(ErrorCode::ConnectionError, "cluster not connected"));
  }
  // Touch the bucket so the SDK opens it; collection ops will use scope/collection paths.
  (void)impl_->cluster->bucket(config_.bucket);
  return Result<void>::Success();
}

namespace {

couchbase::collection CollectionFor(couchbase::cluster& cluster, const CouchbaseConfig& config,
                                    std::string_view collection) {
  return cluster.bucket(config.bucket).scope(config.scope).collection(std::string(collection));
}

}  // namespace

/// Decode JSON bytes regardless of datatype flags (legacy libcouchbase docs often use flags=0).
struct permissive_json_string_transcoder {
  template<typename Document, std::enable_if_t<std::is_same_v<Document, std::string>, bool> = true>
  static auto encode(const Document& document) -> couchbase::codec::encoded_value {
    return couchbase::codec::raw_json_transcoder::encode<Document>(document);
  }

  template<typename Document, std::enable_if_t<std::is_same_v<Document, std::string>, bool> = true>
  static auto decode(const couchbase::codec::encoded_value& encoded) -> Document {
    return std::string{reinterpret_cast<const char*>(encoded.data.data()), encoded.data.size()};
  }
};

}  // namespace reservations::platform

namespace couchbase::codec {
template<>
struct is_transcoder<reservations::platform::permissive_json_string_transcoder> : public std::true_type {
};
}  // namespace couchbase::codec

namespace reservations::platform {

Result<KvResult> CouchbaseClient::Get(std::string_view collection, std::string_view key) {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->cluster.has_value()) {
    return Result<KvResult>::Failure(Error::Make(ErrorCode::ConnectionError, "cluster not connected"));
  }
  auto coll = CollectionFor(*impl_->cluster, config_, collection);
  auto [err, result] = coll.get(std::string(key)).get();
  if (err.ec() == couchbase::errc::key_value::document_not_found) {
    KvResult empty;
    empty.found = false;
    return Result<KvResult>::Success(std::move(empty));
  }
  if (err) {
    return Result<KvResult>::Failure(FromSdkError(err, "get"));
  }
  KvResult out;
  out.found = true;
  try {
    out.value = result.template content_as<std::string, permissive_json_string_transcoder>();
  } catch (const std::system_error& ex) {
    return Result<KvResult>::Failure(
        Error::Make(ErrorCode::InternalError, fmt::format("get decode: {}", ex.what())));
  }
  out.cas = result.cas().value();
  return Result<KvResult>::Success(std::move(out));
}

Result<MutationResult> CouchbaseClient::Upsert(std::string_view collection, std::string_view key,
                                               std::string_view value,
                                               std::optional<std::uint32_t> expiry_seconds) {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->cluster.has_value()) {
    return Result<MutationResult>::Failure(Error::Make(ErrorCode::ConnectionError, "cluster not connected"));
  }
  auto coll = CollectionFor(*impl_->cluster, config_, collection);
  auto options = couchbase::upsert_options{};
  if (expiry_seconds.has_value()) {
    options.expiry(std::chrono::seconds{*expiry_seconds});
  }
  auto [err, result] =
      coll.upsert<couchbase::codec::raw_json_transcoder>(std::string(key), std::string(value), options).get();
  if (err) {
    return Result<MutationResult>::Failure(FromSdkError(err, "upsert"));
  }
  return Result<MutationResult>::Success(MutationResult{.cas = result.cas().value()});
}

Result<MutationResult> CouchbaseClient::Insert(std::string_view collection, std::string_view key,
                                               std::string_view value,
                                               std::optional<std::uint32_t> expiry_seconds) {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->cluster.has_value()) {
    return Result<MutationResult>::Failure(Error::Make(ErrorCode::ConnectionError, "cluster not connected"));
  }
  auto coll = CollectionFor(*impl_->cluster, config_, collection);
  auto options = couchbase::insert_options{};
  if (expiry_seconds.has_value()) {
    options.expiry(std::chrono::seconds{*expiry_seconds});
  }
  auto [err, result] =
      coll.insert<couchbase::codec::raw_json_transcoder>(std::string(key), std::string(value), options).get();
  if (err) {
    return Result<MutationResult>::Failure(FromSdkError(err, "insert"));
  }
  return Result<MutationResult>::Success(MutationResult{.cas = result.cas().value()});
}

Result<MutationResult> CouchbaseClient::Replace(std::string_view collection, std::string_view key,
                                                std::string_view value, std::uint64_t cas,
                                                std::optional<std::uint32_t> expiry_seconds) {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->cluster.has_value()) {
    return Result<MutationResult>::Failure(Error::Make(ErrorCode::ConnectionError, "cluster not connected"));
  }
  auto coll = CollectionFor(*impl_->cluster, config_, collection);
  auto options = couchbase::replace_options{}.cas(couchbase::cas{cas});
  if (expiry_seconds.has_value()) {
    options.expiry(std::chrono::seconds{*expiry_seconds});
  }
  auto [err, result] =
      coll.replace<couchbase::codec::raw_json_transcoder>(std::string(key), std::string(value), options).get();
  if (err) {
    return Result<MutationResult>::Failure(FromSdkError(err, "replace"));
  }
  return Result<MutationResult>::Success(MutationResult{.cas = result.cas().value()});
}

Result<void> CouchbaseClient::Remove(std::string_view collection, std::string_view key) {
  std::scoped_lock lock(impl_->mutex);
  if (!impl_->cluster.has_value()) {
    return Result<void>::Failure(Error::Make(ErrorCode::ConnectionError, "cluster not connected"));
  }
  auto coll = CollectionFor(*impl_->cluster, config_, collection);
  auto [err, result] = coll.remove(std::string(key)).get();
  (void)result;
  if (err.ec() == couchbase::errc::key_value::document_not_found) {
    return Result<void>::Success();
  }
  if (err) {
    return Result<void>::Failure(FromSdkError(err, "remove"));
  }
  return Result<void>::Success();
}

}  // namespace reservations::platform

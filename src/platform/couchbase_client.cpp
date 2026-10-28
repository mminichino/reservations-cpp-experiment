#include "platform/couchbase_client.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include <curl/curl.h>
#include <libcouchbase/couchbase.h>

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

/// Parses the Data Service memory quota (MB) from GET /pools/default JSON.
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

}  // namespace

struct CouchbaseClient::Impl {
  lcb_INSTANCE* instance{nullptr};
  std::mutex mutex;
};

CouchbaseClient::CouchbaseClient(CouchbaseConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

CouchbaseClient::~CouchbaseClient() { Close(); }

Result<void> CouchbaseClient::Connect() {
  lcb_CREATEOPTS* options = nullptr;
  lcb_createopts_create(&options, LCB_TYPE_CLUSTER);
  lcb_createopts_connstr(options, config_.connection_string.c_str(), config_.connection_string.size());
  lcb_createopts_credentials(options, config_.username.c_str(), config_.username.size(),
                             config_.password.c_str(), config_.password.size());

  lcb_STATUS rc = lcb_create(&impl_->instance, options);
  lcb_createopts_destroy(options);
  if (rc != LCB_SUCCESS) {
    return Result<void>::Failure(
        Error::Make(ErrorCode::ConnectionError, std::string("lcb_create: ") + lcb_strerror_short(rc)));
  }

  rc = lcb_connect(impl_->instance);
  if (rc != LCB_SUCCESS) {
    return Result<void>::Failure(
        Error::Make(ErrorCode::ConnectionError, std::string("lcb_connect: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);

  rc = lcb_get_bootstrap_status(impl_->instance);
  if (rc != LCB_SUCCESS) {
    return Result<void>::Failure(Error::Make(
        ErrorCode::ConnectionError, std::string("bootstrap failed: ") + lcb_strerror_short(rc)));
  }

  // Callbacks are installed after they are defined below; see InstallCallbacks helper.
  return Result<void>::Success();
}

void CouchbaseClient::Close() {
  if (impl_ && impl_->instance != nullptr) {
    lcb_destroy(impl_->instance);
    impl_->instance = nullptr;
  }
}

Result<void> CouchbaseClient::EnsureTopology(const std::vector<std::string>& collections) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const std::string base = "http://" + config_.management_host + ":" + std::to_string(config_.management_port);

  // Data Service memory quota (MB) — use as the bucket RAM quota (maximum for KV).
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

  // Create bucket if missing; otherwise resize to the cluster data quota.
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
      // Wait for bucket readiness.
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

  // Create scope if missing.
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

  // Create collections if missing.
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

  // Brief pause so collection manifest propagates to KV nodes.
  std::this_thread::sleep_for(std::chrono::seconds(2));
  return Result<void>::Success();
}

Result<void> CouchbaseClient::OpenBucket() {
  lcb_STATUS rc = lcb_open(impl_->instance, config_.bucket.c_str(), config_.bucket.size());
  if (rc != LCB_SUCCESS) {
    return Result<void>::Failure(
        Error::Make(ErrorCode::ConnectionError, std::string("lcb_open: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);
  return Result<void>::Success();
}

namespace {

struct GetCookie {
  Result<KvResult> result = Result<KvResult>::Failure(Error::Make(ErrorCode::InternalError, "not set"));
};

void GetCallback(lcb_INSTANCE*, int, const lcb_RESPGET* resp) {
  GetCookie* cookie = nullptr;
  lcb_respget_cookie(resp, reinterpret_cast<void**>(&cookie));
  const lcb_STATUS rc = lcb_respget_status(resp);
  if (rc == LCB_ERR_DOCUMENT_NOT_FOUND) {
    KvResult empty;
    empty.found = false;
    cookie->result = Result<KvResult>::Success(std::move(empty));
    return;
  }
  if (rc != LCB_SUCCESS) {
    cookie->result = Result<KvResult>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("get: ") + lcb_strerror_short(rc)));
    return;
  }
  KvResult out;
  out.found = true;
  const char* value = nullptr;
  std::size_t nvalue = 0;
  lcb_respget_value(resp, &value, &nvalue);
  out.value.assign(value, nvalue);
  lcb_respget_cas(resp, &out.cas);
  cookie->result = Result<KvResult>::Success(std::move(out));
}

struct StoreCookie {
  Result<MutationResult> result =
      Result<MutationResult>::Failure(Error::Make(ErrorCode::InternalError, "not set"));
};

void StoreCallback(lcb_INSTANCE*, int, const lcb_RESPSTORE* resp) {
  StoreCookie* cookie = nullptr;
  lcb_respstore_cookie(resp, reinterpret_cast<void**>(&cookie));
  const lcb_STATUS rc = lcb_respstore_status(resp);
  if (rc == LCB_ERR_DOCUMENT_EXISTS) {
    cookie->result = Result<MutationResult>::Failure(Error::Make(ErrorCode::AlreadyExists, "document exists"));
    return;
  }
  if (rc == LCB_ERR_CAS_MISMATCH) {
    cookie->result = Result<MutationResult>::Failure(Error::Make(ErrorCode::VersionConflict, "CAS mismatch"));
    return;
  }
  if (rc != LCB_SUCCESS) {
    cookie->result = Result<MutationResult>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("store: ") + lcb_strerror_short(rc)));
    return;
  }
  MutationResult out;
  lcb_respstore_cas(resp, &out.cas);
  cookie->result = Result<MutationResult>::Success(out);
}

struct RemoveCookie {
  Result<void> result = Result<void>::Failure(Error::Make(ErrorCode::InternalError, "not set"));
};

void RemoveCallback(lcb_INSTANCE*, int, const lcb_RESPREMOVE* resp) {
  RemoveCookie* cookie = nullptr;
  lcb_respremove_cookie(resp, reinterpret_cast<void**>(&cookie));
  const lcb_STATUS rc = lcb_respremove_status(resp);
  if (rc == LCB_ERR_DOCUMENT_NOT_FOUND) {
    cookie->result = Result<void>::Success();
    return;
  }
  if (rc != LCB_SUCCESS) {
    cookie->result = Result<void>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("remove: ") + lcb_strerror_short(rc)));
    return;
  }
  cookie->result = Result<void>::Success();
}

void InstallCallbacks(lcb_INSTANCE* instance) {
  lcb_install_callback(instance, LCB_CALLBACK_GET, reinterpret_cast<lcb_RESPCALLBACK>(GetCallback));
  lcb_install_callback(instance, LCB_CALLBACK_STORE, reinterpret_cast<lcb_RESPCALLBACK>(StoreCallback));
  lcb_install_callback(instance, LCB_CALLBACK_REMOVE, reinterpret_cast<lcb_RESPCALLBACK>(RemoveCallback));
}

}  // namespace

Result<KvResult> CouchbaseClient::Get(std::string_view collection, std::string_view key) {
  InstallCallbacks(impl_->instance);
  std::scoped_lock lock(impl_->mutex);
  lcb_CMDGET* cmd = nullptr;
  lcb_cmdget_create(&cmd);
  lcb_cmdget_collection(cmd, config_.scope.c_str(), config_.scope.size(), collection.data(), collection.size());
  lcb_cmdget_key(cmd, key.data(), key.size());

  GetCookie cookie;
  lcb_STATUS rc = lcb_get(impl_->instance, &cookie, cmd);
  lcb_cmdget_destroy(cmd);
  if (rc != LCB_SUCCESS) {
    return Result<KvResult>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("lcb_get schedule: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);
  return std::move(cookie.result);
}

Result<MutationResult> CouchbaseClient::Upsert(std::string_view collection, std::string_view key,
                                               std::string_view value,
                                               std::optional<std::uint32_t> expiry_seconds) {
  std::scoped_lock lock(impl_->mutex);
  lcb_CMDSTORE* cmd = nullptr;
  lcb_cmdstore_create(&cmd, LCB_STORE_UPSERT);
  lcb_cmdstore_collection(cmd, config_.scope.c_str(), config_.scope.size(), collection.data(), collection.size());
  lcb_cmdstore_key(cmd, key.data(), key.size());
  lcb_cmdstore_value(cmd, value.data(), value.size());
  if (expiry_seconds.has_value()) {
    lcb_cmdstore_expiry(cmd, *expiry_seconds);
  }

  StoreCookie cookie;
  InstallCallbacks(impl_->instance);
  lcb_STATUS rc = lcb_store(impl_->instance, &cookie, cmd);
  lcb_cmdstore_destroy(cmd);
  if (rc != LCB_SUCCESS) {
    return Result<MutationResult>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("lcb_store schedule: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);
  return std::move(cookie.result);
}

Result<MutationResult> CouchbaseClient::Insert(std::string_view collection, std::string_view key,
                                               std::string_view value,
                                               std::optional<std::uint32_t> expiry_seconds) {
  std::scoped_lock lock(impl_->mutex);
  InstallCallbacks(impl_->instance);
  lcb_CMDSTORE* cmd = nullptr;
  lcb_cmdstore_create(&cmd, LCB_STORE_INSERT);
  lcb_cmdstore_collection(cmd, config_.scope.c_str(), config_.scope.size(), collection.data(), collection.size());
  lcb_cmdstore_key(cmd, key.data(), key.size());
  lcb_cmdstore_value(cmd, value.data(), value.size());
  if (expiry_seconds.has_value()) {
    lcb_cmdstore_expiry(cmd, *expiry_seconds);
  }

  StoreCookie cookie;
  lcb_STATUS rc = lcb_store(impl_->instance, &cookie, cmd);
  lcb_cmdstore_destroy(cmd);
  if (rc != LCB_SUCCESS) {
    return Result<MutationResult>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("lcb_store schedule: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);
  return std::move(cookie.result);
}

Result<MutationResult> CouchbaseClient::Replace(std::string_view collection, std::string_view key,
                                                std::string_view value, std::uint64_t cas,
                                                std::optional<std::uint32_t> expiry_seconds) {
  std::scoped_lock lock(impl_->mutex);
  InstallCallbacks(impl_->instance);
  lcb_CMDSTORE* cmd = nullptr;
  lcb_cmdstore_create(&cmd, LCB_STORE_REPLACE);
  lcb_cmdstore_collection(cmd, config_.scope.c_str(), config_.scope.size(), collection.data(), collection.size());
  lcb_cmdstore_key(cmd, key.data(), key.size());
  lcb_cmdstore_value(cmd, value.data(), value.size());
  lcb_cmdstore_cas(cmd, cas);
  if (expiry_seconds.has_value()) {
    lcb_cmdstore_expiry(cmd, *expiry_seconds);
  }

  StoreCookie cookie;
  lcb_STATUS rc = lcb_store(impl_->instance, &cookie, cmd);
  lcb_cmdstore_destroy(cmd);
  if (rc != LCB_SUCCESS) {
    return Result<MutationResult>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("lcb_store schedule: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);
  return std::move(cookie.result);
}

Result<void> CouchbaseClient::Remove(std::string_view collection, std::string_view key) {
  std::scoped_lock lock(impl_->mutex);
  InstallCallbacks(impl_->instance);
  lcb_CMDREMOVE* cmd = nullptr;
  lcb_cmdremove_create(&cmd);
  lcb_cmdremove_collection(cmd, config_.scope.c_str(), config_.scope.size(), collection.data(), collection.size());
  lcb_cmdremove_key(cmd, key.data(), key.size());

  RemoveCookie cookie;
  lcb_STATUS rc = lcb_remove(impl_->instance, &cookie, cmd);
  lcb_cmdremove_destroy(cmd);
  if (rc != LCB_SUCCESS) {
    return Result<void>::Failure(
        Error::Make(ErrorCode::InternalError, std::string("lcb_remove schedule: ") + lcb_strerror_short(rc)));
  }
  lcb_wait(impl_->instance, LCB_WAIT_DEFAULT);
  return std::move(cookie.result);
}

}  // namespace reservations::platform

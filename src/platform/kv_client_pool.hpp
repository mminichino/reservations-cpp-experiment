#pragma once

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "common/env.hpp"
#include "common/result.hpp"
#include "platform/couchbase_client.hpp"

namespace reservations::platform {

/// Thread-safe pool of CouchbaseClient instances. Each leased client is used by one
/// thread at a time (clients are not internally concurrent).
class KvClientPool {
 public:
  class Lease {
   public:
    Lease() = default;
    Lease(KvClientPool* pool, CouchbaseClient* client) : pool_(pool), client_(client) {}
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept : pool_(other.pool_), client_(other.client_) {
      other.pool_ = nullptr;
      other.client_ = nullptr;
    }
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        Release();
        pool_ = other.pool_;
        client_ = other.client_;
        other.pool_ = nullptr;
        other.client_ = nullptr;
      }
      return *this;
    }
    ~Lease() { Release(); }

    void Release() {
      if (pool_ != nullptr && client_ != nullptr) {
        pool_->Release(client_);
        pool_ = nullptr;
        client_ = nullptr;
      }
    }

    [[nodiscard]] CouchbaseClient& operator*() const { return *client_; }
    [[nodiscard]] CouchbaseClient* get() const noexcept { return client_; }
    [[nodiscard]] explicit operator bool() const noexcept { return client_ != nullptr; }

   private:
    KvClientPool* pool_{nullptr};
    CouchbaseClient* client_{nullptr};
  };

  explicit KvClientPool(CouchbaseConfig config, int capacity = 16)
      : config_(std::move(config)), capacity_(std::max(1, capacity)) {}

  [[nodiscard]] Result<void> EnsureReady() {
    std::unique_lock lock(mu_);
    while (static_cast<int>(clients_.size()) < capacity_) {
      if (connecting_) {
        cv_.wait(lock, [&] {
          return !connecting_ || static_cast<int>(clients_.size()) >= capacity_;
        });
        continue;
      }
      connecting_ = true;
      auto client = std::make_unique<CouchbaseClient>(config_);
      lock.unlock();
      Result<void> ok = Result<void>::Success();
      if (auto connected = client->Connect(); !connected) {
        ok = Result<void>::Failure(connected.error());
      } else if (auto opened = client->OpenBucket(); !opened) {
        ok = Result<void>::Failure(opened.error());
      }
      lock.lock();
      connecting_ = false;
      if (!ok) {
        cv_.notify_all();
        return ok;
      }
      idle_.push_back(client.get());
      clients_.push_back(std::move(client));
      cv_.notify_all();
    }
    return Result<void>::Success();
  }

  [[nodiscard]] Result<Lease> Acquire() {
    if (auto ready = EnsureReady(); !ready) {
      return Result<Lease>::Failure(ready.error());
    }
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return !idle_.empty(); });
    CouchbaseClient* client = idle_.back();
    idle_.pop_back();
    return Result<Lease>::Success(Lease(this, client));
  }

 private:
  void Release(CouchbaseClient* client) {
    {
      std::lock_guard lock(mu_);
      idle_.push_back(client);
    }
    cv_.notify_one();
  }

  CouchbaseConfig config_;
  int capacity_{16};
  std::mutex mu_;
  std::condition_variable cv_;
  bool connecting_{false};
  std::vector<std::unique_ptr<CouchbaseClient>> clients_;
  std::vector<CouchbaseClient*> idle_;
};

}  // namespace reservations::platform

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include "common/env.hpp"
#include "common/result.hpp"
#include "platform/couchbase_client.hpp"

namespace reservations::platform {

/// Persistent worker threads, each owning a dedicated CouchbaseClient for life.
/// Tasks are individual (client, index) units so concurrent ParallelFor calls multiplex.
class KvWorkerPool {
 public:
  using TaskFn = std::function<Result<void>(CouchbaseClient& client, std::size_t index)>;

  explicit KvWorkerPool(CouchbaseConfig config, int workers = 16)
      : config_(std::move(config)), workers_(std::max(1, workers)) {}

  KvWorkerPool(const KvWorkerPool&) = delete;
  KvWorkerPool& operator=(const KvWorkerPool&) = delete;

  ~KvWorkerPool() { Stop(); }

  [[nodiscard]] Result<void> EnsureStarted() {
    std::lock_guard lock(mu_);
    if (started_) {
      return start_error_.has_value() ? Result<void>::Failure(*start_error_) : Result<void>::Success();
    }
    started_ = true;
    threads_.reserve(static_cast<std::size_t>(workers_));
    for (int i = 0; i < workers_; ++i) {
      threads_.emplace_back([this] { WorkerLoop(); });
    }
    // Wait until every worker has connected (or one failed).
    std::unique_lock ready_lock(ready_mu_);
    ready_cv_.wait(ready_lock, [&] { return ready_count_ >= workers_ || start_error_.has_value(); });
    if (start_error_.has_value()) {
      return Result<void>::Failure(*start_error_);
    }
    return Result<void>::Success();
  }

  /// Run fn(client, i) for i in [0, count). Returns first error, if any.
  [[nodiscard]] Result<void> ParallelFor(std::size_t count, TaskFn fn) {
    if (count == 0) {
      return Result<void>::Success();
    }
    if (auto ready = EnsureStarted(); !ready) {
      return ready;
    }

    auto batch = std::make_shared<Batch>();
    batch->fn = std::move(fn);
    batch->count = count;
    batch->remaining = count;

    {
      std::lock_guard lock(mu_);
      for (std::size_t i = 0; i < count; ++i) {
        queue_.push(WorkItem{.batch = batch, .index = i});
      }
    }
    cv_.notify_all();

    std::unique_lock done_lock(batch->done_mu);
    batch->done_cv.wait(done_lock, [&] { return batch->remaining.load(std::memory_order_acquire) == 0; });
    if (batch->failed.load(std::memory_order_relaxed)) {
      return Result<void>::Failure(batch->first_error);
    }
    return Result<void>::Success();
  }

  [[nodiscard]] int workers() const noexcept { return workers_; }

 private:
  struct Batch {
    TaskFn fn;
    std::size_t count{0};
    std::atomic<std::size_t> remaining{0};
    std::atomic<bool> failed{false};
    std::mutex err_mu;
    Error first_error = Error::Make(ErrorCode::InternalError, "kv worker failed");
    std::mutex done_mu;
    std::condition_variable done_cv;
  };

  struct WorkItem {
    std::shared_ptr<Batch> batch;
    std::size_t index{0};
  };

  void Stop() {
    {
      std::lock_guard lock(mu_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& th : threads_) {
      if (th.joinable()) {
        th.join();
      }
    }
    threads_.clear();
  }

  void WorkerLoop() {
    CouchbaseClient client(config_);
    if (auto connected = client.Connect(); !connected) {
      MarkStartFailure(connected.error());
      return;
    }
    if (auto opened = client.OpenBucket(); !opened) {
      MarkStartFailure(opened.error());
      return;
    }
    {
      std::lock_guard lock(ready_mu_);
      ++ready_count_;
    }
    ready_cv_.notify_all();

    while (true) {
      WorkItem item;
      {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) {
          return;
        }
        item = std::move(queue_.front());
        queue_.pop();
      }

      auto& batch = *item.batch;
      if (!batch.failed.load(std::memory_order_relaxed)) {
        auto result = batch.fn(client, item.index);
        if (!result) {
          if (!batch.failed.exchange(true)) {
            std::lock_guard err_lock(batch.err_mu);
            batch.first_error = result.error();
          }
        }
      }
      if (batch.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        batch.done_cv.notify_one();
      }
    }
  }

  void MarkStartFailure(Error err) {
    {
      std::lock_guard lock(ready_mu_);
      if (!start_error_.has_value()) {
        start_error_ = std::move(err);
      }
      ++ready_count_;
    }
    ready_cv_.notify_all();
  }

  CouchbaseConfig config_;
  int workers_{16};
  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<WorkItem> queue_;
  std::vector<std::thread> threads_;
  bool started_{false};
  bool stopping_{false};

  std::mutex ready_mu_;
  std::condition_variable ready_cv_;
  int ready_count_{0};
  std::optional<Error> start_error_;
};

}  // namespace reservations::platform

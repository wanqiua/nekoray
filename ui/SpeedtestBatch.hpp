#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

// A fixed set of workers consumes a shared queue. The coordinator joins all
// workers, including failed/cancelled workers, before reporting completion.
class SpeedtestBatch {
public:
    explicit SpeedtestBatch(int count, int concurrency)
        : count_(count), workers_(std::min(count, std::clamp(concurrency, 1, 64))) {}

    void cancel() { cancelled_.store(true); }
    bool cancelled() const { return cancelled_.load(); }
    const std::atomic_bool *cancellation() const { return &cancelled_; }
    bool retrying() const { return retrying_; }

    // Workers may nominate a failed item once. The second pass starts only
    // after every first-pass worker has joined, and uses a single worker.
    bool retryLater(int index) {
        if (retrying_ || cancelled()) return false;
        std::lock_guard<std::mutex> lock(retryMutex_);
        retries_.push_back(index);
        return true;
    }

    bool next(int &index) {
        if (cancelled()) return false;
        index = next_.fetch_add(1);
        if (retrying_) {
            if (index >= int(retries_.size())) return false;
            index = retries_[index];
            return !cancelled();
        }
        return index < count_ && !cancelled();
    }

    template<typename Worker>
    void run(Worker worker) {
        runWorkers(worker, workers_);
        if (!cancelled() && !retries_.empty()) {
            retrying_ = true;
            next_.store(0);
            runWorkers(worker, 1);
        }
    }

private:
    template<typename Worker>
    void runWorkers(Worker worker, int count) {
        std::vector<std::thread> threads;
        std::mutex errorMutex;
        std::exception_ptr error;
        try {
            for (int n = 0; n < count; ++n) {
                threads.emplace_back([&, worker]() {
                    try {
                        worker();
                    } catch (...) {
                        cancel();
                        std::lock_guard<std::mutex> lock(errorMutex);
                        if (!error) error = std::current_exception();
                    }
                });
            }
        } catch (...) {
            cancel();
            for (auto &thread: threads) thread.join();
            throw;
        }
        for (auto &thread: threads) thread.join();
        if (error) std::rethrow_exception(error);
    }

private:
    int count_;
    int workers_;
    std::atomic<int> next_{0};
    std::atomic_bool cancelled_{false};
    std::atomic_bool retrying_{false}; // Coordinator changes it only between joins.
    std::mutex retryMutex_;
    std::vector<int> retries_;
};

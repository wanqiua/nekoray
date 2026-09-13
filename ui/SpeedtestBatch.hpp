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

    bool next(int &index) {
        if (cancelled()) return false;
        index = next_.fetch_add(1);
        return index < count_ && !cancelled();
    }

    template<typename Worker>
    void run(Worker worker) {
        std::vector<std::thread> threads;
        std::mutex errorMutex;
        std::exception_ptr error;
        try {
            for (int n = 0; n < workers_; ++n) {
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
};

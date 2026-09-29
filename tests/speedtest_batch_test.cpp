#include "../ui/SpeedtestBatch.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

long runTimedBatch(int concurrency) {
    SpeedtestBatch batch(24, concurrency);
    std::atomic<int> completed{0}, active{0}, peak{0};
    std::vector<std::atomic<int>> visits(24);
    for (auto &visit: visits) visit.store(0);
    auto start = std::chrono::steady_clock::now();
    batch.run([&] {
        int index;
        while (batch.next(index)) {
            visits.at(index).fetch_add(1);
            int n = active.fetch_add(1) + 1;
            int previous = peak.load();
            while (previous < n && !peak.compare_exchange_weak(previous, n)) {}
            std::this_thread::sleep_for(80ms);
            active.fetch_sub(1);
            completed.fetch_add(1);
        }
    });
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    require(completed == 24, "batch did not complete all tasks");
    for (const auto &visit: visits) require(visit == 1, "task was skipped or duplicated");
    require(peak <= concurrency, "concurrency limit was exceeded");
    if (concurrency > 1) require(peak > 1, "batch ran serially");
    std::cout << "24 tasks, concurrency " << concurrency << ": " << elapsed << " ms, peak " << peak << '\n';
    return static_cast<long>(elapsed);
}

int main() {
    try {
        auto serial = runTimedBatch(1);
        auto parallel = runTimedBatch(8);
        require(parallel * 3 < serial, "parallel queue is unexpectedly slow");

        SpeedtestBatch cancelled(100, 4);
        std::atomic<int> completed{0};
        cancelled.run([&] {
            int index;
            while (cancelled.next(index)) {
                if (++completed == 4) cancelled.cancel();
                std::this_thread::sleep_for(10ms);
            }
        });
        require(completed < 100, "cancelled batch kept consuming work");

        SpeedtestBatch failed(100, 4);
        bool caught = false;
        try {
            failed.run([] { throw std::runtime_error("worker failure"); });
        } catch (const std::runtime_error &) {
            caught = true;
        }
        require(caught && failed.cancelled(), "worker failure did not complete the batch");

        SpeedtestBatch zero(3, 0);
        int calls = 0;
        zero.run([&] { int index; while (zero.next(index)) ++calls; });
        require(calls == 3, "zero concurrency must still make progress");
        SpeedtestBatch retries(12, 4);
        std::atomic<int> firstDone{0}, retryDone{0}, retryActive{0};
        retries.run([&] {
            int index;
            while (retries.next(index)) {
                if (!retries.retrying()) {
                    if (index % 2 == 0) require(retries.retryLater(index), "retry not queued");
                    std::this_thread::sleep_for(5ms);
                    ++firstDone;
                } else {
                    require(firstDone == 12, "retry overlapped first pass");
                    require(index % 2 == 0, "retried a healthy item");
                    require(++retryActive == 1, "retry pass was not serial");
                    require(!retries.retryLater(index), "retry loop was allowed");
                    ++retryDone;
                    --retryActive;
                }
            }
        });
        require(retryDone == 6, "missing retry results");
        SpeedtestBatch cancelRetry(8, 4);
        int retried = 0;
        cancelRetry.run([&] {
            int index;
            while (cancelRetry.next(index)) {
                if (!cancelRetry.retrying()) cancelRetry.retryLater(index);
                else { ++retried; cancelRetry.cancel(); }
            }
        });
        require(retried == 1, "cancellation kept dispatching retries");
        std::cout << "Cancellation, failure cleanup and invalid concurrency: passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

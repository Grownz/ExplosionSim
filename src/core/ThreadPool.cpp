#include "core/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <memory>

namespace ex::core {

namespace {

struct CompletionState {
    std::atomic<int> remaining{0};
    std::mutex mutex;
    std::condition_variable condition;

    void FinishOne(std::function<void()> onComplete) {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (onComplete) {
                onComplete();
            }
            std::lock_guard<std::mutex> lock(mutex);
            condition.notify_all();
        }
    }
};

} // namespace

ThreadPool::ThreadPool(unsigned threadCount) {
    if (threadCount == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        threadCount = hardware > 1 ? hardware - 1 : 1;
    }
    threadCount = std::clamp(threadCount, 1u, 64u);

    workers_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::Submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    condition_.notify_one();
}

bool ThreadPool::TryPop(std::function<void()>& task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tasks_.empty()) {
        return false;
    }
    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
}

void ThreadPool::ExecutePending() {
    std::function<void()> task;
    while (TryPop(task)) {
        task();
    }
}

void ThreadPool::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

void ThreadPool::ParallelFor(int count, int chunkSize, const std::function<void(int, int)>& fn) {
    if (count <= 0) {
        return;
    }
    chunkSize = std::max(1, chunkSize);
    const int chunks = (count + chunkSize - 1) / chunkSize;

    auto state = std::make_shared<CompletionState>();
    state->remaining.store(chunks, std::memory_order_release);

    for (int chunk = 0; chunk < chunks; ++chunk) {
        const int begin = chunk * chunkSize;
        const int end = std::min(count, begin + chunkSize);
        Submit([state, &fn, begin, end] {
            fn(begin, end);
            state->FinishOne({});
        });
    }

    while (state->remaining.load(std::memory_order_acquire) > 0) {
        std::function<void()> task;
        if (TryPop(task)) {
            task();
        } else {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait_for(lock, std::chrono::milliseconds(1));
        }
    }
}

void ThreadPool::ParallelForAsync(int count, int chunkSize, std::function<void(int, int)> fn,
                                  std::function<void()> onComplete) {
    if (count <= 0) {
        if (onComplete) {
            onComplete();
        }
        return;
    }
    chunkSize = std::max(1, chunkSize);
    const int chunks = (count + chunkSize - 1) / chunkSize;

    auto state = std::make_shared<CompletionState>();
    state->remaining.store(chunks, std::memory_order_release);

    for (int chunk = 0; chunk < chunks; ++chunk) {
        const int begin = chunk * chunkSize;
        const int end = std::min(count, begin + chunkSize);
        Submit([state, fn, begin, end, onComplete] {
            fn(begin, end);
            state->FinishOne(onComplete);
        });
    }
}

std::size_t ThreadPool::PendingTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

} // namespace ex::core

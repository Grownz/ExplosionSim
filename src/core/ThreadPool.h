#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ex::core {

// A small work-stealing-friendly thread pool. The calling thread always helps
// executing tasks while it waits, so a ParallelFor can never deadlock even if
// every worker is busy with another job.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threadCount = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned WorkerCount() const { return unsigned(workers_.size()); }

    void Submit(std::function<void()> task);

    // Splits [0, count) into chunks of `chunkSize` and blocks until every chunk
    // has been processed.
    void ParallelFor(int count, int chunkSize, const std::function<void(int, int)>& fn);

    // Same as ParallelFor, but returns immediately. `onComplete` is invoked on
    // a worker thread once every chunk is finished.
    void ParallelForAsync(int count, int chunkSize, std::function<void(int, int)> fn,
                          std::function<void()> onComplete);

    std::size_t PendingTasks() const;

private:
    void WorkerLoop();
    bool TryPop(std::function<void()>& task);
    void ExecutePending();

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
};

} // namespace ex::core

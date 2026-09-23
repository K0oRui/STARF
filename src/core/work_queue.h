#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Serial I/O worker. Capacity includes the running job; callers never wait for space.
// Owners must stop before unloading the DLL, outside the loader lock.
class WorkQueue {
public:
    ~WorkQueue() { stop(); }
    bool submit(std::function<void()> work, size_t bytes = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || jobs_ >= 16 || bytes > limit_ - bytes_) return false;
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        queue_.push_back({std::move(work), bytes});
        ++jobs_; bytes_ += bytes;
        cv_.notify_one();
        return true;
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
private:
    struct Job { std::function<void()> work; size_t bytes; };
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return;
                job = std::move(queue_.front()); queue_.pop_front();
            }
            job.work();
            std::lock_guard<std::mutex> lock(mutex_);
            --jobs_; bytes_ -= job.bytes;
        }
    }
    static constexpr size_t limit_ = 128u * 1024u * 1024u;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::thread thread_;
    size_t jobs_ = 0, bytes_ = 0;
    bool stopping_ = false;
};

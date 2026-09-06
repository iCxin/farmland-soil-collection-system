// =============================================================================
// data_queue.h - 模块间线程安全数据队列
// 模块间线程安全消息队列：以条件变量实现阻塞/超时弹出，
// 避免共享资源的直接竞争。
// =============================================================================
#pragma once

#include "common.h"

#include <condition_variable>
#include <deque>

namespace soil {

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t capacity = 512) : capacity_(capacity) {}

    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= capacity_) {
                queue_.pop_front();  // 背压：丢弃最旧数据（实时流场景）
                dropped_++;
            }
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // 阻塞弹出；shutdown 后返回 false
    bool pop(T& item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || shutdown_; })) {
            return false;
        }
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // 非阻塞弹出
    bool tryPop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    uint64_t droppedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    bool isShutdown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_;
    }

private:
    size_t capacity_;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_ = false;
    uint64_t dropped_ = 0;
};

}  // namespace soil

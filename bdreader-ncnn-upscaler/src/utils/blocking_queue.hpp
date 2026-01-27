#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <stdexcept>

/// Thread-safe bounded blocking queue (ring buffer) for Producer-Consumer pattern
///
/// Features:
/// - Fixed capacity (backpressure via blocking push)
/// - Thread-safe push/pop with mutex + condition variables
/// - Graceful shutdown via close()
/// - O(1) push/pop operations
///
/// Reference: https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem#Using_monitors
template<typename T>
class BoundedBlockingQueue {
private:
    std::queue<T> queue_;
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_not_full_;   // Signaled when space available
    std::condition_variable cv_not_empty_;  // Signaled when item available
    bool closed_ = false;

public:
    /// Constructor
    /// @param capacity Maximum number of items in queue (e.g., 4)
    explicit BoundedBlockingQueue(size_t capacity)
        : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("BoundedBlockingQueue capacity must be > 0");
        }
    }

    /// Push item to queue (Producer)
    /// Blocks if queue is full until space becomes available
    /// Throws if queue is closed
    /// @param item Item to push (moved)
    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait until queue not full or closed
        cv_not_full_.wait(lock, [this]() {
            return queue_.size() < capacity_ || closed_;
        });

        if (closed_) {
            throw std::runtime_error("Cannot push to closed queue");
        }

        queue_.push(std::move(item));

        // Notify consumer that item available
        cv_not_empty_.notify_one();
    }

    /// Pop item from queue (Consumer)
    /// Blocks if queue is empty until item becomes available
    /// Returns std::nullopt if queue closed and empty
    /// @param item Output parameter for popped item
    /// @return true if item popped, false if queue closed and empty
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait until queue not empty or closed
        cv_not_empty_.wait(lock, [this]() {
            return !queue_.empty() || closed_;
        });

        // If closed and empty, no more items coming
        if (closed_ && queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();

        // Notify producer that space available
        cv_not_full_.notify_one();

        return true;
    }

    /// Try to pop without blocking (non-blocking variant)
    /// @param item Output parameter
    /// @return true if popped, false if empty
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();
        cv_not_full_.notify_one();

        return true;
    }

    /// Close queue (signal no more items coming)
    /// Wakes up all waiting consumers/producers for graceful shutdown
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_not_empty_.notify_all();  // Wake consumers
        cv_not_full_.notify_all();   // Wake producers
    }

    /// Get current queue size (thread-safe)
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /// Check if queue is closed
    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /// Get capacity
    size_t capacity() const {
        return capacity_;
    }

    /// Check if queue is full (thread-safe)
    bool is_full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size() >= capacity_;
    }

    /// Check if queue is empty (thread-safe)
    bool is_empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
};

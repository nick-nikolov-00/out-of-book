#pragma once

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class SPMCQueue {
    std::queue<T> q;
    std::mutex m;
    std::condition_variable cv_not_empty;
    std::condition_variable cv_not_full;
    const size_t maxSize;
    bool finished = false;
public:
    SPMCQueue(size_t maxSize_) : maxSize(maxSize_) {}

    void push(T item) {
        std::unique_lock<std::mutex> lock(m);
        cv_not_full.wait(lock, [&]{ return q.size() < maxSize; }); // wait if full
        q.push(std::move(item));
        cv_not_empty.notify_one();
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(m);
        cv_not_empty.wait(lock, [&]{ return !q.empty() || finished; });
        if (q.empty()) return false;
        item = std::move(q.front());
        q.pop();
        cv_not_full.notify_one();  // notify producer
        return true;
    }

    void setFinished() {
        std::unique_lock<std::mutex> lock(m);
        finished = true;
        cv_not_empty.notify_all();
        cv_not_full.notify_all();
    }
};
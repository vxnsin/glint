#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/fd.hpp"

namespace glint {

class Loop;

// A few threads for decoding and loading. work() runs on a worker, then done() runs on the
// main thread, woken through an eventfd in the main loop. Idle workers sleep on a condition
// variable; nothing polls.
class Workers {
public:
    Workers(Loop& loop, unsigned count);
    ~Workers();
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    void post(std::function<void()> work, std::function<void()> done);
    // Queue a continuation for a later event-loop dispatch, without occupying a worker.
    void post_main(std::function<void()> done);

private:
    struct Job {
        std::function<void()> work;
        std::function<void()> done;
    };
    void run();
    void drain_done();

    Loop& loop_;
    UniqueFd event_;
    int loop_id_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::deque<std::function<void()>> finished_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

}  // namespace glint

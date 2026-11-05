#include "core/worker.hpp"

#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>

#include "core/log.hpp"
#include "core/loop.hpp"

namespace glint {

Workers::Workers(Loop& loop, unsigned count)
    : loop_(loop), event_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    loop_id_ = loop_.add(event_.get(), POLLIN, [this](short) { drain_done(); });
    for (unsigned i = 0; i < count; ++i) {
        threads_.emplace_back([this] { run(); });
        pthread_setname_np(threads_.back().native_handle(), "glint-worker");
    }
}

Workers::~Workers() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    for (auto& thread : threads_) thread.join();
    loop_.remove(loop_id_);
}

void Workers::post(std::function<void()> work, std::function<void()> done) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({std::move(work), std::move(done)});
    }
    wake_.notify_one();
}

void Workers::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job.work();
        } catch (const std::exception& e) {
            // A loader that throws must not take the daemon down; its done() sees no result.
            log::error("worker: {}", e.what());
        } catch (...) {
            log::error("worker: unknown exception");
        }
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            finished_.push_back(std::move(job.done));
        }
        std::uint64_t one = 1;
        [[maybe_unused]] auto n = ::write(event_.get(), &one, sizeof(one));
    }
}

void Workers::drain_done() {
    std::uint64_t count = 0;
    [[maybe_unused]] auto n = ::read(event_.get(), &count, sizeof(count));
    std::vector<std::function<void()>> done;
    {
        std::lock_guard lock(mutex_);
        done.swap(finished_);
    }
    for (auto& callback : done) {
        if (callback) callback();
    }
}

}  // namespace glint

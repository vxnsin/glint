#include "core/loop.hpp"

#include <errno.h>

#include <algorithm>

namespace glint {

int Loop::add(int fd, short events, Callback callback) {
    int id = next_id_++;
    entries_.push_back({id, fd, events, std::move(callback)});
    return id;
}

void Loop::modify(int id, short events) {
    for (auto& entry : entries_) {
        if (entry.id == id) entry.events = events;
    }
}

void Loop::remove(int id) {
    // Marked, not erased: remove() may run from inside a callback of the same dispatch.
    for (auto& entry : entries_) {
        if (entry.id == id) entry.removed = true;
    }
}

short Loop::revents(int id) const {
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        if (ids_[i] == id) return fds_[i].revents;
    }
    return 0;
}

bool Loop::run() {
    running_ = true;
    auto& fds = fds_;
    auto& ids = ids_;
    while (running_) {
        std::erase_if(entries_, [](const Entry& e) { return e.removed; });
        if (before_poll_) before_poll_();
        if (!running_) break;

        fds.clear();
        ids.clear();
        for (const auto& entry : entries_) {
            fds.push_back({entry.fd, entry.events, 0});
            ids.push_back(entry.id);
        }
        int ready = ::poll(fds.data(), fds.size(), -1);
        ++wakeups_;
        if (ready < 0) {
            int error = errno;
            for (auto& fd : fds) fd.revents = 0;
            if (after_poll_) after_poll_();
            if (error == EINTR) continue;
            return false;
        }
        if (after_poll_) after_poll_();
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].revents == 0) continue;
            auto it =
                std::ranges::find_if(entries_, [&](const Entry& e) { return e.id == ids[i]; });
            if (it == entries_.end() || it->removed) continue;
            // Copy: the callback may add entries and invalidate the iterator.
            Callback callback = it->callback;
            callback(fds[i].revents);
        }
    }
    return true;
}

}  // namespace glint

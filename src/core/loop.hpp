#pragma once

#include <poll.h>

#include <cstdint>
#include <functional>
#include <vector>

// The daemon's only wait: poll() on a handful of file descriptors, no timeout. Nothing in
// glint wakes up on a clock; every wakeup is an fd becoming ready.
namespace glint {

class Loop {
public:
    using Callback = std::function<void(short revents)>;

    // events is POLLIN and/or POLLOUT. Returns an id for modify() and remove().
    int add(int fd, short events, Callback callback);
    void modify(int id, short events);
    void remove(int id);

    // Called before every poll, e.g. to prepare the Wayland read and flush.
    void set_before_poll(std::function<void()> hook) { before_poll_ = std::move(hook); }
    // Called right after every poll, before any callback. libwayland needs this: a prepared
    // read must be completed or cancelled before anything else touches the display.
    void set_after_poll(std::function<void()> hook) { after_poll_ = std::move(hook); }
    // During the after-poll hook: what poll reported for an entry.
    short revents(int id) const;

    // Runs until quit(). Returns false if poll failed.
    bool run();
    void quit() { running_ = false; }

    std::uint64_t wakeups() const { return wakeups_; }

private:
    struct Entry {
        int id;
        int fd;
        short events;
        Callback callback;
        bool removed = false;
    };

    std::vector<Entry> entries_;
    std::vector<pollfd> fds_;
    std::vector<int> ids_;
    std::function<void()> before_poll_;
    std::function<void()> after_poll_;
    int next_id_ = 1;
    bool running_ = false;
    std::uint64_t wakeups_ = 0;
};

}  // namespace glint

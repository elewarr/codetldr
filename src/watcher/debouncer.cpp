#include "watcher/debouncer.h"
#include <algorithm>

namespace codetldr {

Debouncer::Debouncer(std::chrono::milliseconds window)
    : window_(window) {}

void Debouncer::touch(const std::string& path) {
    pending_[path] = Clock::now();
}

std::vector<std::string> Debouncer::flush_ready() {
    std::vector<std::string> ready;
    auto now = Clock::now();

    for (auto it = pending_.begin(); it != pending_.end(); ) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
        if (elapsed >= window_) {
            ready.push_back(it->first);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return ready;
}

int Debouncer::next_timeout_ms() const {
    if (pending_.empty()) {
        return -1;
    }

    auto now = Clock::now();
    int min_remaining = std::numeric_limits<int>::max();

    for (const auto& [path, last_touch] : pending_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_touch);
        int remaining = static_cast<int>((window_ - elapsed).count());
        if (remaining < 0) remaining = 0;
        min_remaining = std::min(min_remaining, remaining);
    }

    return min_remaining;
}

} // namespace codetldr

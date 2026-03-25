#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace codetldr {

// Timer-based deduplication: path -> last_event_time map with quiet-window flush.
// All methods are single-threaded (called only from coordinator thread).
// Uses std::chrono::steady_clock for timing.
class Debouncer {
public:
    // window: quiet period before a file is considered ready for processing (default 2000ms)
    explicit Debouncer(std::chrono::milliseconds window = std::chrono::milliseconds(2000));

    // Update the last-event time for path. Creates entry if not present.
    void touch(const std::string& path);

    // Return all paths that have been quiet for >= window_, and erase them from the map.
    std::vector<std::string> flush_ready();

    // Return milliseconds until the earliest pending path is ready.
    // Returns -1 if no paths are pending.
    int next_timeout_ms() const;

    // Return true if any paths are pending.
    bool has_pending() const { return !pending_.empty(); }

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    std::chrono::milliseconds window_;
    std::unordered_map<std::string, TimePoint> pending_;  // path -> last touch time
};

} // namespace codetldr

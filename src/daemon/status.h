#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <sys/types.h>

namespace codetldr {

enum class DaemonState {
    kStarting,
    kInitialScan,
    kIndexing,
    kIdle,
    kStopping,
    kStopped,
    kError
};

std::string to_string(DaemonState state);
DaemonState daemon_state_from_string(const std::string& s);

struct DaemonStatus {
    DaemonState state = DaemonState::kStarting;
    pid_t pid = 0;
    std::string socket_path;
    int files_indexed = 0;
    int files_total = 0;
    std::string last_indexed_at;
    int uptime_seconds = 0;
};

class StatusWriter {
public:
    explicit StatusWriter(std::filesystem::path status_file_path)
        : path_(std::move(status_file_path)) {}

    // Write DaemonStatus to JSON file
    void write(const DaemonStatus& status) const;

    // Read DaemonStatus from JSON file
    static DaemonStatus read(const std::filesystem::path& path);

private:
    std::filesystem::path path_;
};

} // namespace codetldr

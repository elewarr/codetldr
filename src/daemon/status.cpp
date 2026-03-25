#include "daemon/status.h"
#include <fstream>
#include <stdexcept>

namespace codetldr {

std::string to_string(DaemonState state) {
    switch (state) {
        case DaemonState::kStarting:     return "starting";
        case DaemonState::kInitialScan:  return "initial_scan";
        case DaemonState::kIndexing:     return "indexing";
        case DaemonState::kIdle:         return "idle";
        case DaemonState::kStopping:     return "stopping";
        case DaemonState::kStopped:      return "stopped";
        case DaemonState::kError:        return "error";
    }
    return "unknown";
}

DaemonState daemon_state_from_string(const std::string& s) {
    if (s == "starting")      return DaemonState::kStarting;
    if (s == "initial_scan")  return DaemonState::kInitialScan;
    if (s == "indexing")      return DaemonState::kIndexing;
    if (s == "idle")          return DaemonState::kIdle;
    if (s == "stopping")      return DaemonState::kStopping;
    if (s == "stopped")       return DaemonState::kStopped;
    if (s == "error")         return DaemonState::kError;
    return DaemonState::kError;
}

void StatusWriter::write(const DaemonStatus& status) const {
    nlohmann::json j;
    j["state"]            = to_string(status.state);
    j["pid"]              = status.pid;
    j["socket_path"]      = status.socket_path;
    j["files_indexed"]    = status.files_indexed;
    j["files_total"]      = status.files_total;
    j["last_indexed_at"]  = status.last_indexed_at;
    j["uptime_seconds"]   = status.uptime_seconds;

    std::ofstream ofs(path_);
    if (!ofs) {
        throw std::runtime_error("StatusWriter: cannot open " + path_.string());
    }
    ofs << j.dump(2) << "\n";
}

DaemonStatus StatusWriter::read(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("StatusWriter::read: cannot open " + path.string());
    }
    nlohmann::json j = nlohmann::json::parse(ifs);

    DaemonStatus s;
    s.state           = daemon_state_from_string(j.value("state", "error"));
    s.pid             = j.value("pid", 0);
    s.socket_path     = j.value("socket_path", "");
    s.files_indexed   = j.value("files_indexed", 0);
    s.files_total     = j.value("files_total", 0);
    s.last_indexed_at = j.value("last_indexed_at", "");
    s.uptime_seconds  = j.value("uptime_seconds", 0);
    return s;
}

} // namespace codetldr

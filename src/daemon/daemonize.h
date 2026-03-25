#pragma once
#include <filesystem>
#include <optional>
#include <sys/types.h>

namespace codetldr {

// Double-fork daemon creation pattern.
// Fork 1: parent exits, detaches from terminal.
// setsid(): create new session, lose controlling terminal.
// Fork 2: session leader exits, preventing re-acquisition of terminal.
// Redirects stdin/stdout/stderr to /dev/null.
// Writes child PID to pidfile_path.
// Sets umask(0).
// Throws std::runtime_error on failure.
void daemonize(const std::filesystem::path& pidfile_path);

// Check if a process with the given PID is alive.
// Uses kill(pid, 0): returns true if signal can be sent (process exists).
bool is_process_alive(pid_t pid);

// Read PID from a pidfile. Returns nullopt if file doesn't exist or is invalid.
std::optional<pid_t> read_pidfile(const std::filesystem::path& path);

} // namespace codetldr

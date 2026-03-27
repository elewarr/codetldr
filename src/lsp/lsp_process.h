#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

namespace codetldr {

// LspProcess: posix_spawn wrapper that manages a child process with
// stdin/stdout/stderr pipes. Designed for spawning LSP servers in daemon context.
//
// stdout_fd and stderr_fd are set O_NONBLOCK for use with poll().
// stdin_fd is left blocking (writes are synchronous).
class LspProcess {
public:
    LspProcess() = default;
    ~LspProcess();

    // Non-copyable
    LspProcess(const LspProcess&) = delete;
    LspProcess& operator=(const LspProcess&) = delete;

    // Movable
    LspProcess(LspProcess&&) noexcept;
    LspProcess& operator=(LspProcess&&) noexcept;

    // Spawn child process. Returns 0 on success, posix_spawn errno on failure.
    // command: absolute path to binary (e.g. "/usr/bin/clangd")
    // args: command-line arguments (command is prepended automatically)
    int spawn(const std::string& command, const std::vector<std::string>& args);

    // Pipe fds for parent.
    // stdout_fd and stderr_fd are O_NONBLOCK.
    // Return -1 if not spawned or after wait().
    int stdin_fd() const { return stdin_fd_; }   // parent writes to this
    int stdout_fd() const { return stdout_fd_; } // parent reads from this (O_NONBLOCK)
    int stderr_fd() const { return stderr_fd_; } // parent reads from this (O_NONBLOCK)

    pid_t pid() const { return pid_; }           // child PID, -1 if not spawned
    bool is_running() const;                     // true if spawned and not waited

    // Send SIGTERM to child
    void kill();

    // Wait for child to exit. Returns exit status (from waitpid WEXITSTATUS).
    // Closes all pipe fds after wait.
    int wait();

private:
    void close_fds();

    pid_t pid_ = -1;
    int stdin_fd_ = -1;   // write end of stdin pipe (parent side)
    int stdout_fd_ = -1;  // read end of stdout pipe (parent side, O_NONBLOCK)
    int stderr_fd_ = -1;  // read end of stderr pipe (parent side, O_NONBLOCK)
    bool waited_ = false;
};

} // namespace codetldr

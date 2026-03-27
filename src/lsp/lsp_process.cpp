#include "lsp/lsp_process.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>

extern char** environ;

namespace codetldr {

LspProcess::~LspProcess() {
    if (pid_ > 0 && !waited_) {
        kill();
        wait();
    }
}

LspProcess::LspProcess(LspProcess&& other) noexcept
    : pid_(other.pid_)
    , stdin_fd_(other.stdin_fd_)
    , stdout_fd_(other.stdout_fd_)
    , stderr_fd_(other.stderr_fd_)
    , waited_(other.waited_)
{
    other.pid_ = -1;
    other.stdin_fd_ = -1;
    other.stdout_fd_ = -1;
    other.stderr_fd_ = -1;
    other.waited_ = true;
}

LspProcess& LspProcess::operator=(LspProcess&& other) noexcept {
    if (this != &other) {
        if (pid_ > 0 && !waited_) {
            kill();
            wait();
        }
        close_fds();

        pid_ = other.pid_;
        stdin_fd_ = other.stdin_fd_;
        stdout_fd_ = other.stdout_fd_;
        stderr_fd_ = other.stderr_fd_;
        waited_ = other.waited_;

        other.pid_ = -1;
        other.stdin_fd_ = -1;
        other.stdout_fd_ = -1;
        other.stderr_fd_ = -1;
        other.waited_ = true;
    }
    return *this;
}

int LspProcess::spawn(const std::string& command, const std::vector<std::string>& args) {
    // Create three pipes: [read, write]
    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];

    if (::pipe(stdin_pipe) != 0)  return errno;
    if (::pipe(stdout_pipe) != 0) { ::close(stdin_pipe[0]); ::close(stdin_pipe[1]); return errno; }
    if (::pipe(stderr_pipe) != 0) {
        ::close(stdin_pipe[0]);  ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        return errno;
    }

    // Set up file actions: redirect child stdin/stdout/stderr to pipe ends
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    // Child stdin: read from stdin_pipe[0]
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0],  STDIN_FILENO);
    // Child stdout: write to stdout_pipe[1]
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    // Child stderr: write to stderr_pipe[1]
    posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);

    // Close the parent-side ends in child (addclose for the ends child doesn't need)
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);   // parent's write end
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);  // parent's read end
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);  // parent's read end

    // Set up spawn attributes
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

#ifdef __APPLE__
    // POSIX_SPAWN_CLOEXEC_DEFAULT: close all fds except those explicitly set up
    // via file actions — prevents fd leaks into child process.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

    // Build argv: [command, arg0, arg1, ..., nullptr]
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    pid_t child_pid = -1;
    int err = ::posix_spawn(&child_pid, command.c_str(), &actions, &attr,
                            argv.data(), environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    // Close child-side pipe ends in parent (regardless of success/failure)
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    if (err != 0) {
        // Close remaining parent-side ends on failure
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        pid_ = -1;
        return err;
    }

    pid_ = child_pid;
    waited_ = false;

    // Parent-side fds
    stdin_fd_ = stdin_pipe[1];   // parent writes here
    stdout_fd_ = stdout_pipe[0]; // parent reads from here
    stderr_fd_ = stderr_pipe[0]; // parent reads from here

    // Set O_NONBLOCK on stdout and stderr read ends
    int flags;
    flags = ::fcntl(stdout_fd_, F_GETFL, 0);
    ::fcntl(stdout_fd_, F_SETFL, flags | O_NONBLOCK);

    flags = ::fcntl(stderr_fd_, F_GETFL, 0);
    ::fcntl(stderr_fd_, F_SETFL, flags | O_NONBLOCK);

    return 0;
}

bool LspProcess::is_running() const {
    return pid_ > 0 && !waited_;
}

void LspProcess::kill() {
    if (pid_ > 0 && !waited_) {
        ::kill(pid_, SIGTERM);
    }
}

int LspProcess::wait() {
    if (pid_ <= 0 || waited_) {
        return -1;
    }
    int status = 0;
    ::waitpid(pid_, &status, 0);
    waited_ = true;
    close_fds();
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void LspProcess::close_fds() {
    if (stdin_fd_ >= 0)  { ::close(stdin_fd_);  stdin_fd_ = -1;  }
    if (stdout_fd_ >= 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
    if (stderr_fd_ >= 0) { ::close(stderr_fd_); stderr_fd_ = -1; }
}

} // namespace codetldr

#include "daemon/daemonize.h"
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace codetldr {

void daemonize(const std::filesystem::path& pidfile_path) {
    // First fork
    pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("daemonize: fork() 1 failed: " +
                                 std::string(strerror(errno)));
    }
    if (pid > 0) {
        // Parent exits
        ::_exit(0);
    }

    // Child 1: become session leader
    if (::setsid() < 0) {
        throw std::runtime_error("daemonize: setsid() failed: " +
                                 std::string(strerror(errno)));
    }

    // Second fork: session leader exits, child can never acquire terminal
    pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("daemonize: fork() 2 failed: " +
                                 std::string(strerror(errno)));
    }
    if (pid > 0) {
        // Session leader exits
        ::_exit(0);
    }

    // Final daemon process
    ::umask(0);

    // Redirect standard file descriptors to /dev/null
    int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        ::dup2(null_fd, STDIN_FILENO);
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) {
            ::close(null_fd);
        }
    }

    // Write PID file
    std::filesystem::create_directories(pidfile_path.parent_path());
    std::ofstream ofs(pidfile_path);
    if (!ofs) {
        // Non-fatal: continue even if we can't write the PID file
        return;
    }
    ofs << static_cast<long>(::getpid()) << "\n";
}

bool is_process_alive(pid_t pid) {
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0;
}

std::optional<pid_t> read_pidfile(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs) return std::nullopt;

    long pid_val = 0;
    ifs >> pid_val;
    if (!ifs || pid_val <= 0) return std::nullopt;
    return static_cast<pid_t>(pid_val);
}

} // namespace codetldr

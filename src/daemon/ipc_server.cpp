#include "daemon/ipc_server.h"
#include "common/logging.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace {

static void check_socket_path_length(const std::filesystem::path& sock_path) {
    struct sockaddr_un addr{};
    const std::string s = sock_path.string();
    if (s.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error(
            "Socket path too long (" + std::to_string(s.size()) +
            " chars, platform limit " +
            std::to_string(sizeof(addr.sun_path) - 1) + "): " + s);
    }
}

} // anonymous namespace

namespace codetldr {

IpcServer::IpcServer(IpcServer&& other) noexcept
    : server_fd_(other.server_fd_), sock_path_(std::move(other.sock_path_)) {
    other.server_fd_ = -1;
}

IpcServer& IpcServer::operator=(IpcServer&& other) noexcept {
    if (this != &other) {
        cleanup();
        server_fd_ = other.server_fd_;
        sock_path_ = std::move(other.sock_path_);
        other.server_fd_ = -1;
    }
    return *this;
}

bool IpcServer::bind_or_die(const std::filesystem::path& sock_path) {
    check_socket_path_length(sock_path);
    // Stale socket detection: if file exists, try to connect.
    // ECONNREFUSED -> stale, remove it. Connection succeeds -> live daemon, return false.
    if (std::filesystem::exists(sock_path)) {
        int probe_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe_fd < 0) {
            throw std::runtime_error("IpcServer: socket() probe failed: " +
                                     std::string(strerror(errno)));
        }
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        ::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

        int rc = ::connect(probe_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        ::close(probe_fd);

        if (rc == 0) {
            // Live daemon is running — refuse to clobber it
            return false;
        }

        if (errno == ECONNREFUSED || errno == ENOENT) {
            // Stale socket: remove it so we can bind
            ::unlink(sock_path.c_str());
        } else {
            // Some other error during probe — still try to remove and bind
            ::unlink(sock_path.c_str());
        }
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("IpcServer: socket() failed: " +
                                 std::string(strerror(errno)));
    }

    // Set CLOEXEC
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    // Set non-blocking for accept
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("IpcServer: bind() failed on " + sock_path.string() +
                                 ": " + strerror(errno));
    }

    if (::listen(fd, 16) < 0) {
        ::close(fd);
        ::unlink(sock_path.c_str());
        throw std::runtime_error("IpcServer: listen() failed: " +
                                 std::string(strerror(errno)));
    }

    server_fd_ = fd;
    sock_path_ = sock_path;
    return true;
}

int IpcServer::accept_client() const {
    if (server_fd_ < 0) return -1;
    struct sockaddr_un addr{};
    socklen_t addrlen = sizeof(addr);
    int client_fd = ::accept(server_fd_,
                              reinterpret_cast<struct sockaddr*>(&addr),
                              &addrlen);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        return -1;
    }
    // Ensure client fd is blocking for recv/send operations
    int flags = ::fcntl(client_fd, F_GETFL, 0);
    if (flags & O_NONBLOCK) {
        ::fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return client_fd;
}

std::string IpcServer::recv_message(int client_fd) const {
    std::string result;
    char c;
    while (true) {
        ssize_t n = ::read(client_fd, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        result += c;
    }
    return result;
}

void IpcServer::send_message(int client_fd, const nlohmann::json& msg) const {
    std::string s = msg.dump() + "\n";
    ssize_t total = 0;
    ssize_t to_send = static_cast<ssize_t>(s.size());
    while (total < to_send) {
        ssize_t n = ::write(client_fd, s.data() + total,
                            static_cast<size_t>(to_send - total));
        if (n <= 0) break;
        total += n;
    }
}

void IpcServer::cleanup() {
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
    if (!sock_path_.empty()) {
        ::unlink(sock_path_.c_str());
        sock_path_.clear();
    }
}

} // namespace codetldr

#include "daemon/daemon_client.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace codetldr {

bool DaemonClient::connect(const std::filesystem::path& sock_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    return true;
}

nlohmann::json DaemonClient::call(const std::string& method,
                                   const nlohmann::json& params) {
    if (fd_ < 0) {
        throw std::runtime_error("DaemonClient: not connected");
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = next_id_++;
    req["method"]  = method;
    if (!params.is_null()) {
        req["params"] = params;
    }

    std::string msg = req.dump() + "\n";
    ssize_t total = 0;
    ssize_t to_send = static_cast<ssize_t>(msg.size());
    while (total < to_send) {
        ssize_t n = ::write(fd_, msg.data() + total,
                            static_cast<size_t>(to_send - total));
        if (n <= 0) {
            throw std::runtime_error("DaemonClient: write failed");
        }
        total += n;
    }

    // Read response until newline
    std::string response_str;
    char c;
    while (true) {
        ssize_t n = ::read(fd_, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        response_str += c;
    }

    if (response_str.empty()) {
        throw std::runtime_error("DaemonClient: empty response");
    }

    return nlohmann::json::parse(response_str);
}

void DaemonClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace codetldr

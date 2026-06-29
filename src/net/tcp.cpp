#include "lan/net/tcp.h"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string port_to_string(std::uint16_t port) {
    return std::to_string(static_cast<unsigned int>(port));
}

std::string errno_message(std::string action) {
    return action + ": " + std::strerror(errno);
}

class AddrInfo {
public:
    explicit AddrInfo(addrinfo* info = nullptr) : info_(info) {}

    ~AddrInfo() {
        if (info_ != nullptr) {
            ::freeaddrinfo(info_);
        }
    }

    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;

    AddrInfo(AddrInfo&& other) noexcept : info_(std::exchange(other.info_, nullptr)) {}

    AddrInfo& operator=(AddrInfo&& other) noexcept {
        if (this != &other) {
            if (info_ != nullptr) {
                ::freeaddrinfo(info_);
            }
            info_ = std::exchange(other.info_, nullptr);
        }
        return *this;
    }

    addrinfo* get() const {
        return info_;
    }

private:
    addrinfo* info_;
};

Result<AddrInfo> resolve_address(const char* host, std::uint16_t port, const addrinfo& hints) {
    addrinfo* raw = nullptr;
    const auto port_text = port_to_string(port);
    const int result = ::getaddrinfo(host, port_text.c_str(), &hints, &raw);
    if (result != 0) {
        return Result<AddrInfo>::failure(
            make_error(ErrorCode::network_error, "getaddrinfo failed: " + std::string(::gai_strerror(result))));
    }

    return Result<AddrInfo>::success(AddrInfo(raw));
}

}  // namespace

Result<FileDescriptor> listen_tcp(std::string_view bind_address, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const auto host = std::string(bind_address);
    auto addresses = resolve_address(host.empty() ? nullptr : host.c_str(), port, hints);
    if (!addresses) {
        return Result<FileDescriptor>::failure(addresses.error());
    }

    for (auto* address = addresses.value().get(); address != nullptr; address = address->ai_next) {
        FileDescriptor listener(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!listener) {
            continue;
        }

        int enabled = 1;
        (void)::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        if (::bind(listener.get(), address->ai_addr, address->ai_addrlen) != 0) {
            continue;
        }

        if (::listen(listener.get(), 16) != 0) {
            continue;
        }

        return Result<FileDescriptor>::success(std::move(listener));
    }

    return Result<FileDescriptor>::failure(
        make_error(ErrorCode::network_error, errno_message("failed to bind/listen TCP socket")));
}

Result<FileDescriptor> accept_tcp(const FileDescriptor& listener) {
    while (true) {
        const int fd = ::accept(listener.get(), nullptr, nullptr);
        if (fd >= 0) {
            return Result<FileDescriptor>::success(FileDescriptor(fd));
        }

        if (errno == EINTR) {
            continue;
        }

        return Result<FileDescriptor>::failure(
            make_error(ErrorCode::network_error, errno_message("failed to accept TCP connection")));
    }
}

Result<FileDescriptor> connect_tcp(std::string_view host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const auto host_text = std::string(host);
    auto addresses = resolve_address(host_text.c_str(), port, hints);
    if (!addresses) {
        return Result<FileDescriptor>::failure(addresses.error());
    }

    for (auto* address = addresses.value().get(); address != nullptr; address = address->ai_next) {
        FileDescriptor socket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!socket) {
            continue;
        }

        if (::connect(socket.get(), address->ai_addr, address->ai_addrlen) == 0) {
            return Result<FileDescriptor>::success(std::move(socket));
        }
    }

    return Result<FileDescriptor>::failure(
        make_error(ErrorCode::network_error, errno_message("failed to connect TCP socket")));
}

Result<bool> send_all(const FileDescriptor& socket, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto result = ::send(socket.get(), data + sent, size - sent, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<bool>::failure(
                make_error(ErrorCode::network_error, errno_message("failed to send TCP data")));
        }

        if (result == 0) {
            return Result<bool>::failure(
                make_error(ErrorCode::network_error, "failed to send TCP data: connection closed"));
        }

        sent += static_cast<std::size_t>(result);
    }

    return Result<bool>::success(true);
}

Result<bool> send_all(const FileDescriptor& socket, std::string_view data) {
    return send_all(socket, data.data(), data.size());
}

Result<bool> recv_exact(const FileDescriptor& socket, char* data, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const auto result = ::recv(socket.get(), data + received, size - received, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<bool>::failure(
                make_error(ErrorCode::network_error, errno_message("failed to receive TCP data")));
        }

        if (result == 0) {
            return Result<bool>::failure(
                make_error(ErrorCode::network_error, "failed to receive TCP data: connection closed"));
        }

        received += static_cast<std::size_t>(result);
    }

    return Result<bool>::success(true);
}

Result<std::string> recv_some(const FileDescriptor& socket, std::size_t max_bytes) {
    if (max_bytes == 0) {
        return Result<std::string>::failure(
            make_error(ErrorCode::invalid_argument, "recv size must be greater than zero"));
    }

    std::vector<char> buffer(max_bytes);
    while (true) {
        const auto result = ::recv(socket.get(), buffer.data(), buffer.size(), 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<std::string>::failure(
                make_error(ErrorCode::network_error, errno_message("failed to receive TCP data")));
        }

        if (result == 0) {
            return Result<std::string>::success("");
        }

        return Result<std::string>::success(
            std::string(buffer.data(), buffer.data() + static_cast<std::size_t>(result)));
    }
}

}  // namespace lan

#include "lan/net/posix_backend.h"

#include <memory>
#include <utility>

#include "lan/net/tcp.h"

namespace lan {

PosixConnection::PosixConnection(FileDescriptor socket) : socket_(std::move(socket)) {}

Result<bool> PosixConnection::send_all(const char* data, std::size_t size) {
    return lan::send_all(socket_, data, size);
}

Result<bool> PosixConnection::recv_exact(char* data, std::size_t size) {
    return lan::recv_exact(socket_, data, size);
}

const FileDescriptor& PosixConnection::socket() const {
    return socket_;
}

PosixListener::PosixListener(FileDescriptor listener) : listener_(std::move(listener)) {}

Result<std::unique_ptr<Connection>> PosixListener::accept() {
    auto client = accept_tcp(listener_);
    if (!client) {
        return Result<std::unique_ptr<Connection>>::failure(client.error());
    }

    return Result<std::unique_ptr<Connection>>::success(
        std::make_unique<PosixConnection>(std::move(client).value()));
}

Result<std::unique_ptr<Listener>> PosixNetworkBackend::listen(std::string_view bind_address,
                                                              std::uint16_t port) {
    auto listener = listen_tcp(bind_address, port);
    if (!listener) {
        return Result<std::unique_ptr<Listener>>::failure(listener.error());
    }

    return Result<std::unique_ptr<Listener>>::success(
        std::make_unique<PosixListener>(std::move(listener).value()));
}

Result<std::unique_ptr<Connection>> PosixNetworkBackend::connect(std::string_view host,
                                                                 std::uint16_t port) {
    auto socket = connect_tcp(host, port);
    if (!socket) {
        return Result<std::unique_ptr<Connection>>::failure(socket.error());
    }

    return Result<std::unique_ptr<Connection>>::success(
        std::make_unique<PosixConnection>(std::move(socket).value()));
}

NetworkBackend& default_network_backend() {
    static PosixNetworkBackend backend;
    return backend;
}

}  // namespace lan

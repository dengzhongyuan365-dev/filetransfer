#pragma once

#include <memory>
#include <mutex>
#include <string_view>

#include "lan/common/result.h"
#include "lan/fs/file_descriptor.h"
#include "lan/net/connection.h"

namespace lan {

class PosixConnection final : public Connection {
public:
    explicit PosixConnection(FileDescriptor socket);

    Result<bool> send_all(const char* data, std::size_t size) override;
    Result<bool> recv_exact(char* data, std::size_t size) override;
    void close() override;

    const FileDescriptor& socket() const;

private:
    std::mutex mutex_;
    FileDescriptor socket_;
};

class PosixListener final : public Listener {
public:
    explicit PosixListener(FileDescriptor listener);

    Result<std::unique_ptr<Connection>> accept() override;
    void close() override;

private:
    std::mutex mutex_;
    FileDescriptor listener_;
};

class PosixNetworkBackend final : public NetworkBackend {
public:
    Result<std::unique_ptr<Listener>> listen(std::string_view bind_address,
                                             std::uint16_t port) override;
    Result<std::unique_ptr<Connection>> connect(std::string_view host,
                                                std::uint16_t port) override;
};

}  // namespace lan

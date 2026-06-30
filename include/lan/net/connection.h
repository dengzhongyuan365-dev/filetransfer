#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "lan/common/result.h"

namespace lan {

class Connection {
public:
    virtual ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    virtual Result<bool> send_all(const char* data, std::size_t size) = 0;
    virtual Result<bool> recv_exact(char* data, std::size_t size) = 0;
    virtual void close() = 0;

protected:
    Connection() = default;
};

class Listener {
public:
    virtual ~Listener() = default;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    virtual Result<std::unique_ptr<Connection>> accept() = 0;
    virtual void close() = 0;

protected:
    Listener() = default;
};

class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;

    NetworkBackend(const NetworkBackend&) = delete;
    NetworkBackend& operator=(const NetworkBackend&) = delete;

    virtual Result<std::unique_ptr<Listener>> listen(std::string_view bind_address,
                                                     std::uint16_t port) = 0;
    virtual Result<std::unique_ptr<Connection>> connect(std::string_view host,
                                                        std::uint16_t port) = 0;

protected:
    NetworkBackend() = default;
};

NetworkBackend& default_network_backend();

}  // namespace lan

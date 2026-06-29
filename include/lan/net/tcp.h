#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "lan/common/result.h"
#include "lan/fs/file_descriptor.h"

namespace lan {

Result<FileDescriptor> listen_tcp(std::string_view bind_address, std::uint16_t port);
Result<FileDescriptor> accept_tcp(const FileDescriptor& listener);
Result<FileDescriptor> connect_tcp(std::string_view host, std::uint16_t port);
Result<bool> send_all(const FileDescriptor& socket, std::string_view data);
Result<std::string> recv_some(const FileDescriptor& socket, std::size_t max_bytes);

}  // namespace lan

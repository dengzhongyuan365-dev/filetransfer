#pragma once

#include <QString>

#include <cstdint>

namespace lan::gui {

constexpr quint16 kDiscoveryPort = 39124;
constexpr std::uint16_t kTransferPort = 39123;
constexpr const char* kProtocol = "lan-file-transfer/1";

struct Peer {
    QString id;
    QString name;
    QString host;
    std::uint16_t port = kTransferPort;
};

}  // namespace lan::gui

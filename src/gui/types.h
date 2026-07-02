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
    QString trust_token;
    std::uint16_t port = kTransferPort;
    bool online = true;
    bool linked = false;
    bool trusted = false;
    qint64 last_seen_ms = 0;
    qint64 last_linked_ms = 0;
    qint64 trusted_at_ms = 0;
};

}  // namespace lan::gui

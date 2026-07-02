#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>

#include "gui/control_message.h"
#include "gui/types.h"

class QObject;
class QUdpSocket;

namespace lan::gui {

struct DiscoveryDatagram {
    QHostAddress sender;
    quint16 sender_port = 0;
    ControlMessage message;
};

struct DiscoveryProbeReport {
    QStringList broadcast_targets;
    QStringList configured_broadcast_targets;
    QStringList configured_ranges;
    QStringList invalid_configured_networks;
    QStringList skipped_host_scan_networks;
    int configured_host_target_count = 0;
};

class DiscoveryController {
public:
    using DatagramHandler = std::function<void(DiscoveryDatagram)>;

    DiscoveryController();
    ~DiscoveryController();

    DiscoveryController(const DiscoveryController&) = delete;
    DiscoveryController& operator=(const DiscoveryController&) = delete;

    bool bind(QObject* context, DatagramHandler handler);
    QString error_string() const;

    DiscoveryProbeReport send_discovery_probe(const QString& node_id,
                                              const QString& name,
                                              const QStringList& configured_networks);
    bool reply_to_discovery(const QHostAddress& target,
                            quint16 port,
                            const QString& node_id,
                            const QString& name);
    bool send_control(const Peer& peer,
                      const QString& type,
                      const QString& node_id,
                      const QString& name,
                      const QJsonObject& fields = {});

private:
    void read_pending(const DatagramHandler& handler);

    std::unique_ptr<QUdpSocket> socket_;
};

}  // namespace lan::gui

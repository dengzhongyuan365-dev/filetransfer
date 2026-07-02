#include "gui/discovery_controller.h"

#include <QAbstractSocket>
#include <QNetworkInterface>
#include <QSet>
#include <QUdpSocket>

namespace lan::gui {
namespace {

QList<QHostAddress> discovery_targets() {
    QList<QHostAddress> targets;
    QSet<QString> seen;
    const auto add_target = [&targets, &seen](const QHostAddress& address) {
        if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
            return;
        }
        const auto key = address.toString();
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        targets.push_back(address);
    };

    add_target(QHostAddress(QHostAddress::Broadcast));
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            !flags.testFlag(QNetworkInterface::CanBroadcast) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                add_target(entry.broadcast());
            }
        }
    }
    return targets;
}

QString ipv4_text(quint32 address) {
    return QHostAddress(address).toString();
}

quint32 prefix_mask(int prefix) {
    if (prefix <= 0) {
        return 0U;
    }
    return 0xffffffffU << (32 - prefix);
}

struct ConfiguredDiscoveryTargets {
    QList<QHostAddress> host_targets;
    QList<QHostAddress> broadcast_targets;
    QStringList ranges;
    QStringList invalid_networks;
    QStringList skipped_host_scan_networks;
};

ConfiguredDiscoveryTargets configured_discovery_targets(const QStringList& configured_networks) {
    ConfiguredDiscoveryTargets result;
    QSet<QString> seen_hosts;
    QSet<QString> seen_broadcasts;
    const auto add_host = [&result, &seen_hosts](quint32 address) {
        const auto target = QHostAddress(address);
        const auto key = target.toString();
        if (seen_hosts.contains(key)) {
            return;
        }
        seen_hosts.insert(key);
        result.host_targets.push_back(target);
    };
    const auto add_broadcast = [&result, &seen_broadcasts](quint32 address) {
        const auto target = QHostAddress(address);
        const auto key = target.toString();
        if (seen_broadcasts.contains(key)) {
            return;
        }
        seen_broadcasts.insert(key);
        result.broadcast_targets.push_back(target);
    };

    for (const auto& raw_network : configured_networks) {
        const auto network_text = raw_network.trimmed();
        if (network_text.isEmpty()) {
            continue;
        }

        const auto slash = network_text.indexOf('/');
        const auto address_text = slash < 0 ? network_text : network_text.left(slash);
        bool ok = false;
        const auto prefix = slash < 0 ? 32 : network_text.mid(slash + 1).toInt(&ok);
        if (slash >= 0 && (!ok || prefix < 0 || prefix > 32)) {
            result.invalid_networks.push_back(network_text);
            continue;
        }

        const auto address = QHostAddress(address_text);
        if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
            result.invalid_networks.push_back(network_text);
            continue;
        }

        const auto ip = address.toIPv4Address();
        if (prefix == 32) {
            add_host(ip);
            result.ranges.push_back(ipv4_text(ip));
            continue;
        }

        const auto mask = prefix_mask(prefix);
        const auto network = ip & mask;
        const auto broadcast = network | ~mask;
        add_broadcast(broadcast);

        if (prefix >= 24) {
            const auto first_host = prefix >= 31 ? network : network + 1U;
            const auto last_host = prefix >= 31 ? broadcast : broadcast - 1U;
            result.ranges.push_back(QStringLiteral("%1 (%2-%3)")
                                        .arg(network_text, ipv4_text(first_host), ipv4_text(last_host)));
            for (quint32 target = first_host; target <= last_host; ++target) {
                add_host(target);
                if (target == 0xffffffffU) {
                    break;
                }
            }
            continue;
        }

        const auto first_24 = network & 0xffffff00U;
        const auto last_24 = broadcast & 0xffffff00U;
        const auto subnet_count = ((last_24 - first_24) >> 8) + 1U;
        result.ranges.push_back(QStringLiteral("%1 (%2 /24 broadcast domain(s))")
                                    .arg(network_text)
                                    .arg(subnet_count));
        if (subnet_count > 1024U) {
            result.skipped_host_scan_networks.push_back(network_text);
            continue;
        }
        for (quint32 subnet = first_24; subnet <= last_24; subnet += 0x100U) {
            add_broadcast(subnet | 0xffU);
            if (subnet > 0xffffffffU - 0x100U) {
                break;
            }
        }
        result.skipped_host_scan_networks.push_back(network_text);
    }
    return result;
}

}  // namespace

DiscoveryController::DiscoveryController()
    : socket_(std::make_unique<QUdpSocket>()) {}

DiscoveryController::~DiscoveryController() = default;

bool DiscoveryController::bind(QObject* context, DatagramHandler handler) {
    const auto bound = socket_->bind(QHostAddress::AnyIPv4,
                                    kDiscoveryPort,
                                    QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound) {
        return false;
    }
    QObject::connect(socket_.get(), &QUdpSocket::readyRead, context, [this, handler = std::move(handler)] {
        read_pending(handler);
    });
    return true;
}

QString DiscoveryController::error_string() const {
    return socket_ != nullptr ? socket_->errorString() : QString{};
}

DiscoveryProbeReport DiscoveryController::send_discovery_probe(const QString& node_id,
                                                               const QString& name,
                                                               const QStringList& configured_networks) {
    DiscoveryProbeReport report;
    if (socket_ == nullptr) {
        return report;
    }

    const auto message = encode_control_message(QStringLiteral("discover"), node_id, name, kTransferPort);
    const auto targets = discovery_targets();
    for (const auto& target : targets) {
        socket_->writeDatagram(message, target, kDiscoveryPort);
        report.broadcast_targets.push_back(target.toString());
    }
    if (configured_networks.isEmpty()) {
        return report;
    }

    const auto configured_targets = configured_discovery_targets(configured_networks);
    for (const auto& target : configured_targets.broadcast_targets) {
        socket_->writeDatagram(message, target, kDiscoveryPort);
        report.configured_broadcast_targets.push_back(target.toString());
    }
    for (const auto& target : configured_targets.host_targets) {
        socket_->writeDatagram(message, target, kDiscoveryPort);
    }
    report.configured_ranges = configured_targets.ranges;
    report.invalid_configured_networks = configured_targets.invalid_networks;
    report.skipped_host_scan_networks = configured_targets.skipped_host_scan_networks;
    report.configured_host_target_count = configured_targets.host_targets.size();
    return report;
}

bool DiscoveryController::reply_to_discovery(const QHostAddress& target,
                                             quint16 port,
                                             const QString& node_id,
                                             const QString& name) {
    if (socket_ == nullptr) {
        return false;
    }
    const auto message = encode_control_message(QStringLiteral("announce"), node_id, name, kTransferPort);
    return socket_->writeDatagram(message, target, port) >= 0;
}

bool DiscoveryController::send_control(const Peer& peer,
                                       const QString& type,
                                       const QString& node_id,
                                       const QString& name,
                                       const QJsonObject& fields) {
    if (socket_ == nullptr) {
        return false;
    }
    const auto payload = encode_control_message(type, node_id, name, kTransferPort, fields);
    return socket_->writeDatagram(payload, QHostAddress(peer.host), kDiscoveryPort) >= 0;
}

void DiscoveryController::read_pending(const DatagramHandler& handler) {
    while (socket_->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(socket_->pendingDatagramSize()));
        QHostAddress sender;
        quint16 sender_port = 0;
        socket_->readDatagram(datagram.data(), datagram.size(), &sender, &sender_port);

        auto message = decode_control_message(datagram);
        if (!message.has_value()) {
            continue;
        }
        handler(DiscoveryDatagram{
            .sender = sender,
            .sender_port = sender_port,
            .message = std::move(message).value(),
        });
    }
}

}  // namespace lan::gui

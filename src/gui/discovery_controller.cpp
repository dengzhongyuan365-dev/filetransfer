#include "gui/discovery_controller.h"

#include <QAbstractSocket>
#include <QNetworkInterface>
#include <QSet>
#include <QUdpSocket>

#include <algorithm>

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

bool is_private_ipv4(quint32 address) {
    const auto first = (address >> 24) & 0xffU;
    const auto second = (address >> 16) & 0xffU;
    return first == 10U ||
           (first == 172U && second >= 16U && second <= 31U) ||
           (first == 192U && second == 168U);
}

QList<QHostAddress> extended_discovery_targets() {
    QList<QHostAddress> targets;
    QSet<QString> seen;
    const auto add_target = [&targets, &seen](quint32 address) {
        const auto host = address & 0xffU;
        if (host == 0U || host == 255U) {
            return;
        }
        const auto target = QHostAddress(address);
        const auto key = target.toString();
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        targets.push_back(target);
    };

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const auto local = entry.ip().toIPv4Address();
            if (!is_private_ipv4(local)) {
                continue;
            }

            const auto prefix = local & 0xffff0000U;
            const auto local_subnet = static_cast<int>((local >> 8) & 0xffU);
            const auto begin_subnet = std::max(0, local_subnet - 16);
            const auto end_subnet = std::min(255, local_subnet + 16);
            for (int subnet = begin_subnet; subnet <= end_subnet; ++subnet) {
                for (quint32 host = 1; host <= 254; ++host) {
                    add_target(prefix | (static_cast<quint32>(subnet) << 8) | host);
                }
            }
        }
    }
    return targets;
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
                                                               bool extended) {
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
    if (!extended) {
        return report;
    }

    const auto extended_targets = extended_discovery_targets();
    for (const auto& target : extended_targets) {
        socket_->writeDatagram(message, target, kDiscoveryPort);
    }
    report.extended_target_count = extended_targets.size();
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

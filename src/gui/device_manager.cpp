#include "gui/device_manager.h"

#include <algorithm>

namespace lan::gui {

bool DeviceManager::empty() const {
    return peers_.isEmpty();
}

int DeviceManager::count() const {
    return peers_.size();
}

bool DeviceManager::contains(const QString& id) const {
    return peers_.contains(id);
}

Peer DeviceManager::peer(const QString& id) const {
    return peers_.value(id);
}

QList<Peer> DeviceManager::peers() const {
    return peers_.values();
}

void DeviceManager::insert_peer(const Peer& peer) {
    const auto duplicate_id = find_peer_id_by_endpoint(peer.host, peer.port);
    if (!duplicate_id.isEmpty() && duplicate_id != peer.id) {
        const auto duplicate = peers_.value(duplicate_id);
        if (duplicate.last_linked_ms > peer.last_linked_ms) {
            return;
        }
        peers_.remove(duplicate_id);
        if (active_peer_id_ == duplicate_id) {
            active_peer_id_ = peer.id;
        }
        if (send_target_peer_ids_.contains(duplicate_id)) {
            send_target_peer_ids_.removeAll(duplicate_id);
            send_target_peer_ids_.append(peer.id);
        }
    }
    peers_.insert(peer.id, peer);
}

QString DeviceManager::find_peer_id_by_endpoint(const QString& host, std::uint16_t port) const {
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        if (it.value().host == host && it.value().port == port) {
            return it.key();
        }
    }
    return {};
}

Peer DeviceManager::remember_peer(const Peer& peer, qint64 now_ms) {
    auto remembered = peer;
    remembered.last_linked_ms = now_ms;
    remembered.online = true;
    remembered.linked = peers_.value(peer.id).linked || peer.linked;
    peers_.insert(remembered.id, remembered);
    return remembered;
}

QList<Peer> DeviceManager::mark_all_offline() {
    QList<Peer> peers;
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        it.value().online = false;
        peers.append(it.value());
    }
    return peers;
}

QList<Peer> DeviceManager::mark_stale_offline(qint64 now_ms, qint64 stale_ms) {
    QList<Peer> changed;
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        auto& peer = it.value();
        if (!peer.online ||
            peer.last_seen_ms <= 0 ||
            peer.id.startsWith(QStringLiteral("manual:")) ||
            now_ms - peer.last_seen_ms <= stale_ms) {
            continue;
        }
        peer.online = false;
        changed.append(peer);
    }
    return changed;
}

Peer DeviceManager::upsert_manual_peer(const QString& host, std::uint16_t port, qint64 now_ms) {
    auto id = find_peer_id_by_endpoint(host, port);
    if (id.isEmpty()) {
        id = QStringLiteral("manual:%1:%2").arg(host).arg(port);
    }

    auto peer = peers_.value(id);
    peer.id = id;
    peer.name = host;
    peer.host = host;
    peer.port = port;
    peer.online = true;
    peer.last_seen_ms = now_ms;
    peers_.insert(id, peer);
    return peer;
}

PeerUpsertResult DeviceManager::upsert_discovered_peer(const QString& host,
                                                       std::uint16_t port,
                                                       const QString& id,
                                                       const QString& name,
                                                       qint64 now_ms) {
    const auto normalized_id = id.isEmpty() ? host + ":" + QString::number(port) : id;
    const auto endpoint_peer_id = find_peer_id_by_endpoint(host, port);
    auto existing = peers_.value(normalized_id);
    QString previous_id;
    if (!endpoint_peer_id.isEmpty() && endpoint_peer_id != normalized_id) {
        previous_id = endpoint_peer_id;
        existing = peers_.value(endpoint_peer_id);
        peers_.remove(endpoint_peer_id);
        if (active_peer_id_ == endpoint_peer_id) {
            active_peer_id_ = normalized_id;
        }
        if (send_target_peer_ids_.contains(endpoint_peer_id)) {
            send_target_peer_ids_.removeAll(endpoint_peer_id);
            if (!send_target_peer_ids_.contains(normalized_id)) {
                send_target_peer_ids_.append(normalized_id);
            }
        }
    }

    Peer peer{
        .id = normalized_id,
        .name = name,
        .host = host,
        .port = port,
        .online = true,
        .linked = existing.linked,
        .last_seen_ms = now_ms,
        .last_linked_ms = existing.last_linked_ms,
    };
    peers_.insert(normalized_id, peer);
    return PeerUpsertResult{.peer = peer, .previous_id = previous_id};
}

bool DeviceManager::set_active_peer(const QString& id) {
    if (!peers_.contains(id) || !peers_.value(id).linked) {
        return false;
    }
    active_peer_id_ = id;
    reset_send_targets_to_active();
    return true;
}

Peer DeviceManager::set_linked_peer(const Peer& peer, bool activate, qint64 now_ms) {
    auto linked = peers_.value(peer.id, peer);
    linked.linked = true;
    linked.online = true;
    linked.last_linked_ms = now_ms;
    peers_.insert(linked.id, linked);
    if (activate || !has_active_peer()) {
        set_active_peer(linked.id);
    }
    return linked;
}

bool DeviceManager::unlink_peer(const QString& id, Peer* unlinked_peer) {
    if (!peers_.contains(id) || !peers_.value(id).linked) {
        return false;
    }
    auto peer = peers_.value(id);
    peer.linked = false;
    peers_.insert(id, peer);
    if (unlinked_peer != nullptr) {
        *unlinked_peer = peer;
    }

    if (active_peer_id_ == id) {
        active_peer_id_.clear();
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
            if (it.value().linked) {
                active_peer_id_ = it.key();
                break;
            }
        }
        reset_send_targets_to_active();
    } else {
        remove_send_target_peer(id);
    }
    return true;
}

bool DeviceManager::has_active_peer() const {
    return peers_.contains(active_peer_id_) && peers_.value(active_peer_id_).linked;
}

Peer DeviceManager::active_peer() const {
    return peers_.value(active_peer_id_);
}

QString DeviceManager::active_peer_id() const {
    return active_peer_id_;
}

int DeviceManager::linked_peer_count() const {
    int count = 0;
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        if (it.value().linked) {
            ++count;
        }
    }
    return count;
}

QStringList DeviceManager::linked_peer_ids() const {
    QStringList ids;
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        if (it.value().linked) {
            ids.append(it.key());
        }
    }
    return ids;
}

bool DeviceManager::is_linked_peer(const Peer& peer) const {
    return peer.linked;
}

QStringList DeviceManager::send_target_peer_ids() const {
    QStringList ids;
    for (const auto& id : send_target_peer_ids_) {
        if (peers_.contains(id) && peers_.value(id).linked) {
            ids.append(id);
        }
    }
    if (ids.isEmpty() && has_active_peer()) {
        ids.append(active_peer_id_);
    }
    ids.sort();
    return ids;
}

void DeviceManager::reset_send_targets_to_active() {
    send_target_peer_ids_.clear();
    if (has_active_peer()) {
        send_target_peer_ids_.append(active_peer_id_);
    }
}

void DeviceManager::set_send_target_peer_ids(const QStringList& ids) {
    send_target_peer_ids_.clear();
    for (const auto& id : ids) {
        if (!send_target_peer_ids_.contains(id)) {
            send_target_peer_ids_.append(id);
        }
    }
}

void DeviceManager::remove_send_target_peer(const QString& id) {
    send_target_peer_ids_.removeAll(id);
}

}  // namespace lan::gui

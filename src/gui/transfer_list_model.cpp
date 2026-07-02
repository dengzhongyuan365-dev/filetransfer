#include "gui/transfer_list_model.h"

namespace lan::gui {

QString transfer_snapshot_key(const TransferSnapshot& snapshot) {
    const auto direction = snapshot.direction == TransferDirection::receive ? "receive" : "send";
    return QString("%1:%2").arg(direction).arg(snapshot.transfer_id);
}

bool TransferListModel::try_snapshot(const QString& key, TransferSnapshot* snapshot) const {
    const auto it = snapshots_.find(key);
    if (it == snapshots_.end()) {
        return false;
    }
    if (snapshot != nullptr) {
        *snapshot = it.value();
    }
    return true;
}

void TransferListModel::upsert(const TransferSnapshot& snapshot, const QString& peer_id) {
    const auto key = transfer_snapshot_key(snapshot);
    clear_dismissed(key);
    if (!peer_id.isEmpty() && peer_ids_.value(key) != peer_id) {
        peer_ids_.insert(key, peer_id);
    }
    snapshots_.insert(key, snapshot);
}

void TransferListModel::remove(const QString& key) {
    snapshots_.remove(key);
    peer_ids_.remove(key);
    dismissed_keys_.remove(key);
}

void TransferListModel::mark_dismissed(const QString& key) {
    dismissed_keys_.insert(key);
}

void TransferListModel::clear_dismissed(const QString& key) {
    dismissed_keys_.remove(key);
}

bool TransferListModel::is_dismissed(const QString& key) const {
    return dismissed_keys_.contains(key);
}

QString TransferListModel::peer_id_or(const QString& key, const QString& fallback) const {
    return peer_ids_.value(key, fallback);
}

bool TransferListModel::belongs_to_peer(const QString& key, const QString& active_peer_id, bool has_active_peer) const {
    if (!peer_ids_.contains(key)) {
        return false;
    }
    if (!has_active_peer) {
        return false;
    }
    return peer_ids_.value(key) == active_peer_id;
}

QList<TransferListEntry> TransferListModel::visible_entries(const QString& active_peer_id, bool has_active_peer) const {
    QList<TransferListEntry> entries;
    for (auto it = snapshots_.cbegin(); it != snapshots_.cend(); ++it) {
        if (!belongs_to_peer(it.key(), active_peer_id, has_active_peer)) {
            continue;
        }
        entries.append(TransferListEntry{.key = it.key(), .snapshot = it.value()});
    }
    return entries;
}

}  // namespace lan::gui

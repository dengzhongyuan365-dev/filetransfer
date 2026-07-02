#pragma once

#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

#include "lan/app/transfer_snapshot.h"

namespace lan::gui {

QString transfer_snapshot_key(const TransferSnapshot& snapshot);

struct TransferListEntry {
    QString key;
    TransferSnapshot snapshot;
};

class TransferListModel {
public:
    bool try_snapshot(const QString& key, TransferSnapshot* snapshot) const;
    void upsert(const TransferSnapshot& snapshot, const QString& peer_id = {});
    void remove(const QString& key);

    void mark_dismissed(const QString& key);
    void clear_dismissed(const QString& key);
    bool is_dismissed(const QString& key) const;

    QString peer_id_or(const QString& key, const QString& fallback) const;
    bool belongs_to_peer(const QString& key, const QString& active_peer_id, bool has_active_peer) const;
    QList<TransferListEntry> visible_entries(const QString& active_peer_id, bool has_active_peer) const;

private:
    QMap<QString, TransferSnapshot> snapshots_;
    QMap<QString, QString> peer_ids_;
    QSet<QString> dismissed_keys_;
};

}  // namespace lan::gui

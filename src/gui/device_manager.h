#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <cstdint>

#include "gui/types.h"

namespace lan::gui {

struct PeerUpsertResult {
    Peer peer;
    QString previous_id;
};

class DeviceManager {
public:
    bool empty() const;
    int count() const;
    bool contains(const QString& id) const;
    Peer peer(const QString& id) const;
    QList<Peer> peers() const;

    void insert_peer(const Peer& peer);
    QString find_peer_id_by_endpoint(const QString& host, std::uint16_t port) const;
    Peer remember_peer(const Peer& peer, qint64 now_ms);
    QList<Peer> mark_all_offline();
    QList<Peer> mark_stale_offline(qint64 now_ms, qint64 stale_ms);
    Peer upsert_manual_peer(const QString& host, std::uint16_t port, qint64 now_ms);
    PeerUpsertResult upsert_discovered_peer(const QString& host,
                                            std::uint16_t port,
                                            const QString& id,
                                            const QString& name,
                                            qint64 now_ms);

    bool set_active_peer(const QString& id);
    Peer set_linked_peer(const Peer& peer, bool activate, qint64 now_ms);
    bool unlink_peer(const QString& id, Peer* unlinked_peer);
    bool has_active_peer() const;
    Peer active_peer() const;
    QString active_peer_id() const;
    int linked_peer_count() const;
    QStringList linked_peer_ids() const;
    bool is_linked_peer(const Peer& peer) const;

    QStringList send_target_peer_ids() const;
    void reset_send_targets_to_active();
    void set_send_target_peer_ids(const QStringList& ids);
    void remove_send_target_peer(const QString& id);

private:
    QMap<QString, Peer> peers_;
    QString active_peer_id_;
    QStringList send_target_peer_ids_;
};

}  // namespace lan::gui

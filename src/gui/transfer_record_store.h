#pragma once

#include <cstdint>
#include <optional>

#include <QString>
#include <QVariantMap>

#include "lan/app/transfer_snapshot.h"

namespace lan::gui {

struct PersistedTransferRecord {
    QString peer_id;
    TransferSnapshot snapshot;
};

TransferSnapshot snapshot_for_persistence(TransferSnapshot snapshot, const QString& interrupted_message);
QVariantMap transfer_record_to_settings(const PersistedTransferRecord& record, const QString& interrupted_message);
std::optional<PersistedTransferRecord> transfer_record_from_settings(const QVariantMap& map,
                                                                     std::uint64_t restored_id,
                                                                     const QString& interrupted_message);

}  // namespace lan::gui

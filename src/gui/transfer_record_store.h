#pragma once

#include <cstdint>
#include <optional>

#include <QJsonDocument>
#include <QList>
#include <QSettings>
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
QJsonDocument transfer_records_to_json(const QList<PersistedTransferRecord>& records,
                                       const QString& interrupted_message);
QList<PersistedTransferRecord> transfer_records_from_json(const QJsonDocument& document,
                                                          std::uint64_t restored_base,
                                                          const QString& interrupted_message);
QList<PersistedTransferRecord> transfer_records_from_settings_array(QSettings& settings,
                                                                    const QString& array_name,
                                                                    std::uint64_t restored_base,
                                                                    const QString& interrupted_message);

}  // namespace lan::gui

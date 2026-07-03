#include "gui/transfer_record_store.h"

#include <filesystem>

#include <QJsonArray>
#include <QJsonObject>

#include "lan/common/error.h"

namespace lan::gui {
namespace {

constexpr int kTransferRecordSchemaVersion = 1;

QString path_to_qstring(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

std::string qstring_to_string(const QString& text) {
    return text.trimmed().toStdString();
}

bool is_terminal_transfer_state(TransferState state) {
    return state == TransferState::completed ||
           state == TransferState::failed ||
           state == TransferState::cancelled;
}

QJsonObject variant_map_to_json_object(const QVariantMap& map) {
    auto object = QJsonObject::fromVariantMap(map);
    const QStringList large_number_keys{
        QStringLiteral("transferId"),
        QStringLiteral("currentBytes"),
        QStringLiteral("totalBytes"),
        QStringLiteral("processedFiles"),
        QStringLiteral("totalFiles"),
        QStringLiteral("skippedFiles"),
        QStringLiteral("fullFiles"),
        QStringLiteral("deltaFiles"),
        QStringLiteral("payloadBytes"),
        QStringLiteral("resumedFrom"),
    };
    for (const auto& key : large_number_keys) {
        if (map.contains(key)) {
            object.insert(key, QString::number(map.value(key).toULongLong()));
        }
    }
    return object;
}

QVariantMap json_object_to_variant_map(const QJsonObject& object) {
    QVariantMap map;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        map.insert(it.key(), it->toVariant());
    }
    return map;
}

}  // namespace

TransferSnapshot snapshot_for_persistence(TransferSnapshot snapshot, const QString& interrupted_message) {
    if (!is_terminal_transfer_state(snapshot.state)) {
        snapshot.state = TransferState::cancelled;
        snapshot.error = Error{
            ErrorCode::cancelled,
            interrupted_message.toStdString(),
        };
        snapshot.error_category = ErrorCategory::cancellation;
        snapshot.retryable = true;
    }
    return snapshot;
}

QVariantMap transfer_record_to_settings(const PersistedTransferRecord& record, const QString& interrupted_message) {
    const auto snapshot = snapshot_for_persistence(record.snapshot, interrupted_message);
    QVariantMap map;
    map.insert(QStringLiteral("peerId"), record.peer_id);
    map.insert(QStringLiteral("transferId"), QVariant::fromValue<qulonglong>(snapshot.transfer_id));
    map.insert(QStringLiteral("state"), static_cast<int>(snapshot.state));
    map.insert(QStringLiteral("direction"), static_cast<int>(snapshot.direction));
    map.insert(QStringLiteral("kind"), static_cast<int>(snapshot.kind));
    map.insert(QStringLiteral("path"), path_to_qstring(snapshot.path));
    map.insert(QStringLiteral("name"), QString::fromStdString(snapshot.name));
    map.insert(QStringLiteral("currentBytes"), QVariant::fromValue<qulonglong>(snapshot.current_bytes));
    map.insert(QStringLiteral("totalBytes"), QVariant::fromValue<qulonglong>(snapshot.total_bytes));
    map.insert(QStringLiteral("processedFiles"), QVariant::fromValue<qulonglong>(snapshot.processed_files));
    map.insert(QStringLiteral("totalFiles"), QVariant::fromValue<qulonglong>(snapshot.total_files));
    map.insert(QStringLiteral("skippedFiles"), QVariant::fromValue<qulonglong>(snapshot.skipped_files));
    map.insert(QStringLiteral("fullFiles"), QVariant::fromValue<qulonglong>(snapshot.full_files));
    map.insert(QStringLiteral("deltaFiles"), QVariant::fromValue<qulonglong>(snapshot.delta_files));
    map.insert(QStringLiteral("payloadBytes"), QVariant::fromValue<qulonglong>(snapshot.payload_bytes));
    map.insert(QStringLiteral("completionStatus"), static_cast<int>(snapshot.completion_status));
    map.insert(QStringLiteral("resumedFrom"), QVariant::fromValue<qulonglong>(snapshot.resumed_from));
    map.insert(QStringLiteral("elapsedSeconds"), snapshot.elapsed_seconds);
    map.insert(QStringLiteral("source"), static_cast<int>(snapshot.source));
    if (snapshot.error.has_value()) {
        map.insert(QStringLiteral("errorCode"), static_cast<int>(snapshot.error->code));
        map.insert(QStringLiteral("errorMessage"), QString::fromStdString(snapshot.error->message));
        map.insert(QStringLiteral("errorCategory"), static_cast<int>(snapshot.error_category));
        map.insert(QStringLiteral("retryable"), snapshot.retryable);
        map.insert(QStringLiteral("userActionRequired"), snapshot.user_action_required);
    }
    return map;
}

std::optional<PersistedTransferRecord> transfer_record_from_settings(const QVariantMap& map,
                                                                     std::uint64_t restored_id,
                                                                     const QString& interrupted_message) {
    const auto peer_id = map.value(QStringLiteral("peerId")).toString();
    const auto name = map.value(QStringLiteral("name")).toString();
    const auto path = map.value(QStringLiteral("path")).toString();
    if (peer_id.isEmpty() || (name.isEmpty() && path.isEmpty())) {
        return std::nullopt;
    }

    TransferSnapshot snapshot;
    snapshot.transfer_id = restored_id;
    snapshot.state = static_cast<TransferState>(map.value(QStringLiteral("state")).toInt());
    snapshot.direction = static_cast<TransferDirection>(map.value(QStringLiteral("direction")).toInt());
    snapshot.kind = static_cast<TransferKind>(map.value(QStringLiteral("kind")).toInt());
    snapshot.path = std::filesystem::path(qstring_to_string(path));
    snapshot.name = qstring_to_string(name);
    snapshot.current_bytes = map.value(QStringLiteral("currentBytes")).toULongLong();
    snapshot.total_bytes = map.value(QStringLiteral("totalBytes")).toULongLong();
    snapshot.processed_files = map.value(QStringLiteral("processedFiles")).toULongLong();
    snapshot.total_files = map.value(QStringLiteral("totalFiles")).toULongLong();
    snapshot.skipped_files = map.value(QStringLiteral("skippedFiles")).toULongLong();
    snapshot.full_files = map.value(QStringLiteral("fullFiles")).toULongLong();
    snapshot.delta_files = map.value(QStringLiteral("deltaFiles")).toULongLong();
    snapshot.payload_bytes = map.value(QStringLiteral("payloadBytes")).toULongLong();
    snapshot.completion_status = static_cast<TransferCompletionStatus>(map.value(QStringLiteral("completionStatus")).toInt());
    snapshot.resumed_from = map.value(QStringLiteral("resumedFrom")).toULongLong();
    snapshot.elapsed_seconds = map.value(QStringLiteral("elapsedSeconds")).toDouble();
    snapshot.source = static_cast<FileTransferSource>(map.value(QStringLiteral("source")).toInt());
    if (map.contains(QStringLiteral("errorMessage"))) {
        snapshot.error = Error{
            static_cast<ErrorCode>(map.value(QStringLiteral("errorCode")).toInt()),
            qstring_to_string(map.value(QStringLiteral("errorMessage")).toString()),
        };
        snapshot.error_category = static_cast<ErrorCategory>(map.value(QStringLiteral("errorCategory")).toInt());
        snapshot.retryable = map.value(QStringLiteral("retryable")).toBool();
        snapshot.user_action_required = map.value(QStringLiteral("userActionRequired")).toBool();
    }
    snapshot = snapshot_for_persistence(snapshot, interrupted_message);
    return PersistedTransferRecord{.peer_id = peer_id, .snapshot = snapshot};
}

QJsonDocument transfer_records_to_json(const QList<PersistedTransferRecord>& records,
                                       const QString& interrupted_message) {
    QJsonArray array;
    for (const auto& record : records) {
        if (record.peer_id.isEmpty()) {
            continue;
        }
        const auto map = transfer_record_to_settings(record, interrupted_message);
        array.append(variant_map_to_json_object(map));
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kTransferRecordSchemaVersion);
    root.insert(QStringLiteral("records"), array);
    return QJsonDocument(root);
}

QList<PersistedTransferRecord> transfer_records_from_json(const QJsonDocument& document,
                                                          std::uint64_t restored_base,
                                                          const QString& interrupted_message) {
    QList<PersistedTransferRecord> records;
    if (!document.isObject()) {
        return records;
    }

    const auto root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kTransferRecordSchemaVersion) {
        return records;
    }
    const auto array = root.value(QStringLiteral("records")).toArray();
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            continue;
        }
        auto record = transfer_record_from_settings(json_object_to_variant_map(array.at(i).toObject()),
                                                    restored_base + static_cast<std::uint64_t>(i + 1),
                                                    interrupted_message);
        if (record.has_value()) {
            records.append(record.value());
        }
    }
    return records;
}

QList<PersistedTransferRecord> transfer_records_from_settings_array(QSettings& settings,
                                                                    const QString& array_name,
                                                                    std::uint64_t restored_base,
                                                                    const QString& interrupted_message) {
    QList<PersistedTransferRecord> records;
    const auto size = settings.beginReadArray(array_name);
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        QVariantMap map;
        for (const auto& key : settings.childKeys()) {
            map.insert(key, settings.value(key));
        }
        auto record = transfer_record_from_settings(map,
                                                    restored_base + static_cast<std::uint64_t>(i + 1),
                                                    interrupted_message);
        if (record.has_value()) {
            records.append(record.value());
        }
    }
    settings.endArray();
    return records;
}

}  // namespace lan::gui

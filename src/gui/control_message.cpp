#include "gui/control_message.h"

#include <QJsonDocument>

#include "gui/types.h"

namespace lan::gui {

QByteArray encode_control_message(const QString& type,
                                  const QString& node_id,
                                  const QString& name,
                                  std::uint16_t port,
                                  const QJsonObject& fields) {
    QJsonObject message{
        {"protocol", kProtocol},
        {"type", type},
        {"id", node_id},
        {"name", name},
        {"port", static_cast<int>(port)},
    };
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        message.insert(it.key(), it.value());
    }
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

std::optional<ControlMessage> decode_control_message(const QByteArray& payload) {
    const auto doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return std::nullopt;
    }
    const auto obj = doc.object();
    if (obj.value(QStringLiteral("protocol")).toString() != kProtocol) {
        return std::nullopt;
    }
    const auto type = obj.value(QStringLiteral("type")).toString();
    if (type.isEmpty()) {
        return std::nullopt;
    }

    return ControlMessage{
        .type = type,
        .id = obj.value(QStringLiteral("id")).toString(),
        .name = obj.value(QStringLiteral("name")).toString(),
        .port = static_cast<std::uint16_t>(obj.value(QStringLiteral("port")).toInt(kTransferPort)),
        .fields = obj,
    };
}

}  // namespace lan::gui

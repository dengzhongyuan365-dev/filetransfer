#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>

namespace lan::gui {

struct ControlMessage {
    QString type;
    QString id;
    QString name;
    std::uint16_t port = 0;
    QJsonObject fields;
};

QByteArray encode_control_message(const QString& type,
                                  const QString& node_id,
                                  const QString& name,
                                  std::uint16_t port,
                                  const QJsonObject& fields = {});

std::optional<ControlMessage> decode_control_message(const QByteArray& payload);

}  // namespace lan::gui

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

class QWidget;

namespace lan::gui {

struct TargetDevice {
    QString id;
    QString name;
    QString host;
    std::uint16_t port = 0;
    bool selected = false;
};

std::optional<QStringList> choose_send_targets(QWidget* parent, const QList<TargetDevice>& devices);
std::optional<QString> choose_transfer_target(QWidget* parent, const QList<TargetDevice>& devices);

}  // namespace lan::gui

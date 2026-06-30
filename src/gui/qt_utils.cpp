#include "gui/qt_utils.h"

#include <QDir>
#include <QHostInfo>

namespace lan::gui {

QString to_qstring(const std::string& text) {
    return QString::fromStdString(text);
}

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

std::string to_string(const QString& text) {
    return text.trimmed().toStdString();
}

QString default_receive_dir() {
    return QDir::homePath() + "/Downloads/reviewdir";
}

QString machine_name() {
    const auto name = QHostInfo::localHostName();
    return name.isEmpty() ? "unknown" : name;
}

}  // namespace lan::gui

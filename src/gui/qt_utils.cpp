#include "gui/qt_utils.h"

#include <QDir>
#include <QHostInfo>
#include <QPainter>
#include <QSize>
#include <QSizePolicy>

#include <utility>

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

ElidedLabel::ElidedLabel(QString text, QWidget* parent) : QLabel(parent), full_text_(std::move(text)) {
    QLabel::setText(full_text_);
    setToolTip(full_text_);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void ElidedLabel::setText(const QString& text) {
    full_text_ = text;
    QLabel::setText(text);
    setToolTip(text);
    updateGeometry();
    update();
}

QString ElidedLabel::text() const {
    return full_text_;
}

QSize ElidedLabel::minimumSizeHint() const {
    return QSize(16, QLabel::minimumSizeHint().height());
}

void ElidedLabel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto text = fontMetrics().elidedText(full_text_, Qt::ElideMiddle, width());
    painter.drawText(rect(), alignment() | Qt::TextSingleLine, text);
}

}  // namespace lan::gui

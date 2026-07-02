#pragma once

#include <QLabel>
#include <QString>

#include <filesystem>
#include <string>

namespace lan::gui {

QString to_qstring(const std::string& text);
QString to_qstring(const std::filesystem::path& path);
std::string to_string(const QString& text);
QString default_receive_dir();
QString machine_name();

class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QString text, QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString full_text_;
};

}  // namespace lan::gui

#pragma once

#include <QString>

#include <filesystem>
#include <string>

namespace lan::gui {

QString to_qstring(const std::string& text);
QString to_qstring(const std::filesystem::path& path);
std::string to_string(const QString& text);
QString default_receive_dir();
QString machine_name();

}  // namespace lan::gui

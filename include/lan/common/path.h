#pragma once

#include <filesystem>

#include "lan/common/result.h"

namespace lan {

Result<std::filesystem::path> normalize_path(const std::filesystem::path& path);
Result<std::filesystem::path> require_file_or_directory(const std::filesystem::path& path);
Result<std::filesystem::path> ensure_directory(const std::filesystem::path& path);

}  // namespace lan

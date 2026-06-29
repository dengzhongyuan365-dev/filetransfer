#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "lan/common/result.h"
#include "lan/fs/file_hash.h"

namespace lan {

struct LocalCopyProgress {
    std::uint64_t bytes_copied = 0;
    std::uint64_t total_bytes = 0;
    double elapsed_seconds = 0.0;
};

struct LocalCopyOptions {
    std::filesystem::path source_path;
    std::filesystem::path target_path;
    std::uint64_t buffer_size = 1024 * 1024;
    bool overwrite = false;
    std::function<void(const LocalCopyProgress&)> on_progress;
};

struct LocalCopyReport {
    std::uint64_t bytes_copied = 0;
    double elapsed_seconds = 0.0;
    FileHash source_hash;
    FileHash target_hash;
};

Result<LocalCopyReport> copy_file(const LocalCopyOptions& options);

}  // namespace lan

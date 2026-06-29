#include "lan/transfer/part_file_guard.h"

#include <utility>

namespace lan {

PartFileGuard::PartFileGuard(std::filesystem::path path) : path_(std::move(path)) {}

PartFileGuard::~PartFileGuard() {
    if (!committed_) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
}

void PartFileGuard::commit() {
    committed_ = true;
}

}  // namespace lan

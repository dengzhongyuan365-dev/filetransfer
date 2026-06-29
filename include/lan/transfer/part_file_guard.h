#pragma once

#include <filesystem>

namespace lan {

class PartFileGuard {
public:
    explicit PartFileGuard(std::filesystem::path path);
    ~PartFileGuard();

    PartFileGuard(const PartFileGuard&) = delete;
    PartFileGuard& operator=(const PartFileGuard&) = delete;

    void commit();

private:
    std::filesystem::path path_;
    bool committed_ = false;
};

}  // namespace lan

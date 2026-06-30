#include "lan/transfer/sync_plan.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

#include "lan/fs/file_hash.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }

    for (const auto& part : path) {
        if (part == "." || part == "..") {
            return false;
        }
    }

    return true;
}

Result<SyncAction> choose_action(const ManifestEntry& entry, const std::filesystem::path& target) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return Result<SyncAction>::success(SyncAction::full);
    }
    if (ec) {
        return Result<SyncAction>::failure(
            make_error(ErrorCode::io_error,
                       "failed to inspect receive target " + quote_path(target) + ": " + ec.message()));
    }

    if (!std::filesystem::exists(status)) {
        return Result<SyncAction>::success(SyncAction::full);
    }

    if (!std::filesystem::is_regular_file(status)) {
        return Result<SyncAction>::success(SyncAction::full);
    }

    const auto local_size = std::filesystem::file_size(target, ec);
    if (ec) {
        return Result<SyncAction>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read receive target size " + quote_path(target) + ": " + ec.message()));
    }

    if (local_size != entry.size) {
        return Result<SyncAction>::success(SyncAction::delta);
    }

    if (!entry.sha256.empty()) {
        auto local_hash = hash_file(target);
        if (!local_hash) {
            return Result<SyncAction>::failure(local_hash.error());
        }

        if (local_hash.value().hex_digest == entry.sha256) {
            return Result<SyncAction>::success(SyncAction::skip);
        }

        return Result<SyncAction>::success(SyncAction::delta);
    }

    const auto local_mtime = std::filesystem::last_write_time(target, ec);
    if (ec) {
        return Result<SyncAction>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read receive target mtime " + quote_path(target) + ": " + ec.message()));
    }

    const auto local_system_time = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        local_mtime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const auto local_ns = local_system_time.time_since_epoch().count();
    if (local_ns >= 0) {
        constexpr auto mtime_tolerance_ns = 1000LL * 1000LL;
        const auto source_ns = static_cast<long long>(entry.mtime_ns);
        const auto delta = std::llabs(static_cast<long long>(local_ns) - source_ns);
        if (delta <= mtime_tolerance_ns) {
            return Result<SyncAction>::success(SyncAction::skip);
        }
    }

    return Result<SyncAction>::success(SyncAction::delta);
}

}  // namespace

Result<SyncPlan> build_sync_plan(const Manifest& manifest,
                                 const std::filesystem::path& receive_root,
                                 std::uint32_t block_size) {
    if (block_size == 0) {
        return Result<SyncPlan>::failure(
            make_error(ErrorCode::invalid_argument, "block size must be greater than zero"));
    }

    std::error_code ec;
    const auto root_status = std::filesystem::symlink_status(receive_root, ec);
    if (ec) {
        return Result<SyncPlan>::failure(
            make_error(ErrorCode::io_error,
                       "failed to inspect receive root " + quote_path(receive_root) + ": " + ec.message()));
    }

    if (!std::filesystem::is_directory(root_status)) {
        return Result<SyncPlan>::failure(
            make_error(ErrorCode::invalid_argument, "receive root must be a directory"));
    }

    SyncPlan plan;
    plan.receive_root = receive_root;
    plan.block_size = block_size;

    for (const auto& entry : manifest.files) {
        if (!is_safe_relative_path(entry.relative_path)) {
            return Result<SyncPlan>::failure(
                make_error(ErrorCode::invalid_argument,
                           "manifest contains unsafe relative path: " +
                               entry.relative_path.generic_string()));
        }

        const auto target = receive_root / entry.relative_path;
        auto action = choose_action(entry, target);
        if (!action) {
            return Result<SyncPlan>::failure(action.error());
        }

        SyncPlanEntry plan_entry;
        plan_entry.manifest_entry = entry;
        plan_entry.action = action.value();

        if (plan_entry.action == SyncAction::delta) {
            auto signatures = build_block_signatures(target, block_size);
            if (!signatures) {
                return Result<SyncPlan>::failure(signatures.error());
            }
            plan_entry.basis_signatures = std::move(signatures).value();
        }

        plan.entries.push_back(std::move(plan_entry));
    }

    return Result<SyncPlan>::success(std::move(plan));
}

}  // namespace lan

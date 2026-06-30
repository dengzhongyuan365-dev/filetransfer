#include "lan/transfer/sync_session.h"

#include <string>
#include <system_error>
#include <utility>

#include "lan/fs/file_hash.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/transfer/delta.h"
#include "lan/transfer/manifest.h"
#include "lan/transfer/part_file_guard.h"
#include "lan/transfer/sync_codec.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

Result<bool> send_error_frame(Connection& connection, std::string_view message) {
    Frame frame;
    frame.type = MessageType::error;
    frame.body = bytes_from_string(message);
    return write_frame(connection, frame);
}

Result<bool> send_ack_frame(Connection& connection, std::string_view message) {
    Frame frame;
    frame.type = MessageType::ack;
    frame.body = bytes_from_string(message);
    return write_frame(connection, frame);
}

Result<bool> wait_for_ack(Connection& connection, std::string_view context) {
    auto frame = read_frame(connection);
    if (!frame) {
        return Result<bool>::failure(frame.error());
    }

    if (frame.value().type == MessageType::ack) {
        return Result<bool>::success(true);
    }

    if (frame.value().type == MessageType::error) {
        return Result<bool>::failure(
            make_error(ErrorCode::protocol_error,
                       std::string(context) + ": receiver error: " + body_as_string(frame.value())));
    }

    return Result<bool>::failure(
        make_error(ErrorCode::protocol_error,
                   std::string(context) + ": expected ack or error frame, got " +
                       message_type_name(frame.value().type)));
}

template <typename Report>
void count_actions(Report& report, const SyncPlan& plan) {
    for (const auto& entry : plan.entries) {
        if (entry.action == SyncAction::skip) {
            ++report.skip_files;
        } else if (entry.action == SyncAction::full) {
            ++report.full_files;
        } else if (entry.action == SyncAction::delta) {
            ++report.delta_files;
        }
    }
}

Result<ReceiveSyncNegotiationReport> fail_receiver(Connection& connection, Error error) {
    (void)send_error_frame(connection, error.message);
    return Result<ReceiveSyncNegotiationReport>::failure(std::move(error));
}

Result<ReceiveSyncReport> fail_sync_receiver(Connection& connection, Error error) {
    (void)send_error_frame(connection, error.message);
    return Result<ReceiveSyncReport>::failure(std::move(error));
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

std::filesystem::path part_path_for(const std::filesystem::path& target) {
    auto part = target;
    part += ".part";
    return part;
}

Result<bool> verify_file_hash(const std::filesystem::path& path, const ManifestEntry& entry) {
    auto hash = hash_file(path);
    if (!hash) {
        return Result<bool>::failure(hash.error());
    }

    if (hash.value().hex_digest != entry.sha256) {
        return Result<bool>::failure(
            make_error(ErrorCode::checksum_mismatch,
                       "synced file sha256 does not match manifest: " +
                           entry.relative_path.generic_string()));
    }

    return Result<bool>::success(true);
}

Result<bool> apply_delta_to_target(const std::filesystem::path& receive_root,
                                   const SyncPlanEntry& entry,
                                   const DeltaPlan& delta) {
    const auto target = receive_root / entry.manifest_entry.relative_path;
    const auto part_path = part_path_for(target);
    std::filesystem::create_directories(target.parent_path());
    PartFileGuard part_file(part_path);

    auto applied = apply_delta(target, part_path, delta.ops);
    if (!applied) {
        return Result<bool>::failure(applied.error());
    }

    auto verified = verify_file_hash(part_path, entry.manifest_entry);
    if (!verified) {
        return Result<bool>::failure(verified.error());
    }

    std::error_code ec;
    std::filesystem::rename(part_path, target, ec);
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to rename " + quote_path(part_path) + " to " +
                           quote_path(target) + ": " + ec.message()));
    }

    part_file.commit();
    return Result<bool>::success(true);
}

Result<SyncPlan> send_manifest_and_receive_plan(Connection& connection, const Manifest& manifest) {
    Frame hello;
    hello.type = MessageType::hello;
    hello.body = bytes_from_string("sync");
    auto hello_written = write_frame(connection, hello);
    if (!hello_written) {
        return Result<SyncPlan>::failure(hello_written.error());
    }

    Frame manifest_frame;
    manifest_frame.type = MessageType::manifest;
    manifest_frame.body = encode_manifest(manifest);
    auto manifest_written = write_frame(connection, manifest_frame);
    if (!manifest_written) {
        return Result<SyncPlan>::failure(manifest_written.error());
    }

    auto manifest_ack = wait_for_ack(connection, "manifest");
    if (!manifest_ack) {
        return Result<SyncPlan>::failure(manifest_ack.error());
    }

    auto plan_frame = read_frame(connection);
    if (!plan_frame) {
        return Result<SyncPlan>::failure(plan_frame.error());
    }
    if (plan_frame.value().type != MessageType::sync_plan) {
        return Result<SyncPlan>::failure(
            make_error(ErrorCode::protocol_error, "expected sync_plan frame"));
    }

    auto plan = decode_sync_plan(plan_frame.value().body);
    if (!plan) {
        return Result<SyncPlan>::failure(plan.error());
    }

    auto plan_ack = send_ack_frame(connection, "sync_plan received");
    if (!plan_ack) {
        return Result<SyncPlan>::failure(plan_ack.error());
    }

    return Result<SyncPlan>::success(std::move(plan).value());
}

Result<SyncPlan> receive_manifest_and_send_plan(Connection& connection,
                                                const ReceiverConfig& config,
                                                std::uint32_t block_size,
                                                std::uint64_t& manifest_files) {
    auto manifest_frame = read_frame(connection);
    if (!manifest_frame) {
        return Result<SyncPlan>::failure(manifest_frame.error());
    }
    if (manifest_frame.value().type != MessageType::manifest) {
        return Result<SyncPlan>::failure(
            make_error(ErrorCode::protocol_error, "expected manifest frame"));
    }

    auto manifest = decode_manifest(manifest_frame.value().body);
    if (!manifest) {
        return Result<SyncPlan>::failure(manifest.error());
    }
    manifest_files = manifest.value().files.size();

    auto manifest_ack = send_ack_frame(connection, "manifest received");
    if (!manifest_ack) {
        return Result<SyncPlan>::failure(manifest_ack.error());
    }

    auto plan = build_sync_plan(manifest.value(), config.receive_dir, block_size);
    if (!plan) {
        return Result<SyncPlan>::failure(plan.error());
    }

    Frame plan_frame;
    plan_frame.type = MessageType::sync_plan;
    plan_frame.body = encode_sync_plan(plan.value());
    auto plan_written = write_frame(connection, plan_frame);
    if (!plan_written) {
        return Result<SyncPlan>::failure(plan_written.error());
    }

    auto plan_ack = wait_for_ack(connection, "sync_plan");
    if (!plan_ack) {
        return Result<SyncPlan>::failure(plan_ack.error());
    }

    return Result<SyncPlan>::success(std::move(plan).value());
}

}  // namespace

Result<SendSyncNegotiationReport> negotiate_sync_sender(const SenderConfig& config,
                                                        std::uint32_t block_size) {
    (void)block_size;

    auto manifest = build_manifest(config.source_path);
    if (!manifest) {
        return Result<SendSyncNegotiationReport>::failure(manifest.error());
    }

    auto connection = default_network_backend().connect(config.target.host, config.target.port);
    if (!connection) {
        return Result<SendSyncNegotiationReport>::failure(connection.error());
    }

    auto plan = send_manifest_and_receive_plan(*connection.value(), manifest.value());
    if (!plan) {
        return Result<SendSyncNegotiationReport>::failure(plan.error());
    }

    SendSyncNegotiationReport report;
    report.manifest_files = manifest.value().files.size();
    count_actions(report, plan.value());
    return Result<SendSyncNegotiationReport>::success(report);
}

Result<ReceiveSyncNegotiationReport> negotiate_sync_receiver(const ReceiverConfig& config,
                                                             std::uint32_t block_size) {
    auto listener = default_network_backend().listen(config.bind_address, config.port);
    if (!listener) {
        return Result<ReceiveSyncNegotiationReport>::failure(listener.error());
    }

    auto client = listener.value()->accept();
    if (!client) {
        return Result<ReceiveSyncNegotiationReport>::failure(client.error());
    }

    auto hello = read_frame(*client.value());
    if (!hello) {
        return Result<ReceiveSyncNegotiationReport>::failure(hello.error());
    }
    if (hello.value().type != MessageType::hello || body_as_string(hello.value()) != "sync") {
        return fail_receiver(
            *client.value(),
            make_error(ErrorCode::protocol_error, "expected sync hello frame"));
    }

    std::uint64_t manifest_files = 0;
    auto plan = receive_manifest_and_send_plan(*client.value(), config, block_size, manifest_files);
    if (!plan) {
        return fail_receiver(*client.value(), plan.error());
    }

    ReceiveSyncNegotiationReport report;
    report.manifest_files = manifest_files;
    count_actions(report, plan.value());
    return Result<ReceiveSyncNegotiationReport>::success(report);
}

Result<SendSyncReport> sync_sender(const SenderConfig& config, std::uint32_t block_size) {
    auto manifest = build_manifest(config.source_path);
    if (!manifest) {
        return Result<SendSyncReport>::failure(manifest.error());
    }

    auto connection = default_network_backend().connect(config.target.host, config.target.port);
    if (!connection) {
        return Result<SendSyncReport>::failure(connection.error());
    }

    auto plan = send_manifest_and_receive_plan(*connection.value(), manifest.value());
    if (!plan) {
        return Result<SendSyncReport>::failure(plan.error());
    }

    SendSyncReport report;
    report.manifest_files = manifest.value().files.size();

    for (const auto& entry : plan.value().entries) {
        if (entry.action == SyncAction::skip) {
            ++report.skipped_files;
            continue;
        }

        const auto source = config.source_path / entry.manifest_entry.relative_path;
        auto delta = build_delta(source, entry.basis_signatures, block_size);
        if (!delta) {
            return Result<SendSyncReport>::failure(delta.error());
        }

        Frame delta_frame;
        delta_frame.type = MessageType::delta;
        delta_frame.body = encode_delta_plan(delta.value());
        auto delta_written = write_frame(*connection.value(), delta_frame);
        if (!delta_written) {
            return Result<SendSyncReport>::failure(delta_written.error());
        }

        auto delta_ack = wait_for_ack(*connection.value(), "delta");
        if (!delta_ack) {
            return Result<SendSyncReport>::failure(delta_ack.error());
        }

        ++report.delta_frames_sent;
        if (entry.action == SyncAction::full) {
            ++report.full_files;
        } else {
            ++report.delta_files;
        }
    }

    auto done = send_ack_frame(*connection.value(), "sync done");
    if (!done) {
        return Result<SendSyncReport>::failure(done.error());
    }

    return Result<SendSyncReport>::success(report);
}

Result<ReceiveSyncReport> sync_receiver(const ReceiverConfig& config, std::uint32_t block_size) {
    auto listener = default_network_backend().listen(config.bind_address, config.port);
    if (!listener) {
        return Result<ReceiveSyncReport>::failure(listener.error());
    }

    auto client = listener.value()->accept();
    if (!client) {
        return Result<ReceiveSyncReport>::failure(client.error());
    }

    auto hello = read_frame(*client.value());
    if (!hello) {
        return Result<ReceiveSyncReport>::failure(hello.error());
    }
    if (hello.value().type != MessageType::hello || body_as_string(hello.value()) != "sync") {
        return fail_sync_receiver(
            *client.value(),
            make_error(ErrorCode::protocol_error, "expected sync hello frame"));
    }

    return sync_receiver_from_connection(config, block_size, *client.value(), hello.value());
}

Result<ReceiveSyncReport> sync_receiver_from_connection(const ReceiverConfig& config,
                                                        std::uint32_t block_size,
                                                        Connection& connection,
                                                        const Frame& hello) {
    if (hello.type != MessageType::hello || body_as_string(hello) != "sync") {
        return fail_sync_receiver(
            connection,
            make_error(ErrorCode::protocol_error, "expected sync hello frame"));
    }

    std::uint64_t manifest_files = 0;
    auto plan = receive_manifest_and_send_plan(connection, config, block_size, manifest_files);
    if (!plan) {
        return fail_sync_receiver(connection, plan.error());
    }

    ReceiveSyncReport report;
    report.manifest_files = manifest_files;

    for (const auto& entry : plan.value().entries) {
        if (entry.action == SyncAction::skip) {
            ++report.skipped_files;
            continue;
        }

        auto delta_frame = read_frame(connection);
        if (!delta_frame) {
            return Result<ReceiveSyncReport>::failure(delta_frame.error());
        }
        if (delta_frame.value().type != MessageType::delta) {
            return fail_sync_receiver(
                connection,
                make_error(ErrorCode::protocol_error, "expected delta frame"));
        }

        auto delta = decode_delta_plan(delta_frame.value().body);
        if (!delta) {
            return fail_sync_receiver(connection, delta.error());
        }

        auto applied = apply_delta_to_target(config.receive_dir, entry, delta.value());
        if (!applied) {
            return fail_sync_receiver(connection, applied.error());
        }

        auto ack = send_ack_frame(connection, "delta applied");
        if (!ack) {
            return Result<ReceiveSyncReport>::failure(ack.error());
        }

        ++report.files_written;
        if (entry.action == SyncAction::full) {
            ++report.full_files;
        } else {
            ++report.delta_files;
        }
    }

    auto done = read_frame(connection);
    if (!done) {
        return Result<ReceiveSyncReport>::failure(done.error());
    }
    if (done.value().type != MessageType::ack || body_as_string(done.value()) != "sync done") {
        return fail_sync_receiver(
            connection,
            make_error(ErrorCode::protocol_error, "expected sync done ack"));
    }

    return Result<ReceiveSyncReport>::success(report);
}

}  // namespace lan

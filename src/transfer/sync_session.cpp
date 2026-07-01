#include "lan/transfer/sync_session.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <vector>
#include <utility>

#include "lan/common/stopwatch.h"
#include "lan/fs/file_hash.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/protocol/hello.h"
#include "lan/transfer/delta.h"
#include "lan/transfer/manifest.h"
#include "lan/transfer/part_file_guard.h"
#include "lan/transfer/sync_codec.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

struct DeltaStreamSendReport {
    std::uint64_t payload_bytes_sent = 0;
};

using SendProgressPublisher = std::function<void(std::uint64_t current_file_bytes,
                                                 std::uint64_t current_file_total_bytes)>;
using ReceiveProgressPublisher = std::function<void(std::uint64_t current_file_bytes,
                                                    std::uint64_t current_file_total_bytes,
                                                    std::uint64_t current_file_ops)>;

struct DeltaStreamReceiveReport {
    std::uint64_t payload_bytes_received = 0;
};

constexpr std::uint64_t delta_op_metadata_size = 1 + 8 + 8 + 8;
constexpr std::uint64_t max_delta_data_size = max_frame_body_size - delta_op_metadata_size;

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

Result<bool> verify_file_hash(const std::filesystem::path& path,
                              const ManifestEntry& entry,
                              std::string_view expected_sha256) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to read synced file size " + quote_path(path) + ": " + ec.message()));
    }

    if (size != entry.size) {
        return Result<bool>::failure(
            make_error(ErrorCode::checksum_mismatch,
                       "synced file size does not match manifest: " +
                           entry.relative_path.generic_string()));
    }

    if (expected_sha256.empty()) {
        return Result<bool>::success(true);
    }

    auto hash = hash_file(path);
    if (!hash) {
        return Result<bool>::failure(hash.error());
    }

    if (hash.value().hex_digest != expected_sha256) {
        return Result<bool>::failure(
            make_error(ErrorCode::checksum_mismatch,
                       "synced file sha256 does not match sender delta header: " +
                           entry.relative_path.generic_string()));
    }

    return Result<bool>::success(true);
}

std::filesystem::file_time_type file_time_from_mtime_ns(std::uint64_t mtime_ns) {
    const auto system_time =
        std::chrono::system_clock::time_point(std::chrono::nanoseconds(mtime_ns));
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        system_time - std::chrono::system_clock::now() +
        std::filesystem::file_time_type::clock::now());
}

Result<bool> apply_file_metadata(const std::filesystem::path& target, const ManifestEntry& entry) {
    std::error_code ec;
    std::filesystem::last_write_time(target, file_time_from_mtime_ns(entry.mtime_ns), ec);
    if (ec) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to set mtime on " + quote_path(target) + ": " + ec.message()));
    }

    if (entry.mode != 0 && ::chmod(target.c_str(), static_cast<mode_t>(entry.mode & 07777)) != 0) {
        return Result<bool>::failure(
            make_error(ErrorCode::io_error,
                       "failed to chmod " + quote_path(target) + ": " + std::strerror(errno)));
    }

    return Result<bool>::success(true);
}

Result<DeltaStreamReceiveReport> apply_delta_stream_to_target(
    Connection& connection,
    const std::filesystem::path& receive_root,
    const SyncPlanEntry& entry,
    const ReceiveProgressPublisher& publish_progress) {
    const auto target = receive_root / entry.manifest_entry.relative_path;
    const auto part_path = part_path_for(target);
    std::filesystem::create_directories(target.parent_path());
    PartFileGuard part_file(part_path);

    auto begin_frame = read_frame(connection);
    if (!begin_frame) {
        return Result<DeltaStreamReceiveReport>::failure(begin_frame.error());
    }
    if (begin_frame.value().type != MessageType::delta_begin) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::protocol_error, "expected delta_begin frame"));
    }
    DeltaStreamReceiveReport report;
    report.payload_bytes_received += begin_frame.value().body.size();

    auto header = decode_delta_header(begin_frame.value().body);
    if (!header) {
        return Result<DeltaStreamReceiveReport>::failure(header.error());
    }
    if (header.value().source_size != entry.manifest_entry.size) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::protocol_error, "delta header size does not match sync plan entry"));
    }
    if (!entry.manifest_entry.sha256.empty() &&
        header.value().source_sha256 != entry.manifest_entry.sha256) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::protocol_error, "delta header hash does not match sync plan entry"));
    }

    std::ifstream basis(target, std::ios::binary);
    std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::io_error, "failed to open " + quote_path(part_path)));
    }

    std::uint64_t ops_received = 0;
    std::uint64_t expected_ops = 0;
    std::uint64_t bytes_written = 0;
    if (publish_progress) {
        publish_progress(bytes_written, entry.manifest_entry.size, ops_received);
    }
    while (true) {
        auto frame = read_frame(connection);
        if (!frame) {
            return Result<DeltaStreamReceiveReport>::failure(frame.error());
        }

        if (frame.value().type == MessageType::delta_end) {
            report.payload_bytes_received += frame.value().body.size();
            auto end = decode_delta_end(frame.value().body);
            if (!end) {
                return Result<DeltaStreamReceiveReport>::failure(end.error());
            }
            expected_ops = end.value().op_count;
            break;
        }

        if (frame.value().type != MessageType::delta) {
            return Result<DeltaStreamReceiveReport>::failure(
                make_error(ErrorCode::protocol_error, "expected delta or delta_end frame"));
        }
        report.payload_bytes_received += frame.value().body.size();

        auto op = decode_delta_op(frame.value().body);
        if (!op) {
            return Result<DeltaStreamReceiveReport>::failure(op.error());
        }

        auto applied = apply_delta_op(basis, output, op.value());
        if (!applied) {
            return Result<DeltaStreamReceiveReport>::failure(applied.error());
        }
        ++ops_received;
        bytes_written += op.value().size;
        if (publish_progress) {
            publish_progress(bytes_written, entry.manifest_entry.size, ops_received);
        }
    }

    if (ops_received != expected_ops) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::protocol_error, "delta op count does not match delta end"));
    }

    output.close();
    if (!output) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::io_error, "failed to close " + quote_path(part_path)));
    }

    auto verified = verify_file_hash(part_path, entry.manifest_entry, header.value().source_sha256);
    if (!verified) {
        return Result<DeltaStreamReceiveReport>::failure(verified.error());
    }

    std::error_code ec;
    std::filesystem::rename(part_path, target, ec);
    if (ec) {
        return Result<DeltaStreamReceiveReport>::failure(
            make_error(ErrorCode::io_error,
                       "failed to rename " + quote_path(part_path) + " to " +
                           quote_path(target) + ": " + ec.message()));
    }

    part_file.commit();
    auto metadata = apply_file_metadata(target, entry.manifest_entry);
    if (!metadata) {
        return Result<DeltaStreamReceiveReport>::failure(metadata.error());
    }

    return Result<DeltaStreamReceiveReport>::success(report);
}

Result<DeltaStreamSendReport> send_full_stream(Connection& connection,
                                               const std::filesystem::path& source,
                                               const ManifestEntry& entry,
                                               std::uint32_t chunk_size,
                                               const CancellationToken& cancellation,
                                               const SendProgressPublisher& publish_progress) {
    if (chunk_size == 0) {
        return Result<DeltaStreamSendReport>::failure(
            make_error(ErrorCode::invalid_argument, "block size must be greater than zero"));
    }
    if (chunk_size > max_delta_data_size) {
        return Result<DeltaStreamSendReport>::failure(
            make_error(ErrorCode::invalid_argument, "block size exceeds max frame body size"));
    }

    std::ifstream input(source, std::ios::binary);
    if (!input) {
        return Result<DeltaStreamSendReport>::failure(
            make_error(ErrorCode::io_error, "failed to open " + quote_path(source)));
    }

    DeltaStreamSendReport report;
    DeltaPlan header;
    header.source_size = entry.size;
    header.source_sha256 = entry.sha256;

    Frame begin;
    begin.type = MessageType::delta_begin;
    begin.body = encode_delta_header(header);
    report.payload_bytes_sent += begin.body.size();
    auto begin_written = write_frame(connection, begin);
    if (!begin_written) {
        return Result<DeltaStreamSendReport>::failure(begin_written.error());
    }

    std::vector<std::byte> buffer(chunk_size);
    std::uint64_t bytes_sent = 0;
    std::uint32_t ops_sent = 0;
    if (publish_progress) {
        publish_progress(bytes_sent, entry.size);
    }

    while (bytes_sent < entry.size) {
        if (cancellation.is_cancelled()) {
            return Result<DeltaStreamSendReport>::failure(
                make_error(ErrorCode::cancelled, "sync transfer cancelled"));
        }

        const auto remaining = entry.size - bytes_sent;
        const auto want = std::min<std::uint64_t>(buffer.size(), remaining);
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
        const auto got = input.gcount();
        if (got <= 0) {
            return Result<DeltaStreamSendReport>::failure(
                make_error(ErrorCode::io_error, "failed to read " + quote_path(source)));
        }
        if (ops_sent == std::numeric_limits<std::uint32_t>::max()) {
            return Result<DeltaStreamSendReport>::failure(
                make_error(ErrorCode::invalid_argument, "full stream has too many chunks"));
        }

        DeltaOp op;
        op.type = DeltaOpType::literal_data;
        const auto bytes_read = static_cast<std::size_t>(got);
        op.size = static_cast<std::uint64_t>(bytes_read);
        op.data.assign(buffer.begin(), buffer.begin() + bytes_read);

        Frame frame;
        frame.type = MessageType::delta;
        frame.body = encode_delta_op(op);
        report.payload_bytes_sent += frame.body.size();
        auto written = write_frame(connection, frame);
        if (!written) {
            return Result<DeltaStreamSendReport>::failure(written.error());
        }

        bytes_sent += static_cast<std::uint64_t>(got);
        if (publish_progress) {
            publish_progress(bytes_sent, entry.size);
        }
        ++ops_sent;
    }

    DeltaPlan end_plan;
    end_plan.op_count = ops_sent;

    Frame end;
    end.type = MessageType::delta_end;
    end.body = encode_delta_end(end_plan);
    report.payload_bytes_sent += end.body.size();
    auto end_written = write_frame(connection, end);
    if (!end_written) {
        return Result<DeltaStreamSendReport>::failure(end_written.error());
    }

    return Result<DeltaStreamSendReport>::success(report);
}

Result<DeltaStreamSendReport> send_streaming_delta(Connection& connection,
                                                   const std::filesystem::path& source,
                                                   const ManifestEntry& entry,
                                                   const std::vector<BlockSignature>& basis_signatures,
                                                   std::uint32_t block_size,
                                                   const CancellationToken& cancellation,
                                                   const SendProgressPublisher& publish_progress) {
    if (block_size == 0) {
        return Result<DeltaStreamSendReport>::failure(
            make_error(ErrorCode::invalid_argument, "block size must be greater than zero"));
    }
    if (block_size > max_delta_data_size) {
        return Result<DeltaStreamSendReport>::failure(
            make_error(ErrorCode::invalid_argument, "block size exceeds max frame body size"));
    }

    DeltaStreamSendReport report;
    DeltaPlan header;
    header.source_size = entry.size;
    header.source_sha256 = entry.sha256;

    Frame begin;
    begin.type = MessageType::delta_begin;
    begin.body = encode_delta_header(header);
    report.payload_bytes_sent += begin.body.size();
    auto begin_written = write_frame(connection, begin);
    if (!begin_written) {
        return Result<DeltaStreamSendReport>::failure(begin_written.error());
    }

    std::uint64_t bytes_processed = 0;
    if (publish_progress) {
        publish_progress(bytes_processed, entry.size);
    }

    auto op_count = stream_delta_ops(
        source,
        basis_signatures,
        block_size,
        [&](const DeltaOp& op) -> Result<bool> {
            if (cancellation.is_cancelled()) {
                return Result<bool>::failure(
                    make_error(ErrorCode::cancelled, "sync transfer cancelled"));
            }

            Frame frame;
            frame.type = MessageType::delta;
            frame.body = encode_delta_op(op);
            report.payload_bytes_sent += frame.body.size();
            auto written = write_frame(connection, frame);
            if (!written) {
                return Result<bool>::failure(written.error());
            }

            bytes_processed += op.size;
            if (publish_progress) {
                publish_progress(bytes_processed, entry.size);
            }
            return Result<bool>::success(true);
        });
    if (!op_count) {
        return Result<DeltaStreamSendReport>::failure(op_count.error());
    }

    DeltaPlan end_plan;
    end_plan.op_count = op_count.value();

    Frame end;
    end.type = MessageType::delta_end;
    end.body = encode_delta_end(end_plan);
    report.payload_bytes_sent += end.body.size();
    auto end_written = write_frame(connection, end);
    if (!end_written) {
        return Result<DeltaStreamSendReport>::failure(end_written.error());
    }

    return Result<DeltaStreamSendReport>::success(report);
}

Result<SyncPlan> send_manifest_and_receive_plan(Connection& connection, const Manifest& manifest) {
    Frame hello;
    hello.type = MessageType::hello;
    hello.body = encode_hello(HelloMetadata{.mode = HelloMode::sync});
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

    auto receive_root = config.receive_dir;
    if (!manifest.value().root_name.empty()) {
        receive_root /= manifest.value().root_name;
        std::error_code ec;
        std::filesystem::create_directories(receive_root, ec);
        if (ec) {
            return Result<SyncPlan>::failure(
                make_error(ErrorCode::io_error,
                           "failed to create receive root " + quote_path(receive_root) +
                               ": " + ec.message()));
        }
    }

    auto plan = build_sync_plan(manifest.value(), receive_root, block_size);
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
    auto hello_metadata = decode_hello_frame(hello.value());
    if (!hello_metadata) {
        return fail_receiver(*client.value(), hello_metadata.error());
    }
    if (hello_metadata.value().mode != HelloMode::sync) {
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

Result<SendSyncReport> sync_sender_to_connection(const SenderConfig& config,
                                                 std::uint32_t block_size,
                                                 Connection& connection) {
    CancellationToken cancellation;
    return sync_sender_to_connection(
        config, block_size, connection, SendSyncProgressCallback{}, cancellation);
}

Result<SendSyncReport> sync_sender_to_connection(const SenderConfig& config,
                                                 std::uint32_t block_size,
                                                 Connection& connection,
                                                 const CancellationToken& cancellation) {
    return sync_sender_to_connection(
        config, block_size, connection, SendSyncProgressCallback{}, cancellation);
}

Result<SendSyncReport> sync_sender_to_connection(const SenderConfig& config,
                                                 std::uint32_t block_size,
                                                 Connection& connection,
                                                 SendSyncProgressCallback on_progress) {
    CancellationToken cancellation;
    return sync_sender_to_connection(
        config, block_size, connection, std::move(on_progress), cancellation);
}

Result<SendSyncReport> sync_sender_to_connection(const SenderConfig& config,
                                                 std::uint32_t block_size,
                                                 Connection& connection,
                                                 SendSyncProgressCallback on_progress,
                                                 const CancellationToken& cancellation) {
    (void)block_size;

    Stopwatch transfer_timer;
    auto manifest = build_manifest(
        config.source_path,
        1024 * 1024,
        [&](const ManifestProgress& progress) {
            if (on_progress) {
                on_progress(SendSyncProgress{
                    .manifest_scanned_files = progress.files,
                    .manifest_scanned_bytes = progress.bytes,
                    .processed_files = progress.files,
                    .current_file = {},
                    .current_action = SyncAction::skip,
                    .current_file_bytes = 0,
                    .current_file_total_bytes = 0,
                    .elapsed_seconds = transfer_timer.elapsed_seconds(),
                });
            }
        },
        &cancellation);
    if (!manifest) {
        return Result<SendSyncReport>::failure(manifest.error());
    }

    auto plan = send_manifest_and_receive_plan(connection, manifest.value());
    if (!plan) {
        return Result<SendSyncReport>::failure(plan.error());
    }

    SendSyncReport report;
    report.manifest_files = manifest.value().files.size();
    report.block_size = plan.value().block_size;
    std::uint64_t processed_files = 0;
    std::filesystem::path current_file;
    SyncAction current_action = SyncAction::skip;
    std::uint64_t current_file_bytes = 0;
    std::uint64_t current_file_total_bytes = 0;
    auto publish_progress = [&] {
        if (on_progress) {
            on_progress(SendSyncProgress{
                .manifest_files = report.manifest_files,
                .processed_files = processed_files,
                .current_file = current_file,
                .current_action = current_action,
                .current_file_bytes = current_file_bytes,
                .current_file_total_bytes = current_file_total_bytes,
                .skipped_files = report.skipped_files,
                .full_files = report.full_files,
                .delta_files = report.delta_files,
                .delta_frames_sent = report.delta_frames_sent,
                .delta_payload_bytes_sent = report.delta_payload_bytes_sent,
                .block_size = report.block_size,
                .elapsed_seconds = transfer_timer.elapsed_seconds(),
            });
        }
    };
    publish_progress();

    for (const auto& entry : plan.value().entries) {
        if (cancellation.is_cancelled()) {
            return Result<SendSyncReport>::failure(
                make_error(ErrorCode::cancelled, "sync transfer cancelled"));
        }

        if (entry.action == SyncAction::skip) {
            current_file = entry.manifest_entry.relative_path;
            current_action = entry.action;
            current_file_bytes = entry.manifest_entry.size;
            current_file_total_bytes = entry.manifest_entry.size;
            ++report.skipped_files;
            ++processed_files;
            publish_progress();
            continue;
        }

        const auto source = config.source_path / entry.manifest_entry.relative_path;
        current_file = entry.manifest_entry.relative_path;
        current_action = entry.action;
        current_file_bytes = 0;
        current_file_total_bytes = entry.manifest_entry.size;
        publish_progress();

        Result<DeltaStreamSendReport> delta_written =
            Result<DeltaStreamSendReport>::failure(
                make_error(ErrorCode::internal_error, "delta stream did not run"));
        if (entry.action == SyncAction::full) {
            delta_written = send_full_stream(
                connection,
                source,
                entry.manifest_entry,
                plan.value().block_size,
                cancellation,
                [&](std::uint64_t bytes, std::uint64_t total) {
                    current_file_bytes = bytes;
                    current_file_total_bytes = total;
                    publish_progress();
                });
        } else {
            delta_written = send_streaming_delta(
                connection,
                source,
                entry.manifest_entry,
                entry.basis_signatures,
                plan.value().block_size,
                cancellation,
                [&](std::uint64_t bytes, std::uint64_t total) {
                    current_file_bytes = bytes;
                    current_file_total_bytes = total;
                    publish_progress();
                });
        }
        if (!delta_written) {
            return Result<SendSyncReport>::failure(delta_written.error());
        }
        report.delta_payload_bytes_sent += delta_written.value().payload_bytes_sent;

        auto delta_ack = wait_for_ack(connection, "delta");
        if (!delta_ack) {
            return Result<SendSyncReport>::failure(delta_ack.error());
        }

        ++report.delta_frames_sent;
        if (entry.action == SyncAction::full) {
            ++report.full_files;
        } else {
            ++report.delta_files;
        }
        ++processed_files;
        publish_progress();
    }

    auto done = send_ack_frame(connection, "sync done");
    if (!done) {
        return Result<SendSyncReport>::failure(done.error());
    }

    report.elapsed_seconds = transfer_timer.elapsed_seconds();
    return Result<SendSyncReport>::success(report);
}

Result<SendSyncReport> sync_sender(const SenderConfig& config, std::uint32_t block_size) {
    CancellationToken cancellation;
    return sync_sender(config, block_size, SendSyncProgressCallback{}, cancellation);
}

Result<SendSyncReport> sync_sender(const SenderConfig& config,
                                   std::uint32_t block_size,
                                   const CancellationToken& cancellation) {
    return sync_sender(config, block_size, SendSyncProgressCallback{}, cancellation);
}

Result<SendSyncReport> sync_sender(const SenderConfig& config,
                                   std::uint32_t block_size,
                                   SendSyncProgressCallback on_progress) {
    CancellationToken cancellation;
    return sync_sender(config, block_size, std::move(on_progress), cancellation);
}

Result<SendSyncReport> sync_sender(const SenderConfig& config,
                                   std::uint32_t block_size,
                                   SendSyncProgressCallback on_progress,
                                   const CancellationToken& cancellation) {
    auto connection = default_network_backend().connect(config.target.host, config.target.port);
    if (!connection) {
        return Result<SendSyncReport>::failure(connection.error());
    }

    return sync_sender_to_connection(
        config, block_size, *connection.value(), std::move(on_progress), cancellation);
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
    auto hello_metadata = decode_hello_frame(hello.value());
    if (!hello_metadata) {
        return fail_sync_receiver(*client.value(), hello_metadata.error());
    }
    if (hello_metadata.value().mode != HelloMode::sync) {
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
    return sync_receiver_from_connection(config, block_size, connection, hello, {});
}

Result<ReceiveSyncReport> sync_receiver_from_connection(
    const ReceiverConfig& config,
    std::uint32_t block_size,
    Connection& connection,
    const Frame& hello,
    ReceiveSyncProgressCallback on_progress) {
    auto hello_metadata = decode_hello_frame(hello);
    if (!hello_metadata) {
        return fail_sync_receiver(connection, hello_metadata.error());
    }
    if (hello_metadata.value().mode != HelloMode::sync) {
        return fail_sync_receiver(
            connection,
            make_error(ErrorCode::protocol_error, "expected sync hello frame"));
    }

    Stopwatch transfer_timer;
    std::uint64_t manifest_files = 0;
    auto plan = receive_manifest_and_send_plan(connection, config, block_size, manifest_files);
    if (!plan) {
        return fail_sync_receiver(connection, plan.error());
    }

    ReceiveSyncReport report;
    report.target_root = plan.value().receive_root;
    report.manifest_files = manifest_files;
    report.block_size = plan.value().block_size;
    std::uint64_t processed_files = 0;
    std::filesystem::path current_file;
    SyncAction current_action = SyncAction::skip;
    std::uint64_t current_file_bytes = 0;
    std::uint64_t current_file_total_bytes = 0;
    std::uint64_t current_file_ops = 0;
    auto publish_progress = [&] {
        if (on_progress) {
            on_progress(ReceiveSyncProgress{
                .manifest_files = report.manifest_files,
                .processed_files = processed_files,
                .current_file = current_file,
                .current_action = current_action,
                .current_file_bytes = current_file_bytes,
                .current_file_total_bytes = current_file_total_bytes,
                .current_file_ops = current_file_ops,
                .skipped_files = report.skipped_files,
                .full_files = report.full_files,
                .delta_files = report.delta_files,
                .files_written = report.files_written,
                .delta_payload_bytes_received = report.delta_payload_bytes_received,
                .block_size = report.block_size,
                .elapsed_seconds = transfer_timer.elapsed_seconds(),
            });
        }
    };
    publish_progress();

    for (const auto& entry : plan.value().entries) {
        current_file = entry.manifest_entry.relative_path;
        current_action = entry.action;
        current_file_bytes = 0;
        current_file_total_bytes = entry.manifest_entry.size;
        current_file_ops = 0;

        if (entry.action == SyncAction::skip) {
            current_file_bytes = entry.manifest_entry.size;
            ++report.skipped_files;
            ++processed_files;
            publish_progress();
            continue;
        }

        publish_progress();
        auto applied = apply_delta_stream_to_target(
            connection,
            plan.value().receive_root,
            entry,
            [&](std::uint64_t bytes, std::uint64_t total, std::uint64_t ops) {
                current_file_bytes = bytes;
                current_file_total_bytes = total;
                current_file_ops = ops;
                publish_progress();
            });
        if (!applied) {
            return fail_sync_receiver(connection, applied.error());
        }
        report.delta_payload_bytes_received += applied.value().payload_bytes_received;

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
        ++processed_files;
        publish_progress();
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

    report.elapsed_seconds = transfer_timer.elapsed_seconds();
    return Result<ReceiveSyncReport>::success(report);
}

}  // namespace lan

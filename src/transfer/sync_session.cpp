#include "lan/transfer/sync_session.h"

#include <string>
#include <utility>

#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/transfer/manifest.h"
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

    Frame hello;
    hello.type = MessageType::hello;
    hello.body = bytes_from_string("sync");
    auto hello_written = write_frame(*connection.value(), hello);
    if (!hello_written) {
        return Result<SendSyncNegotiationReport>::failure(hello_written.error());
    }

    Frame manifest_frame;
    manifest_frame.type = MessageType::manifest;
    manifest_frame.body = encode_manifest(manifest.value());
    auto manifest_written = write_frame(*connection.value(), manifest_frame);
    if (!manifest_written) {
        return Result<SendSyncNegotiationReport>::failure(manifest_written.error());
    }

    auto manifest_ack = wait_for_ack(*connection.value(), "manifest");
    if (!manifest_ack) {
        return Result<SendSyncNegotiationReport>::failure(manifest_ack.error());
    }

    auto plan_frame = read_frame(*connection.value());
    if (!plan_frame) {
        return Result<SendSyncNegotiationReport>::failure(plan_frame.error());
    }
    if (plan_frame.value().type != MessageType::sync_plan) {
        return Result<SendSyncNegotiationReport>::failure(
            make_error(ErrorCode::protocol_error, "expected sync_plan frame"));
    }

    auto plan = decode_sync_plan(plan_frame.value().body);
    if (!plan) {
        return Result<SendSyncNegotiationReport>::failure(plan.error());
    }

    auto plan_ack = send_ack_frame(*connection.value(), "sync_plan received");
    if (!plan_ack) {
        return Result<SendSyncNegotiationReport>::failure(plan_ack.error());
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

    auto manifest_frame = read_frame(*client.value());
    if (!manifest_frame) {
        return Result<ReceiveSyncNegotiationReport>::failure(manifest_frame.error());
    }
    if (manifest_frame.value().type != MessageType::manifest) {
        return fail_receiver(
            *client.value(),
            make_error(ErrorCode::protocol_error, "expected manifest frame"));
    }

    auto manifest = decode_manifest(manifest_frame.value().body);
    if (!manifest) {
        return fail_receiver(*client.value(), manifest.error());
    }

    auto manifest_ack = send_ack_frame(*client.value(), "manifest received");
    if (!manifest_ack) {
        return Result<ReceiveSyncNegotiationReport>::failure(manifest_ack.error());
    }

    auto plan = build_sync_plan(manifest.value(), config.receive_dir, block_size);
    if (!plan) {
        return fail_receiver(*client.value(), plan.error());
    }

    Frame plan_frame;
    plan_frame.type = MessageType::sync_plan;
    plan_frame.body = encode_sync_plan(plan.value());
    auto plan_written = write_frame(*client.value(), plan_frame);
    if (!plan_written) {
        return Result<ReceiveSyncNegotiationReport>::failure(plan_written.error());
    }

    auto plan_ack = wait_for_ack(*client.value(), "sync_plan");
    if (!plan_ack) {
        return Result<ReceiveSyncNegotiationReport>::failure(plan_ack.error());
    }

    ReceiveSyncNegotiationReport report;
    report.manifest_files = manifest.value().files.size();
    count_actions(report, plan.value());
    return Result<ReceiveSyncNegotiationReport>::success(report);
}

}  // namespace lan

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/common/cancellation.h"
#include "lan/common/result.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"

namespace lan {

enum class FileTransferStatus {
    transferred,
    resumed,
    skipped,
};

struct SendFileReport {
    std::string file_name;
    std::uint64_t bytes_sent = 0;
    std::string sha256;
    double elapsed_seconds = 0.0;
    FileTransferStatus status = FileTransferStatus::transferred;
    std::uint64_t resumed_from = 0;
};

struct SendFileProgress {
    std::filesystem::path source_path;
    std::string file_name;
    std::uint64_t bytes_sent = 0;
    std::uint64_t total_bytes = 0;
    double elapsed_seconds = 0.0;
};

using SendFileProgressCallback = std::function<void(const SendFileProgress&)>;

struct ReceiveFileReport {
    std::filesystem::path target_path;
    std::uint64_t bytes_received = 0;
    std::string sha256;
    double elapsed_seconds = 0.0;
    FileTransferStatus status = FileTransferStatus::transferred;
    std::uint64_t resumed_from = 0;
};

struct ReceiveFileProgress {
    std::filesystem::path target_path;
    std::string file_name;
    std::uint64_t bytes_received = 0;
    std::uint64_t total_bytes = 0;
    double elapsed_seconds = 0.0;
};

using ReceiveFileProgressCallback = std::function<void(const ReceiveFileProgress&)>;

Result<SendFileReport> send_single_file(const SenderConfig& config);
Result<SendFileReport> send_single_file(const SenderConfig& config,
                                        const CancellationToken& cancellation);
Result<SendFileReport> send_single_file(const SenderConfig& config,
                                        SendFileProgressCallback on_progress);
Result<SendFileReport> send_single_file(const SenderConfig& config,
                                        SendFileProgressCallback on_progress,
                                        const CancellationToken& cancellation);
Result<SendFileReport> send_single_file_to_connection(const SenderConfig& config,
                                                       Connection& connection);
Result<SendFileReport> send_single_file_to_connection(const SenderConfig& config,
                                                       Connection& connection,
                                                       const CancellationToken& cancellation);
Result<SendFileReport> send_single_file_to_connection(
    const SenderConfig& config,
    Connection& connection,
    SendFileProgressCallback on_progress);
Result<SendFileReport> send_single_file_to_connection(
    const SenderConfig& config,
    Connection& connection,
    SendFileProgressCallback on_progress,
    const CancellationToken& cancellation);
Result<ReceiveFileReport> receive_single_file(const ReceiverConfig& config);
Result<ReceiveFileReport> receive_single_file_from_connection(const ReceiverConfig& config,
                                                              Connection& connection,
                                                              const Frame& hello);
Result<ReceiveFileReport> receive_single_file_from_connection(
    const ReceiverConfig& config,
    Connection& connection,
    const Frame& hello,
    ReceiveFileProgressCallback on_progress);

}  // namespace lan

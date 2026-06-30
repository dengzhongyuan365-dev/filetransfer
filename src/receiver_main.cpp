#include <iostream>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <utility>

#include "lan/app/receiver_config.h"
#include "lan/app/receiver_server.h"
#include "lan/common/error.h"
#include "lan/common/size.h"

namespace {

std::string_view file_status_label(lan::FileTransferStatus status) {
    switch (status) {
        case lan::FileTransferStatus::transferred:
            return "received file";
        case lan::FileTransferStatus::resumed:
            return "resumed file";
        case lan::FileTransferStatus::skipped:
            return "skipped identical file";
    }

    return "received file";
}

class ConsoleReceiverEvents final : public lan::ReceiverServerEvents {
public:
    void on_listening(const lan::ReceiverConfig& config) override {
        std::cout << (config.once ? "waiting for one transfer...\n"
                                  : "listening for transfers...\n");
        std::cout.flush();
    }

    void on_file_progress(const lan::ReceiveFileProgress& progress) override {
        file_progress_seen_ = true;
        std::cerr << '\r' << "receiving " << progress.file_name << "  "
                  << lan::format_size(progress.bytes_received) << " / "
                  << lan::format_size(progress.total_bytes) << "  "
                  << lan::format_rate(progress.bytes_received, progress.elapsed_seconds);
        std::cerr.flush();
    }

    void on_file_received(const lan::ReceiveFileReport& report) override {
        finish_file_progress_line();
        std::cout << file_status_label(report.status) << '\n';
        std::cout << "  path: " << report.target_path.string() << '\n';
        std::cout << "  size: " << lan::format_size(report.bytes_received) << '\n';
        if (report.status == lan::FileTransferStatus::resumed) {
            std::cout << "  resumed from: " << lan::format_size(report.resumed_from) << '\n';
        }
        std::cout << "  elapsed: " << report.elapsed_seconds << " s\n";
        std::cout << "  average speed: "
                  << lan::format_rate(report.bytes_received, report.elapsed_seconds) << '\n';
        std::cout << "  sha256: " << report.sha256 << '\n';
    }

    void on_directory_progress(const lan::ReceiveSyncProgress& progress) override {
        directory_progress_seen_ = true;
        std::cerr << '\r' << "syncing files " << progress.processed_files << " / "
                  << progress.manifest_files << "  skipped " << progress.skipped_files
                  << "  full " << progress.full_files << "  delta " << progress.delta_files
                  << "  written " << progress.files_written
                  << "  current " << progress.current_file.generic_string()
                  << "  " << lan::format_size(progress.current_file_bytes) << " / "
                  << lan::format_size(progress.current_file_total_bytes)
                  << "  ops " << progress.current_file_ops
                  << "  payload " << lan::format_size(progress.delta_payload_bytes_received)
                  << "  elapsed " << progress.elapsed_seconds << " s";
        std::cerr.flush();
    }

    void on_directory_synced(const lan::ReceiveSyncReport& synced) override {
        finish_directory_progress_line();
        std::cout << "synced directory\n";
        std::cout << "  files: " << synced.manifest_files << '\n';
        std::cout << "  skipped: " << synced.skipped_files << '\n';
        std::cout << "  full: " << synced.full_files << '\n';
        std::cout << "  delta: " << synced.delta_files << '\n';
        std::cout << "  files written: " << synced.files_written << '\n';
        std::cout << "  delta payload received: "
                  << lan::format_size(synced.delta_payload_bytes_received) << '\n';
        std::cout << "  block size: " << lan::format_size(synced.block_size) << '\n';
        std::cout << "  elapsed: " << synced.elapsed_seconds << " s\n";
    }

    void on_client_error(const lan::Error& error) override {
        finish_file_progress_line();
        finish_directory_progress_line();
        std::cerr << lan::format_error(error) << '\n';
    }

private:
    void finish_file_progress_line() {
        if (file_progress_seen_) {
            std::cerr << '\n';
            file_progress_seen_ = false;
        }
    }

    void finish_directory_progress_line() {
        if (directory_progress_seen_) {
            std::cerr << '\n';
            directory_progress_seen_ = false;
        }
    }

    bool file_progress_seen_ = false;
    bool directory_progress_seen_ = false;
};

class SignalStopper {
public:
    SignalStopper(lan::ReceiverServer& server, bool enabled)
        : server_(server), enabled_(enabled) {}

    SignalStopper(const SignalStopper&) = delete;
    SignalStopper& operator=(const SignalStopper&) = delete;

    ~SignalStopper() {
        stop_waiting();
    }

    bool start() {
        if (!enabled_) {
            return true;
        }

        ::sigemptyset(&signals_);
        ::sigaddset(&signals_, SIGINT);
        ::sigaddset(&signals_, SIGTERM);
        ::sigaddset(&signals_, SIGUSR1);

        const int mask_result = ::pthread_sigmask(SIG_BLOCK, &signals_, nullptr);
        if (mask_result != 0) {
            std::cerr << "failed to block receiver signals: " << std::strerror(mask_result) << '\n';
            return false;
        }

        thread_ = std::thread([this] {
            int signal_number = 0;
            const int wait_result = ::sigwait(&signals_, &signal_number);
            if (wait_result != 0 || signal_number == SIGUSR1) {
                return;
            }

            std::cerr << "\nreceived signal " << signal_number << ", stopping receiver...\n";
            server_.stop();
        });

        return true;
    }

    void stop_waiting() {
        if (!thread_.joinable()) {
            return;
        }

        (void)::pthread_kill(thread_.native_handle(), SIGUSR1);
        thread_.join();
    }

private:
    lan::ReceiverServer& server_;
    bool enabled_ = false;
    sigset_t signals_{};
    std::thread thread_;
};

}  // namespace

int main(int argc, char* argv[]) {
    auto result = lan::parse_receiver_args(argc, argv);
    if (!result) {
        std::cerr << lan::format_error(result.error()) << '\n';
        std::cerr << lan::receiver_usage();
        return 1;
    }

    auto config = std::move(result).value();
    if (config.show_help) {
        std::cout << lan::receiver_usage();
        return 0;
    }

    auto validated = lan::validate_receiver_config(std::move(config));
    if (!validated) {
        std::cerr << lan::format_error(validated.error()) << '\n';
        return 1;
    }

    const auto& final_config = validated.value();

    std::cout << "receiver config\n";
    std::cout << "  bind: " << final_config.bind_address << '\n';
    std::cout << "  port: " << final_config.port << '\n';
    std::cout << "  dir: " << final_config.receive_dir.string() << '\n';
    std::cout << "  allow overwrite: " << (final_config.allow_overwrite ? "true" : "false") << '\n';
    std::cout << "  once: " << (final_config.once ? "true" : "false") << '\n';
    std::cout << "  block size: " << lan::format_size(final_config.block_size) << " ("
              << final_config.block_size << " bytes)\n";

    ConsoleReceiverEvents events;
    lan::ReceiverServer server;
    SignalStopper signal_stopper(server, !final_config.once);
    if (!signal_stopper.start()) {
        return 1;
    }

    auto served = server.run(final_config, events);
    signal_stopper.stop_waiting();
    if (!served) {
        std::cerr << served.error().message << '\n';
        return 1;
    }

    return 0;
}

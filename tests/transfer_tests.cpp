#include <gtest/gtest.h>

#include <cstddef>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "lan/app/receiver_config.h"
#include "lan/app/receiver_server.h"
#include "lan/app/sender_config.h"
#include "lan/app/sender_transfer.h"
#include "lan/app/transfer_snapshot.h"
#include "lan/common/error.h"
#include "lan/fs/file_hash.h"
#include "lan/net/connection.h"
#include "lan/protocol/frame.h"
#include "lan/protocol/hello.h"
#include "lan/transfer/block_signature.h"
#include "lan/transfer/chunk_codec.h"
#include "lan/transfer/delta.h"
#include "lan/transfer/file_metadata.h"
#include "lan/transfer/manifest.h"
#include "lan/transfer/sync_codec.h"
#include "lan/transfer/sync_executor.h"
#include "lan/transfer/sync_plan.h"
#include "lan/transfer/sync_session.h"

namespace {

class TempDir {
public:
    explicit TempDir(std::string name)
        : path_(std::filesystem::temp_directory_path() /
                (std::move(name) + "-" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

TEST(TransferStateTest, NamesAndValidatesTransitions) {
    EXPECT_EQ(lan::transfer_state_name(lan::TransferState::pending), "pending");
    EXPECT_EQ(lan::transfer_state_name(lan::TransferState::running), "running");
    EXPECT_EQ(lan::transfer_state_name(lan::TransferState::completed), "completed");

    EXPECT_TRUE(lan::can_transition(lan::TransferState::pending, lan::TransferState::running));
    EXPECT_TRUE(lan::can_transition(lan::TransferState::pending, lan::TransferState::cancelled));
    EXPECT_TRUE(lan::can_transition(lan::TransferState::running, lan::TransferState::completed));
    EXPECT_TRUE(lan::can_transition(lan::TransferState::running, lan::TransferState::failed));
    EXPECT_TRUE(lan::can_transition(lan::TransferState::running, lan::TransferState::cancelled));

    EXPECT_FALSE(lan::can_transition(lan::TransferState::completed, lan::TransferState::running));
    EXPECT_FALSE(lan::can_transition(lan::TransferState::failed, lan::TransferState::running));
    EXPECT_FALSE(lan::can_transition(lan::TransferState::cancelled, lan::TransferState::running));
}

TEST(TransferSnapshotTrackerTest, TracksSuccessfulFileTransfer) {
    lan::TransferSnapshotTracker tracker;

    EXPECT_FALSE(tracker.snapshot());
    ASSERT_TRUE(tracker.apply(lan::TransferStarted{
        .transfer_id = 7,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/source.txt",
        .name = "source.txt",
    }));

    ASSERT_TRUE(tracker.snapshot());
    EXPECT_EQ(tracker.snapshot()->state, lan::TransferState::running);
    EXPECT_EQ(tracker.snapshot()->transfer_id, 7);

    ASSERT_TRUE(tracker.apply(lan::TransferProgress{
        .transfer_id = 7,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/source.txt",
        .name = "source.txt",
        .current_bytes = 3,
        .total_bytes = 6,
        .elapsed_seconds = 1.0,
    }));
    EXPECT_EQ(tracker.snapshot()->current_bytes, 3);
    EXPECT_EQ(tracker.snapshot()->total_bytes, 6);

    ASSERT_TRUE(tracker.apply(lan::TransferCompleted{
        .transfer_id = 7,
        .state = lan::TransferState::completed,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/source.txt",
        .name = "source.txt",
        .bytes = 6,
        .status = lan::TransferCompletionStatus::transferred,
        .elapsed_seconds = 2.0,
    }));
    EXPECT_EQ(tracker.snapshot()->state, lan::TransferState::completed);
    EXPECT_EQ(tracker.snapshot()->current_bytes, 6);
    EXPECT_EQ(tracker.snapshot()->total_bytes, 6);
    EXPECT_EQ(tracker.snapshot()->completion_status,
              lan::TransferCompletionStatus::transferred);
    EXPECT_FALSE(tracker.snapshot()->error);
}

TEST(TransferSnapshotTrackerTest, TracksFailureSemantics) {
    lan::TransferSnapshotTracker tracker;

    ASSERT_TRUE(tracker.apply(lan::TransferStarted{
        .transfer_id = 3,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::receive,
        .kind = lan::TransferKind::file,
        .path = "/tmp/receive",
        .name = {},
    }));

    ASSERT_TRUE(tracker.apply(lan::TransferFailed{
        .transfer_id = 3,
        .state = lan::TransferState::failed,
        .direction = lan::TransferDirection::receive,
        .kind = lan::TransferKind::file,
        .path = "/tmp/receive",
        .name = {},
        .error = lan::Error{lan::ErrorCode::protocol_error, "expected file_begin frame"},
        .category = lan::ErrorCategory::protocol,
        .retryable = false,
        .user_action_required = true,
    }));

    ASSERT_TRUE(tracker.snapshot());
    EXPECT_EQ(tracker.snapshot()->state, lan::TransferState::failed);
    ASSERT_TRUE(tracker.snapshot()->error);
    EXPECT_EQ(tracker.snapshot()->error->code, lan::ErrorCode::protocol_error);
    EXPECT_EQ(tracker.snapshot()->error_category, lan::ErrorCategory::protocol);
    EXPECT_FALSE(tracker.snapshot()->retryable);
    EXPECT_TRUE(tracker.snapshot()->user_action_required);
}

TEST(TransferSnapshotTrackerTest, RejectsInvalidTransitionsAndWrongIds) {
    lan::TransferSnapshotTracker tracker;

    EXPECT_FALSE(tracker.apply(lan::TransferProgress{
        .transfer_id = 1,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    }));

    ASSERT_TRUE(tracker.apply(lan::TransferStarted{
        .transfer_id = 1,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    }));
    EXPECT_FALSE(tracker.apply(lan::TransferProgress{
        .transfer_id = 2,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    }));
    ASSERT_TRUE(tracker.apply(lan::TransferCompleted{
        .transfer_id = 1,
        .state = lan::TransferState::completed,
        .path = {},
        .name = {},
    }));
    EXPECT_FALSE(tracker.apply(lan::TransferFailed{
        .transfer_id = 1,
        .state = lan::TransferState::failed,
        .path = {},
        .name = {},
        .error = lan::Error{},
    }));
}

TEST(TransferSnapshotStoreTest, TracksMultipleTransfersById) {
    lan::TransferSnapshotStore store;

    ASSERT_TRUE(store.apply(lan::TransferStarted{
        .transfer_id = 2,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/two.txt",
        .name = "two.txt",
    }));
    ASSERT_TRUE(store.apply(lan::TransferStarted{
        .transfer_id = 1,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::receive,
        .kind = lan::TransferKind::directory,
        .path = "/tmp/one",
        .name = "one",
    }));

    ASSERT_TRUE(store.apply(lan::TransferProgress{
        .transfer_id = 2,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/two.txt",
        .name = "two.txt",
        .current_bytes = 4,
        .total_bytes = 8,
    }));
    ASSERT_TRUE(store.apply(lan::TransferCompleted{
        .transfer_id = 1,
        .state = lan::TransferState::completed,
        .direction = lan::TransferDirection::receive,
        .kind = lan::TransferKind::directory,
        .path = "/tmp/one",
        .name = "one",
        .total_files = 3,
        .skipped_files = 1,
    }));

    const auto* first = store.find(1);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->state, lan::TransferState::completed);
    EXPECT_EQ(first->total_files, 3);
    EXPECT_EQ(first->skipped_files, 1);

    const auto* second = store.find(2);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->state, lan::TransferState::running);
    EXPECT_EQ(second->current_bytes, 4);
    EXPECT_EQ(second->total_bytes, 8);

    const auto snapshots = store.snapshots();
    ASSERT_EQ(snapshots.size(), 2);
    EXPECT_EQ(snapshots[0].transfer_id, 1);
    EXPECT_EQ(snapshots[1].transfer_id, 2);
}

TEST(TransferSnapshotStoreTest, RejectsDuplicateStartsAndUnknownUpdates) {
    lan::TransferSnapshotStore store;

    EXPECT_FALSE(store.apply(lan::TransferProgress{
        .transfer_id = 9,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    }));

    ASSERT_TRUE(store.apply(lan::TransferStarted{
        .transfer_id = 9,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    }));
    EXPECT_FALSE(store.apply(lan::TransferStarted{
        .transfer_id = 9,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    }));

    store.clear();
    EXPECT_EQ(store.find(9), nullptr);
    EXPECT_TRUE(store.snapshots().empty());
}

TEST(SnapshottingTransferEventsTest, AppliesEventsToStore) {
    lan::SnapshottingTransferEvents events;

    events.on_transfer_started(lan::TransferStarted{
        .transfer_id = 11,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/source.txt",
        .name = "source.txt",
    });
    events.on_transfer_progress(lan::TransferProgress{
        .transfer_id = 11,
        .state = lan::TransferState::running,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/source.txt",
        .name = "source.txt",
        .current_bytes = 5,
        .total_bytes = 10,
    });

    const auto* running = events.snapshot_store().find(11);
    ASSERT_NE(running, nullptr);
    EXPECT_EQ(running->state, lan::TransferState::running);
    EXPECT_EQ(running->current_bytes, 5);
    EXPECT_EQ(running->total_bytes, 10);

    events.on_transfer_completed(lan::TransferCompleted{
        .transfer_id = 11,
        .state = lan::TransferState::completed,
        .direction = lan::TransferDirection::send,
        .kind = lan::TransferKind::file,
        .path = "/tmp/source.txt",
        .name = "source.txt",
        .bytes = 10,
    });

    const auto* completed = events.snapshot_store().find(11);
    ASSERT_NE(completed, nullptr);
    EXPECT_EQ(completed->state, lan::TransferState::completed);
    EXPECT_EQ(completed->current_bytes, 10);
}

TEST(SnapshottingTransferEventsTest, CanBeExtendedByCallingBaseHandlers) {
    class CountingEvents final : public lan::SnapshottingTransferEvents {
    public:
        void on_transfer_started(const lan::TransferStarted& started) override {
            SnapshottingTransferEvents::on_transfer_started(started);
            ++started_count;
        }

        void on_transfer_failed(const lan::TransferFailed& failed) override {
            SnapshottingTransferEvents::on_transfer_failed(failed);
            ++failed_count;
        }

        int started_count = 0;
        int failed_count = 0;
    };

    CountingEvents events;
    events.on_transfer_started(lan::TransferStarted{
        .transfer_id = 12,
        .state = lan::TransferState::running,
        .path = {},
        .name = {},
    });
    events.on_transfer_failed(lan::TransferFailed{
        .transfer_id = 12,
        .state = lan::TransferState::failed,
        .path = {},
        .name = {},
        .error = lan::Error{lan::ErrorCode::network_error, "connection closed"},
        .category = lan::ErrorCategory::network,
        .retryable = true,
    });

    EXPECT_EQ(events.started_count, 1);
    EXPECT_EQ(events.failed_count, 1);
    const auto* snapshot = events.snapshot_store().find(12);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->state, lan::TransferState::failed);
    ASSERT_TRUE(snapshot->error);
    EXPECT_EQ(snapshot->error->code, lan::ErrorCode::network_error);
    EXPECT_TRUE(snapshot->retryable);
}

struct MemoryPipe {
    std::mutex mutex;
    std::condition_variable data_available;
    std::deque<char> bytes;
    bool closed = false;
};

class MemoryConnection final : public lan::Connection {
public:
    MemoryConnection(std::shared_ptr<MemoryPipe> incoming, std::shared_ptr<MemoryPipe> outgoing)
        : incoming_(std::move(incoming)), outgoing_(std::move(outgoing)) {}

    ~MemoryConnection() override {
        close_pipe(outgoing_);
    }

    lan::Result<bool> send_all(const char* data, std::size_t size) override {
        {
            std::lock_guard<std::mutex> lock(outgoing_->mutex);
            if (outgoing_->closed) {
                return lan::Result<bool>::failure(
                    lan::Error{lan::ErrorCode::network_error, "memory connection is closed"});
            }
            outgoing_->bytes.insert(outgoing_->bytes.end(), data, data + size);
        }
        outgoing_->data_available.notify_one();
        return lan::Result<bool>::success(true);
    }

    lan::Result<bool> recv_exact(char* data, std::size_t size) override {
        std::size_t copied = 0;
        while (copied < size) {
            std::unique_lock<std::mutex> lock(incoming_->mutex);
            incoming_->data_available.wait(lock, [this] {
                return !incoming_->bytes.empty() || incoming_->closed;
            });

            if (incoming_->bytes.empty() && incoming_->closed) {
                return lan::Result<bool>::failure(
                    lan::Error{lan::ErrorCode::network_error, "memory connection reached eof"});
            }

            const auto count = std::min(size - copied, incoming_->bytes.size());
            for (std::size_t i = 0; i < count; ++i) {
                data[copied + i] = incoming_->bytes.front();
                incoming_->bytes.pop_front();
            }
            copied += count;
        }

        return lan::Result<bool>::success(true);
    }

    void close() override {
        close_pipe(incoming_);
        close_pipe(outgoing_);
    }

private:
    static void close_pipe(const std::shared_ptr<MemoryPipe>& pipe) {
        {
            std::lock_guard<std::mutex> lock(pipe->mutex);
            pipe->closed = true;
        }
        pipe->data_available.notify_all();
    }

    std::shared_ptr<MemoryPipe> incoming_;
    std::shared_ptr<MemoryPipe> outgoing_;
};

struct MemoryConnectionPair {
    MemoryConnection client;
    MemoryConnection server;
};

MemoryConnectionPair make_memory_connection_pair() {
    auto client_to_server = std::make_shared<MemoryPipe>();
    auto server_to_client = std::make_shared<MemoryPipe>();
    return MemoryConnectionPair{
        .client = MemoryConnection(server_to_client, client_to_server),
        .server = MemoryConnection(client_to_server, server_to_client),
    };
}

struct UniqueMemoryConnectionPair {
    std::unique_ptr<lan::Connection> client;
    std::unique_ptr<lan::Connection> server;
};

UniqueMemoryConnectionPair make_unique_memory_connection_pair() {
    auto client_to_server = std::make_shared<MemoryPipe>();
    auto server_to_client = std::make_shared<MemoryPipe>();
    return UniqueMemoryConnectionPair{
        .client = std::make_unique<MemoryConnection>(server_to_client, client_to_server),
        .server = std::make_unique<MemoryConnection>(client_to_server, server_to_client),
    };
}

class SingleConnectionListener final : public lan::Listener {
public:
    explicit SingleConnectionListener(std::unique_ptr<lan::Connection> connection)
        : connection_(std::move(connection)) {}

    lan::Result<std::unique_ptr<lan::Connection>> accept() override {
        if (!connection_) {
            return lan::Result<std::unique_ptr<lan::Connection>>::failure(
                lan::Error{lan::ErrorCode::network_error, "no queued memory connection"});
        }

        return lan::Result<std::unique_ptr<lan::Connection>>::success(std::move(connection_));
    }

    void close() override {
        connection_.reset();
    }

private:
    std::unique_ptr<lan::Connection> connection_;
};

class BlockingListener final : public lan::Listener {
public:
    lan::Result<std::unique_ptr<lan::Connection>> accept() override {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_changed_.wait(lock, [this] {
            return closed_;
        });
        return lan::Result<std::unique_ptr<lan::Connection>>::failure(
            lan::Error{lan::ErrorCode::cancelled, "listener closed"});
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        closed_changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable closed_changed_;
    bool closed_ = false;
};

class FakeNetworkBackend final : public lan::NetworkBackend {
public:
    explicit FakeNetworkBackend(std::unique_ptr<lan::Listener> listener)
        : listener_(std::move(listener)) {}

    lan::Result<std::unique_ptr<lan::Listener>> listen(std::string_view, std::uint16_t) override {
        if (!listener_) {
            return lan::Result<std::unique_ptr<lan::Listener>>::failure(
                lan::Error{lan::ErrorCode::network_error, "listener already consumed"});
        }

        return lan::Result<std::unique_ptr<lan::Listener>>::success(std::move(listener_));
    }

    lan::Result<std::unique_ptr<lan::Connection>> connect(std::string_view,
                                                          std::uint16_t) override {
        return lan::Result<std::unique_ptr<lan::Connection>>::failure(
            lan::Error{lan::ErrorCode::network_error, "fake backend does not connect"});
    }

private:
    std::unique_ptr<lan::Listener> listener_;
};

class FakeConnectBackend final : public lan::NetworkBackend {
public:
    explicit FakeConnectBackend(std::unique_ptr<lan::Connection> connection)
        : connection_(std::move(connection)) {}

    lan::Result<std::unique_ptr<lan::Listener>> listen(std::string_view, std::uint16_t) override {
        return lan::Result<std::unique_ptr<lan::Listener>>::failure(
            lan::Error{lan::ErrorCode::network_error, "fake backend does not listen"});
    }

    lan::Result<std::unique_ptr<lan::Connection>> connect(std::string_view,
                                                          std::uint16_t) override {
        if (!connection_) {
            return lan::Result<std::unique_ptr<lan::Connection>>::failure(
                lan::Error{lan::ErrorCode::network_error, "connection already consumed"});
        }

        return lan::Result<std::unique_ptr<lan::Connection>>::success(std::move(connection_));
    }

private:
    std::unique_ptr<lan::Connection> connection_;
};

class CapturingSenderEvents : public lan::SenderTransferEvents {
public:
    void on_transfer_started(const lan::TransferStarted& started) override {
        started_ids.push_back(started.transfer_id);
        started_kinds.push_back(started.kind);
        started_states.push_back(started.state);
    }

    void on_transfer_progress(const lan::TransferProgress& progress) override {
        progress_ids.push_back(progress.transfer_id);
        progress_states.push_back(progress.state);
        directions.push_back(progress.direction);
        kinds.push_back(progress.kind);
        if (progress.kind == lan::TransferKind::file) {
            file_progress_bytes.push_back(progress.current_bytes);
            file_progress_totals.push_back(progress.total_bytes);
            return;
        }

        directory_progress_processed.push_back(progress.processed_files);
        directory_progress_totals.push_back(progress.total_files);
    }

    void on_transfer_completed(const lan::TransferCompleted& completed) override {
        completed_ids.push_back(completed.transfer_id);
        completed_kinds.push_back(completed.kind);
        completed_states.push_back(completed.state);
        completed_statuses.push_back(completed.status);
        completed_resumed_from.push_back(completed.resumed_from);
    }

    void on_transfer_failed(const lan::TransferFailed& failed) override {
        failed_ids.push_back(failed.transfer_id);
        failed_kinds.push_back(failed.kind);
        failed_states.push_back(failed.state);
        failed_errors.push_back(failed.error.code);
        failed_categories.push_back(failed.category);
        failed_retryable.push_back(failed.retryable);
        failed_user_action_required.push_back(failed.user_action_required);
    }

    void on_transfer_cancelled(const lan::TransferCancelled& cancelled) override {
        cancelled_ids.push_back(cancelled.transfer_id);
        cancelled_kinds.push_back(cancelled.kind);
        cancelled_states.push_back(cancelled.state);
    }

    std::vector<std::uint64_t> started_ids;
    std::vector<std::uint64_t> progress_ids;
    std::vector<std::uint64_t> completed_ids;
    std::vector<std::uint64_t> failed_ids;
    std::vector<std::uint64_t> cancelled_ids;
    std::vector<lan::TransferKind> started_kinds;
    std::vector<lan::TransferKind> completed_kinds;
    std::vector<lan::TransferKind> failed_kinds;
    std::vector<lan::TransferKind> cancelled_kinds;
    std::vector<lan::TransferState> started_states;
    std::vector<lan::TransferState> progress_states;
    std::vector<lan::TransferState> completed_states;
    std::vector<lan::TransferState> failed_states;
    std::vector<lan::TransferState> cancelled_states;
    std::vector<lan::TransferCompletionStatus> completed_statuses;
    std::vector<std::uint64_t> completed_resumed_from;
    std::vector<lan::ErrorCode> failed_errors;
    std::vector<lan::ErrorCategory> failed_categories;
    std::vector<bool> failed_retryable;
    std::vector<bool> failed_user_action_required;
    std::vector<lan::TransferDirection> directions;
    std::vector<lan::TransferKind> kinds;
    std::vector<std::uint64_t> file_progress_bytes;
    std::vector<std::uint64_t> file_progress_totals;
    std::vector<std::uint64_t> directory_progress_processed;
    std::vector<std::uint64_t> directory_progress_totals;
};

class CapturingReceiverEvents final : public lan::ReceiverServerEvents {
public:
    void on_listening(const lan::ReceiverConfig&) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listening = true;
        }
        listening_changed_.notify_all();
    }

    void on_transfer_started(const lan::TransferStarted& started) override {
        started_ids.push_back(started.transfer_id);
        started_kinds.push_back(started.kind);
        started_states.push_back(started.state);
    }

    void on_transfer_progress(const lan::TransferProgress& progress) override {
        progress_ids.push_back(progress.transfer_id);
        progress_states.push_back(progress.state);
    }

    void on_transfer_completed(const lan::TransferCompleted& completed) override {
        completed_ids.push_back(completed.transfer_id);
        completed_kinds.push_back(completed.kind);
        completed_states.push_back(completed.state);
        completed_statuses.push_back(completed.status);
        completed_resumed_from.push_back(completed.resumed_from);
    }

    void on_transfer_failed(const lan::TransferFailed& failed) override {
        failed_ids.push_back(failed.transfer_id);
        failed_kinds.push_back(failed.kind);
        failed_states.push_back(failed.state);
        failed_errors.push_back(failed.error.code);
        failed_categories.push_back(failed.category);
        failed_retryable.push_back(failed.retryable);
        failed_user_action_required.push_back(failed.user_action_required);
    }

    void on_transfer_cancelled(const lan::TransferCancelled& cancelled) override {
        cancelled_ids.push_back(cancelled.transfer_id);
        cancelled_kinds.push_back(cancelled.kind);
        cancelled_states.push_back(cancelled.state);
    }

    void on_file_received(const lan::ReceiveFileReport& report) override {
        file_received = true;
        file_report = report;
    }

    void on_file_progress(const lan::ReceiveFileProgress& progress) override {
        file_progress_bytes.push_back(progress.bytes_received);
        file_progress_totals.push_back(progress.total_bytes);
    }

    void on_directory_synced(const lan::ReceiveSyncReport& report) override {
        directory_synced = true;
        sync_report = report;
    }

    void on_directory_progress(const lan::ReceiveSyncProgress& progress) override {
        directory_progress_processed.push_back(progress.processed_files);
        directory_progress_totals.push_back(progress.manifest_files);
    }

    void on_client_error(const lan::Error& error) override {
        client_error = true;
        last_error = error;
    }

    bool wait_until_listening() {
        std::unique_lock<std::mutex> lock(mutex_);
        return listening_changed_.wait_for(lock, std::chrono::seconds(1), [this] {
            return listening;
        });
    }

    bool listening = false;
    bool file_received = false;
    bool directory_synced = false;
    bool client_error = false;
    lan::ReceiveFileReport file_report;
    lan::ReceiveSyncReport sync_report;
    lan::Error last_error;
    std::vector<std::uint64_t> started_ids;
    std::vector<std::uint64_t> progress_ids;
    std::vector<std::uint64_t> completed_ids;
    std::vector<std::uint64_t> failed_ids;
    std::vector<std::uint64_t> cancelled_ids;
    std::vector<lan::TransferKind> started_kinds;
    std::vector<lan::TransferKind> completed_kinds;
    std::vector<lan::TransferKind> failed_kinds;
    std::vector<lan::TransferKind> cancelled_kinds;
    std::vector<lan::TransferState> started_states;
    std::vector<lan::TransferState> progress_states;
    std::vector<lan::TransferState> completed_states;
    std::vector<lan::TransferState> failed_states;
    std::vector<lan::TransferState> cancelled_states;
    std::vector<lan::ErrorCode> failed_errors;
    std::vector<lan::ErrorCategory> failed_categories;
    std::vector<bool> failed_retryable;
    std::vector<bool> failed_user_action_required;
    std::vector<lan::TransferCompletionStatus> completed_statuses;
    std::vector<std::uint64_t> completed_resumed_from;
    std::vector<std::uint64_t> file_progress_bytes;
    std::vector<std::uint64_t> file_progress_totals;
    std::vector<std::uint64_t> directory_progress_processed;
    std::vector<std::uint64_t> directory_progress_totals;

private:
    std::mutex mutex_;
    std::condition_variable listening_changed_;
};

TEST(FileMetadataTest, RoundTripsFileBeginMetadata) {
    lan::FileBeginMetadata metadata{
        .name = "demo.txt",
        .size = 42,
        .sha256 = "abcdef",
        .resume = false,
    };

    auto decoded = lan::decode_file_begin(lan::encode_file_begin(metadata));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().name, metadata.name);
    EXPECT_EQ(decoded.value().size, metadata.size);
    EXPECT_EQ(decoded.value().sha256, metadata.sha256);
    EXPECT_EQ(decoded.value().resume, metadata.resume);
}

TEST(FileMetadataTest, RoundTripsFileBeginAckMetadata) {
    lan::FileBeginAckMetadata metadata{
        .offset = 17,
    };

    auto decoded = lan::decode_file_begin_ack(lan::encode_file_begin_ack(metadata));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().offset, metadata.offset);

    auto legacy = lan::decode_file_begin_ack("ready");
    ASSERT_TRUE(legacy);
    EXPECT_EQ(legacy.value().offset, 0);
}

TEST(ChunkCodecTest, RoundTripsOffsetAndPayload) {
    const std::vector<std::byte> payload = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    lan::Frame frame;
    frame.type = lan::MessageType::chunk;
    frame.body = lan::encode_chunk_body(17, payload.data(), payload.size());

    auto decoded = lan::decode_chunk_body(frame);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().offset, 17);
    ASSERT_EQ(decoded.value().size, payload.size());
    EXPECT_EQ(decoded.value().data[0], std::byte{'a'});
    EXPECT_EQ(decoded.value().data[2], std::byte{'c'});
}

TEST(HelloCodecTest, RoundTripsVersionedHello) {
    lan::HelloMetadata metadata{
        .protocol_version = lan::current_hello_version,
        .mode = lan::HelloMode::sync,
    };

    auto decoded = lan::decode_hello_body(lan::encode_hello(metadata));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().protocol_version, lan::current_hello_version);
    EXPECT_EQ(decoded.value().mode, lan::HelloMode::sync);
}

TEST(HelloCodecTest, AcceptsLegacyHelloBodies) {
    auto file = lan::decode_hello_body(lan::bytes_from_string("file"));
    auto sync = lan::decode_hello_body(lan::bytes_from_string("sync"));

    ASSERT_TRUE(file);
    ASSERT_TRUE(sync);
    EXPECT_EQ(file.value().mode, lan::HelloMode::file);
    EXPECT_EQ(sync.value().mode, lan::HelloMode::sync);
}

TEST(HelloCodecTest, RejectsUnsupportedVersion) {
    auto decoded = lan::decode_hello_body(lan::bytes_from_string("lan/99 sync"));

    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, lan::ErrorCode::protocol_error);
}

TEST(ReceiveSingleFileTest, ReportsProgressForChunks) {
    TempDir temp("receive-progress-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");

    auto hash = lan::hash_file(source);
    ASSERT_TRUE(hash);

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_memory_connection_pair();

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::bytes_from_string("file");

    std::vector<std::uint64_t> progress_bytes;
    std::vector<std::uint64_t> progress_totals;
    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});

    std::thread receiver([&] {
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello, [&](const lan::ReceiveFileProgress& progress) {
                progress_bytes.push_back(progress.bytes_received);
                progress_totals.push_back(progress.total_bytes);
            });
    });

    lan::Frame begin;
    begin.type = lan::MessageType::file_begin;
    begin.body = lan::bytes_from_string(lan::encode_file_begin(lan::FileBeginMetadata{
        .name = "progress.txt",
        .size = 6,
        .sha256 = hash.value().hex_digest,
    }));
    ASSERT_TRUE(lan::write_frame(pair.client, begin));

    auto begin_ack = lan::read_frame(pair.client);
    ASSERT_TRUE(begin_ack);
    ASSERT_EQ(begin_ack.value().type, lan::MessageType::ack);

    lan::Frame first_chunk;
    first_chunk.type = lan::MessageType::chunk;
    auto first_payload = lan::bytes_from_string("abc");
    first_chunk.body = lan::encode_chunk_body(0, first_payload.data(), first_payload.size());
    ASSERT_TRUE(lan::write_frame(pair.client, first_chunk));

    lan::Frame second_chunk;
    second_chunk.type = lan::MessageType::chunk;
    auto second_payload = lan::bytes_from_string("def");
    second_chunk.body = lan::encode_chunk_body(3, second_payload.data(), second_payload.size());
    ASSERT_TRUE(lan::write_frame(pair.client, second_chunk));

    lan::Frame end;
    end.type = lan::MessageType::file_end;
    ASSERT_TRUE(lan::write_frame(pair.client, end));

    auto end_ack = lan::read_frame(pair.client);
    ASSERT_TRUE(end_ack);
    ASSERT_EQ(end_ack.value().type, lan::MessageType::ack);

    receiver.join();

    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(received.value().bytes_received, 6);
    EXPECT_EQ(read_text(receive_dir / "progress.txt"), "abcdef");

    const std::vector<std::uint64_t> expected_bytes = {0, 3, 6};
    const std::vector<std::uint64_t> expected_totals = {6, 6, 6};
    EXPECT_EQ(progress_bytes, expected_bytes);
    EXPECT_EQ(progress_totals, expected_totals);
}

TEST(SendSingleFileTest, ReportsProgressForChunks) {
    TempDir temp("send-progress-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");

    lan::SenderConfig sender_config;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_memory_connection_pair();

    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::thread receiver([&] {
        auto hello = lan::read_frame(pair.server);
        if (!hello) {
            received = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello.value());
    });

    std::vector<std::uint64_t> progress_bytes;
    std::vector<std::uint64_t> progress_totals;
    auto sent = lan::send_single_file_to_connection(
        sender_config, pair.client, [&](const lan::SendFileProgress& progress) {
            progress_bytes.push_back(progress.bytes_sent);
            progress_totals.push_back(progress.total_bytes);
        });

    receiver.join();

    ASSERT_TRUE(sent) << sent.error().message;
    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(sent.value().bytes_sent, 6);
    EXPECT_EQ(received.value().bytes_received, 6);
    EXPECT_EQ(sent.value().status, lan::FileTransferStatus::transferred);
    EXPECT_EQ(received.value().status, lan::FileTransferStatus::transferred);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "abcdef");

    const std::vector<std::uint64_t> expected_bytes = {0, 3, 6};
    const std::vector<std::uint64_t> expected_totals = {6, 6, 6};
    EXPECT_EQ(progress_bytes, expected_bytes);
    EXPECT_EQ(progress_totals, expected_totals);
}

TEST(SendSingleFileTest, ResumesFromExistingPartFile) {
    TempDir temp("send-resume-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");
    write_text(receive_dir / "source.txt.part", "abc");

    lan::SenderConfig sender_config;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;
    sender_config.resume = true;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_memory_connection_pair();

    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::thread receiver([&] {
        auto hello = lan::read_frame(pair.server);
        if (!hello) {
            received = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello.value());
    });

    std::vector<std::uint64_t> progress_bytes;
    auto sent = lan::send_single_file_to_connection(
        sender_config, pair.client, [&](const lan::SendFileProgress& progress) {
            progress_bytes.push_back(progress.bytes_sent);
        });

    receiver.join();

    ASSERT_TRUE(sent) << sent.error().message;
    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(sent.value().bytes_sent, 6);
    EXPECT_EQ(received.value().bytes_received, 6);
    EXPECT_EQ(sent.value().status, lan::FileTransferStatus::resumed);
    EXPECT_EQ(received.value().status, lan::FileTransferStatus::resumed);
    EXPECT_EQ(sent.value().resumed_from, 3);
    EXPECT_EQ(received.value().resumed_from, 3);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "abcdef");
    EXPECT_FALSE(std::filesystem::exists(receive_dir / "source.txt.part"));

    const std::vector<std::uint64_t> expected_progress = {3, 6};
    EXPECT_EQ(progress_bytes, expected_progress);
}

TEST(SendSingleFileTest, NoResumeIgnoresExistingPartFile) {
    TempDir temp("send-no-resume-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");
    write_text(receive_dir / "source.txt.part", "abc");

    lan::SenderConfig sender_config;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;
    sender_config.resume = false;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_memory_connection_pair();

    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::thread receiver([&] {
        auto hello = lan::read_frame(pair.server);
        if (!hello) {
            received = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello.value());
    });

    std::vector<std::uint64_t> progress_bytes;
    auto sent = lan::send_single_file_to_connection(
        sender_config, pair.client, [&](const lan::SendFileProgress& progress) {
            progress_bytes.push_back(progress.bytes_sent);
        });

    receiver.join();

    ASSERT_TRUE(sent) << sent.error().message;
    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(sent.value().status, lan::FileTransferStatus::transferred);
    EXPECT_EQ(received.value().status, lan::FileTransferStatus::transferred);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "abcdef");

    const std::vector<std::uint64_t> expected_progress = {0, 3, 6};
    EXPECT_EQ(progress_bytes, expected_progress);
}

TEST(SendSingleFileTest, RestartsWhenPartFileIsTooLarge) {
    TempDir temp("send-large-part-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");
    write_text(receive_dir / "source.txt.part", "abcdef-extra");

    lan::SenderConfig sender_config;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;
    sender_config.resume = true;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_memory_connection_pair();

    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::thread receiver([&] {
        auto hello = lan::read_frame(pair.server);
        if (!hello) {
            received = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello.value());
    });

    std::vector<std::uint64_t> progress_bytes;
    auto sent = lan::send_single_file_to_connection(
        sender_config, pair.client, [&](const lan::SendFileProgress& progress) {
            progress_bytes.push_back(progress.bytes_sent);
        });

    receiver.join();

    ASSERT_TRUE(sent) << sent.error().message;
    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(sent.value().status, lan::FileTransferStatus::transferred);
    EXPECT_EQ(received.value().status, lan::FileTransferStatus::transferred);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "abcdef");

    const std::vector<std::uint64_t> expected_progress = {0, 3, 6};
    EXPECT_EQ(progress_bytes, expected_progress);
}

TEST(SendSingleFileTest, SkipsWhenTargetAlreadyMatches) {
    TempDir temp("send-skip-existing-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");
    write_text(receive_dir / "source.txt", "abcdef");

    lan::SenderConfig sender_config;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_memory_connection_pair();

    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::thread receiver([&] {
        auto hello = lan::read_frame(pair.server);
        if (!hello) {
            received = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello.value());
    });

    std::vector<std::uint64_t> progress_bytes;
    auto sent = lan::send_single_file_to_connection(
        sender_config, pair.client, [&](const lan::SendFileProgress& progress) {
            progress_bytes.push_back(progress.bytes_sent);
        });

    receiver.join();

    ASSERT_TRUE(sent) << sent.error().message;
    ASSERT_TRUE(received) << received.error().message;
    EXPECT_EQ(sent.value().bytes_sent, 6);
    EXPECT_EQ(received.value().bytes_received, 6);
    EXPECT_EQ(sent.value().status, lan::FileTransferStatus::skipped);
    EXPECT_EQ(received.value().status, lan::FileTransferStatus::skipped);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "abcdef");

    const std::vector<std::uint64_t> expected_progress = {6};
    EXPECT_EQ(progress_bytes, expected_progress);
}

TEST(SendSingleFileTest, RejectsDifferentExistingTargetWithoutOverwrite) {
    TempDir temp("send-existing-different-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");
    write_text(receive_dir / "source.txt", "different");

    lan::SenderConfig sender_config;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;
    receiver_config.allow_overwrite = false;

    auto pair = make_memory_connection_pair();

    auto received = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::thread receiver([&] {
        auto hello = lan::read_frame(pair.server);
        if (!hello) {
            received = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        received = lan::receive_single_file_from_connection(
            receiver_config, pair.server, hello.value());
    });

    auto sent = lan::send_single_file_to_connection(sender_config, pair.client);

    receiver.join();

    ASSERT_FALSE(sent);
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code, lan::ErrorCode::invalid_argument);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "different");
}

TEST(ManifestTest, RecursivelyScansRegularFiles) {
    TempDir temp("manifest-test");
    write_text(temp.path() / "a.txt", "a");
    write_text(temp.path() / "sub" / "b.txt", "b");
    std::filesystem::create_directory_symlink(temp.path() / "sub", temp.path() / "link-to-sub");

    auto manifest = lan::build_manifest(temp.path(), 4096);
    ASSERT_TRUE(manifest);
    ASSERT_EQ(manifest.value().files.size(), 2);
    EXPECT_EQ(manifest.value().files[0].relative_path.generic_string(), "a.txt");
    EXPECT_EQ(manifest.value().files[1].relative_path.generic_string(), "sub/b.txt");
}

TEST(SyncCodecTest, RoundTripsManifestSyncPlanAndDeltaPlan) {
    lan::Manifest manifest;
    manifest.files.push_back(lan::ManifestEntry{
        .relative_path = "sub/a.txt",
        .size = 3,
        .sha256 = "hash",
        .mtime_ns = 9,
        .mode = 0644,
    });

    auto decoded_manifest = lan::decode_manifest(lan::encode_manifest(manifest));
    ASSERT_TRUE(decoded_manifest);
    ASSERT_EQ(decoded_manifest.value().files.size(), 1);
    EXPECT_EQ(decoded_manifest.value().files[0].relative_path.generic_string(), "sub/a.txt");

    lan::SyncPlan plan;
    plan.block_size = 8;
    lan::SyncPlanEntry entry;
    entry.action = lan::SyncAction::delta;
    entry.manifest_entry = manifest.files[0];
    entry.basis_signatures.push_back(lan::BlockSignature{
        .index = 0,
        .offset = 0,
        .size = 8,
        .weak_checksum = 123,
        .strong_checksum = "strong",
    });
    plan.entries.push_back(entry);

    auto decoded_plan = lan::decode_sync_plan(lan::encode_sync_plan(plan));
    ASSERT_TRUE(decoded_plan);
    EXPECT_EQ(decoded_plan.value().block_size, 8);
    ASSERT_EQ(decoded_plan.value().entries.size(), 1);
    ASSERT_EQ(decoded_plan.value().entries[0].basis_signatures.size(), 1);

    lan::DeltaPlan delta;
    delta.source_size = 2;
    delta.source_sha256 = "sha";
    delta.op_count = 1;
    delta.ops.push_back(lan::DeltaOp{
        .type = lan::DeltaOpType::literal_data,
        .size = 2,
        .data = {std::byte{'o'}, std::byte{'k'}},
    });

    auto decoded_delta = lan::decode_delta_plan(lan::encode_delta_plan(delta));
    ASSERT_TRUE(decoded_delta);
    EXPECT_EQ(decoded_delta.value().source_sha256, "sha");
    ASSERT_EQ(decoded_delta.value().ops.size(), 1);

    auto decoded_header = lan::decode_delta_header(lan::encode_delta_header(delta));
    ASSERT_TRUE(decoded_header);
    EXPECT_EQ(decoded_header.value().op_count, 1);

    auto decoded_op = lan::decode_delta_op(lan::encode_delta_op(delta.ops[0]));
    ASSERT_TRUE(decoded_op);
    EXPECT_EQ(decoded_op.value().data.size(), 2);
}

TEST(DeltaTest, RebuildsSourceFromBasisAndDelta) {
    TempDir temp("delta-test");
    const auto basis = temp.path() / "basis.txt";
    const auto source = temp.path() / "source.txt";
    const auto rebuilt = temp.path() / "rebuilt.txt";

    write_text(basis, "aaaa-bbbb-cccc-dddd-eeee\n");
    write_text(source, "xxxx-bbbb-cccc-yyyy-eeee\n");

    auto signatures = lan::build_block_signatures(basis, 5);
    ASSERT_TRUE(signatures);

    auto delta = lan::build_delta(source, signatures.value(), 5);
    ASSERT_TRUE(delta);
    EXPECT_GT(delta.value().ops.size(), 0);

    auto applied = lan::apply_delta(basis, rebuilt, delta.value().ops);
    ASSERT_TRUE(applied);
    EXPECT_EQ(read_text(rebuilt), read_text(source));
}

TEST(SyncPlanAndExecutorTest, HandlesSkipFullAndDelta) {
    TempDir source("sync-source-test");
    TempDir receive("sync-receive-test");

    write_text(source.path() / "same.txt", "same\n");
    write_text(receive.path() / "same.txt", "same\n");
    write_text(source.path() / "sub" / "new.txt", "new nested\n");
    write_text(source.path() / "changed.txt", "xxxx-bbbb-cccc-yyyy-eeee\n");
    write_text(receive.path() / "changed.txt", "aaaa-bbbb-cccc-dddd-eeee\n");

    auto manifest = lan::build_manifest(source.path(), 4096);
    ASSERT_TRUE(manifest);

    auto plan = lan::build_sync_plan(manifest.value(), receive.path(), 5);
    ASSERT_TRUE(plan);

    auto report = lan::execute_local_sync(manifest.value(), source.path(), plan.value());
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().skipped_files, 1);
    EXPECT_EQ(report.value().full_files, 1);
    EXPECT_EQ(report.value().delta_files, 1);

    for (const auto& entry : manifest.value().files) {
        auto src_hash = lan::hash_file(source.path() / entry.relative_path);
        auto dst_hash = lan::hash_file(receive.path() / entry.relative_path);
        ASSERT_TRUE(src_hash);
        ASSERT_TRUE(dst_hash);
        EXPECT_EQ(src_hash.value().hex_digest, dst_hash.value().hex_digest);
    }
}

TEST(SyncSessionTest, SyncsDirectoryThroughConnectionInterface) {
    TempDir source("sync-session-source-test");
    TempDir receive("sync-session-receive-test");

    write_text(source.path() / "same.txt", "same\n");
    write_text(receive.path() / "same.txt", "same\n");
    write_text(source.path() / "sub" / "new.txt", "new nested\n");
    write_text(source.path() / "changed.txt", "xxxx-bbbb-cccc-yyyy-eeee\n");
    write_text(receive.path() / "changed.txt", "aaaa-bbbb-cccc-dddd-eeee\n");

    lan::SenderConfig sender_config;
    sender_config.source_path = source.path();

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive.path();

    auto pair = make_memory_connection_pair();

    auto sender_result = lan::Result<lan::SendSyncReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "sender did not run"});
    auto receiver_result = lan::Result<lan::ReceiveSyncReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});
    std::vector<std::uint64_t> sender_progress_processed;
    std::vector<std::uint64_t> sender_progress_totals;

    std::thread sender([&] {
        sender_result = lan::sync_sender_to_connection(
            sender_config, 5, pair.client, [&](const lan::SendSyncProgress& progress) {
                sender_progress_processed.push_back(progress.processed_files);
                sender_progress_totals.push_back(progress.manifest_files);
            });
    });
    std::thread receiver([&] {
        auto initial_hello = lan::read_frame(pair.server);
        if (!initial_hello) {
            receiver_result =
                lan::Result<lan::ReceiveSyncReport>::failure(initial_hello.error());
            return;
        }
        receiver_result = lan::sync_receiver_from_connection(
            receiver_config, 5, pair.server, initial_hello.value());
    });

    sender.join();
    receiver.join();

    ASSERT_TRUE(sender_result) << sender_result.error().message;
    ASSERT_TRUE(receiver_result) << receiver_result.error().message;

    EXPECT_EQ(sender_result.value().manifest_files, 3);
    EXPECT_EQ(sender_result.value().skipped_files, 1);
    EXPECT_EQ(sender_result.value().full_files, 1);
    EXPECT_EQ(sender_result.value().delta_files, 1);
    EXPECT_EQ(sender_result.value().delta_frames_sent, 2);
    EXPECT_GT(sender_result.value().delta_payload_bytes_sent, 0);
    EXPECT_GT(sender_result.value().elapsed_seconds, 0.0);

    const std::vector<std::uint64_t> expected_sender_processed = {1, 2, 3, 0, 1, 2, 3};
    const std::vector<std::uint64_t> expected_sender_totals = {0, 0, 0, 3, 3, 3, 3};
    EXPECT_EQ(sender_progress_processed, expected_sender_processed);
    EXPECT_EQ(sender_progress_totals, expected_sender_totals);

    EXPECT_EQ(receiver_result.value().manifest_files, 3);
    EXPECT_EQ(receiver_result.value().skipped_files, 1);
    EXPECT_EQ(receiver_result.value().full_files, 1);
    EXPECT_EQ(receiver_result.value().delta_files, 1);
    EXPECT_EQ(receiver_result.value().files_written, 2);
    EXPECT_EQ(receiver_result.value().delta_payload_bytes_received,
              sender_result.value().delta_payload_bytes_sent);
    EXPECT_GT(receiver_result.value().elapsed_seconds, 0.0);

    auto manifest = lan::build_manifest(source.path(), 4096);
    ASSERT_TRUE(manifest);
    for (const auto& entry : manifest.value().files) {
        auto src_hash = lan::hash_file(source.path() / entry.relative_path);
        auto dst_hash = lan::hash_file(receive.path() / entry.relative_path);
        ASSERT_TRUE(src_hash);
        ASSERT_TRUE(dst_hash);
        EXPECT_EQ(src_hash.value().hex_digest, dst_hash.value().hex_digest);
    }
}

TEST(SyncSessionTest, RejectsUnexpectedHelloThroughConnectionInterface) {
    TempDir receive("sync-session-bad-hello-test");

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive.path();

    auto pair = make_memory_connection_pair();

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::bytes_from_string("file");

    auto receiver_result = lan::sync_receiver_from_connection(receiver_config, 5, pair.server, hello);
    ASSERT_FALSE(receiver_result);
    EXPECT_EQ(receiver_result.error().code, lan::ErrorCode::protocol_error);

    auto error_frame = lan::read_frame(pair.client);
    ASSERT_TRUE(error_frame);
    EXPECT_EQ(error_frame.value().type, lan::MessageType::error);
    EXPECT_NE(lan::body_as_string(error_frame.value()).find("expected sync hello frame"),
              std::string::npos);
}

TEST(SenderTransferRunnerTest, SendsFileThroughInjectedNetworkBackend) {
    TempDir temp("sender-transfer-file-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    write_text(source, "abcdef");
    std::filesystem::create_directories(receive_dir);

    lan::SenderConfig sender_config;
    sender_config.target.host = "memory";
    sender_config.target.port = 1;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_unique_memory_connection_pair();
    auto server = std::move(pair.server);
    FakeConnectBackend backend(std::move(pair.client));
    CapturingSenderEvents events;
    lan::SenderTransferRunner runner(backend);
    auto receiver_result = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});

    std::thread receiver([&] {
        auto hello = lan::read_frame(*server);
        if (!hello) {
            receiver_result = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        receiver_result =
            lan::receive_single_file_from_connection(receiver_config, *server, hello.value());
    });

    auto transferred = runner.run(sender_config, events);
    receiver.join();

    ASSERT_TRUE(transferred) << transferred.error().message;
    ASSERT_TRUE(receiver_result) << receiver_result.error().message;
    EXPECT_EQ(transferred.value().kind, lan::TransferKind::file);
    EXPECT_EQ(transferred.value().file.bytes_sent, 6);
    EXPECT_EQ(read_text(receive_dir / "source.txt"), "abcdef");

    const std::vector<lan::TransferKind> expected_started = {lan::TransferKind::file};
    const std::vector<lan::TransferKind> expected_completed = {lan::TransferKind::file};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    const std::vector<lan::TransferState> expected_started_states = {lan::TransferState::running};
    const std::vector<lan::TransferState> expected_completed_states = {
        lan::TransferState::completed};
    const std::vector<lan::TransferCompletionStatus> expected_statuses = {
        lan::TransferCompletionStatus::transferred};
    const std::vector<std::uint64_t> expected_resumed_from = {0};
    EXPECT_EQ(events.started_kinds, expected_started);
    EXPECT_EQ(events.completed_kinds, expected_completed);
    EXPECT_EQ(events.started_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.completed_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.started_states, expected_started_states);
    EXPECT_EQ(events.completed_states, expected_completed_states);
    EXPECT_EQ(events.completed_statuses, expected_statuses);
    EXPECT_EQ(events.completed_resumed_from, expected_resumed_from);
    EXPECT_TRUE(events.failed_kinds.empty());

    const std::vector<std::uint64_t> expected_progress = {0, 3, 6};
    const std::vector<std::uint64_t> expected_totals = {6, 6, 6};
    const std::vector<std::uint64_t> expected_progress_ids(expected_progress.size(), 1);
    const std::vector<lan::TransferState> expected_progress_states(
        expected_progress.size(), lan::TransferState::running);
    const std::vector<lan::TransferDirection> expected_directions(
        expected_progress.size(), lan::TransferDirection::send);
    const std::vector<lan::TransferKind> expected_kinds(
        expected_progress.size(), lan::TransferKind::file);
    EXPECT_EQ(events.directions, expected_directions);
    EXPECT_EQ(events.kinds, expected_kinds);
    EXPECT_EQ(events.progress_ids, expected_progress_ids);
    EXPECT_EQ(events.progress_states, expected_progress_states);
    EXPECT_EQ(events.file_progress_bytes, expected_progress);
    EXPECT_EQ(events.file_progress_totals, expected_totals);
}

TEST(SenderTransferRunnerTest, ReportsSkippedFileCompletionStatus) {
    TempDir temp("sender-transfer-skip-status-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    write_text(source, "abcdef");
    std::filesystem::create_directories(receive_dir);
    write_text(receive_dir / "source.txt", "abcdef");

    lan::SenderConfig sender_config;
    sender_config.target.host = "memory";
    sender_config.target.port = 1;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;

    auto pair = make_unique_memory_connection_pair();
    auto server = std::move(pair.server);
    FakeConnectBackend backend(std::move(pair.client));
    CapturingSenderEvents events;
    lan::SenderTransferRunner runner(backend);
    auto receiver_result = lan::Result<lan::ReceiveFileReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});

    std::thread receiver([&] {
        auto hello = lan::read_frame(*server);
        if (!hello) {
            receiver_result = lan::Result<lan::ReceiveFileReport>::failure(hello.error());
            return;
        }
        receiver_result =
            lan::receive_single_file_from_connection(receiver_config, *server, hello.value());
    });

    auto transferred = runner.run(sender_config, events);
    receiver.join();

    ASSERT_TRUE(transferred) << transferred.error().message;
    ASSERT_TRUE(receiver_result) << receiver_result.error().message;
    EXPECT_EQ(transferred.value().file.status, lan::FileTransferStatus::skipped);
    EXPECT_EQ(receiver_result.value().status, lan::FileTransferStatus::skipped);

    const std::vector<lan::TransferCompletionStatus> expected_statuses = {
        lan::TransferCompletionStatus::skipped};
    const std::vector<std::uint64_t> expected_resumed_from = {6};
    EXPECT_EQ(events.completed_statuses, expected_statuses);
    EXPECT_EQ(events.completed_resumed_from, expected_resumed_from);
}

TEST(SenderTransferRunnerTest, SyncsDirectoryThroughInjectedNetworkBackend) {
    TempDir source("sender-transfer-sync-source-test");
    TempDir receive("sender-transfer-sync-receive-test");

    write_text(source.path() / "same.txt", "same\n");
    write_text(receive.path() / "same.txt", "same\n");
    write_text(source.path() / "new.txt", "new\n");

    lan::SenderConfig sender_config;
    sender_config.target.host = "memory";
    sender_config.target.port = 1;
    sender_config.source_path = source.path();
    sender_config.chunk_size = 5;

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive.path();

    auto pair = make_unique_memory_connection_pair();
    auto server = std::move(pair.server);
    FakeConnectBackend backend(std::move(pair.client));
    CapturingSenderEvents events;
    lan::SenderTransferRunner runner(backend);
    auto receiver_result = lan::Result<lan::ReceiveSyncReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "receiver did not run"});

    std::thread receiver([&] {
        auto hello = lan::read_frame(*server);
        if (!hello) {
            receiver_result = lan::Result<lan::ReceiveSyncReport>::failure(hello.error());
            return;
        }
        receiver_result =
            lan::sync_receiver_from_connection(receiver_config, 5, *server, hello.value());
    });

    auto transferred = runner.run(sender_config, events);
    receiver.join();

    ASSERT_TRUE(transferred) << transferred.error().message;
    ASSERT_TRUE(receiver_result) << receiver_result.error().message;
    EXPECT_EQ(transferred.value().kind, lan::TransferKind::directory);
    EXPECT_EQ(transferred.value().directory.manifest_files, 2);
    EXPECT_EQ(transferred.value().directory.skipped_files, 1);
    EXPECT_EQ(transferred.value().directory.full_files, 1);
    EXPECT_EQ(read_text(receive.path() / "new.txt"), "new\n");

    const std::vector<lan::TransferKind> expected_started = {lan::TransferKind::directory};
    const std::vector<lan::TransferKind> expected_completed = {lan::TransferKind::directory};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    EXPECT_EQ(events.started_kinds, expected_started);
    EXPECT_EQ(events.completed_kinds, expected_completed);
    EXPECT_EQ(events.started_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.completed_ids, expected_lifecycle_ids);
    EXPECT_TRUE(events.failed_kinds.empty());

    const std::vector<std::uint64_t> expected_processed = {1, 2, 0, 1, 2};
    const std::vector<std::uint64_t> expected_totals = {0, 0, 2, 2, 2};
    const std::vector<std::uint64_t> expected_progress_ids(expected_processed.size(), 1);
    const std::vector<lan::TransferDirection> expected_directions(
        expected_processed.size(), lan::TransferDirection::send);
    const std::vector<lan::TransferKind> expected_kinds(
        expected_processed.size(), lan::TransferKind::directory);
    EXPECT_EQ(events.directions, expected_directions);
    EXPECT_EQ(events.kinds, expected_kinds);
    EXPECT_EQ(events.progress_ids, expected_progress_ids);
    EXPECT_EQ(events.directory_progress_processed, expected_processed);
    EXPECT_EQ(events.directory_progress_totals, expected_totals);
}

TEST(SenderTransferRunnerTest, ReportsLifecycleFailureWhenConnectionFails) {
    TempDir temp("sender-transfer-failure-test");
    const auto source = temp.path() / "source.txt";
    write_text(source, "abcdef");

    lan::SenderConfig sender_config;
    sender_config.target.host = "memory";
    sender_config.target.port = 1;
    sender_config.source_path = source;

    FakeConnectBackend backend(nullptr);
    CapturingSenderEvents events;
    lan::SenderTransferRunner runner(backend);

    auto transferred = runner.run(sender_config, events);

    ASSERT_FALSE(transferred);
    EXPECT_EQ(transferred.error().code, lan::ErrorCode::network_error);

    const std::vector<lan::TransferKind> expected_started = {lan::TransferKind::file};
    const std::vector<lan::TransferKind> expected_failed = {lan::TransferKind::file};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    const std::vector<lan::TransferState> expected_started_states = {lan::TransferState::running};
    const std::vector<lan::TransferState> expected_failed_states = {lan::TransferState::failed};
    const std::vector<lan::ErrorCode> expected_errors = {lan::ErrorCode::network_error};
    const std::vector<lan::ErrorCategory> expected_categories = {lan::ErrorCategory::network};
    const std::vector<bool> expected_retryable = {true};
    const std::vector<bool> expected_user_action_required = {false};
    EXPECT_EQ(events.started_kinds, expected_started);
    EXPECT_EQ(events.started_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.started_states, expected_started_states);
    EXPECT_TRUE(events.completed_kinds.empty());
    EXPECT_EQ(events.failed_kinds, expected_failed);
    EXPECT_EQ(events.failed_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.failed_states, expected_failed_states);
    EXPECT_EQ(events.failed_errors, expected_errors);
    EXPECT_EQ(events.failed_categories, expected_categories);
    EXPECT_EQ(events.failed_retryable, expected_retryable);
    EXPECT_EQ(events.failed_user_action_required, expected_user_action_required);
    EXPECT_TRUE(events.directions.empty());
    EXPECT_TRUE(events.kinds.empty());
}

TEST(SenderTransferRunnerTest, ReportsLifecycleCancellation) {
    TempDir temp("sender-transfer-cancel-test");
    const auto source = temp.path() / "source.txt";
    write_text(source, "abcdef");

    lan::SenderConfig sender_config;
    sender_config.target.host = "memory";
    sender_config.target.port = 1;
    sender_config.source_path = source;
    sender_config.chunk_size = 3;

    auto pair = make_unique_memory_connection_pair();
    auto server = std::move(pair.server);
    FakeConnectBackend backend(std::move(pair.client));
    lan::SenderTransferRunner runner(backend);
    class CancellingEvents final : public CapturingSenderEvents {
    public:
        explicit CancellingEvents(lan::SenderTransferRunner& runner) : runner_(runner) {}

        void on_file_progress(const lan::SendFileProgress&) override {
            runner_.cancel();
        }

    private:
        lan::SenderTransferRunner& runner_;
    };
    CancellingEvents events(runner);

    std::thread receiver([&] {
        auto hello = lan::read_frame(*server);
        ASSERT_TRUE(hello);

        auto begin = lan::read_frame(*server);
        ASSERT_TRUE(begin);
        ASSERT_EQ(begin.value().type, lan::MessageType::file_begin);

        lan::Frame ack;
        ack.type = lan::MessageType::ack;
        ack.body = lan::bytes_from_string("ready");
        ASSERT_TRUE(lan::write_frame(*server, ack));
    });

    auto transferred = runner.run(sender_config, events);
    receiver.join();

    ASSERT_FALSE(transferred);
    EXPECT_EQ(transferred.error().code, lan::ErrorCode::cancelled);

    const std::vector<lan::TransferKind> expected_started = {lan::TransferKind::file};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    const std::vector<lan::TransferState> expected_started_states = {lan::TransferState::running};
    const std::vector<lan::TransferState> expected_cancelled_states = {
        lan::TransferState::cancelled};
    EXPECT_EQ(events.started_kinds, expected_started);
    EXPECT_EQ(events.started_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.started_states, expected_started_states);
    EXPECT_TRUE(events.completed_kinds.empty());
    EXPECT_TRUE(events.failed_kinds.empty());
    EXPECT_EQ(events.cancelled_kinds, expected_started);
    EXPECT_EQ(events.cancelled_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.cancelled_states, expected_cancelled_states);
}

TEST(ReceiverServerTest, RunsOnceWithInjectedNetworkBackend) {
    TempDir source("receiver-server-source-test");
    TempDir receive("receiver-server-receive-test");

    write_text(source.path() / "same.txt", "same\n");
    write_text(receive.path() / "same.txt", "same\n");
    write_text(source.path() / "new.txt", "new\n");

    auto client_to_server = std::make_shared<MemoryPipe>();
    auto server_to_client = std::make_shared<MemoryPipe>();
    MemoryConnection client(server_to_client, client_to_server);
    auto server_connection =
        std::make_unique<MemoryConnection>(client_to_server, server_to_client);

    auto listener =
        std::make_unique<SingleConnectionListener>(std::move(server_connection));
    FakeNetworkBackend backend(std::move(listener));

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive.path();
    receiver_config.once = true;

    lan::SenderConfig sender_config;
    sender_config.source_path = source.path();

    CapturingReceiverEvents events;
    lan::ReceiverServer server(backend);

    auto sender_result = lan::Result<lan::SendSyncReport>::failure(
        lan::Error{lan::ErrorCode::internal_error, "sender did not run"});
    std::thread sender([&] {
        sender_result = lan::sync_sender_to_connection(sender_config, 5, client);
    });

    auto served = server.run(receiver_config, events);
    sender.join();

    ASSERT_TRUE(sender_result) << sender_result.error().message;
    ASSERT_TRUE(served) << served.error().message;
    EXPECT_TRUE(events.listening);
    EXPECT_TRUE(events.directory_synced);
    EXPECT_FALSE(events.file_received);
    EXPECT_FALSE(events.client_error);
    const std::vector<lan::TransferKind> expected_lifecycle = {lan::TransferKind::directory};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    EXPECT_EQ(events.started_kinds, expected_lifecycle);
    EXPECT_EQ(events.completed_kinds, expected_lifecycle);
    EXPECT_EQ(events.started_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.completed_ids, expected_lifecycle_ids);
    EXPECT_TRUE(events.failed_kinds.empty());
    EXPECT_EQ(served.value().accepted_connections, 1);
    EXPECT_EQ(served.value().failed_connections, 0);
    EXPECT_EQ(events.sync_report.skipped_files, 1);
    EXPECT_EQ(events.sync_report.full_files, 1);

    const std::vector<std::uint64_t> expected_processed = {0, 1, 2};
    const std::vector<std::uint64_t> expected_totals = {2, 2, 2};
    const std::vector<std::uint64_t> expected_progress_ids(expected_processed.size(), 1);
    EXPECT_EQ(events.progress_ids, expected_progress_ids);
    EXPECT_EQ(events.directory_progress_processed, expected_processed);
    EXPECT_EQ(events.directory_progress_totals, expected_totals);

    auto src_hash = lan::hash_file(source.path() / "new.txt");
    auto dst_hash = lan::hash_file(receive.path() / "new.txt");
    ASSERT_TRUE(src_hash);
    ASSERT_TRUE(dst_hash);
    EXPECT_EQ(src_hash.value().hex_digest, dst_hash.value().hex_digest);
}

TEST(ReceiverServerTest, EmitsFileProgressEvents) {
    TempDir temp("receiver-server-progress-test");
    const auto source = temp.path() / "source.txt";
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);
    write_text(source, "abcdef");

    auto hash = lan::hash_file(source);
    ASSERT_TRUE(hash);

    auto client_to_server = std::make_shared<MemoryPipe>();
    auto server_to_client = std::make_shared<MemoryPipe>();
    MemoryConnection client(server_to_client, client_to_server);
    auto server_connection =
        std::make_unique<MemoryConnection>(client_to_server, server_to_client);

    auto listener =
        std::make_unique<SingleConnectionListener>(std::move(server_connection));
    FakeNetworkBackend backend(std::move(listener));

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;
    receiver_config.once = true;

    CapturingReceiverEvents events;
    lan::ReceiverServer server(backend);

    auto served_future = std::async(std::launch::async, [&] {
        return server.run(receiver_config, events);
    });

    ASSERT_TRUE(events.wait_until_listening());

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::bytes_from_string("file");
    ASSERT_TRUE(lan::write_frame(client, hello));

    lan::Frame begin;
    begin.type = lan::MessageType::file_begin;
    begin.body = lan::bytes_from_string(lan::encode_file_begin(lan::FileBeginMetadata{
        .name = "progress.txt",
        .size = 6,
        .sha256 = hash.value().hex_digest,
    }));
    ASSERT_TRUE(lan::write_frame(client, begin));

    auto begin_ack = lan::read_frame(client);
    ASSERT_TRUE(begin_ack);
    ASSERT_EQ(begin_ack.value().type, lan::MessageType::ack);

    auto first_payload = lan::bytes_from_string("abc");
    lan::Frame first_chunk;
    first_chunk.type = lan::MessageType::chunk;
    first_chunk.body = lan::encode_chunk_body(0, first_payload.data(), first_payload.size());
    ASSERT_TRUE(lan::write_frame(client, first_chunk));

    auto second_payload = lan::bytes_from_string("def");
    lan::Frame second_chunk;
    second_chunk.type = lan::MessageType::chunk;
    second_chunk.body = lan::encode_chunk_body(3, second_payload.data(), second_payload.size());
    ASSERT_TRUE(lan::write_frame(client, second_chunk));

    lan::Frame end;
    end.type = lan::MessageType::file_end;
    ASSERT_TRUE(lan::write_frame(client, end));

    auto end_ack = lan::read_frame(client);
    ASSERT_TRUE(end_ack);
    ASSERT_EQ(end_ack.value().type, lan::MessageType::ack);

    auto served = served_future.get();
    ASSERT_TRUE(served) << served.error().message;
    EXPECT_TRUE(events.file_received);
    EXPECT_FALSE(events.directory_synced);
    const std::vector<lan::TransferKind> expected_lifecycle = {lan::TransferKind::file};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    const std::vector<lan::TransferState> expected_started_states = {lan::TransferState::running};
    const std::vector<lan::TransferState> expected_completed_states = {
        lan::TransferState::completed};
    const std::vector<lan::TransferCompletionStatus> expected_statuses = {
        lan::TransferCompletionStatus::transferred};
    const std::vector<std::uint64_t> expected_resumed_from = {0};
    EXPECT_EQ(events.started_kinds, expected_lifecycle);
    EXPECT_EQ(events.completed_kinds, expected_lifecycle);
    EXPECT_EQ(events.started_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.completed_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.started_states, expected_started_states);
    EXPECT_EQ(events.completed_states, expected_completed_states);
    EXPECT_EQ(events.completed_statuses, expected_statuses);
    EXPECT_EQ(events.completed_resumed_from, expected_resumed_from);
    EXPECT_TRUE(events.failed_kinds.empty());
    EXPECT_EQ(read_text(receive_dir / "progress.txt"), "abcdef");

    const std::vector<std::uint64_t> expected_bytes = {0, 3, 6};
    const std::vector<std::uint64_t> expected_totals = {6, 6, 6};
    const std::vector<std::uint64_t> expected_progress_ids(expected_bytes.size(), 1);
    const std::vector<lan::TransferState> expected_progress_states(
        expected_bytes.size(), lan::TransferState::running);
    EXPECT_EQ(events.progress_ids, expected_progress_ids);
    EXPECT_EQ(events.progress_states, expected_progress_states);
    EXPECT_EQ(events.file_progress_bytes, expected_bytes);
    EXPECT_EQ(events.file_progress_totals, expected_totals);
}

TEST(ReceiverServerTest, ReportsProtocolFailureSemantics) {
    TempDir temp("receiver-server-protocol-failure-test");
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);

    auto client_to_server = std::make_shared<MemoryPipe>();
    auto server_to_client = std::make_shared<MemoryPipe>();
    MemoryConnection client(server_to_client, client_to_server);
    auto server_connection =
        std::make_unique<MemoryConnection>(client_to_server, server_to_client);

    auto listener =
        std::make_unique<SingleConnectionListener>(std::move(server_connection));
    FakeNetworkBackend backend(std::move(listener));

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;
    receiver_config.once = true;

    CapturingReceiverEvents events;
    lan::ReceiverServer server(backend);

    auto served_future = std::async(std::launch::async, [&] {
        return server.run(receiver_config, events);
    });

    ASSERT_TRUE(events.wait_until_listening());

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::bytes_from_string("file");
    ASSERT_TRUE(lan::write_frame(client, hello));

    lan::Frame wrong;
    wrong.type = lan::MessageType::file_end;
    ASSERT_TRUE(lan::write_frame(client, wrong));

    auto served = served_future.get();
    ASSERT_FALSE(served);
    EXPECT_TRUE(events.client_error);
    EXPECT_EQ(events.last_error.code, lan::ErrorCode::protocol_error);

    const std::vector<lan::TransferKind> expected_failed = {lan::TransferKind::file};
    const std::vector<std::uint64_t> expected_lifecycle_ids = {1};
    const std::vector<lan::TransferState> expected_failed_states = {lan::TransferState::failed};
    const std::vector<lan::ErrorCode> expected_errors = {lan::ErrorCode::protocol_error};
    const std::vector<lan::ErrorCategory> expected_categories = {lan::ErrorCategory::protocol};
    const std::vector<bool> expected_retryable = {false};
    const std::vector<bool> expected_user_action_required = {true};
    EXPECT_EQ(events.failed_kinds, expected_failed);
    EXPECT_EQ(events.failed_ids, expected_lifecycle_ids);
    EXPECT_EQ(events.failed_states, expected_failed_states);
    EXPECT_EQ(events.failed_errors, expected_errors);
    EXPECT_EQ(events.failed_categories, expected_categories);
    EXPECT_EQ(events.failed_retryable, expected_retryable);
    EXPECT_EQ(events.failed_user_action_required, expected_user_action_required);
    EXPECT_TRUE(events.completed_kinds.empty());
}

TEST(ReceiverServerTest, StopsActiveFileTransferAndCleansPartFile) {
    TempDir temp("receiver-server-active-stop-test");
    const auto receive_dir = temp.path() / "receive";
    std::filesystem::create_directories(receive_dir);

    auto client_to_server = std::make_shared<MemoryPipe>();
    auto server_to_client = std::make_shared<MemoryPipe>();
    MemoryConnection client(server_to_client, client_to_server);
    auto server_connection =
        std::make_unique<MemoryConnection>(client_to_server, server_to_client);

    auto listener =
        std::make_unique<SingleConnectionListener>(std::move(server_connection));
    FakeNetworkBackend backend(std::move(listener));

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = receive_dir;
    receiver_config.once = true;

    CapturingReceiverEvents events;
    lan::ReceiverServer server(backend);

    auto served_future = std::async(std::launch::async, [&] {
        return server.run(receiver_config, events);
    });

    ASSERT_TRUE(events.wait_until_listening());

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::bytes_from_string("file");
    ASSERT_TRUE(lan::write_frame(client, hello));

    lan::Frame begin;
    begin.type = lan::MessageType::file_begin;
    begin.body = lan::bytes_from_string(lan::encode_file_begin(lan::FileBeginMetadata{
        .name = "cancelled.txt",
        .size = 6,
        .sha256 = "not-used-after-cancel",
    }));
    ASSERT_TRUE(lan::write_frame(client, begin));

    auto begin_ack = lan::read_frame(client);
    ASSERT_TRUE(begin_ack);
    ASSERT_EQ(begin_ack.value().type, lan::MessageType::ack);

    auto payload = lan::bytes_from_string("abc");
    lan::Frame chunk;
    chunk.type = lan::MessageType::chunk;
    chunk.body = lan::encode_chunk_body(0, payload.data(), payload.size());
    ASSERT_TRUE(lan::write_frame(client, chunk));

    server.stop();

    auto served = served_future.get();
    ASSERT_TRUE(served) << served.error().message;
    EXPECT_TRUE(served.value().stopped);
    EXPECT_EQ(served.value().accepted_connections, 0);
    EXPECT_EQ(served.value().failed_connections, 0);
    EXPECT_FALSE(events.client_error);

    const std::vector<lan::TransferKind> expected_kinds = {lan::TransferKind::file};
    const std::vector<std::uint64_t> expected_ids = {1};
    EXPECT_EQ(events.started_kinds, expected_kinds);
    EXPECT_EQ(events.started_ids, expected_ids);
    EXPECT_TRUE(events.completed_kinds.empty());
    EXPECT_TRUE(events.failed_kinds.empty());
    EXPECT_EQ(events.cancelled_kinds, expected_kinds);
    EXPECT_EQ(events.cancelled_ids, expected_ids);
    EXPECT_FALSE(std::filesystem::exists(receive_dir / "cancelled.txt"));
    EXPECT_FALSE(std::filesystem::exists(receive_dir / "cancelled.txt.part"));
}

TEST(ReceiverServerTest, StopsWhileWaitingForConnection) {
    auto listener = std::make_unique<BlockingListener>();
    FakeNetworkBackend backend(std::move(listener));

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = std::filesystem::temp_directory_path();

    CapturingReceiverEvents events;
    lan::ReceiverServer server(backend);

    auto served_future = std::async(std::launch::async, [&] {
        return server.run(receiver_config, events);
    });

    ASSERT_TRUE(events.wait_until_listening());
    server.stop();

    auto served = served_future.get();
    ASSERT_TRUE(served) << served.error().message;
    EXPECT_TRUE(served.value().stopped);
    EXPECT_EQ(served.value().accepted_connections, 0);
    EXPECT_EQ(served.value().failed_connections, 0);
    EXPECT_FALSE(events.client_error);
}

TEST(ReceiverServerRunnerTest, StartsStopsAndJoinsBackgroundServer) {
    auto listener = std::make_unique<BlockingListener>();
    FakeNetworkBackend backend(std::move(listener));

    lan::ReceiverConfig receiver_config;
    receiver_config.receive_dir = std::filesystem::temp_directory_path();

    CapturingReceiverEvents events;
    lan::ReceiverServerRunner runner(backend);

    auto started = runner.start(receiver_config, events);
    ASSERT_TRUE(started) << started.error().message;

    ASSERT_TRUE(events.wait_until_listening());
    EXPECT_TRUE(runner.running());

    runner.stop();
    auto served = runner.join();
    ASSERT_TRUE(served) << served.error().message;

    EXPECT_TRUE(served.value().stopped);
    EXPECT_EQ(served.value().accepted_connections, 0);
    EXPECT_EQ(served.value().failed_connections, 0);
    EXPECT_FALSE(runner.running());
}

}  // namespace

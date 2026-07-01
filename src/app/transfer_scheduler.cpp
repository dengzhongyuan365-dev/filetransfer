#include "lan/app/transfer_scheduler.h"

#include <algorithm>
#include <utility>

#include "lan/common/error.h"

namespace lan {

namespace {

int clamp_limit(int value, int min_value, int max_value) {
    return std::clamp(value, min_value, max_value);
}

TransferSnapshot make_failed_snapshot(SchedulerTaskId task_id,
                                      const std::filesystem::path& source_path,
                                      const Error& error) {
    TransferSnapshot snapshot;
    snapshot.transfer_id = task_id;
    snapshot.direction = TransferDirection::send;
    snapshot.kind = std::filesystem::is_directory(source_path)
                        ? TransferKind::directory
                        : TransferKind::file;
    snapshot.path = source_path;
    snapshot.name = source_path.filename().string();
    snapshot.state = TransferState::failed;
    snapshot.error = error;
    snapshot.error_category = error_category(error);
    snapshot.retryable = is_retryable(error);
    snapshot.user_action_required = needs_user_action(error);
    return snapshot;
}

class TaskScopedEvents final : public SenderTransferEvents {
public:
    TaskScopedEvents(SchedulerTaskId task_id,
                     std::string peer_id,
                     std::function<void(SchedulerSnapshot)> on_snapshot)
        : task_id_(task_id),
          peer_id_(std::move(peer_id)),
          on_snapshot_(std::move(on_snapshot)) {}

    void on_transfer_started(const TransferStarted& started) override {
        TransferSnapshot snapshot;
        snapshot.transfer_id = task_id_;
        snapshot.state = started.state;
        snapshot.direction = started.direction;
        snapshot.kind = started.kind;
        snapshot.path = started.path;
        snapshot.name = started.name;
        on_snapshot_(SchedulerSnapshot{.task_id = task_id_, .peer_id = peer_id_, .snapshot = std::move(snapshot)});
    }

    void on_transfer_progress(const TransferProgress& progress) override {
        TransferSnapshot snapshot;
        snapshot.transfer_id = task_id_;
        snapshot.state = progress.state;
        snapshot.direction = progress.direction;
        snapshot.kind = progress.kind;
        snapshot.path = progress.path;
        snapshot.name = progress.name;
        snapshot.current_bytes = progress.current_bytes;
        snapshot.total_bytes = progress.total_bytes;
        snapshot.processed_files = progress.processed_files;
        snapshot.total_files = progress.total_files;
        snapshot.skipped_files = progress.skipped_files;
        snapshot.full_files = progress.full_files;
        snapshot.delta_files = progress.delta_files;
        snapshot.payload_bytes = progress.payload_bytes;
        snapshot.elapsed_seconds = progress.elapsed_seconds;
        on_snapshot_(SchedulerSnapshot{.task_id = task_id_, .peer_id = peer_id_, .snapshot = std::move(snapshot)});
    }

    void on_transfer_completed(const TransferCompleted& completed) override {
        TransferSnapshot snapshot;
        snapshot.transfer_id = task_id_;
        snapshot.state = completed.state;
        snapshot.direction = completed.direction;
        snapshot.kind = completed.kind;
        snapshot.path = completed.path;
        snapshot.name = completed.name;
        snapshot.current_bytes = completed.bytes;
        snapshot.total_bytes = completed.bytes;
        snapshot.total_files = completed.total_files;
        snapshot.skipped_files = completed.skipped_files;
        snapshot.full_files = completed.full_files;
        snapshot.delta_files = completed.delta_files;
        snapshot.payload_bytes = completed.payload_bytes;
        snapshot.completion_status = completed.status;
        snapshot.resumed_from = completed.resumed_from;
        snapshot.elapsed_seconds = completed.elapsed_seconds;
        on_snapshot_(SchedulerSnapshot{.task_id = task_id_, .peer_id = peer_id_, .snapshot = std::move(snapshot)});
    }

    void on_transfer_failed(const TransferFailed& failed) override {
        TransferSnapshot snapshot;
        snapshot.transfer_id = task_id_;
        snapshot.state = failed.state;
        snapshot.direction = failed.direction;
        snapshot.kind = failed.kind;
        snapshot.path = failed.path;
        snapshot.name = failed.name;
        snapshot.error = failed.error;
        snapshot.error_category = failed.category;
        snapshot.retryable = failed.retryable;
        snapshot.user_action_required = failed.user_action_required;
        on_snapshot_(SchedulerSnapshot{.task_id = task_id_, .peer_id = peer_id_, .snapshot = std::move(snapshot)});
    }

    void on_transfer_cancelled(const TransferCancelled& cancelled) override {
        TransferSnapshot snapshot;
        snapshot.transfer_id = task_id_;
        snapshot.state = cancelled.state;
        snapshot.direction = cancelled.direction;
        snapshot.kind = cancelled.kind;
        snapshot.path = cancelled.path;
        snapshot.name = cancelled.name;
        on_snapshot_(SchedulerSnapshot{.task_id = task_id_, .peer_id = peer_id_, .snapshot = std::move(snapshot)});
    }

private:
    SchedulerTaskId task_id_ = 0;
    std::string peer_id_;
    std::function<void(SchedulerSnapshot)> on_snapshot_;
};

}  // namespace

TransferScheduler::TransferScheduler(SchedulerCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

TransferScheduler::~TransferScheduler() {
    stop_all();
}

void TransferScheduler::set_callbacks(SchedulerCallbacks callbacks) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    callbacks_ = std::move(callbacks);
}

void TransferScheduler::set_limits(SchedulerLimits limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    limits_.max_global_sends = clamp_limit(limits.max_global_sends, 1, 8);
    limits_.max_peer_sends = clamp_limit(limits.max_peer_sends, 1, 4);
    start_next_locked();
}

SchedulerLimits TransferScheduler::limits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return limits_;
}

void TransferScheduler::upsert_peer(SchedulerPeer peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_[peer.id] = std::move(peer);
    start_next_locked();
}

void TransferScheduler::remove_peer(const std::string& peer_id) {
    cancel_peer(peer_id);
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(peer_id);
}

SchedulerTaskId TransferScheduler::enqueue_send(const std::string& peer_id,
                                                std::filesystem::path source_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto task_id = next_task_id_++;
    queue_.push_back(QueuedSend{.task_id = task_id, .peer_id = peer_id, .source_path = source_path});
    emit_snapshot(SchedulerSnapshot{.task_id = task_id,
                                    .peer_id = peer_id,
                                    .snapshot = make_pending_snapshot(task_id, source_path)});
    start_next_locked();
    return task_id;
}

void TransferScheduler::cancel_task(SchedulerTaskId task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_queued_locked(task_id);
    cancel_running_locked(task_id);
    reap_completed_locked();
    start_next_locked();
}

void TransferScheduler::cancel_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->peer_id == peer_id) {
            emit_snapshot(SchedulerSnapshot{
                .task_id = it->task_id,
                .peer_id = it->peer_id,
                .snapshot = make_cancelled_snapshot(it->task_id, it->source_path),
            });
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<SchedulerTaskId> running_ids;
    for (const auto& [task_id, running] : running_) {
        if (running->peer_id == peer_id) {
            running_ids.push_back(task_id);
        }
    }
    for (const auto task_id : running_ids) {
        cancel_running_locked(task_id);
    }
    reap_completed_locked();
    start_next_locked();
}

void TransferScheduler::stop_all() {
    std::vector<std::unique_ptr<RunningSend>> running;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        for (auto& [_, send] : running_) {
            if (send->runner != nullptr) {
                send->runner->cancel();
            }
            running.push_back(std::move(send));
        }
        running_.clear();
    }

    for (auto& send : running) {
        if (send->thread.joinable()) {
            send->thread.join();
        }
    }
}

void TransferScheduler::pump() {
    std::lock_guard<std::mutex> lock(mutex_);
    reap_completed_locked();
    start_next_locked();
}

bool TransferScheduler::has_pending_or_running_for_peer(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto queued = std::any_of(queue_.cbegin(), queue_.cend(), [&peer_id](const QueuedSend& item) {
        return item.peer_id == peer_id;
    });
    return queued || running_count_for_peer_locked(peer_id) > 0;
}

void TransferScheduler::start_next_locked() {
    bool started_any = false;
    while (!queue_.empty() &&
           static_cast<int>(running_.size()) < limits_.max_global_sends) {
        const auto it = std::find_if(queue_.begin(), queue_.end(), [this](const QueuedSend& item) {
            const auto peer_it = peers_.find(item.peer_id);
            if (peer_it == peers_.end()) {
                return false;
            }
            return peer_it->second.linked &&
                   peer_it->second.online &&
                   running_count_for_peer_locked(item.peer_id) < limits_.max_peer_sends;
        });
        if (it == queue_.end()) {
            if (!started_any) {
                emit_log("queued sends are waiting for linked machines to come online");
            }
            return;
        }

        auto item = *it;
        queue_.erase(it);
        if (start_sender_locked(item)) {
            started_any = true;
        }
    }
}

bool TransferScheduler::start_sender_locked(const QueuedSend& item) {
    const auto peer_it = peers_.find(item.peer_id);
    if (peer_it == peers_.end() || !peer_it->second.linked) {
        const Error error{ErrorCode::network_error, "target machine is no longer linked"};
        emit_snapshot(SchedulerSnapshot{
            .task_id = item.task_id,
            .peer_id = item.peer_id,
            .snapshot = make_failed_snapshot(item.task_id, item.source_path, error),
        });
        return false;
    }

    SenderConfig config;
    config.target.host = peer_it->second.host;
    config.target.port = peer_it->second.port;
    config.source_path = item.source_path;
    config.resume = true;

    auto validated = validate_sender_config(std::move(config));
    if (!validated) {
        emit_snapshot(SchedulerSnapshot{
            .task_id = item.task_id,
            .peer_id = item.peer_id,
            .snapshot = make_failed_snapshot(item.task_id, item.source_path, validated.error()),
        });
        return false;
    }

    auto running = std::make_unique<RunningSend>();
    running->task_id = item.task_id;
    running->peer_id = item.peer_id;
    running->source_path = item.source_path;
    running->config = std::move(validated).value();
    running->runner = std::make_unique<SenderTransferRunner>();
    running->events = std::make_shared<TaskScopedEvents>(
        item.task_id,
        item.peer_id,
        [this](SchedulerSnapshot snapshot) {
            emit_snapshot(std::move(snapshot));
        });

    auto* runner = running->runner.get();
    const auto send_config = running->config;
    const auto task_id = item.task_id;
    auto events = running->events;
    running->thread = std::thread([this, task_id, send_config, runner, events] {
        auto result = runner->run(send_config, *events);
        if (!result) {
            emit_log("connect or send failed: " + format_error(result.error()));
        }
        mark_completed(task_id);
    });

    running_.emplace(item.task_id, std::move(running));
    return true;
}

void TransferScheduler::reap_completed_locked() {
    for (auto it = running_.begin(); it != running_.end();) {
        if (!it->second->completed) {
            ++it;
            continue;
        }
        auto running = std::move(it->second);
        it = running_.erase(it);
        if (running->thread.joinable()) {
            running->thread.join();
        }
    }
}

void TransferScheduler::cancel_running_locked(SchedulerTaskId task_id) {
    const auto it = running_.find(task_id);
    if (it == running_.end()) {
        return;
    }
    if (it->second->runner != nullptr) {
        it->second->runner->cancel();
    }
}

void TransferScheduler::cancel_queued_locked(SchedulerTaskId task_id) {
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->task_id == task_id) {
            emit_snapshot(SchedulerSnapshot{
                .task_id = it->task_id,
                .peer_id = it->peer_id,
                .snapshot = make_cancelled_snapshot(it->task_id, it->source_path),
            });
            it = queue_.erase(it);
            continue;
        }
        ++it;
    }
}

int TransferScheduler::running_count_for_peer_locked(const std::string& peer_id) const {
    int count = 0;
    for (const auto& [_, running] : running_) {
        if (running->peer_id == peer_id) {
            ++count;
        }
    }
    return count;
}

void TransferScheduler::mark_completed(SchedulerTaskId task_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = running_.find(task_id);
        if (it != running_.end()) {
            it->second->completed = true;
        }
    }
    emit_wakeup();
}

SchedulerCallbacks TransferScheduler::callbacks() const {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    return callbacks_;
}

void TransferScheduler::emit_snapshot(SchedulerSnapshot snapshot) {
    auto callbacks_copy = callbacks();
    if (callbacks_copy.on_snapshot) {
        callbacks_copy.on_snapshot(std::move(snapshot));
    }
}

void TransferScheduler::emit_log(std::string line) {
    auto callbacks_copy = callbacks();
    if (callbacks_copy.on_log) {
        callbacks_copy.on_log(std::move(line));
    }
}

void TransferScheduler::emit_wakeup() {
    auto callbacks_copy = callbacks();
    if (callbacks_copy.on_wakeup) {
        callbacks_copy.on_wakeup();
    }
}

TransferSnapshot TransferScheduler::make_pending_snapshot(SchedulerTaskId task_id,
                                                          const std::filesystem::path& source_path) const {
    TransferSnapshot snapshot;
    snapshot.transfer_id = task_id;
    snapshot.state = TransferState::pending;
    snapshot.direction = TransferDirection::send;
    snapshot.kind = std::filesystem::is_directory(source_path)
                        ? TransferKind::directory
                        : TransferKind::file;
    snapshot.path = source_path;
    snapshot.name = source_path.filename().string();
    if (snapshot.name.empty()) {
        snapshot.name = source_path.string();
    }
    return snapshot;
}

TransferSnapshot TransferScheduler::make_cancelled_snapshot(SchedulerTaskId task_id,
                                                            const std::filesystem::path& source_path) const {
    auto snapshot = make_pending_snapshot(task_id, source_path);
    snapshot.state = TransferState::cancelled;
    return snapshot;
}

}  // namespace lan

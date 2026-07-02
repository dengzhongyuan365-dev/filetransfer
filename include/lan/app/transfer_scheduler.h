#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lan/app/sender_config.h"
#include "lan/app/sender_transfer.h"
#include "lan/app/transfer_snapshot.h"

namespace lan {

using SchedulerTaskId = std::uint64_t;

struct SchedulerLimits {
    int max_global_sends = 1;
    int max_peer_sends = 1;
};

struct SchedulerPeer {
    std::string id;
    std::string name;
    std::string host;
    std::uint16_t port = 0;
    bool online = false;
    bool linked = false;
};

struct SchedulerSnapshot {
    SchedulerTaskId task_id = 0;
    std::string peer_id;
    TransferSnapshot snapshot;
};

struct SchedulerPeerStats {
    int queued = 0;
    int running = 0;
    int waiting = 0;
    int paused = 0;
};

enum class SchedulerTaskStatus {
    queued,
    waiting_for_peer,
    paused,
    running,
};

struct SchedulerTaskInfo {
    SchedulerTaskId task_id = 0;
    std::string peer_id;
    std::filesystem::path source_path;
    SchedulerTaskStatus status = SchedulerTaskStatus::queued;
    TransferSnapshot snapshot;
};

struct SchedulerCallbacks {
    std::function<void(SchedulerSnapshot)> on_snapshot;
    std::function<void(std::string)> on_log;
    std::function<void()> on_wakeup;
};

class TransferScheduler {
public:
    explicit TransferScheduler(SchedulerCallbacks callbacks = {});
    ~TransferScheduler();

    TransferScheduler(const TransferScheduler&) = delete;
    TransferScheduler& operator=(const TransferScheduler&) = delete;

    void set_callbacks(SchedulerCallbacks callbacks);
    void set_limits(SchedulerLimits limits);
    SchedulerLimits limits() const;

    void upsert_peer(SchedulerPeer peer);
    void remove_peer(const std::string& peer_id);

    SchedulerTaskId enqueue_send(const std::string& peer_id, std::filesystem::path source_path);
    std::vector<SchedulerTaskId> enqueue_send_to_peers(const std::vector<std::string>& peer_ids,
                                                       std::filesystem::path source_path);
    bool move_queued_task(SchedulerTaskId task_id, const std::string& peer_id);
    bool pause_task(SchedulerTaskId task_id);
    bool resume_task(SchedulerTaskId task_id);
    void cancel_task(SchedulerTaskId task_id);
    void cancel_peer(const std::string& peer_id);
    void stop_all();
    void pump();

    bool has_pending_or_running_for_peer(const std::string& peer_id) const;
    SchedulerPeerStats peer_stats(const std::string& peer_id) const;
    std::vector<SchedulerTaskInfo> tasks() const;
    std::vector<SchedulerTaskInfo> peer_tasks(const std::string& peer_id) const;

private:
    struct QueuedSend {
        SchedulerTaskId task_id = 0;
        std::string peer_id;
        std::filesystem::path source_path;
    };

    struct RunningSend {
        SchedulerTaskId task_id = 0;
        std::string peer_id;
        std::filesystem::path source_path;
        SenderConfig config;
        std::unique_ptr<SenderTransferRunner> runner;
        std::shared_ptr<SenderTransferEvents> events;
        std::thread thread;
        bool completed = false;
    };

    struct SchedulerEmissions {
        std::vector<SchedulerSnapshot> snapshots;
        std::vector<std::string> logs;
    };

    void start_next_locked(SchedulerEmissions& emissions);
    bool start_sender_locked(const QueuedSend& item, SchedulerEmissions& emissions);
    void reap_completed_locked();
    void cancel_running_locked(SchedulerTaskId task_id);
    void cancel_queued_locked(SchedulerTaskId task_id, SchedulerEmissions& emissions);
    void cancel_paused_locked(SchedulerTaskId task_id, SchedulerEmissions& emissions);
    int running_count_for_peer_locked(const std::string& peer_id) const;
    SchedulerTaskStatus queued_status_locked(const QueuedSend& item) const;
    void mark_completed(SchedulerTaskId task_id);
    SchedulerCallbacks callbacks() const;
    void flush_emissions(SchedulerEmissions emissions);
    void emit_snapshot(SchedulerSnapshot snapshot);
    void emit_log(std::string line);
    void emit_wakeup();
    TransferSnapshot make_pending_snapshot(SchedulerTaskId task_id,
                                           const std::filesystem::path& source_path) const;
    TransferSnapshot make_running_snapshot(SchedulerTaskId task_id,
                                           const std::filesystem::path& source_path) const;
    TransferSnapshot make_paused_snapshot(SchedulerTaskId task_id,
                                          const std::filesystem::path& source_path) const;
    TransferSnapshot make_cancelled_snapshot(SchedulerTaskId task_id,
                                             const std::filesystem::path& source_path) const;

    mutable std::mutex mutex_;
    mutable std::mutex callbacks_mutex_;
    SchedulerCallbacks callbacks_;
    SchedulerLimits limits_;
    std::map<std::string, SchedulerPeer> peers_;
    std::deque<QueuedSend> queue_;
    std::map<SchedulerTaskId, QueuedSend> paused_;
    std::map<SchedulerTaskId, std::unique_ptr<RunningSend>> running_;
    SchedulerTaskId next_task_id_ = 1;
};

}  // namespace lan

#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include <QList>
#include <QString>

#include "gui/transfer_record_store.h"

namespace lan::gui {

class TransferRecordRepository {
public:
    TransferRecordRepository(std::filesystem::path path,
                             QString interrupted_message,
                             std::chrono::milliseconds debounce = std::chrono::milliseconds(750));
    ~TransferRecordRepository();

    TransferRecordRepository(const TransferRecordRepository&) = delete;
    TransferRecordRepository& operator=(const TransferRecordRepository&) = delete;

    QList<PersistedTransferRecord> load(std::uint64_t restored_base) const;
    void request_save(QList<PersistedTransferRecord> records);
    bool flush(std::chrono::milliseconds timeout);
    QString last_error() const;

private:
    void worker_loop();
    bool write_records(const QList<PersistedTransferRecord>& records);
    QString path_string() const;

    std::filesystem::path path_;
    QString interrupted_message_;
    std::chrono::milliseconds debounce_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    QList<PersistedTransferRecord> latest_records_;
    mutable QString last_error_;
    std::thread worker_;
    std::uint64_t generation_ = 0;
    bool dirty_ = false;
    bool writing_ = false;
    bool flush_requested_ = false;
    bool stop_ = false;
};

}  // namespace lan::gui

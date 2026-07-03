#include "gui/transfer_record_repository.h"

#include <QFile>
#include <QJsonParseError>
#include <QSaveFile>

namespace lan::gui {

TransferRecordRepository::TransferRecordRepository(std::filesystem::path path,
                                                   QString interrupted_message,
                                                   std::chrono::milliseconds debounce)
    : path_(std::move(path)),
      interrupted_message_(std::move(interrupted_message)),
      debounce_(debounce) {
    worker_ = std::thread([this] {
        worker_loop();
    });
}

TransferRecordRepository::~TransferRecordRepository() {
    (void)flush(std::chrono::seconds(2));
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        flush_requested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

QList<PersistedTransferRecord> TransferRecordRepository::load(std::uint64_t restored_base) const {
    QFile file(path_string());
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        std::lock_guard lock(mutex_);
        last_error_ = file.errorString();
        return {};
    }

    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        std::lock_guard lock(mutex_);
        last_error_ = parse_error.errorString();
        return {};
    }

    return transfer_records_from_json(document, restored_base, interrupted_message_);
}

void TransferRecordRepository::request_save(QList<PersistedTransferRecord> records) {
    {
        std::lock_guard lock(mutex_);
        latest_records_ = std::move(records);
        dirty_ = true;
        ++generation_;
    }
    cv_.notify_all();
}

bool TransferRecordRepository::flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (dirty_) {
        flush_requested_ = true;
        cv_.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    return cv_.wait_until(lock, deadline, [this] {
        return !dirty_ && !writing_ && !flush_requested_;
    });
}

QString TransferRecordRepository::last_error() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

void TransferRecordRepository::worker_loop() {
    while (true) {
        QList<PersistedTransferRecord> records;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return stop_ || dirty_;
            });
            if (stop_ && !dirty_) {
                break;
            }

            while (!stop_ && !flush_requested_) {
                const auto observed_generation = generation_;
                const auto due = std::chrono::steady_clock::now() + debounce_;
                if (!cv_.wait_until(lock, due, [this, observed_generation] {
                        return stop_ || flush_requested_ || generation_ != observed_generation;
                    })) {
                    break;
                }
            }

            records = latest_records_;
            dirty_ = false;
            flush_requested_ = false;
            writing_ = true;
        }

        const auto ok = write_records(records);

        {
            std::lock_guard lock(mutex_);
            if (ok) {
                last_error_.clear();
            }
            writing_ = false;
        }
        cv_.notify_all();
    }
}

bool TransferRecordRepository::write_records(const QList<PersistedTransferRecord>& records) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
        std::lock_guard lock(mutex_);
        last_error_ = QString::fromStdString(ec.message());
        return false;
    }

    QSaveFile file(path_string());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::lock_guard lock(mutex_);
        last_error_ = file.errorString();
        return false;
    }

    const auto document = transfer_records_to_json(records, interrupted_message_);
    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        std::lock_guard lock(mutex_);
        last_error_ = file.errorString();
        return false;
    }
    if (!file.commit()) {
        std::lock_guard lock(mutex_);
        last_error_ = file.errorString();
        return false;
    }
    return true;
}

QString TransferRecordRepository::path_string() const {
    return QString::fromStdString(path_.string());
}

}  // namespace lan::gui

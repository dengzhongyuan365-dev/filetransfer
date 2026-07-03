#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <QJsonArray>
#include <QJsonObject>

#include "gui/transfer_list_model.h"
#include "gui/transfer_record_matcher.h"
#include "gui/transfer_record_repository.h"
#include "gui/transfer_record_store.h"

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

lan::TransferSnapshot make_snapshot(std::uint64_t id, lan::TransferDirection direction) {
    lan::TransferSnapshot snapshot;
    snapshot.transfer_id = id;
    snapshot.direction = direction;
    snapshot.state = lan::TransferState::running;
    snapshot.name = "demo.txt";
    return snapshot;
}

TEST(TransferListModelTest, ReceiveSnapshotsAreScopedToTheirPeer) {
    lan::gui::TransferListModel model;
    const auto snapshot = make_snapshot(1, lan::TransferDirection::receive);

    model.upsert(snapshot, QStringLiteral("node-a"));

    EXPECT_TRUE(model.belongs_to_peer(lan::gui::transfer_snapshot_key(snapshot),
                                      QStringLiteral("node-a"),
                                      true));
    EXPECT_FALSE(model.belongs_to_peer(lan::gui::transfer_snapshot_key(snapshot),
                                       QStringLiteral("node-b"),
                                       true));
    EXPECT_EQ(model.visible_entries(QStringLiteral("node-a"), true).size(), 1);
    EXPECT_TRUE(model.visible_entries(QStringLiteral("node-b"), true).isEmpty());
}

TEST(TransferListModelTest, UnscopedSnapshotsAreNotGlobalDevicePageItems) {
    lan::gui::TransferListModel model;
    const auto snapshot = make_snapshot(2, lan::TransferDirection::receive);

    model.upsert(snapshot);

    EXPECT_FALSE(model.belongs_to_peer(lan::gui::transfer_snapshot_key(snapshot),
                                       QStringLiteral("node-a"),
                                       true));
    EXPECT_TRUE(model.visible_entries(QStringLiteral("node-a"), true).isEmpty());
}

TEST(TransferListModelTest, ExposesAllEntriesAndPeerIds) {
    lan::gui::TransferListModel model;
    const auto first = make_snapshot(3, lan::TransferDirection::send);
    const auto second = make_snapshot(4, lan::TransferDirection::receive);

    model.upsert(first, QStringLiteral("node-a"));
    model.upsert(second, QStringLiteral("node-b"));

    EXPECT_EQ(model.entries().size(), 2);
    EXPECT_EQ(model.peer_id(lan::gui::transfer_snapshot_key(first)), QStringLiteral("node-a"));
    EXPECT_EQ(model.peer_id(lan::gui::transfer_snapshot_key(second)), QStringLiteral("node-b"));
}

TEST(TransferRecordMatcherTest, MatchesResendRequestForOriginalSenderRecord) {
    auto snapshot = make_snapshot(5, lan::TransferDirection::send);
    snapshot.kind = lan::TransferKind::directory;
    snapshot.total_bytes = 1024;
    snapshot.total_files = 7;
    const auto entry = lan::gui::TransferListEntry{.key = lan::gui::transfer_snapshot_key(snapshot),
                                                   .snapshot = snapshot};
    const auto request = lan::gui::TransferResendRequest{
        .peer_id = QStringLiteral("node-a"),
        .name = QStringLiteral("Music"),
        .kind = lan::TransferKind::directory,
        .total_bytes = 1024,
        .total_files = 7,
    };

    EXPECT_TRUE(lan::gui::transfer_matches_resend_request(entry,
                                                          QStringLiteral("node-a"),
                                                          QStringLiteral("Music"),
                                                          request));
}

TEST(TransferRecordMatcherTest, RejectsDifferentPeerOrMetadata) {
    auto snapshot = make_snapshot(6, lan::TransferDirection::send);
    snapshot.total_bytes = 2048;
    const auto entry = lan::gui::TransferListEntry{.key = lan::gui::transfer_snapshot_key(snapshot),
                                                   .snapshot = snapshot};
    const auto request = lan::gui::TransferResendRequest{
        .peer_id = QStringLiteral("node-a"),
        .name = QStringLiteral("demo.txt"),
        .kind = lan::TransferKind::file,
        .total_bytes = 2048,
        .total_files = 0,
    };

    EXPECT_FALSE(lan::gui::transfer_matches_resend_request(entry,
                                                           QStringLiteral("node-b"),
                                                           QStringLiteral("demo.txt"),
                                                           request));
    EXPECT_FALSE(lan::gui::transfer_matches_resend_request(entry,
                                                           QStringLiteral("node-a"),
                                                           QStringLiteral("other.txt"),
                                                           request));
}

TEST(TransferRecordStoreTest, ConvertsRunningSnapshotsToRetryableCancelledRecords) {
    auto snapshot = make_snapshot(7, lan::TransferDirection::send);
    snapshot.state = lan::TransferState::running;
    snapshot.path = "/tmp/demo.txt";

    const auto map = lan::gui::transfer_record_to_settings(
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-a"), .snapshot = snapshot},
        QStringLiteral("interrupted"));
    const auto record = lan::gui::transfer_record_from_settings(map, 700, QStringLiteral("interrupted"));

    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->peer_id, QStringLiteral("node-a"));
    EXPECT_EQ(record->snapshot.transfer_id, 700);
    EXPECT_EQ(record->snapshot.state, lan::TransferState::cancelled);
    ASSERT_TRUE(record->snapshot.error.has_value());
    EXPECT_EQ(record->snapshot.error->message, "interrupted");
    EXPECT_TRUE(record->snapshot.retryable);
}

TEST(TransferRecordStoreTest, RejectsRecordsWithoutPeerOrDisplayablePath) {
    QVariantMap missing_peer;
    missing_peer.insert(QStringLiteral("name"), QStringLiteral("demo.txt"));

    QVariantMap missing_name_and_path;
    missing_name_and_path.insert(QStringLiteral("peerId"), QStringLiteral("node-a"));

    EXPECT_FALSE(lan::gui::transfer_record_from_settings(missing_peer, 1, QStringLiteral("interrupted")).has_value());
    EXPECT_FALSE(lan::gui::transfer_record_from_settings(missing_name_and_path, 2, QStringLiteral("interrupted")).has_value());
}

TEST(TransferRecordStoreTest, JsonRoundTripsPersistedRecords) {
    auto first = make_snapshot(8, lan::TransferDirection::send);
    first.state = lan::TransferState::completed;
    first.kind = lan::TransferKind::directory;
    first.path = "/tmp/music";
    first.name = "music";
    first.current_bytes = 99;
    first.total_bytes = 100;
    first.processed_files = 9;
    first.total_files = 10;
    first.skipped_files = 1;
    first.full_files = 7;
    first.delta_files = 2;
    first.payload_bytes = 33;
    first.completion_status = lan::TransferCompletionStatus::resumed;
    first.resumed_from = 10;
    first.elapsed_seconds = 2.5;
    first.source = lan::FileTransferSource::clipboard_image;

    auto second = make_snapshot(9, lan::TransferDirection::receive);
    second.state = lan::TransferState::failed;
    second.path = "/tmp/demo.txt";
    second.error = lan::Error{lan::ErrorCode::io_error, "disk failed"};
    second.error_category = lan::ErrorCategory::filesystem;
    second.retryable = true;
    second.user_action_required = true;

    const QList<lan::gui::PersistedTransferRecord> records{
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-a"), .snapshot = first},
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-b"), .snapshot = second},
    };

    const auto document = lan::gui::transfer_records_to_json(records, QStringLiteral("interrupted"));
    const auto restored = lan::gui::transfer_records_from_json(document, 900, QStringLiteral("interrupted"));

    ASSERT_EQ(restored.size(), 2);
    EXPECT_EQ(restored.at(0).peer_id, QStringLiteral("node-a"));
    EXPECT_EQ(restored.at(0).snapshot.transfer_id, 901);
    EXPECT_EQ(restored.at(0).snapshot.state, lan::TransferState::completed);
    EXPECT_EQ(restored.at(0).snapshot.kind, lan::TransferKind::directory);
    EXPECT_EQ(restored.at(0).snapshot.path, "/tmp/music");
    EXPECT_EQ(restored.at(0).snapshot.total_files, 10);
    EXPECT_EQ(restored.at(0).snapshot.payload_bytes, 33);
    EXPECT_EQ(restored.at(0).snapshot.source, lan::FileTransferSource::clipboard_image);

    EXPECT_EQ(restored.at(1).peer_id, QStringLiteral("node-b"));
    EXPECT_EQ(restored.at(1).snapshot.transfer_id, 902);
    ASSERT_TRUE(restored.at(1).snapshot.error.has_value());
    EXPECT_EQ(restored.at(1).snapshot.error->message, "disk failed");
    EXPECT_TRUE(restored.at(1).snapshot.retryable);
    EXPECT_TRUE(restored.at(1).snapshot.user_action_required);
}

TEST(TransferRecordStoreTest, JsonSkipsUnsupportedOrInvalidRecords) {
    QJsonObject unsupported;
    unsupported.insert(QStringLiteral("version"), 99);
    unsupported.insert(QStringLiteral("records"), QJsonArray{});
    EXPECT_TRUE(lan::gui::transfer_records_from_json(QJsonDocument(unsupported),
                                                     100,
                                                     QStringLiteral("interrupted"))
                    .isEmpty());

    QJsonObject valid_record;
    valid_record.insert(QStringLiteral("peerId"), QStringLiteral("node-a"));
    valid_record.insert(QStringLiteral("name"), QStringLiteral("demo.txt"));

    QJsonObject invalid_record;
    invalid_record.insert(QStringLiteral("name"), QStringLiteral("missing-peer.txt"));

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("records"), QJsonArray{invalid_record, valid_record});

    const auto restored = lan::gui::transfer_records_from_json(QJsonDocument(root),
                                                               100,
                                                               QStringLiteral("interrupted"));
    ASSERT_EQ(restored.size(), 1);
    EXPECT_EQ(restored.front().peer_id, QStringLiteral("node-a"));
    EXPECT_EQ(restored.front().snapshot.transfer_id, 102);
}

TEST(TransferRecordStoreTest, ReadsLegacyQSettingsTransferArray) {
    TempDir temp("transfer-record-store-legacy-settings");
    QSettings settings(QString::fromStdString((temp.path() / "settings.ini").string()), QSettings::IniFormat);
    auto snapshot = make_snapshot(13, lan::TransferDirection::receive);
    snapshot.state = lan::TransferState::completed;
    snapshot.path = "/tmp/legacy.txt";
    const auto map = lan::gui::transfer_record_to_settings(
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-a"), .snapshot = snapshot},
        QStringLiteral("interrupted"));

    settings.beginWriteArray(QStringLiteral("transfers"), 1);
    settings.setArrayIndex(0);
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        settings.setValue(it.key(), it.value());
    }
    settings.endArray();
    settings.sync();

    const auto records = lan::gui::transfer_records_from_settings_array(settings,
                                                                        QStringLiteral("transfers"),
                                                                        1300,
                                                                        QStringLiteral("interrupted"));
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records.front().peer_id, QStringLiteral("node-a"));
    EXPECT_EQ(records.front().snapshot.transfer_id, 1301);
    EXPECT_EQ(records.front().snapshot.path, "/tmp/legacy.txt");
}

TEST(TransferRecordRepositoryTest, SavesAndLoadsJsonRecords) {
    TempDir temp("transfer-record-repository-save");
    auto snapshot = make_snapshot(10, lan::TransferDirection::send);
    snapshot.state = lan::TransferState::completed;
    snapshot.path = "/tmp/demo.txt";

    lan::gui::TransferRecordRepository repository(temp.path() / "transfers.json",
                                                  QStringLiteral("interrupted"),
                                                  std::chrono::milliseconds(200));
    repository.request_save(QList<lan::gui::PersistedTransferRecord>{
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-a"), .snapshot = snapshot},
    });

    EXPECT_TRUE(repository.flush(std::chrono::seconds(2)));

    const auto restored = repository.load(1000);
    ASSERT_EQ(restored.size(), 1);
    EXPECT_EQ(restored.front().peer_id, QStringLiteral("node-a"));
    EXPECT_EQ(restored.front().snapshot.transfer_id, 1001);
    EXPECT_EQ(restored.front().snapshot.state, lan::TransferState::completed);
}

TEST(TransferRecordRepositoryTest, CoalescesMultipleSaveRequests) {
    TempDir temp("transfer-record-repository-coalesce");
    auto first = make_snapshot(11, lan::TransferDirection::send);
    first.state = lan::TransferState::completed;
    first.path = "/tmp/first.txt";
    first.name = "first.txt";

    auto second = make_snapshot(12, lan::TransferDirection::receive);
    second.state = lan::TransferState::completed;
    second.path = "/tmp/second.txt";
    second.name = "second.txt";

    lan::gui::TransferRecordRepository repository(temp.path() / "transfers.json",
                                                  QStringLiteral("interrupted"),
                                                  std::chrono::seconds(60));
    repository.request_save(QList<lan::gui::PersistedTransferRecord>{
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-a"), .snapshot = first},
    });
    repository.request_save(QList<lan::gui::PersistedTransferRecord>{
        lan::gui::PersistedTransferRecord{.peer_id = QStringLiteral("node-b"), .snapshot = second},
    });

    EXPECT_TRUE(repository.flush(std::chrono::seconds(2)));

    const auto restored = repository.load(2000);
    ASSERT_EQ(restored.size(), 1);
    EXPECT_EQ(restored.front().peer_id, QStringLiteral("node-b"));
    EXPECT_EQ(restored.front().snapshot.name, "second.txt");
}

TEST(TransferRecordRepositoryTest, BadJsonLoadsAsEmptyAndReportsError) {
    TempDir temp("transfer-record-repository-bad-json");
    const auto path = temp.path() / "transfers.json";
    write_text(path, "{not json");

    lan::gui::TransferRecordRepository repository(path,
                                                  QStringLiteral("interrupted"),
                                                  std::chrono::milliseconds(1));

    EXPECT_TRUE(repository.load(3000).isEmpty());
    EXPECT_FALSE(repository.last_error().isEmpty());
}

}  // namespace

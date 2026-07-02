#include <gtest/gtest.h>

#include "gui/transfer_list_model.h"
#include "gui/transfer_record_matcher.h"

namespace {

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

}  // namespace

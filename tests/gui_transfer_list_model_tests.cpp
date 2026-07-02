#include <gtest/gtest.h>

#include "gui/transfer_list_model.h"

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

}  // namespace

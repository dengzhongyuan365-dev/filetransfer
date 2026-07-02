#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "gui/device_manager.h"

namespace {

lan::gui::Peer make_peer(QString id,
                         QString name = QStringLiteral("Desk"),
                         QString host = QStringLiteral("10.0.0.2"),
                         std::uint16_t port = lan::gui::kTransferPort) {
    return lan::gui::Peer{
        .id = std::move(id),
        .name = std::move(name),
        .host = std::move(host),
        .trust_token = {},
        .port = port,
        .online = true,
    };
}

TEST(DeviceManagerTrustTest, TrustsAndUntrustsPeer) {
    lan::gui::DeviceManager devices;
    const auto peer = make_peer(QStringLiteral("node-a"));

    devices.insert_peer(peer);
    EXPECT_FALSE(devices.is_trusted_peer(peer));

    const auto trusted = devices.trust_peer(peer.id, 100, QStringLiteral("token-a"));
    EXPECT_TRUE(trusted.trusted);
    EXPECT_EQ(trusted.trust_token, QStringLiteral("token-a"));
    EXPECT_EQ(trusted.trusted_at_ms, 100);
    EXPECT_TRUE(devices.is_trusted_peer(trusted));
    EXPECT_TRUE(devices.can_auto_accept_peer(trusted, QStringLiteral("token-a")));
    EXPECT_FALSE(devices.can_auto_accept_peer(trusted, QStringLiteral("wrong-token")));

    lan::gui::Peer updated;
    EXPECT_TRUE(devices.untrust_peer(peer.id, &updated));
    EXPECT_FALSE(updated.trusted);
    EXPECT_TRUE(updated.trust_token.isEmpty());
    EXPECT_EQ(updated.trusted_at_ms, 0);
    EXPECT_FALSE(devices.is_trusted_peer(updated));
}

TEST(DeviceManagerTrustTest, PreservesTrustWhenManualPeerBecomesDiscoveredPeer) {
    lan::gui::DeviceManager devices;

    const auto manual = devices.upsert_manual_peer(QStringLiteral("10.0.0.2"), lan::gui::kTransferPort, 10);
    devices.trust_peer(manual.id, 20, QStringLiteral("manual-token"));

    const auto result = devices.upsert_discovered_peer(
        QStringLiteral("10.0.0.2"),
        lan::gui::kTransferPort,
        QStringLiteral("node-real"),
        QStringLiteral("Desk"),
        30);

    EXPECT_EQ(result.previous_id, manual.id);
    EXPECT_FALSE(devices.contains(manual.id));
    EXPECT_TRUE(devices.contains(QStringLiteral("node-real")));
    EXPECT_TRUE(result.peer.trusted);
    EXPECT_EQ(result.peer.trust_token, QStringLiteral("manual-token"));
    EXPECT_EQ(result.peer.trusted_at_ms, 20);
    EXPECT_TRUE(devices.is_trusted_peer(result.peer));
}

TEST(DeviceManagerTrustTest, PreservesTrustWhenOlderDuplicateEndpointIsIgnored) {
    lan::gui::DeviceManager devices;

    auto newer = make_peer(QStringLiteral("node-newer"));
    newer.last_linked_ms = 200;
    devices.insert_peer(newer);

    auto older_trusted = make_peer(QStringLiteral("node-older"));
    older_trusted.last_linked_ms = 100;
    older_trusted.trust_token = QStringLiteral("older-token");
    older_trusted.trusted = true;
    older_trusted.trusted_at_ms = 150;
    devices.insert_peer(older_trusted);

    EXPECT_TRUE(devices.contains(newer.id));
    EXPECT_FALSE(devices.contains(older_trusted.id));

    const auto kept = devices.peer(newer.id);
    EXPECT_TRUE(kept.trusted);
    EXPECT_EQ(kept.trust_token, QStringLiteral("older-token"));
    EXPECT_EQ(kept.trusted_at_ms, 150);
    EXPECT_EQ(kept.last_linked_ms, 200);
}

TEST(DeviceManagerTrustTest, LinkedPeerKeepsExistingTrust) {
    lan::gui::DeviceManager devices;
    const auto peer = make_peer(QStringLiteral("node-a"));
    devices.insert_peer(peer);
    devices.trust_peer(peer.id, 100, QStringLiteral("token-a"));

    const auto linked = devices.set_linked_peer(peer, true, 200);

    EXPECT_TRUE(linked.linked);
    EXPECT_TRUE(linked.trusted);
    EXPECT_EQ(linked.trust_token, QStringLiteral("token-a"));
    EXPECT_EQ(linked.trusted_at_ms, 100);
    EXPECT_EQ(linked.last_linked_ms, 200);
}

TEST(DeviceManagerTrustTest, AutoAcceptRequiresStoredTrustToken) {
    lan::gui::DeviceManager devices;
    const auto peer = make_peer(QStringLiteral("node-a"));
    devices.insert_peer(peer);

    const auto legacy_trusted = devices.trust_peer(peer.id, 100);
    EXPECT_TRUE(devices.is_trusted_peer(legacy_trusted));
    EXPECT_FALSE(devices.can_auto_accept_peer(legacy_trusted, QString()));

    const auto token_trusted = devices.trust_peer(peer.id, 200, QStringLiteral("token-a"));
    EXPECT_TRUE(devices.can_auto_accept_peer(token_trusted, QStringLiteral("token-a")));
    EXPECT_FALSE(devices.can_auto_accept_peer(token_trusted, QString()));
}

}  // namespace

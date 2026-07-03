#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QRect>
#include <QToolButton>
#include <QWidget>

#include "gui/transfer_card.h"

namespace {

lan::TransferSnapshot make_running_snapshot() {
    lan::TransferSnapshot snapshot;
    snapshot.transfer_id = 1;
    snapshot.direction = lan::TransferDirection::send;
    snapshot.kind = lan::TransferKind::directory;
    snapshot.state = lan::TransferState::failed;
    snapshot.name = "A very long folder name that should never push action buttons outside the card";
    snapshot.current_bytes = 1024;
    snapshot.total_bytes = 1024 * 1024;
    snapshot.processed_files = 13;
    snapshot.total_files = 152;
    snapshot.error = lan::Error{lan::ErrorCode::network_error,
                                "very long error detail that should be elided instead of expanding the layout"};
    return snapshot;
}

lan::TransferSnapshot make_completed_snapshot() {
    auto snapshot = make_running_snapshot();
    snapshot.state = lan::TransferState::completed;
    snapshot.current_bytes = snapshot.total_bytes;
    snapshot.processed_files = snapshot.total_files;
    snapshot.error.reset();
    return snapshot;
}

QRect geometry_in_card(QWidget& card, QWidget& child) {
    return QRect(child.mapTo(&card, QPoint(0, 0)), child.size());
}

void expect_widget_inside_card(QWidget& card, QWidget& child) {
    const auto card_rect = card.contentsRect();
    const auto child_rect = geometry_in_card(card, child);
    EXPECT_GE(child_rect.left(), card_rect.left());
    EXPECT_GE(child_rect.top(), card_rect.top());
    EXPECT_LE(child_rect.right(), card_rect.right());
    EXPECT_LE(child_rect.bottom(), card_rect.bottom());
}

void expect_widget_has_card_edge_padding(QWidget& card, QWidget& child) {
    const auto card_rect = card.contentsRect();
    const auto child_rect = geometry_in_card(card, child);
    EXPECT_LE(child_rect.right(), card_rect.right() - 2);
}

}  // namespace

TEST(TransferCardLayoutTest, KeepsStatusAndActionsInsideNarrowCard) {
    auto snapshot = make_running_snapshot();
    lan::gui::TransferCard card(
        snapshot,
        lan::gui::TransferCardText{
            .detail = "13/152 | very long detail that should elide before it reaches the buttons",
            .speed = "1.68 MiB/s",
            .size = "13/152",
            .state = QStringLiteral("failed"),
        },
        lan::gui::TransferCardActions{
            .resume_enabled = true,
            .open_enabled = true,
            .stop_enabled = true,
            .remove_enabled = true,
        },
        lan::gui::TransferCardCallbacks{});

    card.resize(280, card.minimumSizeHint().height());
    card.show();
    QApplication::processEvents();

    auto* state = card.findChild<QLabel*>("stateBadge");
    ASSERT_NE(state, nullptr);
    expect_widget_inside_card(card, *state);

    const auto buttons = card.findChildren<QToolButton*>();
    ASSERT_EQ(buttons.size(), 3);
    for (auto* button : buttons) {
        expect_widget_inside_card(card, *button);
        expect_widget_has_card_edge_padding(card, *button);
    }
}

TEST(TransferCardLayoutTest, KeepsCompletedCardActionsInsideMinimumHeight) {
    auto snapshot = make_completed_snapshot();
    lan::gui::TransferCard card(
        snapshot,
        lan::gui::TransferCardText{
            .detail = "150/152 | skipped 14, full 138",
            .speed = "0 B/s",
            .size = "150/152",
            .state = QStringLiteral("completed"),
        },
        lan::gui::TransferCardActions{
            .resume_enabled = false,
            .open_enabled = true,
            .stop_enabled = false,
            .remove_enabled = true,
        },
        lan::gui::TransferCardCallbacks{});

    card.resize(280, card.minimumSizeHint().height());
    card.show();
    QApplication::processEvents();

    auto* state = card.findChild<QLabel*>("stateBadge");
    ASSERT_NE(state, nullptr);
    expect_widget_inside_card(card, *state);

    const auto buttons = card.findChildren<QToolButton*>();
    ASSERT_EQ(buttons.size(), 3);
    for (auto* button : buttons) {
        expect_widget_inside_card(card, *button);
        expect_widget_has_card_edge_padding(card, *button);
    }
}

TEST(TransferCardTextTest, CompletedStateUsesDirectionSpecificLabel) {
    auto send_snapshot = make_completed_snapshot();
    send_snapshot.direction = lan::TransferDirection::send;
    lan::gui::TransferCard send_card(
        send_snapshot,
        lan::gui::TransferCardText{
            .detail = "150/152 | skipped 14, full 138",
            .speed = "0 B/s",
            .size = "150/152",
            .state = QStringLiteral("completed"),
        },
        lan::gui::TransferCardActions{},
        lan::gui::TransferCardCallbacks{});

    auto* send_state = send_card.findChild<QLabel*>("stateBadge");
    ASSERT_NE(send_state, nullptr);
    EXPECT_EQ(send_state->text(), QStringLiteral("transferred"));

    auto receive_snapshot = make_completed_snapshot();
    receive_snapshot.direction = lan::TransferDirection::receive;
    lan::gui::TransferCard receive_card(
        receive_snapshot,
        lan::gui::TransferCardText{
            .detail = "150/152 | skipped 14, full 138",
            .speed = "0 B/s",
            .size = "150/152",
            .state = QStringLiteral("completed"),
        },
        lan::gui::TransferCardActions{},
        lan::gui::TransferCardCallbacks{});

    auto* receive_state = receive_card.findChild<QLabel*>("stateBadge");
    ASSERT_NE(receive_state, nullptr);
    EXPECT_EQ(receive_state->text(), QStringLiteral("received"));
}

TEST(TransferCardPaintTest, ActionButtonPaintsInsetCircleInsteadOfFullSquare) {
    auto snapshot = make_completed_snapshot();
    lan::gui::TransferCard card(
        snapshot,
        lan::gui::TransferCardText{
            .detail = "150/152 | skipped 14, full 138",
            .speed = "0 B/s",
            .size = "150/152",
            .state = QStringLiteral("completed"),
        },
        lan::gui::TransferCardActions{
            .resume_enabled = false,
            .open_enabled = true,
            .stop_enabled = false,
            .remove_enabled = true,
        },
        lan::gui::TransferCardCallbacks{});

    card.resize(280, card.minimumSizeHint().height());
    card.show();
    QApplication::processEvents();

    auto* open = card.findChild<QToolButton*>("taskOpenButton");
    ASSERT_NE(open, nullptr);
    open->setDown(true);

    const auto image = open->grab().toImage();
    const auto corner = image.pixelColor(0, 0);
    const auto circle_background = image.pixelColor(image.width() / 2, 4);

    EXPECT_NE(corner, circle_background);
    EXPECT_GT(circle_background.blue(), circle_background.red());
    EXPECT_GT(circle_background.blue(), circle_background.green());
}

TEST(TransferCardPaintTest, ClearButtonUsesLightDangerColor) {
    auto snapshot = make_completed_snapshot();
    lan::gui::TransferCard card(
        snapshot,
        lan::gui::TransferCardText{
            .detail = "150/152 | skipped 14, full 138",
            .speed = "0 B/s",
            .size = "150/152",
            .state = QStringLiteral("completed"),
        },
        lan::gui::TransferCardActions{
            .resume_enabled = false,
            .open_enabled = true,
            .stop_enabled = false,
            .remove_enabled = true,
        },
        lan::gui::TransferCardCallbacks{});

    card.resize(280, card.minimumSizeHint().height());
    card.show();
    QApplication::processEvents();

    auto* remove = card.findChild<QToolButton*>("taskRemoveButton");
    ASSERT_NE(remove, nullptr);

    const auto image = remove->grab().toImage();
    const auto circle_background = image.pixelColor(image.width() / 2, 4);

    EXPECT_GT(circle_background.red(), circle_background.green());
    EXPECT_GT(circle_background.red(), circle_background.blue());
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

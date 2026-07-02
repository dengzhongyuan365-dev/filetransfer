#pragma once

#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

namespace lan::gui {

struct ReceiveHistoryItem {
    QString text;
    QString open_dir;
    bool has_error = false;
};

enum class ReceiveHistoryActionType {
    open,
    clear,
};

struct ReceiveHistoryAction {
    ReceiveHistoryActionType type = ReceiveHistoryActionType::open;
    QString open_dir;
};

std::optional<ReceiveHistoryAction> show_receive_history_dialog(QWidget* parent,
                                                                const std::vector<ReceiveHistoryItem>& items);

}  // namespace lan::gui

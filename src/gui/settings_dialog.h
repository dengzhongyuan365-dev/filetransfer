#pragma once

#include <QString>
#include <QWidget>

#include <functional>
#include <optional>

namespace lan::gui {

enum class SettingsCloseAction {
    ask,
    tray,
    quit,
};

struct SettingsDialogState {
    QString receive_dir;
    QString discovery_networks;
    int max_global_sends = 1;
    int max_peer_sends = 1;
    SettingsCloseAction close_action = SettingsCloseAction::ask;
};

std::optional<SettingsDialogState> edit_settings(QWidget* parent,
                                                 const SettingsDialogState& state,
                                                 std::function<void(QWidget*)> show_debug_logs = {});

}  // namespace lan::gui

#include "gui/settings_dialog.h"

#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "gui/qt_utils.h"

namespace lan::gui {

std::optional<SettingsDialogState> edit_settings(QWidget* parent,
                                                 const SettingsDialogState& state,
                                                 std::function<void(QWidget*)> show_debug_logs,
                                                 std::function<void(QWidget*)> show_devices) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("SettingsDialog", "Settings"));
    dialog.resize(360, 500);
    dialog.setMinimumSize(340, 430);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* title = new QLabel(QCoreApplication::translate("SettingsDialog", "Settings"), &dialog);
    title->setObjectName("dialogTitle");

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 8, 0);
    content_layout->setSpacing(8);

    auto* receive_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Receiving folder"), &dialog);
    receive_label->setObjectName("mutedText");

    auto* path_box = new QFrame(&dialog);
    path_box->setObjectName("pathBox");
    auto* path_row = new QHBoxLayout(path_box);
    path_row->setContentsMargins(10, 8, 8, 8);
    path_row->setSpacing(8);

    auto* path = new ElidedLabel(state.receive_dir, path_box);
    path->setObjectName("pathLabel");
    auto* choose = new QPushButton(QCoreApplication::translate("SettingsDialog", "Choose"), path_box);
    choose->setObjectName("secondaryButton");
    path_row->addWidget(path, 1);
    path_row->addWidget(choose);

    auto* hint = new QLabel(QCoreApplication::translate("SettingsDialog", "Changing this folder restarts the local receiver."), &dialog);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);

    auto* discovery_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Extra discovery networks"), &dialog);
    discovery_label->setObjectName("mutedText");
    auto* discovery_networks = new QLineEdit(state.discovery_networks, &dialog);
    discovery_networks->setObjectName("searchInput");
    discovery_networks->setPlaceholderText(QCoreApplication::translate("SettingsDialog", "Example: 10.8.12.0/24, 10.8.0.0/16"));
    auto* discovery_hint = new QLabel(
        QCoreApplication::translate("SettingsDialog", "Use this only when machines are routed across different broadcast domains."),
        &dialog);
    discovery_hint->setObjectName("mutedText");
    discovery_hint->setWordWrap(true);

    auto* scheduler_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Transfer scheduling"), &dialog);
    scheduler_label->setObjectName("mutedText");
    auto* max_global_sends = new QSpinBox(&dialog);
    max_global_sends->setRange(1, 8);
    max_global_sends->setValue(state.max_global_sends);
    max_global_sends->setAlignment(Qt::AlignCenter);
    max_global_sends->setMinimumWidth(96);
    max_global_sends->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* max_peer_sends = new QSpinBox(&dialog);
    max_peer_sends->setRange(1, 4);
    max_peer_sends->setValue(state.max_peer_sends);
    max_peer_sends->setAlignment(Qt::AlignCenter);
    max_peer_sends->setMinimumWidth(96);
    max_peer_sends->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* global_row = new QHBoxLayout();
    global_row->setSpacing(8);
    auto* global_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Max simultaneous sends"), &dialog);
    global_label->setObjectName("mutedText");
    global_row->addWidget(global_label, 1);
    global_row->addWidget(max_global_sends);

    auto* peer_row = new QHBoxLayout();
    peer_row->setSpacing(8);
    auto* peer_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Max sends per machine"), &dialog);
    peer_label->setObjectName("mutedText");
    peer_row->addWidget(peer_label, 1);
    peer_row->addWidget(max_peer_sends);

    auto* close_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Close button action"), &dialog);
    close_label->setObjectName("mutedText");
    auto* ask_on_close = new QRadioButton(QCoreApplication::translate("SettingsDialog", "Ask every time"), &dialog);
    auto* tray_on_close = new QRadioButton(QCoreApplication::translate("SettingsDialog", "Minimize to system tray"), &dialog);
    auto* quit_on_close = new QRadioButton(QCoreApplication::translate("SettingsDialog", "Quit"), &dialog);
    switch (state.close_action) {
        case SettingsCloseAction::tray:
            tray_on_close->setChecked(true);
            break;
        case SettingsCloseAction::quit:
            quit_on_close->setChecked(true);
            break;
        case SettingsCloseAction::ask:
            ask_on_close->setChecked(true);
            break;
    }

    auto* debug_row = new QHBoxLayout();
    debug_row->setSpacing(8);
    auto* debug_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Debug"), &dialog);
    debug_label->setObjectName("mutedText");
    auto* debug_logs = new QPushButton(QCoreApplication::translate("SettingsDialog", "Logs"), &dialog);
    debug_logs->setObjectName("secondaryButton");
    debug_logs->setMinimumWidth(86);
    debug_logs->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    debug_row->addWidget(debug_label, 1);
    debug_row->addWidget(debug_logs);

    auto* devices_row = new QHBoxLayout();
    devices_row->setSpacing(8);
    auto* devices_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Devices"), &dialog);
    devices_label->setObjectName("mutedText");
    auto* devices = new QPushButton(QCoreApplication::translate("SettingsDialog", "Manage"), &dialog);
    devices->setObjectName("secondaryButton");
    devices->setMinimumWidth(86);
    devices->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    devices_row->addWidget(devices_label, 1);
    devices_row->addWidget(devices);

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("SettingsDialog", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* save = new QPushButton(QCoreApplication::translate("SettingsDialog", "Save"), &dialog);
    save->setObjectName("primaryButton");
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    content_layout->addWidget(receive_label);
    content_layout->addWidget(path_box);
    content_layout->addWidget(hint);
    content_layout->addSpacing(2);
    content_layout->addWidget(discovery_label);
    content_layout->addWidget(discovery_networks);
    content_layout->addWidget(discovery_hint);
    content_layout->addSpacing(2);
    content_layout->addWidget(scheduler_label);
    content_layout->addLayout(global_row);
    content_layout->addLayout(peer_row);
    content_layout->addSpacing(2);
    content_layout->addWidget(close_label);
    content_layout->addWidget(ask_on_close);
    content_layout->addWidget(tray_on_close);
    content_layout->addWidget(quit_on_close);
    content_layout->addSpacing(2);
    content_layout->addLayout(devices_row);
    content_layout->addSpacing(2);
    content_layout->addLayout(debug_row);
    content_layout->addStretch(1);
    scroll->setWidget(content);

    layout->addWidget(title);
    layout->addWidget(scroll, 1);
    layout->addLayout(buttons);

    SettingsDialogState result = state;
    QObject::connect(choose, &QPushButton::clicked, &dialog, [&dialog, path] {
        const auto dir = QFileDialog::getExistingDirectory(&dialog, QCoreApplication::translate("SettingsDialog", "Receiving folder"), path->text());
        if (!dir.isEmpty()) {
            path->setText(dir);
        }
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(debug_logs, &QPushButton::clicked, &dialog, [show_debug_logs, &dialog] {
        if (show_debug_logs) {
            show_debug_logs(&dialog);
        }
    });
    QObject::connect(devices, &QPushButton::clicked, &dialog, [show_devices, &dialog] {
        if (show_devices) {
            show_devices(&dialog);
        }
    });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, &result, path, discovery_networks, ask_on_close, tray_on_close, max_global_sends, max_peer_sends] {
        const auto next_dir = path->text().trimmed();
        if (next_dir.isEmpty()) {
            QMessageBox::warning(&dialog,
                                 QCoreApplication::translate("SettingsDialog", "Settings"),
                                 QCoreApplication::translate("SettingsDialog", "Receiving folder cannot be empty."));
            return;
        }

        result.receive_dir = next_dir;
        result.discovery_networks = discovery_networks->text().trimmed();
        result.max_global_sends = max_global_sends->value();
        result.max_peer_sends = max_peer_sends->value();
        if (ask_on_close->isChecked()) {
            result.close_action = SettingsCloseAction::ask;
        } else if (tray_on_close->isChecked()) {
            result.close_action = SettingsCloseAction::tray;
        } else {
            result.close_action = SettingsCloseAction::quit;
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return result;
}

}  // namespace lan::gui

#include "gui/settings_dialog.h"

#include <QCoreApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace lan::gui {

std::optional<SettingsDialogState> edit_settings(QWidget* parent, const SettingsDialogState& state) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("SettingsDialog", "Settings"));
    dialog.resize(360, 390);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(9);

    auto* title = new QLabel(QCoreApplication::translate("SettingsDialog", "Settings"), &dialog);
    title->setObjectName("dialogTitle");
    auto* receive_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Receiving folder"), &dialog);
    receive_label->setObjectName("mutedText");

    auto* path_box = new QFrame(&dialog);
    path_box->setObjectName("pathBox");
    auto* path_row = new QHBoxLayout(path_box);
    path_row->setContentsMargins(10, 8, 8, 8);
    path_row->setSpacing(8);

    auto* path = new QLabel(state.receive_dir, path_box);
    path->setObjectName("pathLabel");
    path->setWordWrap(true);
    auto* choose = new QPushButton(QCoreApplication::translate("SettingsDialog", "Choose"), path_box);
    choose->setObjectName("secondaryButton");
    path_row->addWidget(path, 1);
    path_row->addWidget(choose);

    auto* hint = new QLabel(QCoreApplication::translate("SettingsDialog", "Changing this folder restarts the local receiver."), &dialog);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);

    auto* scheduler_label = new QLabel(QCoreApplication::translate("SettingsDialog", "Transfer scheduling"), &dialog);
    scheduler_label->setObjectName("mutedText");
    auto* max_global_sends = new QSpinBox(&dialog);
    max_global_sends->setRange(1, 8);
    max_global_sends->setValue(state.max_global_sends);
    auto* max_peer_sends = new QSpinBox(&dialog);
    max_peer_sends->setRange(1, 4);
    max_peer_sends->setValue(state.max_peer_sends);

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

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("SettingsDialog", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* save = new QPushButton(QCoreApplication::translate("SettingsDialog", "Save"), &dialog);
    save->setObjectName("primaryButton");
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    layout->addWidget(title);
    layout->addWidget(receive_label);
    layout->addWidget(path_box);
    layout->addWidget(hint);
    layout->addSpacing(2);
    layout->addWidget(scheduler_label);
    layout->addLayout(global_row);
    layout->addLayout(peer_row);
    layout->addSpacing(2);
    layout->addWidget(close_label);
    layout->addWidget(ask_on_close);
    layout->addWidget(tray_on_close);
    layout->addWidget(quit_on_close);
    layout->addStretch(1);
    layout->addLayout(buttons);

    SettingsDialogState result = state;
    QObject::connect(choose, &QPushButton::clicked, &dialog, [&dialog, path] {
        const auto dir = QFileDialog::getExistingDirectory(&dialog, QCoreApplication::translate("SettingsDialog", "Receiving folder"), path->text());
        if (!dir.isEmpty()) {
            path->setText(dir);
        }
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, &result, path, ask_on_close, tray_on_close, max_global_sends, max_peer_sends] {
        const auto next_dir = path->text().trimmed();
        if (next_dir.isEmpty()) {
            QMessageBox::warning(&dialog,
                                 QCoreApplication::translate("SettingsDialog", "Settings"),
                                 QCoreApplication::translate("SettingsDialog", "Receiving folder cannot be empty."));
            return;
        }

        result.receive_dir = next_dir;
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

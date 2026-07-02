#include "gui/target_dialogs.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace lan::gui {
namespace {

QString device_label(const TargetDevice& device) {
    return QStringLiteral("%1  %2:%3").arg(device.name, device.host).arg(device.port);
}

}  // namespace

std::optional<QStringList> choose_send_targets(QWidget* parent, const QList<TargetDevice>& devices) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("TargetDialogs", "Send targets"));
    dialog.resize(340, 320);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(9);

    auto* title = new QLabel(QCoreApplication::translate("TargetDialogs", "Send targets"), &dialog);
    title->setObjectName("dialogTitle");
    auto* hint = new QLabel(
        QCoreApplication::translate("TargetDialogs", "Dropped and pasted files will be queued for selected machines."),
        &dialog);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);

    auto* list = new QWidget(&dialog);
    auto* list_layout = new QVBoxLayout(list);
    list_layout->setContentsMargins(0, 0, 0, 0);
    list_layout->setSpacing(6);

    QMap<QString, QCheckBox*> checks;
    for (const auto& device : devices) {
        auto* check = new QCheckBox(device_label(device), list);
        check->setChecked(device.selected);
        checks.insert(device.id, check);
        list_layout->addWidget(check);
    }
    list_layout->addStretch(1);

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("TargetDialogs", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* save = new QPushButton(QCoreApplication::translate("TargetDialogs", "Save"), &dialog);
    save->setObjectName("primaryButton");
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(list, 1);
    layout->addLayout(buttons);

    QStringList selected_ids;
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, &selected_ids, checks] {
        selected_ids.clear();
        for (auto it = checks.cbegin(); it != checks.cend(); ++it) {
            if (it.value()->isChecked()) {
                selected_ids.append(it.key());
            }
        }
        if (selected_ids.isEmpty()) {
            QMessageBox::warning(
                &dialog,
                QCoreApplication::translate("TargetDialogs", "Send targets"),
                QCoreApplication::translate("TargetDialogs", "Choose at least one linked machine."));
            return;
        }
        selected_ids.sort();
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return selected_ids;
}

std::optional<QString> choose_transfer_target(QWidget* parent, const QList<TargetDevice>& devices) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("TargetDialogs", "Change target machine"));
    dialog.resize(340, 300);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(9);

    auto* title = new QLabel(QCoreApplication::translate("TargetDialogs", "Change target machine"), &dialog);
    title->setObjectName("dialogTitle");
    auto* hint = new QLabel(
        QCoreApplication::translate("TargetDialogs", "Only queued sends can be moved to another linked machine."),
        &dialog);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);

    auto* choices = new QWidget(&dialog);
    auto* choices_layout = new QVBoxLayout(choices);
    choices_layout->setContentsMargins(0, 0, 0, 0);
    choices_layout->setSpacing(6);

    QMap<QString, QRadioButton*> radios;
    for (const auto& device : devices) {
        auto* radio = new QRadioButton(device_label(device), choices);
        radio->setChecked(device.selected);
        radios.insert(device.id, radio);
        choices_layout->addWidget(radio);
    }
    choices_layout->addStretch(1);

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("TargetDialogs", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* save = new QPushButton(QCoreApplication::translate("TargetDialogs", "Move"), &dialog);
    save->setObjectName("primaryButton");
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(choices, 1);
    layout->addLayout(buttons);

    QString selected_id;
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, &selected_id, radios] {
        selected_id.clear();
        for (auto it = radios.cbegin(); it != radios.cend(); ++it) {
            if (it.value()->isChecked()) {
                selected_id = it.key();
                break;
            }
        }
        if (selected_id.isEmpty()) {
            QMessageBox::warning(
                &dialog,
                QCoreApplication::translate("TargetDialogs", "Change target machine"),
                QCoreApplication::translate("TargetDialogs", "Choose a linked machine."));
            return;
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return selected_id;
}

}  // namespace lan::gui

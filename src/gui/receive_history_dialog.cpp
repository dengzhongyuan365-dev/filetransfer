#include "gui/receive_history_dialog.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSize>
#include <QVBoxLayout>

namespace lan::gui {

std::optional<ReceiveHistoryAction> show_receive_history_dialog(QWidget* parent,
                                                                const std::vector<ReceiveHistoryItem>& items) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QCoreApplication::translate("ReceiveHistoryDialog", "Receive history"));
    dialog.resize(380, 420);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* list = new QListWidget(&dialog);
    list->setFrameShape(QFrame::NoFrame);
    list->setWordWrap(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    if (items.empty()) {
        auto* item = new QListWidgetItem(QCoreApplication::translate("ReceiveHistoryDialog", "No receive history yet."));
        item->setFlags(Qt::NoItemFlags);
        list->addItem(item);
    } else {
        for (const auto& entry : items) {
            auto* item = new QListWidgetItem(entry.text);
            item->setData(Qt::UserRole, entry.open_dir);
            item->setSizeHint(QSize(0, entry.has_error ? 82 : 64));
            list->addItem(item);
        }
    }

    auto* buttons = new QHBoxLayout();
    auto* open = new QPushButton(QCoreApplication::translate("ReceiveHistoryDialog", "Open folder"), &dialog);
    open->setObjectName("secondaryButton");
    auto* clear = new QPushButton(QCoreApplication::translate("ReceiveHistoryDialog", "Clear history"), &dialog);
    clear->setObjectName("secondaryButton");
    auto* close = new QPushButton(QCoreApplication::translate("ReceiveHistoryDialog", "Close"), &dialog);
    close->setObjectName("primaryButton");
    open->setEnabled(!items.empty());
    clear->setEnabled(!items.empty());
    buttons->addWidget(open);
    buttons->addWidget(clear);
    buttons->addStretch(1);
    buttons->addWidget(close);

    layout->addWidget(list, 1);
    layout->addLayout(buttons);

    std::optional<ReceiveHistoryAction> action;
    const auto open_selected = [&dialog, &action, list] {
        auto* item = list->currentItem();
        if (item == nullptr) {
            return;
        }
        action = ReceiveHistoryAction{
            .type = ReceiveHistoryActionType::open,
            .open_dir = item->data(Qt::UserRole).toString(),
        };
        dialog.accept();
    };

    QObject::connect(open, &QPushButton::clicked, &dialog, open_selected);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, open_selected);
    QObject::connect(clear, &QPushButton::clicked, &dialog, [&dialog, &action] {
        action = ReceiveHistoryAction{
            .type = ReceiveHistoryActionType::clear,
            .open_dir = {},
        };
        dialog.accept();
    });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);

    dialog.exec();
    return action;
}

}  // namespace lan::gui

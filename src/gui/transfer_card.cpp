#include "gui/transfer_card.h"

#include <QApplication>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "gui/qt_utils.h"

namespace lan::gui {
namespace {

QLabel* make_metric_label(const QString& title, const QString& value, QWidget* parent) {
    auto* label = new QLabel(QString("%1\n%2").arg(title, value), parent);
    label->setObjectName("transferMetric");
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    return label;
}

QToolButton* make_task_tool_button(const QIcon& icon,
                                   const QString& tooltip,
                                   QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setIcon(icon);
    button->setIconSize(QSize(12, 12));
    button->setToolTip(tooltip);
    button->setFixedSize(24, 24);
    return button;
}

QString resume_tooltip(const TransferCardActions& actions) {
    if (actions.resume_queued) {
        return QCoreApplication::translate("TransferCard", "Resume queued transfer");
    }
    if (actions.change_target) {
        return QCoreApplication::translate("TransferCard", "Change target machine");
    }
    return QCoreApplication::translate("TransferCard", "Resume transfer");
}

QString stop_tooltip(const TransferCardActions& actions) {
    return actions.pause
               ? QCoreApplication::translate("TransferCard", "Pause queued transfer")
               : QCoreApplication::translate("TransferCard", "Stop transfer");
}

}  // namespace

TransferCard::TransferCard(const TransferSnapshot& snapshot,
                           TransferCardText text,
                           TransferCardActions actions,
                           TransferCardCallbacks callbacks,
                           QWidget* parent)
    : QWidget(parent) {
    setObjectName("transferCard");

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(8, 9, 8, 9);
    row->setSpacing(4);

    auto* name = new ElidedLabel(QString::fromStdString(snapshot.name), this);
    name->setObjectName("transferName");
    name->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    auto* detail = new ElidedLabel(text.detail, this);
    detail->setObjectName("transferDetail");
    detail->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    auto* text_box = new QVBoxLayout();
    text_box->setContentsMargins(0, 0, 0, 0);
    text_box->setSpacing(2);
    text_box->addWidget(name);
    text_box->addWidget(detail);

    auto* rate = make_metric_label(QCoreApplication::translate("TransferCard", "Speed"), text.speed, this);
    rate->setFixedWidth(40);
    rate->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* size = make_metric_label(QCoreApplication::translate("TransferCard", "Size"), text.size, this);
    size->setFixedWidth(62);
    size->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto* state = new QLabel(text.state, this);
    state->setObjectName("stateBadge");
    state->setAlignment(Qt::AlignCenter);
    state->setFixedWidth(76);
    state->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* action_layout = new QHBoxLayout();
    action_layout->setContentsMargins(0, 0, 0, 0);
    action_layout->setSpacing(4);

    auto* resume = make_task_tool_button(
        QApplication::style()->standardIcon(actions.change_target ? QStyle::SP_ComputerIcon : QStyle::SP_ArrowForward),
        resume_tooltip(actions),
        this);
    resume->setObjectName("taskResumeButton");
    resume->setEnabled(actions.resume_enabled);
    QObject::connect(resume, &QToolButton::clicked, this, [callback = std::move(callbacks.on_resume)] {
        if (callback) {
            callback();
        }
    });

    auto* open = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_DirOpenIcon),
        QCoreApplication::translate("TransferCard", "Open containing folder"),
        this);
    open->setObjectName("taskOpenButton");
    open->setEnabled(actions.open_enabled);
    QObject::connect(open, &QToolButton::clicked, this, [callback = std::move(callbacks.on_open)] {
        if (callback) {
            callback();
        }
    });

    auto* stop = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_MediaStop),
        stop_tooltip(actions),
        this);
    stop->setObjectName("taskStopButton");
    stop->setEnabled(actions.stop_enabled);
    QObject::connect(stop, &QToolButton::clicked, this, [callback = std::move(callbacks.on_stop)] {
        if (callback) {
            callback();
        }
    });

    auto* remove = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_DialogCloseButton),
        QCoreApplication::translate("TransferCard", "Clear from list"),
        this);
    remove->setObjectName("taskRemoveButton");
    remove->setEnabled(actions.remove_enabled);
    QObject::connect(remove, &QToolButton::clicked, this, [callback = std::move(callbacks.on_remove)] {
        if (callback) {
            callback();
        }
    });

    action_layout->addWidget(resume);
    action_layout->addWidget(open);
    action_layout->addWidget(stop);
    action_layout->addWidget(remove);
    action_layout->setSizeConstraint(QLayout::SetFixedSize);

    row->addLayout(text_box, 1);
    row->addWidget(rate, 0);
    row->addWidget(size, 0);
    row->addWidget(state, 0);
    row->addLayout(action_layout);
}

}  // namespace lan::gui

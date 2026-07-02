#include "gui/transfer_card.h"

#include <QApplication>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "gui/qt_utils.h"

namespace lan::gui {
namespace {

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

int progress_percent(const TransferSnapshot& snapshot) {
    if (snapshot.state == TransferState::completed) {
        return 100;
    }

    const auto use_file_progress = snapshot.kind == TransferKind::directory && snapshot.total_files > 0;
    const std::uint64_t current = use_file_progress ? snapshot.processed_files : snapshot.current_bytes;
    const std::uint64_t total = use_file_progress ? snapshot.total_files : snapshot.total_bytes;
    if (total == 0) {
        return 0;
    }

    const auto percent = static_cast<int>((static_cast<long double>(current) * 100.0L) /
                                          static_cast<long double>(total));
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

bool has_known_progress(const TransferSnapshot& snapshot) {
    return snapshot.total_bytes > 0 || snapshot.total_files > 0 || snapshot.state == TransferState::completed;
}

QString progress_text(const TransferSnapshot& snapshot) {
    if (!has_known_progress(snapshot) && snapshot.state == TransferState::running) {
        return QStringLiteral("...");
    }
    return QString("%1%").arg(progress_percent(snapshot));
}

QString state_style_name(TransferState state) {
    switch (state) {
        case TransferState::pending:
            return QStringLiteral("pending");
        case TransferState::paused:
            return QStringLiteral("paused");
        case TransferState::running:
            return QStringLiteral("running");
        case TransferState::completed:
            return QStringLiteral("completed");
        case TransferState::failed:
            return QStringLiteral("failed");
        case TransferState::cancelled:
            return QStringLiteral("cancelled");
    }
    return QStringLiteral("pending");
}

QString metric_text(const TransferCardText& text) {
    auto value = QString("%1 | %2").arg(text.speed, text.size);
    if (!text.detail.isEmpty()) {
        value += QString(" | %1").arg(text.detail);
    }
    return value;
}

}  // namespace

TransferCard::TransferCard(const TransferSnapshot& snapshot,
                           TransferCardText text,
                           TransferCardActions actions,
                           TransferCardCallbacks callbacks,
                           QWidget* parent)
    : QWidget(parent) {
    setObjectName("transferCard");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 9, 10, 9);
    root->setSpacing(7);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    auto* name = new ElidedLabel(QString::fromStdString(snapshot.name), this);
    name->setObjectName("transferName");
    name->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    auto* state = new QLabel(text.state, this);
    state->setObjectName("stateBadge");
    state->setProperty("transferState", state_style_name(snapshot.state));
    state->setAlignment(Qt::AlignCenter);
    state->setFixedWidth(72);
    state->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    header->addWidget(name, 1);
    header->addWidget(state);

    auto* progress_row = new QHBoxLayout();
    progress_row->setContentsMargins(0, 0, 0, 0);
    progress_row->setSpacing(8);

    auto* progress = new QProgressBar(this);
    progress->setObjectName("transferProgress");
    progress->setTextVisible(false);
    progress->setFixedHeight(6);
    progress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (!has_known_progress(snapshot) && snapshot.state == TransferState::running) {
        progress->setRange(0, 0);
    } else {
        progress->setRange(0, 100);
        progress->setValue(progress_percent(snapshot));
    }

    auto* percent = new QLabel(progress_text(snapshot), this);
    percent->setObjectName("transferProgressText");
    percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    percent->setFixedWidth(38);
    percent->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    progress_row->addWidget(progress, 1);
    progress_row->addWidget(percent);

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);

    auto* detail = new ElidedLabel(metric_text(text), this);
    detail->setObjectName("transferDetail");
    detail->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

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

    footer->addWidget(detail, 1);
    footer->addLayout(action_layout);

    root->addLayout(header);
    root->addLayout(progress_row);
    root->addLayout(footer);
}

}  // namespace lan::gui

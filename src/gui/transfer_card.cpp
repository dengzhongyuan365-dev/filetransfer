#include "gui/transfer_card.h"

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProgressBar>
#include <QSize>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include "gui/qt_utils.h"

namespace lan::gui {
namespace {

constexpr int kTransferCardHeight = 92;
constexpr int kCompletedTransferCardHeight = 88;
constexpr int kTransferCardMinWidth = 280;
constexpr int kTransferCardPreferredWidth = 360;
constexpr int kTransferActionRailWidth = 84;

enum class TaskButtonGlyph {
    stop,
    play,
    pause,
    folder,
    clear,
};

struct TaskButtonColors {
    QColor foreground;
    QColor background;
    QColor border;
};

enum class TaskButtonState {
    normal,
    hover,
    pressed,
    disabled,
};

TaskButtonState task_button_state(const QToolButton& button) {
    if (!button.isEnabled()) {
        return TaskButtonState::disabled;
    }
    if (button.isDown()) {
        return TaskButtonState::pressed;
    }
    if (button.underMouse()) {
        return TaskButtonState::hover;
    }
    return TaskButtonState::normal;
}

bool is_dark_widget(const QWidget& widget) {
    return widget.palette().color(QPalette::Window).lightness() < 128;
}

TaskButtonColors action_colors(const QToolButton& button) {
    const auto state = task_button_state(button);
    const auto dark = is_dark_widget(button);
    const auto object = button.objectName();
    const auto control = button.property("controlState").toString();

    if (state == TaskButtonState::disabled) {
        return dark
                   ? TaskButtonColors{QColor("#6d7583"), QColor("#252b36"), QColor(Qt::transparent)}
                   : TaskButtonColors{QColor("#a6adb8"), QColor("#f3f4f6"), QColor(Qt::transparent)};
    }

    auto solid = [](const QString& normal_fg,
                    const QString& normal_bg,
                    const QString& normal_border,
                    const QString& hover_bg,
                    const QString& pressed_bg,
                    TaskButtonState state) {
        if (state == TaskButtonState::pressed) {
            return TaskButtonColors{QColor("#ffffff"), QColor(pressed_bg), QColor(pressed_bg)};
        }
        if (state == TaskButtonState::hover) {
            return TaskButtonColors{QColor("#ffffff"), QColor(hover_bg), QColor(hover_bg)};
        }
        return TaskButtonColors{QColor(normal_fg), QColor(normal_bg), QColor(normal_border)};
    };

    if (object == QStringLiteral("taskOpenButton") || control == QStringLiteral("change")) {
        return dark
                   ? solid("#93c5fd", "#172a4c", "#294775", "#3b82f6", "#2563eb", state)
                   : solid("#2563eb", "#eff6ff", "#bfdbfe", "#2563eb", "#1d4ed8", state);
    }
    if (object == QStringLiteral("taskRemoveButton")) {
        return dark
                   ? solid("#fda4af", "#4a1720", "#7f1d2d", "#e11d48", "#be123c", state)
                   : solid("#be123c", "#fff1f2", "#fecdd3", "#e11d48", "#be123c", state);
    }
    if (control == QStringLiteral("resume")) {
        return dark
                   ? solid("#86efac", "#16351f", "#256f3b", "#16a34a", "#15803d", state)
                   : solid("#047857", "#ecfdf3", "#bbf7d0", "#059669", "#047857", state);
    }
    if (control == QStringLiteral("pause")) {
        return dark
                   ? solid("#fcd34d", "#3d2f12", "#5c4618", "#d97706", "#b45309", state)
                   : solid("#b45309", "#fffbeb", "#fde68a", "#d97706", "#b45309", state);
    }
    return dark
               ? solid("#fda4af", "#4a1720", "#7f1d2d", "#e11d48", "#be123c", state)
               : solid("#e11d48", "#fff1f2", "#fecdd3", "#e11d48", "#be123c", state);
}

class TaskToolButton final : public QToolButton {
public:
    TaskToolButton(TaskButtonGlyph glyph, const QString& tooltip, QWidget* parent)
        : QToolButton(parent), glyph_(glyph) {
        setToolTip(tooltip);
        setFixedSize(24, 24);
        setAutoRaise(true);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    bool event(QEvent* event) override {
        const bool handled = QToolButton::event(event);
        switch (event->type()) {
            case QEvent::Enter:
            case QEvent::Leave:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::EnabledChange:
                update();
                break;
            default:
                break;
        }
        return handled;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const auto colors = action_colors(*this);
        const QRectF circle = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
        painter.setPen(QPen(colors.border, 1.0));
        painter.setBrush(colors.background);
        painter.drawEllipse(circle);

        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.foreground);
        draw_glyph(painter, colors.foreground);
    }

private:
    void draw_glyph(QPainter& painter, const QColor& color) const {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);

        switch (glyph_) {
            case TaskButtonGlyph::stop:
                painter.drawRoundedRect(QRectF(8, 8, 8, 8), 1.8, 1.8);
                break;
            case TaskButtonGlyph::play: {
                QPainterPath path;
                path.moveTo(9.2, 7.1);
                path.lineTo(17.2, 12.0);
                path.lineTo(9.2, 16.9);
                path.closeSubpath();
                painter.drawPath(path);
                break;
            }
            case TaskButtonGlyph::pause:
                painter.drawRoundedRect(QRectF(8.0, 7.0, 3.2, 10.0), 1.1, 1.1);
                painter.drawRoundedRect(QRectF(12.8, 7.0, 3.2, 10.0), 1.1, 1.1);
                break;
            case TaskButtonGlyph::folder: {
                QPainterPath path;
                path.moveTo(5.0, 9.0);
                path.lineTo(10.0, 9.0);
                path.lineTo(11.8, 11.0);
                path.lineTo(19.0, 11.0);
                path.lineTo(19.0, 17.2);
                path.quadTo(19.0, 18.0, 18.2, 18.0);
                path.lineTo(5.8, 18.0);
                path.quadTo(5.0, 18.0, 5.0, 17.2);
                path.closeSubpath();
                painter.drawPath(path);

                painter.setBrush(color.lighter(122));
                painter.drawRoundedRect(QRectF(5.0, 11.2, 14.0, 6.8), 1.5, 1.5);
                break;
            }
            case TaskButtonGlyph::clear:
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(QPointF(8.2, 8.2), QPointF(15.8, 15.8));
                painter.drawLine(QPointF(15.8, 8.2), QPointF(8.2, 15.8));
                break;
        }
    }

    TaskButtonGlyph glyph_ = TaskButtonGlyph::stop;
};

QToolButton* make_task_tool_button(TaskButtonGlyph glyph,
                                   const QString& tooltip,
                                   QWidget* parent) {
    return new TaskToolButton(glyph, tooltip, parent);
}

QString resume_tooltip(const TransferCardActions& actions) {
    if (actions.request_resend) {
        return QCoreApplication::translate("TransferCard", "Request again");
    }
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

bool use_resume_control(const TransferCardActions& actions) {
    return actions.resume_enabled &&
           (!actions.stop_enabled || actions.resume_queued || actions.change_target);
}

TaskButtonGlyph control_glyph(const TransferCardActions& actions) {
    if (use_resume_control(actions)) {
        return TaskButtonGlyph::play;
    }
    if (actions.pause) {
        return TaskButtonGlyph::pause;
    }
    return TaskButtonGlyph::stop;
}

QString control_style_name(const TransferCardActions& actions) {
    if (use_resume_control(actions)) {
        if (actions.change_target) {
            return QStringLiteral("change");
        }
        return QStringLiteral("resume");
    }
    if (actions.pause) {
        return QStringLiteral("pause");
    }
    return QStringLiteral("stop");
}

QString control_tooltip(const TransferCardActions& actions) {
    return use_resume_control(actions) ? resume_tooltip(actions) : stop_tooltip(actions);
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

bool should_show_progress(const TransferSnapshot& snapshot) {
    return snapshot.state != TransferState::completed;
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

QString display_state_text(const TransferSnapshot& snapshot, const TransferCardText& text) {
    if (snapshot.state == TransferState::completed) {
        return snapshot.direction == TransferDirection::receive
                   ? QCoreApplication::translate("TransferCard", "received")
                   : QCoreApplication::translate("TransferCard", "transferred");
    }
    return text.state;
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
    setMinimumHeight(snapshot.state == TransferState::completed ? kCompletedTransferCardHeight
                                                                : kTransferCardHeight);
    setMinimumWidth(kTransferCardMinWidth);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 9, 10, 9);
    root->setSpacing(7);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);

    auto* name = new ElidedLabel(QString::fromStdString(snapshot.name), this);
    name->setObjectName("transferName");
    name->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    name->setMinimumWidth(0);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* state = new QLabel(display_state_text(snapshot, text), this);
    state->setObjectName("stateBadge");
    state->setProperty("transferState", state_style_name(snapshot.state));
    state->setAlignment(Qt::AlignCenter);
    state->setFixedWidth(72);
    state->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    header->addWidget(name, 1);
    header->addWidget(state);

    QHBoxLayout* progress_row = nullptr;
    if (should_show_progress(snapshot)) {
        progress_row = new QHBoxLayout();
        progress_row->setContentsMargins(0, 0, 0, 0);
        progress_row->setSpacing(8);

        auto* progress = new QProgressBar(this);
        progress->setObjectName("transferProgress");
        progress->setTextVisible(false);
        progress->setFixedHeight(6);
        progress->setMinimumWidth(0);
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
    }

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);

    auto* detail = new ElidedLabel(metric_text(text), this);
    detail->setObjectName("transferDetail");
    detail->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    detail->setMinimumWidth(0);
    detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* action_rail = new QWidget(this);
    action_rail->setObjectName("transferActionRail");
    action_rail->setFixedWidth(kTransferActionRailWidth);
    action_rail->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* action_layout = new QHBoxLayout(action_rail);
    action_layout->setContentsMargins(2, 0, 2, 0);
    action_layout->setSpacing(4);

    const auto control_is_resume = use_resume_control(actions);
    auto* control = make_task_tool_button(control_glyph(actions), control_tooltip(actions), action_rail);
    control->setObjectName("taskControlButton");
    control->setProperty("controlState", control_style_name(actions));
    control->setEnabled(control_is_resume ? actions.resume_enabled : actions.stop_enabled);
    auto control_callback = control_is_resume ? std::move(callbacks.on_resume) : std::move(callbacks.on_stop);
    QObject::connect(control, &QToolButton::clicked, this, [callback = std::move(control_callback)] {
        if (callback) {
            callback();
        }
    });

    auto* open = make_task_tool_button(
        TaskButtonGlyph::folder,
        QCoreApplication::translate("TransferCard", "Open containing folder"),
        action_rail);
    open->setObjectName("taskOpenButton");
    open->setEnabled(actions.open_enabled);
    QObject::connect(open, &QToolButton::clicked, this, [callback = std::move(callbacks.on_open)] {
        if (callback) {
            callback();
        }
    });

    auto* remove = make_task_tool_button(
        TaskButtonGlyph::clear,
        QCoreApplication::translate("TransferCard", "Clear from list"),
        action_rail);
    remove->setObjectName("taskRemoveButton");
    remove->setEnabled(actions.remove_enabled);
    QObject::connect(remove, &QToolButton::clicked, this, [callback = std::move(callbacks.on_remove)] {
        if (callback) {
            callback();
        }
    });

    action_layout->addWidget(control);
    action_layout->addWidget(open);
    action_layout->addWidget(remove);

    footer->addWidget(detail, 1);
    footer->addWidget(action_rail, 0, Qt::AlignRight);
    footer->setStretch(0, 1);
    footer->setStretch(1, 0);

    root->addLayout(header);
    if (progress_row != nullptr) {
        root->addLayout(progress_row);
    }
    root->addLayout(footer);
}

QSize TransferCard::sizeHint() const {
    return QSize(kTransferCardPreferredWidth, minimumSizeHint().height());
}

QSize TransferCard::minimumSizeHint() const {
    const auto progress = findChild<QProgressBar*>("transferProgress");
    return QSize(kTransferCardMinWidth,
                 progress != nullptr ? kTransferCardHeight : kCompletedTransferCardHeight);
}

}  // namespace lan::gui

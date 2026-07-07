#pragma once

#include <QWidget>

#include <functional>

#include "lan/app/transfer_snapshot.h"

namespace lan::gui {

struct TransferCardActions {
    bool resume_enabled = false;
    bool open_enabled = false;
    bool copy_clipboard_enabled = false;
    bool stop_enabled = false;
    bool remove_enabled = false;
    bool resume_queued = false;
    bool change_target = false;
    bool request_resend = false;
    bool pause = false;
};

struct TransferCardText {
    QString detail;
    QString speed;
    QString size;
    QString state;
};

struct TransferCardCallbacks {
    std::function<void()> on_resume;
    std::function<void()> on_open;
    std::function<void()> on_copy_clipboard;
    std::function<void()> on_stop;
    std::function<void()> on_remove;
};

class TransferCard final : public QWidget {
public:
    TransferCard(const TransferSnapshot& snapshot,
                 TransferCardText text,
                 TransferCardActions actions,
                 TransferCardCallbacks callbacks,
                 QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
};

}  // namespace lan::gui

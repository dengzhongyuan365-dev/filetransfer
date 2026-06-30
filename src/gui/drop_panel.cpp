#include "gui/drop_panel.h"

#include <QDragEnterEvent>
#include <QLabel>
#include <QMimeData>
#include <QUrl>
#include <QVBoxLayout>

namespace lan::gui {

DropPanel::DropPanel(QWidget* parent) : QFrame(parent) {
    setAcceptDrops(true);
    setObjectName("dropPanel");
    setFrameShape(QFrame::StyledPanel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(6);

    auto* title = new QLabel("Drop files or folders", this);
    title->setObjectName("dropTitle");
    title->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel("Send to the linked machine", this);
    subtitle->setObjectName("mutedText");
    subtitle->setAlignment(Qt::AlignCenter);

    layout->addStretch(1);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addStretch(1);
}

void DropPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void DropPanel::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (!paths.empty() && on_drop) {
        on_drop(paths);
    }
    event->acceptProposedAction();
}

}  // namespace lan::gui

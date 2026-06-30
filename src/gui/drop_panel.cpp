#include "gui/drop_panel.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QUrl>

namespace lan::gui {

DropPanel::DropPanel(QWidget* parent) : QWidget(parent) {
    setAcceptDrops(true);
    setObjectName("transferDropArea");
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

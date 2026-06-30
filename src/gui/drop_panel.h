#pragma once

#include <QWidget>
#include <QStringList>

#include <functional>

namespace lan::gui {

class DropPanel final : public QWidget {
public:
    explicit DropPanel(QWidget* parent = nullptr);

    std::function<void(QStringList)> on_drop;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

}  // namespace lan::gui

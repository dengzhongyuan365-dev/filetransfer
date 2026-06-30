#include <QApplication>

#include "gui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    lan::gui::MainWindow window;
    window.show();
    return app.exec();
}

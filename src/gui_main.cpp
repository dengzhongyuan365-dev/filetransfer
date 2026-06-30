#include <QApplication>
#include <QLocale>
#include <QTranslator>

#include "gui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QTranslator translator;
    const QString locale = QLocale::system().name();
    const QString language = QLocale::system().name().section('_', 0, 0);
#ifdef LAN_TRANSLATION_DIR
    const bool translation_loaded =
        translator.load("lan-file-transfer_" + locale, LAN_TRANSLATION_DIR) ||
        translator.load("lan-file-transfer_" + language, LAN_TRANSLATION_DIR);
#else
    const bool translation_loaded =
        translator.load("lan-file-transfer_" + locale, "translations") ||
        translator.load("lan-file-transfer_" + language, "translations");
#endif
    if (translation_loaded) {
        app.installTranslator(&translator);
    }

    lan::gui::MainWindow window;
    window.show();
    return app.exec();
}

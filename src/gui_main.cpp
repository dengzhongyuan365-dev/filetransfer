#include <QApplication>
#include <QLocale>
#include <QTranslator>

#include "gui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QTranslator translator;
    const QString locale = QLocale::system().name();
#ifdef LAN_TRANSLATION_DIR
    const bool translation_loaded = translator.load("lan-file-transfer_" + locale, LAN_TRANSLATION_DIR);
#else
    const bool translation_loaded = translator.load("lan-file-transfer_" + locale, "translations");
#endif
    if (translation_loaded) {
        app.installTranslator(&translator);
    }

    lan::gui::MainWindow window;
    window.show();
    return app.exec();
}

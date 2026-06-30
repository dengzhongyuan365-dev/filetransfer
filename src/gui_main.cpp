#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QTranslator>

#include "gui/main_window.h"

namespace {

bool load_translation(QTranslator& translator, const QString& name) {
    const QStringList dirs{
        qEnvironmentVariable("LAN_TRANSLATION_DIR"),
#ifdef LAN_TRANSLATION_DIR
        QString::fromUtf8(LAN_TRANSLATION_DIR),
#endif
        QStringLiteral("/usr/share/lan-file-transfer/translations"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/lan-file-transfer/translations"),
        QDir::currentPath() + QStringLiteral("/translations"),
    };

    for (const auto& dir : dirs) {
        if (!dir.isEmpty() && translator.load(name, dir)) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QTranslator translator;
    const QString locale = QLocale::system().name();
    const QString language = QLocale::system().name().section('_', 0, 0);
    const bool translation_loaded =
        load_translation(translator, "lan-file-transfer_" + locale) ||
        load_translation(translator, "lan-file-transfer_" + language);
    if (translation_loaded) {
        app.installTranslator(&translator);
    }

    lan::gui::MainWindow window;
    window.show();
    return app.exec();
}

#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
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

QString single_instance_server_name() {
    return QStringLiteral("brinstrom-lan-file-transfer-gui");
}

bool notify_running_instance(const QString& server_name) {
    QLocalSocket socket;
    socket.connectToServer(server_name, QIODevice::WriteOnly);
    if (!socket.waitForConnected(200)) {
        return false;
    }
    socket.write("activate\n");
    socket.flush();
    socket.waitForBytesWritten(200);
    return true;
}

void activate_window(lan::gui::MainWindow& window) {
    if (window.isMinimized()) {
        window.showNormal();
    } else {
        window.show();
    }
    window.raise();
    window.activateWindow();
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("brinstrom"));
    QCoreApplication::setApplicationName(QStringLiteral("lan-file-transfer"));

    const auto server_name = single_instance_server_name();
    if (notify_running_instance(server_name)) {
        return 0;
    }

    QLocalServer instance_server;
    QLocalServer::removeServer(server_name);
    if (!instance_server.listen(server_name)) {
        qWarning() << "Failed to create single instance server:" << instance_server.errorString();
        return 1;
    }

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
    QObject::connect(&instance_server, &QLocalServer::newConnection, &window, [&] {
        while (auto* connection = instance_server.nextPendingConnection()) {
            connection->readAll();
            connection->disconnectFromServer();
            connection->deleteLater();
        }
        activate_window(window);
    });
    window.show();
    return app.exec();
}

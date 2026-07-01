#include "gui/main_window.h"

#include <QAbstractSocket>
#include <QAbstractItemView>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkInterface>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QShortcut>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QTime>
#include <QToolButton>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>
#include <QCheckBox>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <set>
#include <utility>

#include "gui/drop_panel.h"
#include "gui/qt_utils.h"
#include "gui/transfer_events.h"
#include "lan/app/receiver_config.h"
#include "lan/common/error.h"
#include "lan/common/size.h"

namespace lan::gui {
namespace {

constexpr int kMaxRememberedPeers = 10;
constexpr int kMaxReceiveHistory = 100;

QString snapshot_key(const TransferSnapshot& snapshot) {
    const auto direction = snapshot.direction == TransferDirection::receive ? "receive" : "send";
    return QString("%1:%2").arg(direction).arg(snapshot.transfer_id);
}

QString state_text(TransferState state) {
    switch (state) {
        case TransferState::pending:
            return QCoreApplication::translate("MainWindow", "pending");
        case TransferState::running:
            return QCoreApplication::translate("MainWindow", "running");
        case TransferState::completed:
            return QCoreApplication::translate("MainWindow", "completed");
        case TransferState::failed:
            return QCoreApplication::translate("MainWindow", "failed");
        case TransferState::cancelled:
            return QCoreApplication::translate("MainWindow", "cancelled");
    }
    return to_qstring(std::string(transfer_state_name(state)));
}

QString kind_text(TransferKind kind) {
    switch (kind) {
        case TransferKind::file:
            return QCoreApplication::translate("MainWindow", "file");
        case TransferKind::directory:
            return QCoreApplication::translate("MainWindow", "folder");
    }
    return {};
}

bool is_terminal_state(TransferState state) {
    return state == TransferState::completed ||
           state == TransferState::failed ||
           state == TransferState::cancelled;
}

QString make_link_code() {
    const auto value = QRandomGenerator::global()->bounded(100000, 1000000);
    return QString::number(value);
}

qint64 now_ms() {
    return QDateTime::currentMSecsSinceEpoch();
}

QList<QHostAddress> discovery_targets() {
    QList<QHostAddress> targets;
    QSet<QString> seen;
    const auto add_target = [&targets, &seen](const QHostAddress& address) {
        if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
            return;
        }
        const auto key = address.toString();
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        targets.push_back(address);
    };

    add_target(QHostAddress(QHostAddress::Broadcast));
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            !flags.testFlag(QNetworkInterface::CanBroadcast) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            add_target(entry.broadcast());
        }
    }
    return targets;
}

bool is_private_ipv4(quint32 address) {
    const auto first = (address >> 24) & 0xffU;
    const auto second = (address >> 16) & 0xffU;
    return first == 10U ||
           (first == 172U && second >= 16U && second <= 31U) ||
           (first == 192U && second == 168U);
}

QList<QHostAddress> extended_discovery_targets() {
    QList<QHostAddress> targets;
    QSet<QString> seen;
    const auto add_target = [&targets, &seen](quint32 address) {
        const auto host = address & 0xffU;
        if (host == 0U || host == 255U) {
            return;
        }
        const auto target = QHostAddress(address);
        const auto key = target.toString();
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        targets.push_back(target);
    };

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto& interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const auto& entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const auto local = entry.ip().toIPv4Address();
            if (!is_private_ipv4(local)) {
                continue;
            }

            const auto prefix = local & 0xffff0000U;
            const auto local_subnet = static_cast<int>((local >> 8) & 0xffU);
            const auto begin_subnet = std::max(0, local_subnet - 16);
            const auto end_subnet = std::min(255, local_subnet + 16);
            for (int subnet = begin_subnet; subnet <= end_subnet; ++subnet) {
                for (quint32 host = 1; host <= 254; ++host) {
                    add_target(prefix | (static_cast<quint32>(subnet) << 8) | host);
                }
            }
        }
    }
    return targets;
}

bool parse_manual_peer_endpoint(const QString& text, QString& host, std::uint16_t& port) {
    auto value = text.trimmed();
    if (value.isEmpty()) {
        return false;
    }
    std::uint16_t parsed_port = kTransferPort;
    const auto colon = value.lastIndexOf(':');
    if (colon > 0 && value.indexOf(':') == colon) {
        bool ok = false;
        const auto port_value = value.mid(colon + 1).toUInt(&ok);
        if (!ok || port_value == 0 || port_value > 65535) {
            return false;
        }
        parsed_port = static_cast<std::uint16_t>(port_value);
        value = value.left(colon);
    }

    const auto address = QHostAddress(value);
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }

    host = address.toString();
    port = parsed_port;
    return true;
}

QSettings app_settings() {
    return QSettings(QStringLiteral("brinstrom"), QStringLiteral("lan-file-transfer"));
}

QString saved_receive_dir() {
    return app_settings().value(QStringLiteral("receiveDir"), default_receive_dir()).toString();
}

void save_receive_dir(const QString& dir) {
    app_settings().setValue(QStringLiteral("receiveDir"), dir);
}

QString remembered_close_action() {
    return app_settings().value(QStringLiteral("closeAction")).toString();
}

void save_close_action(const QString& action) {
    auto settings = app_settings();
    if (action.isEmpty()) {
        settings.remove(QStringLiteral("closeAction"));
        return;
    }
    settings.setValue(QStringLiteral("closeAction"), action);
}

int remembered_max_global_sends() {
    return std::clamp(app_settings().value(QStringLiteral("maxGlobalSends"), 1).toInt(), 1, 8);
}

int remembered_max_peer_sends() {
    return std::clamp(app_settings().value(QStringLiteral("maxPeerSends"), 1).toInt(), 1, 4);
}

void save_send_scheduler_settings(int max_global_sends, int max_peer_sends) {
    auto settings = app_settings();
    settings.setValue(QStringLiteral("maxGlobalSends"), std::clamp(max_global_sends, 1, 8));
    settings.setValue(QStringLiteral("maxPeerSends"), std::clamp(max_peer_sends, 1, 4));
}

void append_local_file_path(const QUrl& url, QStringList& paths, std::set<QString>& seen) {
    if (!url.isLocalFile()) {
        return;
    }
    const auto path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    if (seen.insert(path).second) {
        paths.push_back(path);
    }
}

QStringList local_file_paths_from_mime(const QMimeData* mime) {
    QStringList paths;
    std::set<QString> seen;
    if (mime == nullptr) {
        return paths;
    }

    for (const auto& url : mime->urls()) {
        append_local_file_path(url, paths, seen);
    }

    if (mime->hasFormat(QStringLiteral("x-special/gnome-copied-files"))) {
        const auto lines = QString::fromUtf8(mime->data(QStringLiteral("x-special/gnome-copied-files")))
                               .split('\n', Qt::SkipEmptyParts);
        for (int i = 1; i < lines.size(); ++i) {
            append_local_file_path(QUrl(lines.at(i).trimmed()), paths, seen);
        }
    }

    return paths;
}

QImage image_from_mime(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasImage()) {
        return {};
    }
    const auto data = mime->imageData();
    if (data.canConvert<QImage>()) {
        return data.value<QImage>();
    }
    return {};
}

QString clipboard_image_dir() {
    auto root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath() + QStringLiteral("/lan-file-transfer");
    }
    return QDir(root).filePath(QStringLiteral("clipboard"));
}

QIcon application_icon() {
    auto icon = QIcon::fromTheme("lan-file-transfer");
    if (!icon.isNull()) {
        return icon;
    }

    const QStringList paths{
        QStringLiteral("/usr/share/icons/hicolor/scalable/apps/lan-file-transfer.svg"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../share/icons/hicolor/scalable/apps/lan-file-transfer.svg"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../resources/icons/lan-file-transfer.svg"),
        QDir::currentPath() + QStringLiteral("/resources/icons/lan-file-transfer.svg"),
    };
    for (const auto& path : paths) {
        icon = QIcon(path);
        if (!icon.isNull()) {
            return icon;
        }
    }
    return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QString text, QWidget* parent = nullptr) : QLabel(parent), full_text_(std::move(text)) {
        setToolTip(full_text_);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override {
        return QSize(16, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto text = fontMetrics().elidedText(full_text_, Qt::ElideMiddle, width());
        painter.drawText(rect(), alignment() | Qt::TextSingleLine, text);
    }

private:
    QString full_text_;
};

}  // namespace

MainWindow::MainWindow() : node_id_(load_or_create_node_id()) {
    setWindowTitle(QCoreApplication::translate("MainWindow", "LAN File Transfer"));
    setWindowIcon(application_icon());
    resize(400, 600);
    setMinimumSize(380, 560);
    setObjectName("app");
    build_ui();
    load_remembered_peers();
    setup_tray();
    setup_scheduler();
    setup_discovery();
}

MainWindow::~MainWindow() {
    stop_receiver();
    stop_sender();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!force_quit_ && tray_icon_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable()) {
        auto action = CloseAction::cancel;
        const auto remembered = remembered_close_action();
        if (remembered == QStringLiteral("tray")) {
            action = CloseAction::minimizeToTray;
        } else if (remembered == QStringLiteral("quit")) {
            action = CloseAction::quit;
        } else {
            action = ask_close_action();
        }

        if (action == CloseAction::cancel) {
            event->ignore();
            return;
        }
        if (action == CloseAction::quit) {
            force_quit_ = true;
            stop_receiver();
            stop_sender();
            event->accept();
            return;
        }

        hide();
        event->ignore();
        if (!tray_message_shown_) {
            tray_icon_->showMessage(
                QCoreApplication::translate("MainWindow", "LAN File Transfer"),
                QCoreApplication::translate("MainWindow", "LAN File Transfer is still running in the tray."),
                QSystemTrayIcon::Information,
                2500);
            tray_message_shown_ = true;
        }
        return;
    }
    stop_receiver();
    stop_sender();
    event->accept();
}

void MainWindow::build_ui() {
    stack_ = new QStackedWidget(this);
    stack_->addWidget(build_setup_page());
    stack_->addWidget(build_peer_page());
    stack_->addWidget(build_transfer_page());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(stack_);
    apply_style();
}

QWidget* MainWindow::build_setup_page() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 28, 28, 28);
    layout->setSpacing(0);

    auto* card = new QFrame(page);
    card->setObjectName("setupCard");
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(24, 24, 24, 24);
    card_layout->setSpacing(14);

    auto* title = new QLabel(QCoreApplication::translate("MainWindow", "Receiving folder"), page);
    title->setObjectName("title");
    auto* subtitle = new QLabel(QCoreApplication::translate("MainWindow", "Files sent to this machine will be saved here."), page);
    subtitle->setObjectName("mutedText");

    auto* path_box = new QFrame(page);
    path_box->setObjectName("pathBox");
    auto* row = new QHBoxLayout(path_box);
    row->setContentsMargins(12, 10, 10, 10);
    row->setSpacing(10);

    receive_dir_ = new QLabel(saved_receive_dir(), page);
    receive_dir_->setObjectName("pathLabel");
    receive_dir_->setWordWrap(true);

    auto* choose = new QPushButton(QCoreApplication::translate("MainWindow", "Choose"), page);
    choose->setObjectName("secondaryButton");
    row->addWidget(receive_dir_, 1);
    row->addWidget(choose);

    auto* continue_button = new QPushButton(QCoreApplication::translate("MainWindow", "Start"), page);
    continue_button->setObjectName("primaryButton");

    card_layout->addWidget(title);
    card_layout->addWidget(subtitle);
    card_layout->addSpacing(8);
    card_layout->addWidget(path_box);
    card_layout->addSpacing(6);
    card_layout->addWidget(continue_button);

    layout->addStretch(1);
    layout->addWidget(card);
    layout->addStretch(1);

    connect(choose, &QPushButton::clicked, this, [this] {
        const auto dir = QFileDialog::getExistingDirectory(this, QCoreApplication::translate("MainWindow", "Receiving folder"), receive_dir_->text());
        if (!dir.isEmpty()) {
            receive_dir_->setText(dir);
        }
    });
    connect(continue_button, &QPushButton::clicked, this, [this] {
        save_receive_dir(receive_dir_->text());
        if (start_receiver()) {
            search_peers();
            stack_->setCurrentWidget(peer_page_);
        }
    });
    return page;
}

QWidget* MainWindow::build_peer_page() {
    peer_page_ = new QWidget(this);
    auto* layout = new QVBoxLayout(peer_page_);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(12);

    auto* title = new QLabel(QCoreApplication::translate("MainWindow", "Nearby machines"), peer_page_);
    title->setObjectName("title");
    auto* settings = new QPushButton(QCoreApplication::translate("MainWindow", "Settings"), peer_page_);
    settings->setObjectName("secondaryButton");
    auto* search = new QPushButton(QCoreApplication::translate("MainWindow", "Refresh"), peer_page_);
    search->setObjectName("secondaryButton");
    back_to_transfer_ = new QPushButton(QCoreApplication::translate("MainWindow", "Back to Transfer"), peer_page_);
    back_to_transfer_->setObjectName("secondaryButton");
    back_to_transfer_->setVisible(false);

    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    header->addWidget(title, 1);
    header->addWidget(back_to_transfer_);
    header->addWidget(settings);
    header->addWidget(search);

    peer_filter_ = new QLineEdit(peer_page_);
    peer_filter_->setObjectName("searchInput");
    peer_filter_->setPlaceholderText(QCoreApplication::translate("MainWindow", "Search device name or IP"));

    peer_list_ = new QListWidget(peer_page_);
    peer_list_->setObjectName("peerList");
    peer_list_->setFrameShape(QFrame::NoFrame);
    peer_list_->setSpacing(10);
    peer_list_->setSelectionMode(QAbstractItemView::NoSelection);
    peer_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    status_ = new QLabel(QCoreApplication::translate("MainWindow", "Ready to find machines."), peer_page_);
    status_->setObjectName("mutedText");

    layout->addLayout(header);
    layout->addWidget(peer_filter_);
    layout->addWidget(peer_list_, 1);
    layout->addWidget(status_);

    connect(search, &QPushButton::clicked, this, [this] {
        search_peers();
    });
    connect(settings, &QPushButton::clicked, this, [this] {
        show_settings();
    });
    connect(back_to_transfer_, &QPushButton::clicked, this, [this] {
        open_transfer_page();
    });
    connect(peer_filter_, &QLineEdit::textChanged, this, [this] {
        refresh_peer_list();
    });
    connect(peer_filter_, &QLineEdit::returnPressed, this, [this] {
        add_manual_peer_from_filter();
    });
    connect(peer_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        link_peer(item);
    });
    return peer_page_;
}

QWidget* MainWindow::build_transfer_page() {
    transfer_page_ = new QWidget(this);
    auto* layout = new QVBoxLayout(transfer_page_);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(12);

    linked_label_ = new QLabel(QCoreApplication::translate("MainWindow", "Not linked"), transfer_page_);
    linked_label_->setObjectName("title");
    auto* back = new QPushButton(QCoreApplication::translate("MainWindow", "Change"), transfer_page_);
    back->setObjectName("secondaryButton");
    auto* disconnect = new QPushButton(QCoreApplication::translate("MainWindow", "Disconnect"), transfer_page_);
    disconnect->setObjectName("secondaryButton");

    auto* header = new QHBoxLayout();
    header->addWidget(linked_label_, 1);
    header->addWidget(disconnect);
    header->addWidget(back);

    drop_panel_ = new DropPanel(transfer_page_);
    drop_panel_->setMinimumHeight(360);
    auto* drop_layout = new QVBoxLayout(drop_panel_);
    drop_layout->setContentsMargins(0, 0, 0, 0);
    drop_layout->setSpacing(0);

    auto* scroll = new QScrollArea(drop_panel_);
    scroll->setObjectName("transferScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* transfer_list = new QWidget(scroll);
    transfers_layout_ = new QVBoxLayout(transfer_list);
    transfers_layout_->setContentsMargins(0, 0, 0, 0);
    transfers_layout_->setSpacing(8);

    empty_transfer_label_ = new QLabel(QCoreApplication::translate("MainWindow", "Drop or paste files and folders here\nSend to the linked machine"), transfer_list);
    empty_transfer_label_->setObjectName("emptyTransfer");
    empty_transfer_label_->setAlignment(Qt::AlignCenter);
    empty_transfer_label_->setMinimumHeight(320);
    transfers_layout_->addWidget(empty_transfer_label_);
    transfers_layout_->addStretch(1);
    scroll->setWidget(transfer_list);
    drop_layout->addWidget(scroll);

    log_ = new QPlainTextEdit(transfer_page_);
    log_->setObjectName("logView");
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(120);
    log_->setFixedHeight(96);
    log_->setPlaceholderText(QCoreApplication::translate("MainWindow", "Logs"));

    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    auto* history = new QPushButton(QCoreApplication::translate("MainWindow", "History"), transfer_page_);
    history->setObjectName("secondaryButton");
    footer->addStretch(1);
    footer->addWidget(history);

    layout->addLayout(header);
    layout->addWidget(drop_panel_, 1);
    layout->addLayout(footer);
    layout->addWidget(log_);

    connect(back, &QPushButton::clicked, this, [this] {
        stack_->setCurrentWidget(peer_page_);
    });
    connect(history, &QPushButton::clicked, this, [this] {
        show_receive_history();
    });
    connect(disconnect, &QPushButton::clicked, this, [this] {
        disconnect_peer();
    });
    auto* paste_shortcut = new QShortcut(QKeySequence::Paste, transfer_page_);
    paste_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(paste_shortcut, &QShortcut::activated, this, [this] {
        paste_paths_from_clipboard();
    });
    drop_panel_->on_drop = [this](const QStringList& paths) {
        send_paths(paths);
    };
    return transfer_page_;
}

void MainWindow::apply_style() {
    setStyleSheet(R"(
        #app {
            background: #f6f7fb;
            color: #1e2430;
            font-family: "Noto Sans", "Inter", sans-serif;
            font-size: 14px;
        }
        #title {
            font-size: 22px;
            font-weight: 700;
            letter-spacing: 0px;
        }
        #dialogTitle {
            font-size: 17px;
            font-weight: 700;
            letter-spacing: 0px;
        }
        #mutedText {
            color: #6d7480;
            font-size: 13px;
        }
        #setupCard {
            background: #ffffff;
            border: 1px solid #e4e7ec;
            border-radius: 8px;
        }
        #pathBox {
            background: #f8fafc;
            border: 1px solid #dde3ea;
            border-radius: 8px;
        }
        #pathLabel {
            color: #364152;
            font-size: 13px;
        }
        #transferDropArea {
            border: 1px dashed #97a3b6;
            border-radius: 8px;
            background: #ffffff;
        }
        #searchInput {
            min-height: 38px;
            padding: 0 12px;
            border: 1px solid #d9dee8;
            border-radius: 8px;
            background: #ffffff;
            color: #1e2430;
        }
        #peerList, #transferScroll {
            background: transparent;
            border: none;
            outline: none;
        }
        #peerCard, #transferCard {
            background: #ffffff;
            border: 1px solid #e2e7ef;
            border-radius: 8px;
        }
        #peerIcon {
            border-radius: 8px;
        }
        #peerName, #transferName {
            color: #1e2430;
            font-size: 14px;
            font-weight: 700;
        }
        #transferName {
            min-width: 0px;
        }
        #peerMeta, #transferMeta, #transferMetric {
            color: #717b8b;
            font-size: 12px;
        }
        #transferDetail {
            color: #5f6978;
            font-size: 12px;
        }
        #transferMetric {
            line-height: 16px;
        }
        #onlineBadge, #linkedBadge, #offlineBadge, #stateBadge {
            color: #087443;
            background: #e7f8ef;
            border-radius: 7px;
            padding: 3px 4px;
            font-size: 12px;
        }
        #linkedBadge {
            color: #1d4ed8;
            background: #eaf1ff;
            border: none;
            min-height: 0px;
        }
        #offlineBadge {
            color: #6b7280;
            background: #eef2f7;
        }
        #emptyTransfer {
            color: #8a93a3;
            background: transparent;
            border: none;
        }
        #logView {
            background: #ffffff;
            border: 1px solid #e2e7ef;
            border-radius: 8px;
            color: #5f6978;
            font-family: "Noto Sans Mono", "JetBrains Mono", monospace;
            font-size: 11px;
            padding: 6px;
        }
        QPushButton {
            min-height: 34px;
            padding: 0 14px;
            border-radius: 8px;
            border: 1px solid #cfd7e3;
            background: #ffffff;
            color: #243044;
            outline: none;
        }
        QPushButton:focus {
            outline: none;
            border: 1px solid #cfd7e3;
        }
        QPushButton:hover {
            background: #f8fafc;
            border-color: #b8c2d2;
        }
        QPushButton:pressed {
            background: #eef2f7;
            border-color: #aab6c8;
        }
        QPushButton:disabled {
            background: #f3f4f6;
            border-color: #e2e7ef;
            color: #9aa3b2;
        }
        #primaryButton, #linkButton {
            background: #2563eb;
            border-color: #2563eb;
            color: #ffffff;
            font-weight: 700;
        }
        #primaryButton:focus, #linkButton:focus {
            border-color: #2563eb;
        }
        #primaryButton:hover, #linkButton:hover {
            background: #1d4ed8;
            border-color: #1d4ed8;
        }
        #primaryButton:pressed, #linkButton:pressed {
            background: #1e40af;
            border-color: #1e40af;
        }
        #primaryButton:disabled, #linkButton:disabled {
            background: #b8c7ee;
            border-color: #b8c7ee;
            color: #eef3ff;
        }
        #secondaryButton {
            background: #ffffff;
            border-color: #d8dee8;
            color: #273549;
        }
        #secondaryButton:focus {
            border-color: #d8dee8;
        }
        #secondaryButton:hover {
            background: #f8fafc;
            border-color: #b8c2d2;
        }
        #secondaryButton:pressed {
            background: #eef2f7;
            border-color: #aab6c8;
        }
        #secondaryButton:disabled {
            background: #f3f4f6;
            border-color: #e2e7ef;
            color: #9aa3b2;
        }
        #taskResumeButton, #taskOpenButton, #taskStopButton, #taskRemoveButton {
            min-width: 24px;
            max-width: 24px;
            min-height: 24px;
            max-height: 24px;
            padding: 0;
            border-radius: 12px;
            border-color: transparent;
            background: #f3f4f6;
        }
        #taskOpenButton {
            color: #2563eb;
        }
        #taskOpenButton:hover {
            background: #eef4ff;
            color: #1d4ed8;
        }
        #taskOpenButton:pressed {
            background: #dbe8ff;
            color: #1e40af;
        }
        #taskOpenButton:disabled {
            color: #a6adb8;
            background: #f3f4f6;
        }
        #taskResumeButton {
            color: #087443;
        }
        #taskResumeButton:hover {
            background: #e7f8ef;
            color: #06643a;
        }
        #taskResumeButton:pressed {
            background: #d4f1e1;
            color: #055730;
        }
        #taskResumeButton:disabled {
            color: #a6adb8;
            background: #f3f4f6;
        }
        #taskStopButton {
            color: #9f2f2f;
        }
        #taskStopButton:hover {
            background: #fdecec;
            color: #8a2424;
        }
        #taskStopButton:pressed {
            background: #f9dada;
            color: #7f1d1d;
        }
        #taskStopButton:disabled {
            color: #a6adb8;
            background: #f3f4f6;
        }
        #taskRemoveButton {
            color: #687386;
        }
        #taskRemoveButton:hover {
            background: #eef2f7;
            color: #1e2430;
        }
        #taskRemoveButton:pressed {
            background: #dfe5ee;
            color: #111827;
        }
        #taskRemoveButton:disabled {
            color: #a6adb8;
            background: #f3f4f6;
        }
    )");
}

void MainWindow::setup_tray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    auto* menu = new QMenu(this);
    auto* show_action = menu->addAction(QCoreApplication::translate("MainWindow", "Show"));
    auto* hide_action = menu->addAction(QCoreApplication::translate("MainWindow", "Hide"));
    menu->addSeparator();
    auto* quit_action = menu->addAction(QCoreApplication::translate("MainWindow", "Quit"));

    connect(show_action, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    connect(hide_action, &QAction::triggered, this, [this] {
        hide();
    });
    connect(quit_action, &QAction::triggered, this, [this] {
        quit_from_tray();
    });

    tray_icon_ = new QSystemTrayIcon(application_icon(), this);
    tray_icon_->setToolTip(QCoreApplication::translate("MainWindow", "LAN File Transfer"));
    tray_icon_->setContextMenu(menu);
    connect(tray_icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            toggle_window_visibility();
        }
    });
    tray_icon_->show();
}

void MainWindow::setup_scheduler() {
    scheduler_ = std::make_unique<TransferScheduler>(SchedulerCallbacks{
        .on_snapshot = [this](SchedulerSnapshot snapshot) {
            QMetaObject::invokeMethod(this, [this, snapshot = std::move(snapshot)]() mutable {
                handle_scheduler_snapshot(std::move(snapshot));
            }, Qt::QueuedConnection);
        },
        .on_log = [this](std::string line) {
            QMetaObject::invokeMethod(this, [this, line = std::move(line)]() mutable {
                handle_scheduler_log(std::move(line));
            }, Qt::QueuedConnection);
        },
        .on_wakeup = [this] {
            QMetaObject::invokeMethod(this, [this] {
                wake_scheduler();
            }, Qt::QueuedConnection);
        },
    });
    scheduler_->set_limits(SchedulerLimits{
        .max_global_sends = remembered_max_global_sends(),
        .max_peer_sends = remembered_max_peer_sends(),
    });
    for (const auto& peer : peers_) {
        sync_scheduler_peer(peer);
    }
}

void MainWindow::toggle_window_visibility() {
    if (isVisible() && !isMinimized()) {
        hide();
        return;
    }
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quit_from_tray() {
    force_quit_ = true;
    close();
}

CloseAction MainWindow::ask_close_action() {
    QDialog dialog(this);
    dialog.setWindowTitle(QCoreApplication::translate("MainWindow", "Choose action"));
    dialog.setModal(true);
    dialog.resize(360, 210);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto* title = new QLabel(QCoreApplication::translate("MainWindow", "Please choose your action"), &dialog);
    title->setAlignment(Qt::AlignCenter);

    auto* minimize = new QRadioButton(QCoreApplication::translate("MainWindow", "Minimize to system tray"), &dialog);
    auto* quit = new QRadioButton(QCoreApplication::translate("MainWindow", "Quit"), &dialog);
    auto* remember = new QCheckBox(QCoreApplication::translate("MainWindow", "Do not ask again"), &dialog);
    minimize->setChecked(true);

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("MainWindow", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* ok = new QPushButton(QCoreApplication::translate("MainWindow", "OK"), &dialog);
    ok->setObjectName("primaryButton");
    buttons->addWidget(cancel);
    buttons->addWidget(ok);

    layout->addWidget(title);
    layout->addWidget(minimize);
    layout->addWidget(quit);
    layout->addWidget(remember);
    layout->addStretch(1);
    layout->addLayout(buttons);

    CloseAction action = CloseAction::cancel;
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, [&dialog, minimize, remember, &action] {
        action = minimize->isChecked() ? CloseAction::minimizeToTray : CloseAction::quit;
        if (remember->isChecked()) {
            save_close_action(action == CloseAction::minimizeToTray ? QStringLiteral("tray") : QStringLiteral("quit"));
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted) {
        return CloseAction::cancel;
    }
    return action;
}

void MainWindow::setup_discovery() {
    discovery_ = std::make_unique<QUdpSocket>();
    const auto bound = discovery_->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (bound) {
        log_event(QCoreApplication::translate("MainWindow", "UDP discovery listening on %1").arg(kDiscoveryPort));
    } else {
        log_event(QCoreApplication::translate("MainWindow", "UDP discovery bind failed on %1: %2")
                      .arg(kDiscoveryPort)
                      .arg(discovery_->errorString()));
    }
    connect(discovery_.get(), &QUdpSocket::readyRead, this, [this] {
        read_discovery();
    });
}

QString MainWindow::load_or_create_node_id() {
    auto settings = app_settings();
    auto id = settings.value(QStringLiteral("nodeId")).toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QStringLiteral("nodeId"), id);
    }
    return id;
}

void MainWindow::load_remembered_peers() {
    auto settings = app_settings();
    const auto size = settings.beginReadArray(QStringLiteral("peers"));
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        Peer peer{
            .id = settings.value(QStringLiteral("id")).toString(),
            .name = settings.value(QStringLiteral("name"), QCoreApplication::translate("MainWindow", "Unknown")).toString(),
            .host = settings.value(QStringLiteral("host")).toString(),
            .port = static_cast<std::uint16_t>(settings.value(QStringLiteral("port"), kTransferPort).toUInt()),
            .online = false,
            .linked = false,
            .last_seen_ms = settings.value(QStringLiteral("lastSeen"), 0).toLongLong(),
            .last_linked_ms = settings.value(QStringLiteral("lastLinked"), 0).toLongLong(),
        };
        if (!peer.id.isEmpty() && !peer.host.isEmpty()) {
            const auto duplicate_id = find_peer_id_by_endpoint(peer.host, peer.port);
            if (!duplicate_id.isEmpty()) {
                const auto duplicate = peers_.value(duplicate_id);
                if (duplicate.last_linked_ms > peer.last_linked_ms) {
                    continue;
                }
                peers_.remove(duplicate_id);
            }
            peers_.insert(peer.id, peer);
        }
    }
    settings.endArray();
    refresh_peer_list();
}

QString MainWindow::find_peer_id_by_endpoint(const QString& host, std::uint16_t port) const {
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        if (it.value().host == host && it.value().port == port) {
            return it.key();
        }
    }
    return {};
}

void MainWindow::remember_peer(const Peer& peer) {
    auto remembered = peer;
    remembered.last_linked_ms = now_ms();
    remembered.online = true;
    remembered.linked = peers_.value(peer.id).linked || peer.linked;
    peers_.insert(remembered.id, remembered);
    save_remembered_peers();
    refresh_peer_list();
}

void MainWindow::save_remembered_peers() {
    auto peers = peers_.values();
    peers.erase(std::remove_if(peers.begin(), peers.end(), [](const Peer& peer) {
        return peer.last_linked_ms <= 0;
    }), peers.end());
    std::sort(peers.begin(), peers.end(), [](const Peer& left, const Peer& right) {
        return left.last_linked_ms > right.last_linked_ms;
    });
    std::set<QString> endpoints;
    peers.erase(std::remove_if(peers.begin(), peers.end(), [&endpoints](const Peer& peer) {
        const auto endpoint = peer.host + ":" + QString::number(peer.port);
        if (endpoints.contains(endpoint)) {
            return true;
        }
        endpoints.insert(endpoint);
        return false;
    }), peers.end());
    if (peers.size() > kMaxRememberedPeers) {
        peers.erase(peers.begin() + kMaxRememberedPeers, peers.end());
    }

    auto settings = app_settings();
    settings.beginWriteArray(QStringLiteral("peers"), peers.size());
    for (int i = 0; i < peers.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("id"), peers.at(i).id);
        settings.setValue(QStringLiteral("name"), peers.at(i).name);
        settings.setValue(QStringLiteral("host"), peers.at(i).host);
        settings.setValue(QStringLiteral("port"), static_cast<int>(peers.at(i).port));
        settings.setValue(QStringLiteral("lastSeen"), peers.at(i).last_seen_ms);
        settings.setValue(QStringLiteral("lastLinked"), peers.at(i).last_linked_ms);
    }
    settings.endArray();
}

bool MainWindow::start_receiver() {
    receiver_ready_ = false;
    log_event(QCoreApplication::translate("MainWindow", "Starting receiver in %1").arg(receive_dir_->text()));
    ReceiverConfig config;
    config.bind_address = "0.0.0.0";
    config.port = kTransferPort;
    config.receive_dir = std::filesystem::path(to_string(receive_dir_->text()));
    config.allow_overwrite = false;
    config.once = false;

    auto validated = validate_receiver_config(std::move(config));
    if (!validated) {
        const auto message = to_qstring(format_error(validated.error()));
        status_->setText(message);
        log_event(QCoreApplication::translate("MainWindow", "Receiver config invalid: %1").arg(message));
        return false;
    }

    auto listening_promise = std::make_shared<std::promise<void>>();
    auto listening_reported = std::make_shared<bool>(false);
    auto listening = listening_promise->get_future();
    receiver_events_ = std::make_unique<GuiReceiverEvents>(
        [this](TransferSnapshotStore store) {
            merge_snapshots(std::move(store));
        },
        [this](QString line) {
            show_log(std::move(line));
        },
        [listening_promise, listening_reported](const ReceiverConfig&) {
            if (!*listening_reported) {
                *listening_reported = true;
                listening_promise->set_value();
            }
        });
    receiver_runner_ = std::make_unique<ReceiverServerRunner>();
    auto started = receiver_runner_->start(validated.value(), *receiver_events_);
    if (!started) {
        receiver_runner_.reset();
        receiver_events_.reset();
        const auto message = to_qstring(format_error(started.error()));
        status_->setText(message);
        log_event(QCoreApplication::translate("MainWindow", "Receiver thread failed to start: %1").arg(message));
        return false;
    }
    if (listening.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        receiver_runner_->stop();
        auto result = receiver_runner_->join();
        receiver_runner_.reset();
        receiver_events_.reset();
        if (!result) {
            const auto message = to_qstring(format_error(result.error()));
            status_->setText(message);
            log_event(QCoreApplication::translate("MainWindow", "Receiver listen failed: %1").arg(message));
            return false;
        }
        status_->setText(QCoreApplication::translate("MainWindow", "Receiver failed to start listening."));
        log_event(QCoreApplication::translate("MainWindow", "Receiver failed to start listening within 2s."));
        return false;
    }
    status_->setText(QCoreApplication::translate("MainWindow", "Receiver listening on TCP %1.").arg(kTransferPort));
    receiver_ready_ = true;
    log_event(QCoreApplication::translate("MainWindow", "Receiver listening on 0.0.0.0:%1").arg(kTransferPort));
    return true;
}

void MainWindow::stop_receiver() {
    if (receiver_runner_ == nullptr) {
        return;
    }
    receiver_runner_->stop();
    receiver_runner_->join();
    receiver_runner_.reset();
    receiver_events_.reset();
    receiver_ready_ = false;
    log_event(QCoreApplication::translate("MainWindow", "Receiver stopped."));
}

void MainWindow::show_settings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QCoreApplication::translate("MainWindow", "Settings"));
    dialog.resize(360, 390);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(9);

    auto* title = new QLabel(QCoreApplication::translate("MainWindow", "Settings"), &dialog);
    title->setObjectName("dialogTitle");
    auto* receive_label = new QLabel(QCoreApplication::translate("MainWindow", "Receiving folder"), &dialog);
    receive_label->setObjectName("mutedText");

    auto* path_box = new QFrame(&dialog);
    path_box->setObjectName("pathBox");
    auto* path_row = new QHBoxLayout(path_box);
    path_row->setContentsMargins(10, 8, 8, 8);
    path_row->setSpacing(8);

    auto* path = new QLabel(receive_dir_ != nullptr ? receive_dir_->text() : saved_receive_dir(), path_box);
    path->setObjectName("pathLabel");
    path->setWordWrap(true);
    auto* choose = new QPushButton(QCoreApplication::translate("MainWindow", "Choose"), path_box);
    choose->setObjectName("secondaryButton");
    path_row->addWidget(path, 1);
    path_row->addWidget(choose);

    auto* hint = new QLabel(QCoreApplication::translate("MainWindow", "Changing this folder restarts the local receiver."), &dialog);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);

    auto* scheduler_label = new QLabel(QCoreApplication::translate("MainWindow", "Transfer scheduling"), &dialog);
    scheduler_label->setObjectName("mutedText");
    auto* max_global_sends = new QSpinBox(&dialog);
    max_global_sends->setRange(1, 8);
    max_global_sends->setValue(remembered_max_global_sends());
    auto* max_peer_sends = new QSpinBox(&dialog);
    max_peer_sends->setRange(1, 4);
    max_peer_sends->setValue(remembered_max_peer_sends());

    auto* global_row = new QHBoxLayout();
    global_row->setSpacing(8);
    auto* global_label = new QLabel(QCoreApplication::translate("MainWindow", "Max simultaneous sends"), &dialog);
    global_label->setObjectName("mutedText");
    global_row->addWidget(global_label, 1);
    global_row->addWidget(max_global_sends);

    auto* peer_row = new QHBoxLayout();
    peer_row->setSpacing(8);
    auto* peer_label = new QLabel(QCoreApplication::translate("MainWindow", "Max sends per machine"), &dialog);
    peer_label->setObjectName("mutedText");
    peer_row->addWidget(peer_label, 1);
    peer_row->addWidget(max_peer_sends);

    auto* close_label = new QLabel(QCoreApplication::translate("MainWindow", "Close button action"), &dialog);
    close_label->setObjectName("mutedText");
    auto* ask_on_close = new QRadioButton(QCoreApplication::translate("MainWindow", "Ask every time"), &dialog);
    auto* tray_on_close = new QRadioButton(QCoreApplication::translate("MainWindow", "Minimize to system tray"), &dialog);
    auto* quit_on_close = new QRadioButton(QCoreApplication::translate("MainWindow", "Quit"), &dialog);
    const auto close_action = remembered_close_action();
    if (close_action == QStringLiteral("tray")) {
        tray_on_close->setChecked(true);
    } else if (close_action == QStringLiteral("quit")) {
        quit_on_close->setChecked(true);
    } else {
        ask_on_close->setChecked(true);
    }

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("MainWindow", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* save = new QPushButton(QCoreApplication::translate("MainWindow", "Save"), &dialog);
    save->setObjectName("primaryButton");
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    layout->addWidget(title);
    layout->addWidget(receive_label);
    layout->addWidget(path_box);
    layout->addWidget(hint);
    layout->addSpacing(2);
    layout->addWidget(scheduler_label);
    layout->addLayout(global_row);
    layout->addLayout(peer_row);
    layout->addSpacing(2);
    layout->addWidget(close_label);
    layout->addWidget(ask_on_close);
    layout->addWidget(tray_on_close);
    layout->addWidget(quit_on_close);
    layout->addStretch(1);
    layout->addLayout(buttons);

    connect(choose, &QPushButton::clicked, this, [this, path] {
        const auto dir = QFileDialog::getExistingDirectory(this, QCoreApplication::translate("MainWindow", "Receiving folder"), path->text());
        if (!dir.isEmpty()) {
            path->setText(dir);
        }
    });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this, &dialog, path, ask_on_close, tray_on_close, max_global_sends, max_peer_sends] {
        const auto next_dir = path->text().trimmed();
        if (next_dir.isEmpty()) {
            QMessageBox::warning(this,
                                 QCoreApplication::translate("MainWindow", "Settings"),
                                 QCoreApplication::translate("MainWindow", "Receiving folder cannot be empty."));
            return;
        }

        if (ask_on_close->isChecked()) {
            save_close_action({});
        } else if (tray_on_close->isChecked()) {
            save_close_action(QStringLiteral("tray"));
        } else {
            save_close_action(QStringLiteral("quit"));
        }
        save_send_scheduler_settings(max_global_sends->value(), max_peer_sends->value());
        if (scheduler_ != nullptr) {
            scheduler_->set_limits(SchedulerLimits{
                .max_global_sends = max_global_sends->value(),
                .max_peer_sends = max_peer_sends->value(),
            });
        }

        const auto old_dir = receive_dir_ != nullptr ? receive_dir_->text() : QString{};
        if (next_dir == old_dir) {
            dialog.accept();
            return;
        }

        const auto was_ready = receiver_ready_;
        stop_receiver();
        if (receive_dir_ != nullptr) {
            receive_dir_->setText(next_dir);
        }
        save_receive_dir(next_dir);
        log_event(QCoreApplication::translate("MainWindow", "Receiving folder changed to %1").arg(next_dir));
        if (was_ready && !start_receiver()) {
            QMessageBox::warning(this,
                                 QCoreApplication::translate("MainWindow", "Settings"),
                                 QCoreApplication::translate("MainWindow", "Receiver failed to restart. Check the receiving folder."));
            return;
        }
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::search_peers() {
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        it.value().online = false;
        sync_scheduler_peer(it.value());
    }
    wake_scheduler();
    refresh_peer_list();
    status_->setText(QCoreApplication::translate("MainWindow", "Searching..."));
    const auto message = QJsonDocument(QJsonObject{
        {"protocol", kProtocol},
        {"type", "discover"},
        {"id", node_id_},
        {"name", machine_name()},
        {"port", static_cast<int>(kTransferPort)},
    }).toJson(QJsonDocument::Compact);
    const auto targets = discovery_targets();
    QStringList target_labels;
    for (const auto& target : targets) {
        discovery_->writeDatagram(message, target, kDiscoveryPort);
        target_labels.push_back(target.toString());
    }
    log_event(QCoreApplication::translate("MainWindow", "Broadcast discover on UDP %1 to %2")
                  .arg(kDiscoveryPort)
                  .arg(target_labels.join(QStringLiteral(", "))));
    const auto extended_targets = extended_discovery_targets();
    for (const auto& target : extended_targets) {
        discovery_->writeDatagram(message, target, kDiscoveryPort);
    }
    if (!extended_targets.isEmpty()) {
        log_event(QCoreApplication::translate("MainWindow", "Extended discovery sent to %1 address(es).").arg(extended_targets.size()));
    }
    QTimer::singleShot(1200, this, [this] {
        if (peers_.isEmpty()) {
            status_->setText(QCoreApplication::translate("MainWindow", "No machines found."));
            log_event(QCoreApplication::translate("MainWindow", "Discovery finished: no machines found."));
        } else {
            status_->setText(QCoreApplication::translate("MainWindow", "%1 machine(s) found.").arg(peers_.size()));
            log_event(QCoreApplication::translate("MainWindow", "Discovery finished: %1 machine(s) found.").arg(peers_.size()));
        }
    });
}

void MainWindow::add_manual_peer_from_filter() {
    QString host;
    std::uint16_t port = kTransferPort;
    if (!parse_manual_peer_endpoint(peer_filter_->text(), host, port)) {
        show_log(QCoreApplication::translate("MainWindow", "Enter an IPv4 address in the search box, for example 10.8.7.203."));
        return;
    }

    auto id = find_peer_id_by_endpoint(host, port);
    if (id.isEmpty()) {
        id = QStringLiteral("manual:%1:%2").arg(host).arg(port);
    }
    auto peer = peers_.value(id);
    peer.id = id;
    peer.name = host;
    peer.host = host;
    peer.port = port;
    peer.online = true;
    peer.last_seen_ms = now_ms();
    peers_.insert(id, peer);
    status_->setText(QCoreApplication::translate("MainWindow", "Manual machine added."));
    log_event(QCoreApplication::translate("MainWindow", "Manual peer added: %1:%2").arg(host).arg(port));
    refresh_peer_list();
}

void MainWindow::read_discovery() {
    while (discovery_->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(discovery_->pendingDatagramSize()));
        QHostAddress sender;
        quint16 sender_port = 0;
        discovery_->readDatagram(datagram.data(), datagram.size(), &sender, &sender_port);

        const auto doc = QJsonDocument::fromJson(datagram);
        if (!doc.isObject()) {
            continue;
        }
        const auto obj = doc.object();
        if (obj.value("protocol").toString() != kProtocol) {
            continue;
        }
        const auto type = obj.value("type").toString();
        if (type == "discover") {
            if (obj.value("id").toString() != node_id_) {
                if (!receiver_ready_) {
                    log_event(QCoreApplication::translate("MainWindow", "Ignored discover from %1 because receiver is not ready.")
                                  .arg(sender.toString()));
                    continue;
                }
                log_event(QCoreApplication::translate("MainWindow", "Received discover from %1:%2").arg(sender.toString()).arg(sender_port));
                reply_to_discovery(sender, sender_port);
            }
        } else if (type == "announce") {
            log_event(QCoreApplication::translate("MainWindow", "Received announce from %1").arg(sender.toString()));
            add_peer(sender, obj);
        } else if (type == "link_request") {
            if (!receiver_ready_) {
                log_event(QCoreApplication::translate("MainWindow", "Ignored link request from %1 because receiver is not ready.")
                              .arg(sender.toString()));
                continue;
            }
            log_event(QCoreApplication::translate("MainWindow", "Received link request from %1").arg(sender.toString()));
            receive_link_request(sender, obj);
        } else if (type == "link_accept") {
            log_event(QCoreApplication::translate("MainWindow", "Received link accept from %1").arg(sender.toString()));
            receive_link_response(sender, obj, true);
        } else if (type == "link_reject") {
            log_event(QCoreApplication::translate("MainWindow", "Received link reject from %1").arg(sender.toString()));
            receive_link_response(sender, obj, false);
        } else if (type == "link_disconnect") {
            log_event(QCoreApplication::translate("MainWindow", "Received link disconnect from %1").arg(sender.toString()));
            receive_link_disconnect(sender, obj);
        }
    }
}

void MainWindow::reply_to_discovery(const QHostAddress& target, quint16 port) {
    const auto message = QJsonDocument(QJsonObject{
        {"protocol", kProtocol},
        {"type", "announce"},
        {"id", node_id_},
        {"name", machine_name()},
        {"port", static_cast<int>(kTransferPort)},
    }).toJson(QJsonDocument::Compact);
    discovery_->writeDatagram(message, target, port);
    log_event(QCoreApplication::translate("MainWindow", "Sent announce to %1:%2").arg(target.toString()).arg(port));
}

void MainWindow::add_peer(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    const auto host = address.toString();
    const auto port = static_cast<std::uint16_t>(obj.value("port").toInt(kTransferPort));
    const auto id = obj.value("id").toString(host + ":" + QString::number(port));
    const auto endpoint_peer_id = find_peer_id_by_endpoint(host, port);
    auto existing = peers_.value(id);
    if (!endpoint_peer_id.isEmpty() && endpoint_peer_id != id) {
        existing = peers_.value(endpoint_peer_id);
        peers_.remove(endpoint_peer_id);
        if (active_peer_id_ == endpoint_peer_id) {
            active_peer_id_ = id;
        }
        if (pending_link_codes_.contains(endpoint_peer_id)) {
            pending_link_codes_.insert(id, pending_link_codes_.take(endpoint_peer_id));
        }
    }

    Peer peer{
        .id = id,
        .name = obj.value("name").toString(QCoreApplication::translate("MainWindow", "Unknown")),
        .host = host,
        .port = port,
        .online = true,
        .linked = existing.linked,
        .last_seen_ms = now_ms(),
        .last_linked_ms = existing.last_linked_ms,
    };
    peers_.insert(id, peer);
    log_event(QCoreApplication::translate("MainWindow", "Peer available: %1 %2:%3").arg(peer.name, peer.host).arg(peer.port));
    if (peer.last_linked_ms > 0) {
        save_remembered_peers();
    }
    sync_scheduler_peer(peer);
    wake_scheduler();
    update_linked_header();
    refresh_peer_list();
}

bool MainWindow::peer_matches_filter(const Peer& peer) const {
    if (peer_filter_ == nullptr || peer_filter_->text().trimmed().isEmpty()) {
        return true;
    }
    const auto needle = peer_filter_->text().trimmed();
    return peer.name.contains(needle, Qt::CaseInsensitive) ||
           peer.host.contains(needle, Qt::CaseInsensitive) ||
           peer.id.contains(needle, Qt::CaseInsensitive);
}

void MainWindow::refresh_peer_list() {
    if (peer_list_ == nullptr) {
        return;
    }
    peer_list_->clear();
    int visible = 0;
    auto peers = peers_.values();
    std::sort(peers.begin(), peers.end(), [this](const Peer& left, const Peer& right) {
        if (is_linked_peer(left) != is_linked_peer(right)) {
            return is_linked_peer(left);
        }
        if (left.online != right.online) {
            return left.online;
        }
        if (left.last_linked_ms != right.last_linked_ms) {
            return left.last_linked_ms > right.last_linked_ms;
        }
        return left.name.localeAwareCompare(right.name) < 0;
    });
    for (const auto& peer : peers) {
        if (!peer_matches_filter(peer)) {
            continue;
        }
        auto* item = new QListWidgetItem();
        item->setData(Qt::UserRole, peer.id);
        item->setSizeHint(QSize(0, 76));
        peer_list_->addItem(item);
        peer_list_->setItemWidget(item, make_peer_card(peer));
        ++visible;
    }
    if (visible == 0) {
        const auto text = peers_.isEmpty() ? QCoreApplication::translate("MainWindow", "No machines yet. Refresh to search the LAN.")
                                           : QCoreApplication::translate("MainWindow", "No matching machines.");
        auto* item = new QListWidgetItem();
        item->setFlags(Qt::NoItemFlags);
        item->setSizeHint(QSize(0, 86));
        peer_list_->addItem(item);
        peer_list_->setItemWidget(item, make_empty_peer_card(text));
    }
}

QWidget* MainWindow::make_empty_peer_card(const QString& text) {
    auto* card = new QFrame(peer_list_);
    card->setObjectName("emptyCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    auto* label = new QLabel(text, card);
    label->setObjectName("mutedText");
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    return card;
}

QWidget* MainWindow::make_peer_card(const Peer& peer) {
    auto* card = new QFrame(peer_list_);
    card->setObjectName("peerCard");
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(12, 10, 12, 10);
    row->setSpacing(12);

    auto* icon = new QLabel(card);
    icon->setObjectName("peerIcon");
    icon->setFixedSize(44, 44);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(QApplication::style()
                        ->standardIcon(QStyle::SP_ComputerIcon)
                        .pixmap(38, 38));

    auto* text_box = new QVBoxLayout();
    text_box->setContentsMargins(0, 0, 0, 0);
    text_box->setSpacing(3);
    auto* name = new QLabel(peer.name, card);
    name->setObjectName("peerName");
    auto* meta = new QLabel(QString("%1:%2").arg(peer.host).arg(peer.port), card);
    meta->setObjectName("peerMeta");
    text_box->addWidget(name);
    text_box->addWidget(meta);

    const auto linked = is_linked_peer(peer);
    QWidget* badge = nullptr;
    if (linked) {
        auto* button = new QToolButton(card);
        button->setObjectName("linkedBadge");
        button->setText(QCoreApplication::translate("MainWindow", "linked"));
        button->setToolTip(QCoreApplication::translate("MainWindow", "Disconnect"));
        button->setAutoRaise(false);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        connect(button, &QToolButton::clicked, this, [this, id = peer.id] {
            disconnect_peer(id);
        });
        badge = button;
    } else {
        auto* label = new QLabel(peer.online ? QCoreApplication::translate("MainWindow", "online")
                                             : QCoreApplication::translate("MainWindow", "offline"),
                                 card);
        label->setObjectName(peer.online ? "onlineBadge" : "offlineBadge");
        label->setAlignment(Qt::AlignCenter);
        label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        badge = label;
    }

    auto* link = new QPushButton(linked ? QCoreApplication::translate("MainWindow", "Open")
                                        : QCoreApplication::translate("MainWindow", "Link"),
                                 card);
    link->setObjectName("linkButton");
    link->setEnabled(peer.online || linked);
    connect(link, &QPushButton::clicked, this, [this, id = peer.id] {
        request_link(id);
    });

    row->addWidget(icon);
    row->addLayout(text_box, 1);
    row->addWidget(badge);
    row->addWidget(link);
    return card;
}

QWidget* MainWindow::make_transfer_card(const TransferSnapshot& snapshot) {
    const auto key = snapshot_key(snapshot);
    auto* card = new QFrame(transfer_page_);
    card->setObjectName("transferCard");
    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(8, 9, 8, 9);
    row->setSpacing(4);

    auto* name = new ElidedLabel(QString::fromStdString(snapshot.name), card);
    name->setObjectName("transferName");
    name->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    auto* detail = new ElidedLabel(transfer_detail_text(snapshot), card);
    detail->setObjectName("transferDetail");
    detail->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

    auto* text_box = new QVBoxLayout();
    text_box->setContentsMargins(0, 0, 0, 0);
    text_box->setSpacing(2);
    text_box->addWidget(name);
    text_box->addWidget(detail);

    auto* rate = make_metric_label(QCoreApplication::translate("MainWindow", "Speed"), transfer_rate_text(snapshot), card);
    rate->setFixedWidth(40);
    rate->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* size = make_metric_label(QCoreApplication::translate("MainWindow", "Size"), transfer_size_text(snapshot), card);
    size->setFixedWidth(62);
    size->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto* state = new QLabel(state_text(snapshot.state), card);
    state->setObjectName("stateBadge");
    state->setAlignment(Qt::AlignCenter);
    state->setFixedWidth(76);
    state->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(4);
    auto* resume = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_ArrowForward), QCoreApplication::translate("MainWindow", "Resume transfer"), card);
    resume->setObjectName("taskResumeButton");
    resume->setEnabled(can_resume_transfer(snapshot));
    connect(resume, &QToolButton::clicked, this, [this, key] {
        resume_transfer(key);
    });
    auto* open = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_DirOpenIcon), QCoreApplication::translate("MainWindow", "Open containing folder"), card);
    open->setObjectName("taskOpenButton");
    open->setEnabled(can_open_transfer_dir(snapshot));
    connect(open, &QToolButton::clicked, this, [this, key] {
        open_transfer_dir(key);
    });
    auto* stop = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_MediaStop), QCoreApplication::translate("MainWindow", "Stop transfer"), card);
    stop->setObjectName("taskStopButton");
    stop->setEnabled(can_stop_transfer(snapshot));
    connect(stop, &QToolButton::clicked, this, [this, key] {
        stop_transfer(key);
    });
    auto* remove = make_task_tool_button(
        QApplication::style()->standardIcon(QStyle::SP_DialogCloseButton), QCoreApplication::translate("MainWindow", "Clear from list"), card);
    remove->setObjectName("taskRemoveButton");
    remove->setEnabled(can_clear_transfer(snapshot));
    connect(remove, &QToolButton::clicked, this, [this, key] {
        remove_transfer_card(key);
    });
    actions->addWidget(resume);
    actions->addWidget(open);
    actions->addWidget(stop);
    actions->addWidget(remove);
    actions->setSizeConstraint(QLayout::SetFixedSize);

    row->addLayout(text_box, 1);
    row->addWidget(rate, 0);
    row->addWidget(size, 0);
    row->addWidget(state, 0);
    row->addLayout(actions);
    return card;
}

QLabel* MainWindow::make_metric_label(const QString& title, const QString& value, QWidget* parent) {
    auto* label = new QLabel(QString("%1\n%2").arg(title, value), parent);
    label->setObjectName("transferMetric");
    label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    return label;
}

QToolButton* MainWindow::make_task_tool_button(const QIcon& icon,
                                               const QString& tooltip,
                                               QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setIcon(icon);
    button->setIconSize(QSize(12, 12));
    button->setToolTip(tooltip);
    button->setFixedSize(24, 24);
    return button;
}

QString MainWindow::transfer_rate_text(const TransferSnapshot& snapshot) const {
    return to_qstring(format_rate(snapshot.current_bytes, snapshot.elapsed_seconds));
}

QString MainWindow::transfer_size_text(const TransferSnapshot& snapshot) const {
    if (snapshot.kind == TransferKind::directory && snapshot.total_files > 0) {
        return QString("%1/%2").arg(snapshot.processed_files).arg(snapshot.total_files);
    }
    if (snapshot.kind == TransferKind::directory && snapshot.processed_files > 0) {
        return QCoreApplication::translate("MainWindow", "%1 file(s)")
            .arg(snapshot.processed_files);
    }
    if (snapshot.total_bytes == 0) {
        return to_qstring(format_size(snapshot.current_bytes));
    }
    return to_qstring(format_size(snapshot.total_bytes));
}

QString MainWindow::transfer_detail_text(const TransferSnapshot& snapshot) const {
    if (snapshot.state == TransferState::pending && snapshot.direction == TransferDirection::send) {
        return QCoreApplication::translate("MainWindow", "Queued. Resume is enabled if a partial file exists.");
    }
    if ((snapshot.state == TransferState::failed || snapshot.state == TransferState::cancelled) &&
        snapshot.direction == TransferDirection::send) {
        return can_resume_transfer(snapshot)
                   ? QCoreApplication::translate("MainWindow", "Can continue with resume.")
                   : QCoreApplication::translate("MainWindow", "Cannot continue. Check the source file or linked machine.");
    }
    if (snapshot.completion_status == TransferCompletionStatus::skipped) {
        return QCoreApplication::translate("MainWindow", "Skipped because target already matches.");
    }
    if (snapshot.resumed_from > 0) {
        return QCoreApplication::translate("MainWindow", "Resumed from %1")
            .arg(to_qstring(format_size(snapshot.resumed_from)));
    }
    if (snapshot.kind == TransferKind::directory &&
        (snapshot.skipped_files > 0 || snapshot.delta_files > 0 || snapshot.full_files > 0)) {
        return QCoreApplication::translate("MainWindow", "Skipped %1, Delta %2, Full %3, Payload %4")
            .arg(snapshot.skipped_files)
            .arg(snapshot.delta_files)
            .arg(snapshot.full_files)
            .arg(to_qstring(format_size(snapshot.payload_bytes)));
    }
    if (snapshot.error.has_value()) {
        return to_qstring(format_error(*snapshot.error));
    }
    if (snapshot.direction == TransferDirection::send) {
        return QCoreApplication::translate("MainWindow", "Resume is enabled for interrupted transfers.");
    }
    return QCoreApplication::translate("MainWindow", "Saved in the receiving folder.");
}

bool MainWindow::can_stop_transfer(const TransferSnapshot& snapshot) const {
    return snapshot.state == TransferState::running;
}

bool MainWindow::can_clear_transfer(const TransferSnapshot& snapshot) const {
    return snapshot.state == TransferState::pending ||
           snapshot.state == TransferState::completed ||
           snapshot.state == TransferState::failed ||
           snapshot.state == TransferState::cancelled;
}

bool MainWindow::can_open_transfer_dir(const TransferSnapshot& snapshot) const {
    return snapshot.direction == TransferDirection::receive &&
           !transfer_open_dir(snapshot).isEmpty();
}

bool MainWindow::can_resume_transfer(const TransferSnapshot& snapshot) const {
    if (snapshot.direction != TransferDirection::send ||
        (snapshot.state != TransferState::failed && snapshot.state != TransferState::cancelled) ||
        !has_active_peer() ||
        snapshot.path.empty()) {
        return false;
    }

    std::error_code ec;
    return std::filesystem::exists(snapshot.path, ec);
}

QString MainWindow::transfer_open_dir(const TransferSnapshot& snapshot) const {
    if (snapshot.path.empty()) {
        return {};
    }

    std::filesystem::path dir = snapshot.path;
    if (snapshot.kind == TransferKind::file && dir.has_parent_path()) {
        dir = dir.parent_path();
    }
    return to_qstring(dir);
}

void MainWindow::open_transfer_dir(const QString& key) {
    const auto it = transfer_snapshots_.find(key);
    if (it == transfer_snapshots_.end() || !can_open_transfer_dir(it.value())) {
        show_log(QCoreApplication::translate("MainWindow", "No local receive folder for this transfer."));
        return;
    }

    const auto dir = transfer_open_dir(it.value());
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        show_log(QCoreApplication::translate("MainWindow", "Failed to open folder: %1").arg(dir));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Opened folder: %1").arg(dir));
}

void MainWindow::show_receive_history() {
    auto settings = app_settings();
    const auto history = settings.value(QStringLiteral("receiveHistory")).toList();

    QDialog dialog(this);
    dialog.setWindowTitle(QCoreApplication::translate("MainWindow", "Receive history"));
    dialog.resize(380, 420);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* list = new QListWidget(&dialog);
    list->setFrameShape(QFrame::NoFrame);
    list->setWordWrap(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    if (history.isEmpty()) {
        auto* item = new QListWidgetItem(QCoreApplication::translate("MainWindow", "No receive history yet."));
        item->setFlags(Qt::NoItemFlags);
        list->addItem(item);
    } else {
        for (const auto& value : history) {
            const auto map = value.toMap();
            const auto finished_at = QDateTime::fromMSecsSinceEpoch(map.value(QStringLiteral("finishedAt")).toLongLong());
            const auto state = state_text(static_cast<TransferState>(map.value(QStringLiteral("state")).toInt()));
            const auto kind = kind_text(static_cast<TransferKind>(map.value(QStringLiteral("kind")).toInt()));
            const auto name = map.value(QStringLiteral("name")).toString();
            const auto path = map.value(QStringLiteral("path")).toString();
            const auto open_dir = map.value(QStringLiteral("openDir")).toString();
            const auto bytes = map.value(QStringLiteral("bytes")).toULongLong();
            const auto files = map.value(QStringLiteral("files")).toULongLong();
            const auto error = map.value(QStringLiteral("error")).toString();
            const auto amount = files > 0
                                    ? QCoreApplication::translate("MainWindow", "%1 file(s)").arg(files)
                                    : to_qstring(format_size(bytes));
            auto text = QStringLiteral("%1\n%2, %3, %4, %5\n%6")
                            .arg(name,
                                 state,
                                 kind,
                                 amount,
                                 finished_at.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                 path);
            if (!error.isEmpty()) {
                text += QStringLiteral("\n%1").arg(error);
            }
            auto* item = new QListWidgetItem(text);
            item->setData(Qt::UserRole, open_dir);
            item->setSizeHint(QSize(0, error.isEmpty() ? 64 : 82));
            list->addItem(item);
        }
    }

    auto* buttons = new QHBoxLayout();
    auto* open = new QPushButton(QCoreApplication::translate("MainWindow", "Open folder"), &dialog);
    open->setObjectName("secondaryButton");
    auto* clear = new QPushButton(QCoreApplication::translate("MainWindow", "Clear history"), &dialog);
    clear->setObjectName("secondaryButton");
    auto* close = new QPushButton(QCoreApplication::translate("MainWindow", "Close"), &dialog);
    close->setObjectName("primaryButton");
    open->setEnabled(!history.isEmpty());
    clear->setEnabled(!history.isEmpty());
    buttons->addWidget(open);
    buttons->addWidget(clear);
    buttons->addStretch(1);
    buttons->addWidget(close);

    layout->addWidget(list, 1);
    layout->addLayout(buttons);

    const auto open_selected = [this, list] {
        auto* item = list->currentItem();
        if (item == nullptr) {
            return;
        }
        const auto dir = item->data(Qt::UserRole).toString();
        if (dir.isEmpty()) {
            show_log(QCoreApplication::translate("MainWindow", "No local receive folder for this history item."));
            return;
        }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
            show_log(QCoreApplication::translate("MainWindow", "Failed to open folder: %1").arg(dir));
        }
    };

    connect(open, &QPushButton::clicked, this, open_selected);
    connect(list, &QListWidget::itemDoubleClicked, this, open_selected);
    connect(clear, &QPushButton::clicked, this, [&dialog, this] {
        auto settings = app_settings();
        settings.remove(QStringLiteral("receiveHistory"));
        recorded_history_keys_.clear();
        log_event(QCoreApplication::translate("MainWindow", "Receive history cleared."));
        dialog.accept();
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}

void MainWindow::record_receive_history(const TransferSnapshot& snapshot) {
    if (snapshot.direction != TransferDirection::receive || !is_terminal_state(snapshot.state)) {
        return;
    }

    const auto key = snapshot_key(snapshot);
    if (recorded_history_keys_.contains(key)) {
        return;
    }
    recorded_history_keys_.insert(key);

    QVariantMap entry;
    entry.insert(QStringLiteral("finishedAt"), QDateTime::currentMSecsSinceEpoch());
    entry.insert(QStringLiteral("state"), static_cast<int>(snapshot.state));
    entry.insert(QStringLiteral("kind"), static_cast<int>(snapshot.kind));
    entry.insert(QStringLiteral("name"), QString::fromStdString(snapshot.name));
    entry.insert(QStringLiteral("path"), to_qstring(snapshot.path));
    entry.insert(QStringLiteral("openDir"), transfer_open_dir(snapshot));
    entry.insert(QStringLiteral("bytes"), QVariant::fromValue<qulonglong>(snapshot.total_bytes));
    entry.insert(QStringLiteral("files"), QVariant::fromValue<qulonglong>(
                                           snapshot.total_files > 0 ? snapshot.total_files : snapshot.processed_files));
    if (snapshot.error.has_value()) {
        entry.insert(QStringLiteral("error"), to_qstring(format_error(*snapshot.error)));
    }

    auto settings = app_settings();
    auto history = settings.value(QStringLiteral("receiveHistory")).toList();
    history.prepend(entry);
    while (history.size() > kMaxReceiveHistory) {
        history.removeLast();
    }
    settings.setValue(QStringLiteral("receiveHistory"), history);
}

void MainWindow::resume_transfer(const QString& key) {
    const auto it = transfer_snapshots_.find(key);
    if (it == transfer_snapshots_.end()) {
        return;
    }
    const auto snapshot = it.value();
    if (!can_resume_transfer(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer cannot be resumed."));
        return;
    }

    const auto path = to_qstring(snapshot.path);
    const auto peer_id = transfer_peer_ids_.value(key, active_peer_id_);
    if (peer_id.isEmpty()) {
        show_log(QCoreApplication::translate("MainWindow", "No linked machine for this transfer."));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Resuming transfer: %1").arg(path));
    remove_transfer_snapshot(key);
    if (scheduler_ != nullptr) {
        scheduler_->enqueue_send(to_string(peer_id), std::filesystem::path(to_string(path)));
    }
}

void MainWindow::stop_transfer(const QString& key) {
    const auto it = transfer_snapshots_.find(key);
    if (it == transfer_snapshots_.end()) {
        return;
    }
    if (!can_stop_transfer(it.value())) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer is not running."));
        return;
    }
    if (it.value().direction == TransferDirection::receive) {
        if (receiver_runner_ == nullptr) {
            show_log(QCoreApplication::translate("MainWindow", "No active receiver for this transfer."));
            return;
        }
        show_log(QCoreApplication::translate("MainWindow", "Stopping receive transfer..."));
        stop_receiver();
        return;
    }
    if (scheduler_ != nullptr) {
        scheduler_->cancel_task(it.value().transfer_id);
    }
    show_log(QCoreApplication::translate("MainWindow", "Stopping transfer..."));
}

void MainWindow::remove_transfer_card(const QString& key) {
    const auto it = transfer_snapshots_.find(key);
    if (it != transfer_snapshots_.end()) {
        if (it.value().state == TransferState::pending &&
            it.value().direction == TransferDirection::send) {
            dismissed_transfer_keys_.insert(key);
            if (scheduler_ != nullptr) {
                scheduler_->cancel_task(it.value().transfer_id);
            }
        } else if (!can_clear_transfer(it.value())) {
            show_log(QCoreApplication::translate("MainWindow", "Stop the transfer before clearing it from the list."));
            return;
        }
    }

    remove_transfer_snapshot(key);
}

void MainWindow::remove_transfer_snapshot(const QString& key) {
    transfer_snapshots_.remove(key);
    transfer_peer_ids_.remove(key);
    auto* card = transfer_cards_.take(key);
    if (card != nullptr) {
        transfers_layout_->removeWidget(card);
        card->deleteLater();
    }
    if (transfer_cards_.isEmpty() && empty_transfer_label_ != nullptr) {
        empty_transfer_label_->show();
    }
}

bool MainWindow::transfer_belongs_to_active_peer(const QString& key) const {
    if (!transfer_peer_ids_.contains(key)) {
        return true;
    }
    if (!has_active_peer()) {
        return false;
    }
    return transfer_peer_ids_.value(key) == active_peer_id_;
}

void MainWindow::clear_transfer_cards() {
    for (auto* card : transfer_cards_) {
        transfers_layout_->removeWidget(card);
        card->deleteLater();
    }
    transfer_cards_.clear();
}

void MainWindow::refresh_transfer_list() {
    if (transfers_layout_ == nullptr) {
        return;
    }
    clear_transfer_cards();
    for (auto it = transfer_snapshots_.cbegin(); it != transfer_snapshots_.cend(); ++it) {
        if (!transfer_belongs_to_active_peer(it.key())) {
            continue;
        }
        auto* card = make_transfer_card(it.value());
        transfer_cards_.insert(it.key(), card);
        transfers_layout_->insertWidget(0, card);
    }
    if (empty_transfer_label_ != nullptr) {
        empty_transfer_label_->setVisible(transfer_cards_.isEmpty());
    }
}

void MainWindow::request_link(const QString& id) {
    if (!peers_.contains(id)) {
        return;
    }
    const auto peer = peers_.value(id);
    if (is_linked_peer(peer)) {
        set_active_peer(id);
        open_transfer_page();
        return;
    }
    if (!peer.online) {
        show_log(QCoreApplication::translate("MainWindow", "This machine is offline. Refresh and try again."));
        return;
    }
    const auto code = make_link_code();
    pending_link_codes_.insert(id, code);
    status_->setText(QCoreApplication::translate("MainWindow", "Waiting for %1 to accept code %2...").arg(peer.name, code));
    log_event(QCoreApplication::translate("MainWindow", "Sending link request to %1 %2:%3 code=%4")
                  .arg(peer.name, peer.host)
                  .arg(peer.port)
                  .arg(code));
    send_control(peer, "link_request", QJsonObject{{"code", code}});
}

void MainWindow::receive_link_request(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    add_peer(address, obj);
    const auto id = obj.value("id").toString();
    if (!peers_.contains(id)) {
        return;
    }
    const auto peer = peers_.value(id);
    const auto code = obj.value("code").toString();
    log_event(QCoreApplication::translate("MainWindow", "Link request from %1 %2:%3 code=%4")
                  .arg(peer.name, peer.host)
                  .arg(peer.port)
                  .arg(code));
    const auto answer = QMessageBox::question(
        this,
        QCoreApplication::translate("MainWindow", "Link request"),
        QCoreApplication::translate("MainWindow", "%1 wants to link with this machine.\nCode: %2").arg(peer.name, code),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        log_event(QCoreApplication::translate("MainWindow", "Accepted link request from %1").arg(peer.name));
        send_control(peer, "link_accept", QJsonObject{{"code", code}});
        set_linked_peer(peer, !has_active_peer());
    } else {
        log_event(QCoreApplication::translate("MainWindow", "Rejected link request from %1").arg(peer.name));
        send_control(peer, "link_reject", QJsonObject{{"code", code}});
    }
}

void MainWindow::receive_link_response(const QHostAddress& address, const QJsonObject& obj, bool accepted) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    add_peer(address, obj);
    const auto id = obj.value("id").toString();
    const auto code = obj.value("code").toString();
    if (pending_link_codes_.value(id) != code) {
        return;
    }
    pending_link_codes_.remove(id);
    if (!accepted) {
        status_->setText(QCoreApplication::translate("MainWindow", "Link request rejected."));
        log_event(QCoreApplication::translate("MainWindow", "Link request rejected by peer."));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Link accepted by %1").arg(peers_.value(id).name));
    set_linked_peer(peers_.value(id), true);
}

void MainWindow::receive_link_disconnect(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    add_peer(address, obj);
    const auto id = obj.value("id").toString();
    if (!peers_.contains(id) || !peers_.value(id).linked) {
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "%1 disconnected.").arg(peers_.value(id).name));
    disconnect_peer(id, false, false);
}

void MainWindow::send_control(const Peer& peer, const QString& type, const QJsonObject& fields) {
    QJsonObject message{
        {"protocol", kProtocol},
        {"type", type},
        {"id", node_id_},
        {"name", machine_name()},
        {"port", static_cast<int>(kTransferPort)},
    };
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        message.insert(it.key(), it.value());
    }
    const auto payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    discovery_->writeDatagram(payload, QHostAddress(peer.host), kDiscoveryPort);
    log_event(QCoreApplication::translate("MainWindow", "Sent control '%1' to %2:%3").arg(type, peer.host).arg(kDiscoveryPort));
}

void MainWindow::link_peer(QListWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    request_link(item->data(Qt::UserRole).toString());
}

void MainWindow::open_transfer_page() {
    if (!has_active_peer()) {
        show_log(QCoreApplication::translate("MainWindow", "No linked machine."));
        return;
    }
    stack_->setCurrentWidget(transfer_page_);
}

void MainWindow::set_linked_peer(const Peer& peer, bool activate) {
    auto linked = peers_.value(peer.id, peer);
    linked.linked = true;
    linked.online = true;
    linked.last_linked_ms = now_ms();
    peers_.insert(linked.id, linked);
    sync_scheduler_peer(linked);
    remember_peer(linked);
    if (activate || !has_active_peer()) {
        set_active_peer(linked.id);
    } else {
        update_linked_header();
    }
    status_->setText(QCoreApplication::translate("MainWindow", "Linked to %1.").arg(linked.name));
    log_event(QCoreApplication::translate("MainWindow", "Linked peer: %1 %2:%3").arg(linked.name, linked.host).arg(linked.port));
    refresh_peer_list();
    if (activate) {
        stack_->setCurrentWidget(transfer_page_);
    }
}

void MainWindow::set_active_peer(const QString& id) {
    if (!peers_.contains(id) || !peers_.value(id).linked) {
        return;
    }
    active_peer_id_ = id;
    update_linked_header();
    refresh_transfer_list();
    refresh_peer_list();
}

void MainWindow::disconnect_peer(bool notify_peer, bool confirm_active_sends) {
    if (!has_active_peer()) {
        stack_->setCurrentWidget(peer_page_);
        return;
    }
    disconnect_peer(active_peer_id_, notify_peer, confirm_active_sends);
}

void MainWindow::disconnect_peer(const QString& id, bool notify_peer, bool confirm_active_sends) {
    if (!peers_.contains(id) || !peers_.value(id).linked) {
        return;
    }
    const auto has_sends_for_peer = scheduler_ != nullptr &&
                                    scheduler_->has_pending_or_running_for_peer(to_string(id));
    if (has_sends_for_peer) {
        if (confirm_active_sends) {
            const auto answer = QMessageBox::question(
                this,
                QCoreApplication::translate("MainWindow", "Disconnect machine"),
                QCoreApplication::translate("MainWindow", "There are active or queued sends for this machine. Stop and disconnect?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
        if (scheduler_ != nullptr) {
            scheduler_->cancel_peer(to_string(id));
        }
    }

    auto peer = peers_.value(id);
    if (notify_peer && !peer.id.isEmpty() && peer.online) {
        send_control(peer, "link_disconnect");
    }
    const auto name = peer.name;
    peer.linked = false;
    peers_.insert(id, peer);
    sync_scheduler_peer(peer);
    pending_link_codes_.remove(id);
    if (active_peer_id_ == id) {
        active_peer_id_.clear();
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
            if (it.value().linked) {
                active_peer_id_ = it.key();
                break;
            }
        }
    }
    update_linked_header();
    status_->setText(QCoreApplication::translate("MainWindow", "Disconnected."));
    log_event(QCoreApplication::translate("MainWindow", "Disconnected from %1").arg(name));
    refresh_peer_list();
    if (!has_active_peer()) {
        stack_->setCurrentWidget(peer_page_);
    }
}

bool MainWindow::is_linked_peer(const Peer& peer) const {
    return peer.linked;
}

bool MainWindow::has_active_peer() const {
    return peers_.contains(active_peer_id_) && peers_.value(active_peer_id_).linked;
}

Peer MainWindow::active_peer() const {
    return peers_.value(active_peer_id_);
}

int MainWindow::linked_peer_count() const {
    int count = 0;
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        if (it.value().linked) {
            ++count;
        }
    }
    return count;
}

void MainWindow::update_linked_header() {
    if (linked_label_ == nullptr) {
        return;
    }
    if (!has_active_peer()) {
        linked_label_->setText(QCoreApplication::translate("MainWindow", "Not linked"));
        if (back_to_transfer_ != nullptr) {
            back_to_transfer_->setVisible(false);
        }
        return;
    }
    const auto peer = active_peer();
    const auto count = linked_peer_count();
    linked_label_->setText(QCoreApplication::translate("MainWindow", "Send to %1 (%2 linked)").arg(peer.name).arg(count));
    if (back_to_transfer_ != nullptr) {
        back_to_transfer_->setVisible(true);
    }
}

void MainWindow::send_paths(const QStringList& paths) {
    if (!has_active_peer()) {
        show_log(QCoreApplication::translate("MainWindow", "No linked machine."));
        return;
    }
    if (scheduler_ == nullptr) {
        show_log(QCoreApplication::translate("MainWindow", "Transfer scheduler is not ready."));
        return;
    }
    const auto peer = active_peer();
    log_event(QCoreApplication::translate("MainWindow", "Queued %1 path(s) for sending.").arg(paths.size()));
    for (const auto& path : paths) {
        scheduler_->enqueue_send(to_string(peer.id), std::filesystem::path(to_string(path)));
    }
}

void MainWindow::paste_paths_from_clipboard() {
    const auto* mime = QApplication::clipboard()->mimeData();
    auto paths = local_file_paths_from_mime(mime);
    if (paths.empty()) {
        const auto image = image_from_mime(mime);
        if (!image.isNull()) {
            QDir dir(clipboard_image_dir());
            if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
                show_log(QCoreApplication::translate("MainWindow", "Failed to create clipboard image folder: %1").arg(dir.path()));
                return;
            }

            const auto file_name = QStringLiteral("clipboard-image-%1.png")
                                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
            const auto path = dir.filePath(file_name);
            if (!image.save(path, "PNG")) {
                show_log(QCoreApplication::translate("MainWindow", "Failed to save clipboard image: %1").arg(path));
                return;
            }
            paths.push_back(path);
            log_event(QCoreApplication::translate("MainWindow", "Saved clipboard image: %1").arg(path));
        }
    }
    if (paths.empty()) {
        show_log(QCoreApplication::translate("MainWindow", "Clipboard does not contain local files, folders, or images."));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Pasted %1 path(s) from clipboard.").arg(paths.size()));
    send_paths(paths);
}

void MainWindow::stop_sender() {
    if (scheduler_ != nullptr) {
        scheduler_->stop_all();
    }
}

void MainWindow::sync_scheduler_peer(const Peer& peer) {
    if (scheduler_ == nullptr || peer.id.isEmpty()) {
        return;
    }
    scheduler_->upsert_peer(SchedulerPeer{
        .id = to_string(peer.id),
        .name = to_string(peer.name),
        .host = to_string(peer.host),
        .port = peer.port,
        .online = peer.online,
        .linked = peer.linked,
    });
}

void MainWindow::handle_scheduler_snapshot(SchedulerSnapshot snapshot) {
    const auto key = snapshot_key(snapshot.snapshot);
    if (dismissed_transfer_keys_.contains(key)) {
        if (snapshot.snapshot.state != TransferState::pending) {
            transfer_snapshots_.remove(key);
            transfer_peer_ids_.remove(key);
        }
        return;
    }
    const auto peer_id = to_qstring(snapshot.peer_id);
    upsert_snapshot(snapshot.snapshot, peer_id);
}

void MainWindow::handle_scheduler_log(std::string line) {
    show_log(to_qstring(std::move(line)));
}

void MainWindow::wake_scheduler() {
    if (scheduler_ != nullptr) {
        scheduler_->pump();
    }
}

void MainWindow::merge_snapshots(TransferSnapshotStore store, const QString& peer_id) {
    QMetaObject::invokeMethod(this, [this, store = std::move(store), peer_id] {
        for (const auto& snapshot : store.snapshots()) {
            upsert_snapshot(snapshot, peer_id);
        }
    }, Qt::QueuedConnection);
}

void MainWindow::upsert_snapshot(const TransferSnapshot& snapshot, const QString& peer_id) {
    record_receive_history(snapshot);
    const auto key = snapshot_key(snapshot);
    dismissed_transfer_keys_.remove(key);
    if (snapshot.direction == TransferDirection::send &&
        !peer_id.isEmpty() &&
        !transfer_peer_ids_.contains(key)) {
        transfer_peer_ids_.insert(key, peer_id);
    }
    transfer_snapshots_.insert(key, snapshot);
    if (!transfer_belongs_to_active_peer(key)) {
        return;
    }
    if (empty_transfer_label_ != nullptr) {
        empty_transfer_label_->hide();
    }
    if (transfer_cards_.contains(key)) {
        auto* old = transfer_cards_.take(key);
        transfers_layout_->removeWidget(old);
        old->deleteLater();
    }
    auto* card = make_transfer_card(snapshot);
    transfer_cards_.insert(key, card);
    transfers_layout_->insertWidget(0, card);
}

void MainWindow::show_log(QString text) {
    QMetaObject::invokeMethod(this, [this, text = std::move(text)] {
        log_event(text);
    }, Qt::QueuedConnection);
}

void MainWindow::log_event(QString text) {
    if (log_ == nullptr) {
        return;
    }
    const auto line = QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), text);
    if (QThread::currentThread() == thread()) {
        log_->appendPlainText(line);
        auto* bar = log_->verticalScrollBar();
        bar->setValue(bar->maximum());
        return;
    }
    QMetaObject::invokeMethod(this, [this, line] {
        log_->appendPlainText(line);
        auto* bar = log_->verticalScrollBar();
        bar->setValue(bar->maximum());
    }, Qt::QueuedConnection);
}

}  // namespace lan::gui

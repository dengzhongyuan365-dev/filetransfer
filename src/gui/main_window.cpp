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
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QShortcut>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QTime>
#include <QToolButton>
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
#include "gui/receive_history_dialog.h"
#include "gui/settings_dialog.h"
#include "gui/target_dialogs.h"
#include "gui/transfer_card.h"
#include "gui/transfer_events.h"
#include "lan/app/receiver_config.h"
#include "lan/common/error.h"
#include "lan/common/size.h"

namespace lan::gui {
namespace {

constexpr int kMaxRememberedPeers = 10;
constexpr int kMaxReceiveHistory = 100;
constexpr int kPresenceProbeMs = 5000;
constexpr qint64 kPeerStaleMs = 15000;
constexpr const char* kClipboardImagePrefix = "lan-clipboard-image-";
constexpr const char* kLegacyClipboardImagePrefix = "clipboard-image-";

QString state_text(TransferState state) {
    switch (state) {
        case TransferState::pending:
            return QCoreApplication::translate("MainWindow", "pending");
        case TransferState::paused:
            return QCoreApplication::translate("MainWindow", "paused");
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

QString display_peer_name(const Peer& peer) {
    const auto alias = peer.alias.trimmed();
    return alias.isEmpty() ? peer.name : alias;
}

QString peer_detail_text(const Peer& peer) {
    auto detail = QString("%1:%2").arg(peer.host).arg(peer.port);
    if (!peer.alias.trimmed().isEmpty() && !peer.name.isEmpty()) {
        detail = QStringLiteral("%1 - %2").arg(peer.name, detail);
    }
    return detail;
}

QString timestamp_text(qint64 ms) {
    if (ms <= 0) {
        return QCoreApplication::translate("MainWindow", "Never");
    }
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
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

QString make_trust_token() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

qint64 now_ms() {
    return QDateTime::currentMSecsSinceEpoch();
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

QString saved_discovery_networks() {
    return app_settings().value(QStringLiteral("discoveryNetworks")).toString();
}

void save_discovery_networks(const QString& networks) {
    auto settings = app_settings();
    const auto trimmed = networks.trimmed();
    if (trimmed.isEmpty()) {
        settings.remove(QStringLiteral("discoveryNetworks"));
        return;
    }
    settings.setValue(QStringLiteral("discoveryNetworks"), trimmed);
}

QStringList discovery_network_entries(const QString& networks) {
    QStringList entries;
    for (const auto& entry : networks.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
        const auto trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) {
            entries.push_back(trimmed);
        }
    }
    return entries;
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

bool is_clipboard_image_transfer(const TransferSnapshot& snapshot) {
    if (snapshot.direction != TransferDirection::receive ||
        snapshot.kind != TransferKind::file ||
        snapshot.state != TransferState::completed ||
        snapshot.path.empty()) {
        return false;
    }

    if (snapshot.source == FileTransferSource::clipboard_image) {
        return true;
    }

    const auto name = QString::fromStdString(snapshot.name.empty()
                                                ? snapshot.path.filename().string()
                                                : snapshot.name);
    if (!name.startsWith(QString::fromUtf8(kClipboardImagePrefix)) &&
        !name.startsWith(QString::fromUtf8(kLegacyClipboardImagePrefix))) {
        return false;
    }

    const auto suffix = QFileInfo(name).suffix().toLower();
    return suffix == QStringLiteral("png") ||
           suffix == QStringLiteral("jpg") ||
           suffix == QStringLiteral("jpeg") ||
           suffix == QStringLiteral("bmp") ||
           suffix == QStringLiteral("webp");
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

void MainWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        apply_style();
    }
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
    linked_count_label_ = new QLabel(QCoreApplication::translate("MainWindow", "Not linked"), peer_page_);
    linked_count_label_->setObjectName("linkedCountBadge");
    linked_count_label_->setAlignment(Qt::AlignCenter);
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
    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    footer->addWidget(status_, 1);
    footer->addWidget(linked_count_label_, 0, Qt::AlignRight);

    layout->addLayout(header);
    layout->addWidget(peer_filter_);
    layout->addWidget(peer_list_, 1);
    layout->addLayout(footer);

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
    layout->setContentsMargins(20, 20, 20, 18);
    layout->setSpacing(12);

    linked_label_ = new ElidedLabel(QCoreApplication::translate("MainWindow", "Not linked"), transfer_page_);
    linked_label_->setObjectName("title");
    auto* back = new QPushButton(QCoreApplication::translate("MainWindow", "Change"), transfer_page_);
    back->setObjectName("secondaryButton");
    back->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* disconnect = new QPushButton(QCoreApplication::translate("MainWindow", "Disconnect"), transfer_page_);
    disconnect->setObjectName("secondaryButton");
    disconnect->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* header = new QHBoxLayout();
    header->setSpacing(8);
    header->addWidget(linked_label_, 1);
    header->addWidget(disconnect, 0, Qt::AlignRight);
    header->addWidget(back, 0, Qt::AlignRight);

    auto* transfer_frame = new QFrame(transfer_page_);
    transfer_frame->setObjectName("transferPanel");
    transfer_frame->setMinimumHeight(380);
    auto* transfer_frame_layout = new QVBoxLayout(transfer_frame);
    transfer_frame_layout->setContentsMargins(8, 8, 8, 8);
    transfer_frame_layout->setSpacing(0);

    drop_panel_ = new DropPanel(transfer_frame);
    auto* drop_layout = new QVBoxLayout(drop_panel_);
    drop_layout->setContentsMargins(0, 0, 0, 0);
    drop_layout->setSpacing(0);

    auto* scroll = new QScrollArea(drop_panel_);
    scroll->setObjectName("transferScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* transfer_list = new QWidget(scroll);
    transfer_list->setObjectName("transferListSurface");
    transfers_layout_ = new QVBoxLayout(transfer_list);
    transfers_layout_->setContentsMargins(0, 0, 0, 0);
    transfers_layout_->setSpacing(8);

    empty_transfer_label_ = new QLabel(QCoreApplication::translate("MainWindow", "Drop or paste files and folders here\nSend to selected machine(s)"), transfer_list);
    empty_transfer_label_->setObjectName("emptyTransfer");
    empty_transfer_label_->setAlignment(Qt::AlignCenter);
    empty_transfer_label_->setMinimumHeight(320);
    transfers_layout_->addWidget(empty_transfer_label_);
    transfers_layout_->addStretch(1);
    scroll->setWidget(transfer_list);
    drop_layout->addWidget(scroll);
    transfer_frame_layout->addWidget(drop_panel_);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    target_button_ = new QPushButton(QCoreApplication::translate("MainWindow", "Targets"), transfer_page_);
    target_button_->setObjectName("secondaryButton");
    auto* history = new QPushButton(QCoreApplication::translate("MainWindow", "History"), transfer_page_);
    history->setObjectName("secondaryButton");
    footer->addWidget(target_button_);
    footer->addStretch(1);
    footer->addWidget(history);

    layout->addLayout(header);
    layout->addWidget(transfer_frame, 1);
    layout->addLayout(footer);

    connect(back, &QPushButton::clicked, this, [this] {
        stack_->setCurrentWidget(peer_page_);
    });
    connect(history, &QPushButton::clicked, this, [this] {
        show_receive_history();
    });
    connect(target_button_, &QPushButton::clicked, this, [this] {
        show_send_targets();
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
    if (applying_style_) {
        return;
    }
    applying_style_ = true;

    const auto window_lightness = QApplication::palette().color(QPalette::Window).lightness();
    const auto dark = window_lightness < 128;
    QString style = R"(
        #app {
            background: #f6f7fb;
            color: #1e2430;
            font-family: "Noto Sans", "Inter", sans-serif;
            font-size: 14px;
        }
        QDialog {
            background: #f6f7fb;
            color: #1e2430;
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
        #transferPanel {
            background: #ffffff;
            border: 1px solid #dfe5ee;
            border-radius: 8px;
        }
        #transferDropArea {
            border: none;
            background: transparent;
        }
        #searchInput {
            min-height: 38px;
            padding: 0 12px;
            border: 1px solid #d9dee8;
            border-radius: 8px;
            background: #ffffff;
            color: #1e2430;
        }
        #peerList, #transferScroll, #transferListSurface {
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
        #peerMeta, #transferMeta, #transferMetric, #transferProgressText {
            color: #717b8b;
            font-size: 12px;
        }
        #transferDetail {
            color: #5f6978;
            font-size: 12px;
        }
        #transferProgress {
            border: none;
            border-radius: 3px;
            background: #e9eef5;
        }
        #transferProgress::chunk {
            border-radius: 3px;
            background: #2563eb;
        }
        #transferMetric {
            line-height: 16px;
        }
        #onlineBadge, #linkedBadge, #trustedBadge, #offlineBadge, #stateBadge {
            color: #087443;
            background: #e7f8ef;
            border-radius: 7px;
            padding: 3px 4px;
            font-size: 12px;
        }
        #stateBadge[transferState="running"] {
            color: #1d4ed8;
            background: #eaf1ff;
        }
        #stateBadge[transferState="completed"] {
            color: #087443;
            background: #e7f8ef;
        }
        #stateBadge[transferState="pending"] {
            color: #7c4a03;
            background: #fff4d6;
        }
        #stateBadge[transferState="paused"] {
            color: #9a3412;
            background: #ffedd5;
        }
        #stateBadge[transferState="failed"] {
            color: #b42318;
            background: #fee4e2;
        }
        #stateBadge[transferState="cancelled"] {
            color: #475467;
            background: #eef2f7;
        }
        #linkedCountBadge {
            color: #1d4ed8;
            background: #eaf1ff;
            border-radius: 7px;
            padding: 3px 7px;
            font-size: 12px;
        }
        #linkedBadge {
            color: #1d4ed8;
            background: #eaf1ff;
            border: none;
            min-height: 0px;
        }
        #trustedBadge {
            color: #0f766e;
            background: #e6f7f4;
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
    )";

    if (dark) {
        style += R"(
            #app {
                background: #171a21;
                color: #e7eaf0;
            }
            QDialog {
                background: #171a21;
                color: #e7eaf0;
            }
            #title {
                color: #f1f4f8;
            }
            #dialogTitle {
                color: #f1f4f8;
            }
            #mutedText {
                color: #9aa4b2;
            }
            #setupCard, #transferPanel, #peerCard, #transferCard {
                background: #222733;
                border-color: #343b4a;
            }
            #pathBox {
                background: #1c2029;
                border-color: #343b4a;
            }
            #pathLabel {
                color: #d8dde6;
            }
            #transferDropArea {
                background: transparent;
            }
            #searchInput {
                background: #1c2029;
                border-color: #343b4a;
                color: #e7eaf0;
            }
            #peerName, #transferName {
                color: #f1f4f8;
            }
            #peerMeta, #transferMeta, #transferMetric, #transferProgressText {
                color: #9aa4b2;
            }
            #transferDetail {
                color: #aeb7c5;
            }
            #transferProgress {
                background: #303746;
            }
            #transferProgress::chunk {
                background: #60a5fa;
            }
            #onlineBadge {
                color: #8ff0b7;
                background: #173625;
            }
            #linkedBadge {
                color: #9dc2ff;
                background: #172a4c;
            }
            #trustedBadge {
                color: #88e7dc;
                background: #143834;
            }
            #linkedCountBadge {
                color: #9dc2ff;
                background: #172a4c;
            }
            #offlineBadge {
                color: #a4adba;
                background: #2a303d;
            }
            #stateBadge {
                color: #8ff0b7;
                background: #173625;
            }
            #stateBadge[transferState="running"] {
                color: #9dc2ff;
                background: #172a4c;
            }
            #stateBadge[transferState="completed"] {
                color: #8ff0b7;
                background: #173625;
            }
            #stateBadge[transferState="pending"] {
                color: #ffd987;
                background: #3d2f12;
            }
            #stateBadge[transferState="paused"] {
                color: #fdba74;
                background: #3a2113;
            }
            #stateBadge[transferState="failed"] {
                color: #fda29b;
                background: #421b1b;
            }
            #stateBadge[transferState="cancelled"] {
                color: #c4ccd8;
                background: #2a303d;
            }
            #emptyTransfer {
                color: #8f98a8;
            }
            #logView {
                background: #1c2029;
                border-color: #343b4a;
                color: #c8d0dc;
            }
            QPushButton {
                background: #252b36;
                border-color: #3a4252;
                color: #e8edf5;
            }
            QPushButton:focus {
                border-color: #4a5468;
            }
            QPushButton:hover {
                background: #2d3542;
                border-color: #4a5468;
            }
            QPushButton:pressed {
                background: #202631;
                border-color: #59657a;
            }
            QPushButton:disabled {
                background: #202631;
                border-color: #303746;
                color: #6d7583;
            }
            #primaryButton, #linkButton {
                background: #3b82f6;
                border-color: #3b82f6;
                color: #ffffff;
            }
            #primaryButton:hover, #linkButton:hover {
                background: #60a5fa;
                border-color: #60a5fa;
            }
            #primaryButton:pressed, #linkButton:pressed {
                background: #2563eb;
                border-color: #2563eb;
            }
            #secondaryButton {
                background: #252b36;
                border-color: #3a4252;
                color: #e8edf5;
            }
            #secondaryButton:hover {
                background: #2d3542;
                border-color: #4a5468;
            }
            #secondaryButton:pressed {
                background: #202631;
                border-color: #59657a;
            }
            #taskResumeButton, #taskOpenButton, #taskStopButton, #taskRemoveButton {
                background: #2a303d;
                border-color: transparent;
            }
            #taskOpenButton {
                color: #8bb7ff;
            }
            #taskOpenButton:hover {
                background: #1d355f;
                color: #b6d2ff;
            }
            #taskResumeButton {
                color: #8ff0b7;
            }
            #taskResumeButton:hover {
                background: #173625;
                color: #b6f6cd;
            }
            #taskStopButton {
                color: #ffaaa5;
            }
            #taskStopButton:hover {
                background: #4a2022;
                color: #ffc3bf;
            }
            #taskRemoveButton {
                color: #b7c0ce;
            }
            #taskRemoveButton:hover {
                background: #343b4a;
                color: #f1f4f8;
            }
            #taskResumeButton:disabled, #taskOpenButton:disabled, #taskStopButton:disabled, #taskRemoveButton:disabled {
                color: #6d7583;
                background: #252b36;
            }
        )";
    }

    setStyleSheet(style);
    applying_style_ = false;
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
    scheduler_->set_sender_id(to_string(node_id_));
    for (const auto& peer : devices_.peers()) {
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
    discovery_ = std::make_unique<DiscoveryController>();
    const auto bound = discovery_->bind(this, [this](DiscoveryDatagram datagram) {
        handle_discovery_datagram(std::move(datagram));
    });
    if (bound) {
        log_event(QCoreApplication::translate("MainWindow", "UDP discovery listening on %1").arg(kDiscoveryPort));
    } else {
        log_event(QCoreApplication::translate("MainWindow", "UDP discovery bind failed on %1: %2")
                      .arg(kDiscoveryPort)
                      .arg(discovery_->error_string()));
    }

    auto* presence = new QTimer(this);
    connect(presence, &QTimer::timeout, this, [this] {
        refresh_peer_presence();
        if (receiver_ready_) {
            send_discovery_probe(false);
        }
    });
    presence->start(kPresenceProbeMs);
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
            .alias = settings.value(QStringLiteral("alias")).toString(),
            .host = settings.value(QStringLiteral("host")).toString(),
            .trust_token = settings.value(QStringLiteral("trustToken")).toString(),
            .port = static_cast<std::uint16_t>(settings.value(QStringLiteral("port"), kTransferPort).toUInt()),
            .online = false,
            .linked = false,
            .trusted = settings.value(QStringLiteral("trusted"), false).toBool(),
            .last_seen_ms = settings.value(QStringLiteral("lastSeen"), 0).toLongLong(),
            .last_linked_ms = settings.value(QStringLiteral("lastLinked"), 0).toLongLong(),
            .trusted_at_ms = settings.value(QStringLiteral("trustedAt"), 0).toLongLong(),
        };
        if (!peer.id.isEmpty() && !peer.host.isEmpty()) {
            devices_.insert_peer(peer);
        }
    }
    settings.endArray();
    refresh_peer_list();
}

QString MainWindow::find_peer_id_by_endpoint(const QString& host, std::uint16_t port) const {
    return devices_.find_peer_id_by_endpoint(host, port);
}

void MainWindow::remember_peer(const Peer& peer) {
    devices_.remember_peer(peer, now_ms());
    save_remembered_peers();
    refresh_peer_list();
}

void MainWindow::save_remembered_peers() {
    auto peers = devices_.peers();
    peers.erase(std::remove_if(peers.begin(), peers.end(), [](const Peer& peer) {
        return peer.last_linked_ms <= 0 && peer.trusted_at_ms <= 0 && peer.alias.trimmed().isEmpty();
    }), peers.end());
    std::sort(peers.begin(), peers.end(), [](const Peer& left, const Peer& right) {
        return std::max(left.last_linked_ms, left.trusted_at_ms) > std::max(right.last_linked_ms, right.trusted_at_ms);
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
        settings.setValue(QStringLiteral("alias"), peers.at(i).alias);
        settings.setValue(QStringLiteral("host"), peers.at(i).host);
        settings.setValue(QStringLiteral("trustToken"), peers.at(i).trust_token);
        settings.setValue(QStringLiteral("port"), static_cast<int>(peers.at(i).port));
        settings.setValue(QStringLiteral("trusted"), peers.at(i).trusted);
        settings.setValue(QStringLiteral("lastSeen"), peers.at(i).last_seen_ms);
        settings.setValue(QStringLiteral("lastLinked"), peers.at(i).last_linked_ms);
        settings.setValue(QStringLiteral("trustedAt"), peers.at(i).trusted_at_ms);
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
        [this](TransferSnapshotStore store, QMap<std::uint64_t, QString> peer_ids) {
            merge_snapshots(std::move(store), std::move(peer_ids));
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
    SettingsDialogState state{
        .receive_dir = receive_dir_ != nullptr ? receive_dir_->text() : saved_receive_dir(),
        .discovery_networks = saved_discovery_networks(),
        .max_global_sends = remembered_max_global_sends(),
        .max_peer_sends = remembered_max_peer_sends(),
        .close_action = SettingsCloseAction::ask,
    };
    const auto close_action = remembered_close_action();
    if (close_action == QStringLiteral("tray")) {
        state.close_action = SettingsCloseAction::tray;
    } else if (close_action == QStringLiteral("quit")) {
        state.close_action = SettingsCloseAction::quit;
    }

    const auto result = edit_settings(
        this,
        state,
        [this](QWidget* parent) {
            show_debug_logs(parent);
        },
        [this](QWidget* parent) {
            show_device_management(parent);
        });
    if (!result.has_value()) {
        return;
    }

    switch (result->close_action) {
        case SettingsCloseAction::ask:
            save_close_action({});
            break;
        case SettingsCloseAction::tray:
            save_close_action(QStringLiteral("tray"));
            break;
        case SettingsCloseAction::quit:
            save_close_action(QStringLiteral("quit"));
            break;
    }
    save_send_scheduler_settings(result->max_global_sends, result->max_peer_sends);
    const auto old_discovery_networks = saved_discovery_networks();
    save_discovery_networks(result->discovery_networks);
    if (result->discovery_networks != old_discovery_networks) {
        log_event(QCoreApplication::translate("MainWindow", "Extra discovery networks changed to: %1")
                      .arg(result->discovery_networks.isEmpty()
                               ? QCoreApplication::translate("MainWindow", "none")
                               : result->discovery_networks));
    }
    if (scheduler_ != nullptr) {
        scheduler_->set_limits(SchedulerLimits{
            .max_global_sends = result->max_global_sends,
            .max_peer_sends = result->max_peer_sends,
        });
    }

    const auto old_dir = receive_dir_ != nullptr ? receive_dir_->text() : QString{};
    if (result->receive_dir == old_dir) {
        return;
    }

    const auto was_ready = receiver_ready_;
    stop_receiver();
    if (receive_dir_ != nullptr) {
        receive_dir_->setText(result->receive_dir);
    }
    save_receive_dir(result->receive_dir);
    log_event(QCoreApplication::translate("MainWindow", "Receiving folder changed to %1").arg(result->receive_dir));
    if (was_ready && !start_receiver()) {
        QMessageBox::warning(this,
                             QCoreApplication::translate("MainWindow", "Settings"),
                             QCoreApplication::translate("MainWindow", "Receiver failed to restart. Check the receiving folder."));
    }
}

void MainWindow::show_debug_logs(QWidget* parent) {
    if (debug_log_view_ != nullptr) {
        debug_log_view_->window()->raise();
        debug_log_view_->window()->activateWindow();
        return;
    }

    auto* dialog = new QDialog(parent != nullptr ? parent : this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QCoreApplication::translate("MainWindow", "Debug logs"));
    dialog->resize(360, 320);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* view = new QPlainTextEdit(dialog);
    view->setObjectName("logView");
    view->setReadOnly(true);
    view->setMaximumBlockCount(120);
    view->setPlaceholderText(QCoreApplication::translate("MainWindow", "No logs yet."));
    view->setPlainText(log_lines_.join(QLatin1Char('\n')));

    auto* close = new QPushButton(QCoreApplication::translate("MainWindow", "Close"), dialog);
    close->setObjectName("secondaryButton");
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    buttons->addWidget(close);

    layout->addWidget(view, 1);
    layout->addLayout(buttons);

    debug_log_view_ = view;
    auto* bar = view->verticalScrollBar();
    bar->setValue(bar->maximum());
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    connect(dialog, &QObject::destroyed, this, [this] {
        debug_log_view_ = nullptr;
    });
    dialog->show();
}

void MainWindow::show_device_management(QWidget* parent) {
    QDialog dialog(parent != nullptr ? parent : this);
    dialog.setWindowTitle(QCoreApplication::translate("MainWindow", "Devices"));
    dialog.resize(360, 420);
    dialog.setMinimumSize(340, 360);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(9);

    auto* title = new QLabel(QCoreApplication::translate("MainWindow", "Devices"), &dialog);
    title->setObjectName("dialogTitle");
    auto* hint = new QLabel(
        QCoreApplication::translate("MainWindow", "Set display names and manage trusted reconnect."),
        &dialog);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);

    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 8, 0);
    content_layout->setSpacing(8);

    QMap<QString, QLineEdit*> aliases;
    QMap<QString, QCheckBox*> trusted_checks;
    auto peers = devices_.peers();
    peers.erase(std::remove_if(peers.begin(), peers.end(), [](const Peer& peer) {
        return peer.id.isEmpty() || peer.host.isEmpty();
    }), peers.end());
    std::sort(peers.begin(), peers.end(), [](const Peer& left, const Peer& right) {
        if (left.trusted != right.trusted) {
            return left.trusted;
        }
        if (left.last_linked_ms != right.last_linked_ms) {
            return left.last_linked_ms > right.last_linked_ms;
        }
        return display_peer_name(left).localeAwareCompare(display_peer_name(right)) < 0;
    });

    if (peers.isEmpty()) {
        auto* empty = new QLabel(QCoreApplication::translate("MainWindow", "No known devices yet."), content);
        empty->setObjectName("mutedText");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(180);
        content_layout->addWidget(empty);
    } else {
        for (const auto& peer : peers) {
            auto* card = new QFrame(content);
            card->setObjectName("peerCard");
            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(10, 9, 10, 9);
            card_layout->setSpacing(6);

            auto* name = new QLabel(display_peer_name(peer), card);
            name->setObjectName("peerName");
            auto meta_parts = QStringList{
                peer_detail_text(peer),
                QCoreApplication::translate("MainWindow", "Last linked: %1").arg(timestamp_text(peer.last_linked_ms)),
            };
            if (peer.trusted) {
                meta_parts.append(QCoreApplication::translate("MainWindow", "Trusted: %1").arg(timestamp_text(peer.trusted_at_ms)));
            }
            auto* meta = new QLabel(meta_parts.join(QStringLiteral(" - ")), card);
            meta->setObjectName("peerMeta");
            meta->setWordWrap(true);

            auto* alias = new QLineEdit(peer.alias, card);
            alias->setObjectName("searchInput");
            alias->setPlaceholderText(peer.name);
            aliases.insert(peer.id, alias);

            card_layout->addWidget(name);
            card_layout->addWidget(meta);
            card_layout->addWidget(alias);

            if (peer.trusted) {
                auto* trusted = new QCheckBox(QCoreApplication::translate("MainWindow", "Trusted reconnect"), card);
                trusted->setChecked(true);
                trusted_checks.insert(peer.id, trusted);
                card_layout->addWidget(trusted);
            } else {
                auto* untrusted = new QLabel(QCoreApplication::translate("MainWindow", "Not trusted"), card);
                untrusted->setObjectName("mutedText");
                card_layout->addWidget(untrusted);
            }
            content_layout->addWidget(card);
        }
    }
    content_layout->addStretch(1);
    scroll->setWidget(content);

    auto* buttons = new QHBoxLayout();
    auto* cancel = new QPushButton(QCoreApplication::translate("MainWindow", "Cancel"), &dialog);
    cancel->setObjectName("secondaryButton");
    auto* save = new QPushButton(QCoreApplication::translate("MainWindow", "Save"), &dialog);
    save->setObjectName("primaryButton");
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(save);

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(scroll, 1);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, aliases, trusted_checks] {
        bool changed = false;
        for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
            if (!devices_.contains(it.key())) {
                continue;
            }
            const auto next_alias = it.value()->text().trimmed();
            if (devices_.peer(it.key()).alias == next_alias) {
                continue;
            }
            Peer updated;
            if (devices_.set_alias(it.key(), next_alias, &updated)) {
                sync_scheduler_peer(updated);
                changed = true;
            }
        }
        for (auto it = trusted_checks.cbegin(); it != trusted_checks.cend(); ++it) {
            if (it.value()->isChecked()) {
                continue;
            }
            Peer updated;
            if (devices_.untrust_peer(it.key(), &updated)) {
                sync_scheduler_peer(updated);
                log_event(QCoreApplication::translate("MainWindow", "Trust removed for %1").arg(display_peer_name(updated)));
                changed = true;
            }
        }
        if (changed) {
            save_remembered_peers();
            refresh_peer_list();
            refresh_transfer_list();
            update_linked_header();
            log_event(QCoreApplication::translate("MainWindow", "Device settings updated."));
        }
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::search_peers() {
    for (const auto& peer : devices_.mark_all_offline()) {
        sync_scheduler_peer(peer);
    }
    wake_scheduler();
    refresh_peer_list();
    status_->setText(QCoreApplication::translate("MainWindow", "Searching..."));
    send_discovery_probe(true);
    QTimer::singleShot(1200, this, [this] {
        if (devices_.empty()) {
            status_->setText(QCoreApplication::translate("MainWindow", "No machines found."));
            log_event(QCoreApplication::translate("MainWindow", "Discovery finished: no machines found."));
        } else {
            status_->setText(QCoreApplication::translate("MainWindow", "%1 machine(s) found.").arg(devices_.count()));
            log_event(QCoreApplication::translate("MainWindow", "Discovery finished: %1 machine(s) found.").arg(devices_.count()));
        }
    });
}

void MainWindow::send_discovery_probe(bool extended) {
    if (discovery_ == nullptr) {
        return;
    }
    const auto configured_networks = extended ? discovery_network_entries(saved_discovery_networks()) : QStringList{};
    const auto report = discovery_->send_discovery_probe(node_id_, machine_name(), configured_networks);
    if (extended) {
        log_event(QCoreApplication::translate("MainWindow", "Broadcast discover on UDP %1 to %2")
                      .arg(kDiscoveryPort)
                      .arg(report.broadcast_targets.join(QStringLiteral(", "))));
    }
    if (extended && report.configured_host_target_count > 0) {
        log_event(QCoreApplication::translate("MainWindow", "Configured discovery sent to %1 host address(es).").arg(report.configured_host_target_count));
    }
    if (extended && !report.configured_broadcast_targets.isEmpty()) {
        log_event(QCoreApplication::translate("MainWindow", "Configured subnet broadcast sent to %1")
                      .arg(report.configured_broadcast_targets.join(QStringLiteral(", "))));
    }
    if (extended && !report.configured_ranges.isEmpty()) {
        log_event(QCoreApplication::translate("MainWindow", "Configured discovery scope: %1")
                      .arg(report.configured_ranges.join(QStringLiteral(", "))));
    }
    if (extended && !report.skipped_host_scan_networks.isEmpty()) {
        log_event(QCoreApplication::translate("MainWindow", "Host scan skipped for broad network(s): %1")
                      .arg(report.skipped_host_scan_networks.join(QStringLiteral(", "))));
    }
    if (extended && !report.invalid_configured_networks.isEmpty()) {
        log_event(QCoreApplication::translate("MainWindow", "Invalid discovery network(s): %1")
                      .arg(report.invalid_configured_networks.join(QStringLiteral(", "))));
    }
}

void MainWindow::refresh_peer_presence() {
    const auto changed_peers = devices_.mark_stale_offline(now_ms(), kPeerStaleMs);
    for (const auto& peer : changed_peers) {
        sync_scheduler_peer(peer);
        log_event(QCoreApplication::translate("MainWindow", "Machine went offline: %1").arg(display_peer_name(peer)));
    }
    if (changed_peers.isEmpty()) {
        return;
    }
    wake_scheduler();
    update_linked_header();
    refresh_peer_list();
}

void MainWindow::add_manual_peer_from_filter() {
    QString host;
    std::uint16_t port = kTransferPort;
    if (!parse_manual_peer_endpoint(peer_filter_->text(), host, port)) {
        show_log(QCoreApplication::translate("MainWindow", "Enter an IPv4 address in the search box, for example 10.8.7.203."));
        return;
    }

    const auto peer = devices_.upsert_manual_peer(host, port, now_ms());
    status_->setText(QCoreApplication::translate("MainWindow", "Manual machine added."));
    log_event(QCoreApplication::translate("MainWindow", "Manual peer added: %1:%2").arg(host).arg(port));
    refresh_peer_list();
}

void MainWindow::handle_discovery_datagram(DiscoveryDatagram datagram) {
    const auto& obj = datagram.message.fields;
    if (datagram.message.type == "discover") {
        if (datagram.message.id == node_id_) {
            return;
        }
        if (!receiver_ready_) {
            log_event(QCoreApplication::translate("MainWindow", "Ignored discover from %1 because receiver is not ready.")
                          .arg(datagram.sender.toString()));
            return;
        }
        log_event(QCoreApplication::translate("MainWindow", "Received discover from %1:%2").arg(datagram.sender.toString()).arg(datagram.sender_port));
        reply_to_discovery(datagram.sender, datagram.sender_port);
        return;
    }
    if (datagram.message.type == "announce") {
        log_event(QCoreApplication::translate("MainWindow", "Received announce from %1").arg(datagram.sender.toString()));
        add_peer(datagram.sender, obj);
        return;
    }
    if (datagram.message.type == "link_request") {
        if (!receiver_ready_) {
            log_event(QCoreApplication::translate("MainWindow", "Ignored link request from %1 because receiver is not ready.")
                          .arg(datagram.sender.toString()));
            return;
        }
        log_event(QCoreApplication::translate("MainWindow", "Received link request from %1").arg(datagram.sender.toString()));
        receive_link_request(datagram.sender, obj);
        return;
    }
    if (datagram.message.type == "link_accept") {
        log_event(QCoreApplication::translate("MainWindow", "Received link accept from %1").arg(datagram.sender.toString()));
        receive_link_response(datagram.sender, obj, true);
        return;
    }
    if (datagram.message.type == "link_reject") {
        log_event(QCoreApplication::translate("MainWindow", "Received link reject from %1").arg(datagram.sender.toString()));
        receive_link_response(datagram.sender, obj, false);
        return;
    }
    if (datagram.message.type == "link_disconnect") {
        log_event(QCoreApplication::translate("MainWindow", "Received link disconnect from %1").arg(datagram.sender.toString()));
        receive_link_disconnect(datagram.sender, obj);
    }
}

void MainWindow::reply_to_discovery(const QHostAddress& target, quint16 port) {
    if (discovery_ == nullptr) {
        return;
    }
    discovery_->reply_to_discovery(target, port, node_id_, machine_name());
    log_event(QCoreApplication::translate("MainWindow", "Sent announce to %1:%2").arg(target.toString()).arg(port));
}

void MainWindow::add_peer(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    const auto host = address.toString();
    const auto port = static_cast<std::uint16_t>(obj.value("port").toInt(kTransferPort));
    const auto id = obj.value("id").toString(host + ":" + QString::number(port));
    const auto result = devices_.upsert_discovered_peer(
        host,
        port,
        id,
        obj.value("name").toString(QCoreApplication::translate("MainWindow", "Unknown")),
        now_ms());
    if (!result.previous_id.isEmpty()) {
        if (pending_link_codes_.contains(result.previous_id)) {
            pending_link_codes_.insert(result.peer.id, pending_link_codes_.take(result.previous_id));
        }
    }

    const auto peer = result.peer;
    log_event(QCoreApplication::translate("MainWindow", "Peer available: %1 %2:%3").arg(display_peer_name(peer), peer.host).arg(peer.port));
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
    return peer.alias.contains(needle, Qt::CaseInsensitive) ||
           peer.name.contains(needle, Qt::CaseInsensitive) ||
           peer.host.contains(needle, Qt::CaseInsensitive) ||
           peer.id.contains(needle, Qt::CaseInsensitive);
}

void MainWindow::refresh_peer_list() {
    if (peer_list_ == nullptr) {
        return;
    }
    peer_list_->clear();
    int visible = 0;
    auto peers = devices_.peers();
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
        return display_peer_name(left).localeAwareCompare(display_peer_name(right)) < 0;
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
        const auto text = devices_.empty() ? QCoreApplication::translate("MainWindow", "No machines yet. Refresh to search the LAN.")
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
    auto* name = new QLabel(display_peer_name(peer), card);
    name->setObjectName("peerName");
    auto meta_text = peer_detail_text(peer);
    if (scheduler_ != nullptr) {
        const auto stats = scheduler_->peer_stats(to_string(peer.id));
        if (stats.running > 0 || stats.queued > 0 || stats.paused > 0) {
            QStringList parts;
            const auto queued_ready = std::max(0, stats.queued - stats.waiting);
            if (stats.running > 0) {
                parts.append(QCoreApplication::translate("MainWindow", "%1 running").arg(stats.running));
            }
            if (queued_ready > 0) {
                parts.append(QCoreApplication::translate("MainWindow", "%1 queued").arg(queued_ready));
            }
            if (stats.waiting > 0) {
                parts.append(QCoreApplication::translate("MainWindow", "%1 waiting").arg(stats.waiting));
            }
            if (stats.paused > 0) {
                parts.append(QCoreApplication::translate("MainWindow", "%1 paused").arg(stats.paused));
            }
            meta_text += QStringLiteral(" - ");
            meta_text += parts.join(QStringLiteral(", "));
        }
    }
    auto* meta = new QLabel(meta_text, card);
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
    } else if (peer.trusted) {
        auto* button = new QToolButton(card);
        button->setObjectName("trustedBadge");
        button->setText(QCoreApplication::translate("MainWindow", "trusted"));
        button->setToolTip(QCoreApplication::translate("MainWindow", "Forget trust"));
        button->setAutoRaise(false);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        connect(button, &QToolButton::clicked, this, [this, id = peer.id] {
            untrust_peer(id);
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
    const auto key = transfer_snapshot_key(snapshot);
    const auto can_change_target = can_change_transfer_target(snapshot);
    const auto can_resume_queue = can_resume_queued_transfer(snapshot);
    const auto can_pause = can_pause_transfer(snapshot);
    auto* card = new TransferCard(
        snapshot,
        TransferCardText{
            .detail = transfer_detail_text(snapshot),
            .speed = transfer_rate_text(snapshot),
            .size = transfer_size_text(snapshot),
            .state = state_text(snapshot.state),
        },
        TransferCardActions{
            .resume_enabled = can_resume_queue || can_change_target || can_resume_transfer(snapshot),
            .open_enabled = can_open_transfer_dir(snapshot),
            .stop_enabled = can_pause || can_stop_transfer(snapshot),
            .remove_enabled = can_clear_transfer(snapshot),
            .resume_queued = can_resume_queue,
            .change_target = can_change_target,
            .pause = can_pause,
        },
        TransferCardCallbacks{
            .on_resume = [this, key] {
                TransferSnapshot snapshot;
                if (transfer_model_.try_snapshot(key, &snapshot) && can_resume_queued_transfer(snapshot)) {
                    resume_queued_transfer(key);
                    return;
                }
                if (transfer_model_.try_snapshot(key, &snapshot) && can_change_transfer_target(snapshot)) {
                    change_transfer_target(key);
                    return;
                }
                resume_transfer(key);
            },
            .on_open = [this, key] {
                open_transfer_dir(key);
            },
            .on_stop = [this, key] {
                TransferSnapshot snapshot;
                if (transfer_model_.try_snapshot(key, &snapshot) && can_pause_transfer(snapshot)) {
                    pause_transfer(key);
                    return;
                }
                stop_transfer(key);
            },
            .on_remove = [this, key] {
                remove_transfer_card(key);
            },
        },
        transfer_page_);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return card;
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
    if (snapshot.error.has_value()) {
        return to_qstring(format_error(*snapshot.error));
    }
    if (snapshot.completion_status == TransferCompletionStatus::skipped) {
        return QCoreApplication::translate("MainWindow", "Skipped because target already matches.");
    }
    if (snapshot.kind == TransferKind::directory &&
        (snapshot.skipped_files > 0 || snapshot.delta_files > 0 || snapshot.full_files > 0)) {
        return QCoreApplication::translate("MainWindow", "Skipped %1, Delta %2, Full %3")
            .arg(snapshot.skipped_files)
            .arg(snapshot.delta_files)
            .arg(snapshot.full_files);
    }
    return {};
}

bool MainWindow::can_stop_transfer(const TransferSnapshot& snapshot) const {
    return snapshot.state == TransferState::running;
}

bool MainWindow::can_pause_transfer(const TransferSnapshot& snapshot) const {
    return scheduler_ != nullptr &&
           snapshot.direction == TransferDirection::send &&
           snapshot.state == TransferState::pending;
}

bool MainWindow::can_clear_transfer(const TransferSnapshot& snapshot) const {
    return snapshot.state == TransferState::pending ||
           snapshot.state == TransferState::paused ||
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

bool MainWindow::can_resume_queued_transfer(const TransferSnapshot& snapshot) const {
    return scheduler_ != nullptr &&
           snapshot.direction == TransferDirection::send &&
           snapshot.state == TransferState::paused;
}

bool MainWindow::can_change_transfer_target(const TransferSnapshot& snapshot) const {
    return scheduler_ != nullptr &&
           snapshot.direction == TransferDirection::send &&
           (snapshot.state == TransferState::pending || snapshot.state == TransferState::paused) &&
           linked_peer_count() > 1;
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
    TransferSnapshot snapshot;
    if (!transfer_model_.try_snapshot(key, &snapshot) || !can_open_transfer_dir(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "No local receive folder for this transfer."));
        return;
    }

    const auto dir = transfer_open_dir(snapshot);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        show_log(QCoreApplication::translate("MainWindow", "Failed to open folder: %1").arg(dir));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Opened folder: %1").arg(dir));
}

void MainWindow::show_receive_history() {
    auto settings = app_settings();
    const auto history = settings.value(QStringLiteral("receiveHistory")).toList();
    std::vector<ReceiveHistoryItem> items;
    items.reserve(static_cast<std::size_t>(history.size()));
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
        items.push_back(ReceiveHistoryItem{
            .text = text,
            .open_dir = open_dir,
            .has_error = !error.isEmpty(),
        });
    }

    const auto action = show_receive_history_dialog(this, items);
    if (!action.has_value()) {
        return;
    }
    if (action->type == ReceiveHistoryActionType::clear) {
        settings.remove(QStringLiteral("receiveHistory"));
        recorded_history_keys_.clear();
        log_event(QCoreApplication::translate("MainWindow", "Receive history cleared."));
        return;
    }

    const auto dir = action->open_dir;
    if (dir.isEmpty()) {
        show_log(QCoreApplication::translate("MainWindow", "No local receive folder for this history item."));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        show_log(QCoreApplication::translate("MainWindow", "Failed to open folder: %1").arg(dir));
    }
}

void MainWindow::record_receive_history(const TransferSnapshot& snapshot) {
    if (snapshot.direction != TransferDirection::receive || !is_terminal_state(snapshot.state)) {
        return;
    }

    const auto key = transfer_snapshot_key(snapshot);
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

void MainWindow::copy_received_clipboard_image(const TransferSnapshot& snapshot) {
    if (!is_clipboard_image_transfer(snapshot)) {
        return;
    }

    const auto key = transfer_snapshot_key(snapshot);
    if (copied_clipboard_image_keys_.contains(key)) {
        return;
    }
    copied_clipboard_image_keys_.insert(key);

    const auto path = to_qstring(snapshot.path);
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto image = reader.read();
    if (image.isNull()) {
        log_event(QCoreApplication::translate("MainWindow", "Failed to copy received clipboard image: %1").arg(path));
        return;
    }

    QApplication::clipboard()->setImage(image);
    log_event(QCoreApplication::translate("MainWindow", "Received clipboard image copied to clipboard."));
    if (tray_icon_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_icon_->showMessage(
            QCoreApplication::translate("MainWindow", "Clipboard image received"),
            QCoreApplication::translate("MainWindow", "The received image has been copied to the clipboard."),
            QSystemTrayIcon::Information,
            2500);
    }
}

void MainWindow::resume_transfer(const QString& key) {
    TransferSnapshot snapshot;
    if (!transfer_model_.try_snapshot(key, &snapshot)) {
        return;
    }
    if (!can_resume_transfer(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer cannot be resumed."));
        return;
    }

    const auto path = to_qstring(snapshot.path);
    const auto peer_id = transfer_model_.peer_id_or(key, devices_.active_peer_id());
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

void MainWindow::resume_queued_transfer(const QString& key) {
    TransferSnapshot snapshot;
    if (!transfer_model_.try_snapshot(key, &snapshot) || !can_resume_queued_transfer(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "This queued transfer cannot be resumed."));
        return;
    }
    if (scheduler_ == nullptr || !scheduler_->resume_task(snapshot.transfer_id)) {
        show_log(QCoreApplication::translate("MainWindow", "This queued transfer cannot be resumed."));
        return;
    }
    show_log(QCoreApplication::translate("MainWindow", "Queued transfer resumed."));
}

void MainWindow::change_transfer_target(const QString& key) {
    TransferSnapshot snapshot;
    if (!transfer_model_.try_snapshot(key, &snapshot) || !can_change_transfer_target(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer target cannot be changed."));
        return;
    }

    const auto current_peer_id = transfer_model_.peer_id_or(key, devices_.active_peer_id());
    QList<TargetDevice> devices;
    for (const auto& id : linked_peer_ids()) {
        const auto peer = devices_.peer(id);
        devices.append(TargetDevice{
            .id = id,
            .name = display_peer_name(peer),
            .host = peer.host,
            .port = peer.port,
            .selected = id == current_peer_id,
        });
    }

    const auto next_peer_id = choose_transfer_target(this, devices);
    if (!next_peer_id.has_value() || next_peer_id.value() == current_peer_id) {
        return;
    }
    if (scheduler_ == nullptr ||
        !scheduler_->move_queued_task(snapshot.transfer_id, to_string(next_peer_id.value()))) {
        QMessageBox::warning(this,
                             QCoreApplication::translate("MainWindow", "Change target machine"),
                             QCoreApplication::translate("MainWindow", "Only queued sends can be moved."));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Moved queued transfer to %1.")
                  .arg(display_peer_name(devices_.peer(next_peer_id.value()))));
}

void MainWindow::pause_transfer(const QString& key) {
    TransferSnapshot snapshot;
    if (!transfer_model_.try_snapshot(key, &snapshot) || !can_pause_transfer(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer cannot be paused."));
        return;
    }
    if (scheduler_ == nullptr || !scheduler_->pause_task(snapshot.transfer_id)) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer cannot be paused."));
        return;
    }
    show_log(QCoreApplication::translate("MainWindow", "Queued transfer paused."));
}

void MainWindow::stop_transfer(const QString& key) {
    TransferSnapshot snapshot;
    if (!transfer_model_.try_snapshot(key, &snapshot)) {
        return;
    }
    if (!can_stop_transfer(snapshot)) {
        show_log(QCoreApplication::translate("MainWindow", "This transfer is not running."));
        return;
    }
    if (snapshot.direction == TransferDirection::receive) {
        if (receiver_runner_ == nullptr) {
            show_log(QCoreApplication::translate("MainWindow", "No active receiver for this transfer."));
            return;
        }
        show_log(QCoreApplication::translate("MainWindow", "Stopping receive transfer..."));
        stop_receiver();
        return;
    }
    if (scheduler_ != nullptr) {
        scheduler_->cancel_task(snapshot.transfer_id);
    }
    show_log(QCoreApplication::translate("MainWindow", "Stopping transfer..."));
}

void MainWindow::remove_transfer_card(const QString& key) {
    TransferSnapshot snapshot;
    if (transfer_model_.try_snapshot(key, &snapshot)) {
        if ((snapshot.state == TransferState::pending ||
             snapshot.state == TransferState::paused) &&
            snapshot.direction == TransferDirection::send) {
            transfer_model_.mark_dismissed(key);
            if (scheduler_ != nullptr) {
                scheduler_->cancel_task(snapshot.transfer_id);
            }
        } else if (!can_clear_transfer(snapshot)) {
            show_log(QCoreApplication::translate("MainWindow", "Stop the transfer before clearing it from the list."));
            return;
        }
    }

    remove_transfer_snapshot(key);
}

void MainWindow::remove_transfer_snapshot(const QString& key) {
    transfer_model_.remove(key);
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
    return transfer_model_.belongs_to_peer(key, devices_.active_peer_id(), has_active_peer());
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
    for (const auto& entry : transfer_model_.visible_entries(devices_.active_peer_id(), has_active_peer())) {
        auto* card = make_transfer_card(entry.snapshot);
        transfer_cards_.insert(entry.key, card);
        transfers_layout_->insertWidget(0, card);
    }
    if (empty_transfer_label_ != nullptr) {
        empty_transfer_label_->setVisible(transfer_cards_.isEmpty());
    }
}

void MainWindow::request_link(const QString& id) {
    if (!devices_.contains(id)) {
        return;
    }
    const auto peer = devices_.peer(id);
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
    status_->setText(QCoreApplication::translate("MainWindow", "Waiting for %1 to accept code %2...").arg(display_peer_name(peer), code));
    log_event(QCoreApplication::translate("MainWindow", "Sending link request to %1 %2:%3 code=%4")
                  .arg(display_peer_name(peer), peer.host)
                  .arg(peer.port)
                  .arg(code));
    QJsonObject fields{{"code", code}};
    if (peer.trusted && !peer.trust_token.isEmpty()) {
        fields.insert(QStringLiteral("trustToken"), peer.trust_token);
    }
    send_control(peer, "link_request", fields);
}

void MainWindow::receive_link_request(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    add_peer(address, obj);
    const auto id = obj.value("id").toString();
    if (!devices_.contains(id)) {
        return;
    }
    const auto peer = devices_.peer(id);
    const auto code = obj.value("code").toString();
    const auto trust_token = obj.value("trustToken").toString();
    log_event(QCoreApplication::translate("MainWindow", "Link request from %1 %2:%3 code=%4")
                  .arg(display_peer_name(peer), peer.host)
                  .arg(peer.port)
                  .arg(code));
    if (devices_.can_auto_accept_peer(peer, trust_token)) {
        log_event(QCoreApplication::translate("MainWindow", "Auto accepted trusted peer: %1").arg(display_peer_name(peer)));
        send_control(peer, "link_accept", QJsonObject{{"code", code}, {"trusted", true}, {"trustToken", peer.trust_token}});
        set_linked_peer(peer, !has_active_peer());
        return;
    }
    if (devices_.needs_trust_token_migration(peer)) {
        const auto token = trust_token.isEmpty() ? make_trust_token() : trust_token;
        const auto trusted = trust_peer(peer.id, token);
        log_event(QCoreApplication::translate("MainWindow", "Auto accepted legacy trusted peer and refreshed trust token: %1")
                      .arg(display_peer_name(trusted)));
        send_control(trusted, "link_accept", QJsonObject{{"code", code}, {"trusted", true}, {"trustToken", token}});
        set_linked_peer(trusted, !has_active_peer());
        return;
    }
    const auto answer = QMessageBox::question(
        this,
        QCoreApplication::translate("MainWindow", "Link request"),
        QCoreApplication::translate("MainWindow", "%1 wants to link with this machine.\nCode: %2").arg(display_peer_name(peer), code),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        log_event(QCoreApplication::translate("MainWindow", "Accepted link request from %1").arg(display_peer_name(peer)));
        const auto token = make_trust_token();
        const auto trusted = trust_peer(peer.id, token);
        send_control(trusted, "link_accept", QJsonObject{{"code", code}, {"trusted", true}, {"trustToken", token}});
        set_linked_peer(trusted, !has_active_peer());
    } else {
        log_event(QCoreApplication::translate("MainWindow", "Rejected link request from %1").arg(display_peer_name(peer)));
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
    log_event(QCoreApplication::translate("MainWindow", "Link accepted by %1").arg(display_peer_name(devices_.peer(id))));
    const auto trusted = trust_peer(id, obj.value("trustToken").toString());
    set_linked_peer(trusted, true);
}

void MainWindow::receive_link_disconnect(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    add_peer(address, obj);
    const auto id = obj.value("id").toString();
    if (!devices_.contains(id) || !devices_.peer(id).linked) {
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "%1 disconnected.").arg(display_peer_name(devices_.peer(id))));
    disconnect_peer(id, false, false);
}

void MainWindow::send_control(const Peer& peer, const QString& type, const QJsonObject& fields) {
    if (discovery_ == nullptr) {
        return;
    }
    discovery_->send_control(peer, type, node_id_, machine_name(), fields);
    log_event(QCoreApplication::translate("MainWindow", "Sent control '%1' to %2:%3").arg(type, peer.host).arg(kDiscoveryPort));
}

Peer MainWindow::trust_peer(const QString& id, const QString& trust_token) {
    auto peer = devices_.trust_peer(id, now_ms(), trust_token);
    if (peer.id.isEmpty()) {
        return peer;
    }
    save_remembered_peers();
    refresh_peer_list();
    log_event(QCoreApplication::translate("MainWindow", "Trusted peer: %1").arg(display_peer_name(peer)));
    return peer;
}

void MainWindow::untrust_peer(const QString& id) {
    if (!devices_.contains(id)) {
        return;
    }
    const auto peer = devices_.peer(id);
    const auto answer = QMessageBox::question(
        this,
        QCoreApplication::translate("MainWindow", "Forget trusted machine"),
        QCoreApplication::translate("MainWindow", "Forget trust for %1? You will need to confirm the next link request.")
            .arg(display_peer_name(peer)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    Peer updated;
    if (!devices_.untrust_peer(id, &updated)) {
        return;
    }
    save_remembered_peers();
    refresh_peer_list();
    log_event(QCoreApplication::translate("MainWindow", "Trust removed for %1").arg(display_peer_name(updated)));
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
    auto linked = devices_.set_linked_peer(peer, activate, now_ms());
    sync_scheduler_peer(linked);
    remember_peer(linked);
    update_linked_header();
    refresh_transfer_list();
    status_->setText(QCoreApplication::translate("MainWindow", "Linked to %1.").arg(display_peer_name(linked)));
    log_event(QCoreApplication::translate("MainWindow", "Linked peer: %1 %2:%3").arg(display_peer_name(linked), linked.host).arg(linked.port));
    refresh_peer_list();
    if (activate) {
        stack_->setCurrentWidget(transfer_page_);
    }
}

void MainWindow::set_active_peer(const QString& id) {
    if (!devices_.set_active_peer(id)) {
        return;
    }
    update_linked_header();
    refresh_transfer_list();
    refresh_peer_list();
}

void MainWindow::disconnect_peer(bool notify_peer, bool confirm_active_sends) {
    if (!has_active_peer()) {
        stack_->setCurrentWidget(peer_page_);
        return;
    }
    disconnect_peer(devices_.active_peer_id(), notify_peer, confirm_active_sends);
}

void MainWindow::disconnect_peer(const QString& id, bool notify_peer, bool confirm_active_sends) {
    if (!devices_.contains(id) || !devices_.peer(id).linked) {
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

    auto peer = devices_.peer(id);
    if (notify_peer && !peer.id.isEmpty() && peer.online) {
        send_control(peer, "link_disconnect");
    }
    const auto name = display_peer_name(peer);
    devices_.unlink_peer(id, &peer);
    sync_scheduler_peer(peer);
    pending_link_codes_.remove(id);
    update_linked_header();
    status_->setText(QCoreApplication::translate("MainWindow", "Disconnected."));
    log_event(QCoreApplication::translate("MainWindow", "Disconnected from %1").arg(name));
    refresh_peer_list();
    if (!has_active_peer()) {
        stack_->setCurrentWidget(peer_page_);
    }
}

bool MainWindow::is_linked_peer(const Peer& peer) const {
    return devices_.is_linked_peer(peer);
}

bool MainWindow::has_active_peer() const {
    return devices_.has_active_peer();
}

Peer MainWindow::active_peer() const {
    return devices_.active_peer();
}

int MainWindow::linked_peer_count() const {
    return devices_.linked_peer_count();
}

QStringList MainWindow::linked_peer_ids() const {
    return devices_.linked_peer_ids();
}

QStringList MainWindow::send_target_peer_ids() const {
    return devices_.send_target_peer_ids();
}

void MainWindow::reset_send_targets_to_active() {
    devices_.reset_send_targets_to_active();
}

void MainWindow::show_send_targets() {
    const auto linked_ids = linked_peer_ids();
    if (linked_ids.isEmpty()) {
        show_log(QCoreApplication::translate("MainWindow", "No linked machine."));
        return;
    }

    const auto selected_ids = send_target_peer_ids();
    QList<TargetDevice> devices;
    for (const auto& id : linked_ids) {
        const auto peer = devices_.peer(id);
        devices.append(TargetDevice{
            .id = id,
            .name = display_peer_name(peer),
            .host = peer.host,
            .port = peer.port,
            .selected = selected_ids.contains(id),
        });
    }

    const auto next_target_ids = choose_send_targets(this, devices);
    if (!next_target_ids.has_value()) {
        return;
    }

    devices_.set_send_target_peer_ids(next_target_ids.value());
    update_linked_header();
    log_event(QCoreApplication::translate("MainWindow", "Send targets updated: %1 machine(s).")
                  .arg(send_target_peer_ids().size()));
}

void MainWindow::update_linked_header() {
    const auto count = linked_peer_count();
    if (linked_count_label_ != nullptr) {
        linked_count_label_->setText(count > 0
                                         ? QCoreApplication::translate("MainWindow", "Linked %1").arg(count)
                                         : QCoreApplication::translate("MainWindow", "Not linked"));
    }
    if (linked_label_ == nullptr) {
        return;
    }
    if (!has_active_peer()) {
        linked_label_->setText(QCoreApplication::translate("MainWindow", "Not linked"));
        if (back_to_transfer_ != nullptr) {
            back_to_transfer_->setVisible(false);
        }
        if (target_button_ != nullptr) {
            target_button_->setVisible(false);
        }
        return;
    }
    const auto peer = active_peer();
    const auto target_count = send_target_peer_ids().size();
    linked_label_->setText(display_peer_name(peer));
    if (back_to_transfer_ != nullptr) {
        back_to_transfer_->setVisible(true);
    }
    if (target_button_ != nullptr) {
        target_button_->setVisible(count > 1);
        target_button_->setText(QCoreApplication::translate("MainWindow", "Targets (%1)").arg(target_count));
    }
}

void MainWindow::send_paths(const QStringList& paths, FileTransferSource source) {
    if (!has_active_peer()) {
        show_log(QCoreApplication::translate("MainWindow", "No linked machine."));
        return;
    }
    if (scheduler_ == nullptr) {
        show_log(QCoreApplication::translate("MainWindow", "Transfer scheduler is not ready."));
        return;
    }
    const auto target_ids = send_target_peer_ids();
    if (target_ids.isEmpty()) {
        show_log(QCoreApplication::translate("MainWindow", "No linked machine."));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Queued %1 path(s) for %2 machine(s).")
                  .arg(paths.size())
                  .arg(target_ids.size()));
    for (const auto& path : paths) {
        if (target_ids.size() == 1) {
            scheduler_->enqueue_send(to_string(target_ids.front()), std::filesystem::path(to_string(path)), source);
            continue;
        }
        std::vector<std::string> peer_ids;
        peer_ids.reserve(target_ids.size());
        for (const auto& id : target_ids) {
            peer_ids.push_back(to_string(id));
        }
        scheduler_->enqueue_send_to_peers(peer_ids, std::filesystem::path(to_string(path)), source);
    }
}

void MainWindow::paste_paths_from_clipboard() {
    const auto* mime = QApplication::clipboard()->mimeData();
    auto paths = local_file_paths_from_mime(mime);
    auto source = FileTransferSource::file;
    if (paths.empty()) {
        const auto image = image_from_mime(mime);
        if (!image.isNull()) {
            QDir dir(clipboard_image_dir());
            if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
                show_log(QCoreApplication::translate("MainWindow", "Failed to create clipboard image folder: %1").arg(dir.path()));
                return;
            }

            const auto file_name = QStringLiteral("%1%2.png")
                                       .arg(QString::fromUtf8(kClipboardImagePrefix),
                                            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
            const auto path = dir.filePath(file_name);
            if (!image.save(path, "PNG")) {
                show_log(QCoreApplication::translate("MainWindow", "Failed to save clipboard image: %1").arg(path));
                return;
            }
            paths.push_back(path);
            source = FileTransferSource::clipboard_image;
            log_event(QCoreApplication::translate("MainWindow", "Saved clipboard image: %1").arg(path));
        }
    }
    if (paths.empty()) {
        show_log(QCoreApplication::translate("MainWindow", "Clipboard does not contain local files, folders, or images."));
        return;
    }
    log_event(QCoreApplication::translate("MainWindow", "Pasted %1 path(s) from clipboard.").arg(paths.size()));
    send_paths(paths, source);
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
        .name = to_string(display_peer_name(peer)),
        .host = to_string(peer.host),
        .port = peer.port,
        .online = peer.online,
        .linked = peer.linked,
    });
}

void MainWindow::handle_scheduler_snapshot(SchedulerSnapshot snapshot) {
    const auto key = transfer_snapshot_key(snapshot.snapshot);
    if (transfer_model_.is_dismissed(key)) {
        if (snapshot.snapshot.state != TransferState::pending) {
            transfer_model_.remove(key);
        }
        refresh_peer_list();
        return;
    }
    const auto peer_id = to_qstring(snapshot.peer_id);
    upsert_snapshot(snapshot.snapshot, peer_id);
    refresh_peer_list();
}

void MainWindow::handle_scheduler_log(std::string line) {
    show_log(to_qstring(std::move(line)));
}

void MainWindow::wake_scheduler() {
    if (scheduler_ != nullptr) {
        scheduler_->pump();
    }
    refresh_peer_list();
}

void MainWindow::merge_snapshots(TransferSnapshotStore store, const QString& peer_id) {
    QMetaObject::invokeMethod(this, [this, store = std::move(store), peer_id] {
        for (const auto& snapshot : store.snapshots()) {
            upsert_snapshot(snapshot, peer_id);
        }
    }, Qt::QueuedConnection);
}

void MainWindow::merge_snapshots(TransferSnapshotStore store, QMap<std::uint64_t, QString> peer_ids) {
    QMetaObject::invokeMethod(this, [this, store = std::move(store), peer_ids = std::move(peer_ids)] {
        const auto linked_ids = linked_peer_ids();
        const auto fallback_peer_id = linked_ids.size() == 1 ? linked_ids.front() : QString{};
        for (const auto& snapshot : store.snapshots()) {
            auto peer_id = peer_ids.value(snapshot.transfer_id);
            if (peer_id.isEmpty() && snapshot.direction == TransferDirection::receive) {
                peer_id = fallback_peer_id;
            }
            upsert_snapshot(snapshot, peer_id);
        }
    }, Qt::QueuedConnection);
}

void MainWindow::upsert_snapshot(const TransferSnapshot& snapshot, const QString& peer_id) {
    record_receive_history(snapshot);
    copy_received_clipboard_image(snapshot);
    const auto key = transfer_snapshot_key(snapshot);
    if (snapshot.direction == TransferDirection::receive && !is_terminal_state(snapshot.state)) {
        transfer_model_.remove(key);
        pending_transfer_render_keys_.remove(key);
        auto* old = transfer_cards_.take(key);
        if (old != nullptr) {
            transfers_layout_->removeWidget(old);
            old->deleteLater();
        }
        if (transfer_cards_.isEmpty() && empty_transfer_label_ != nullptr) {
            empty_transfer_label_->show();
        }
        return;
    }
    transfer_model_.upsert(snapshot, peer_id);
    if (!transfer_belongs_to_active_peer(key)) {
        pending_transfer_render_keys_.remove(key);
        auto* old = transfer_cards_.take(key);
        if (old != nullptr) {
            transfers_layout_->removeWidget(old);
            old->deleteLater();
        }
        if (transfer_cards_.isEmpty() && empty_transfer_label_ != nullptr) {
            empty_transfer_label_->show();
        }
        return;
    }
    if (!is_terminal_state(snapshot.state)) {
        schedule_transfer_render(key);
        return;
    }
    pending_transfer_render_keys_.remove(key);
    render_transfer_snapshot(key, snapshot);
}

void MainWindow::render_transfer_snapshot(const QString& key, const TransferSnapshot& snapshot) {
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

void MainWindow::schedule_transfer_render(const QString& key) {
    pending_transfer_render_keys_.insert(key);
    if (transfer_render_scheduled_) {
        return;
    }

    if (!transfer_render_timer_.isValid()) {
        transfer_render_timer_.start();
    }

    const auto elapsed = transfer_render_timer_.elapsed();
    const auto delay = elapsed >= 150 ? 0 : static_cast<int>(150 - elapsed);
    transfer_render_scheduled_ = true;
    QTimer::singleShot(delay, this, [this] {
        transfer_render_scheduled_ = false;
        flush_pending_transfer_renders();
    });
}

void MainWindow::flush_pending_transfer_renders() {
    if (pending_transfer_render_keys_.isEmpty()) {
        transfer_render_timer_.restart();
        return;
    }
    const auto keys = pending_transfer_render_keys_.values();
    pending_transfer_render_keys_.clear();
    for (const auto& key : keys) {
        TransferSnapshot snapshot;
        if (transfer_model_.try_snapshot(key, &snapshot) && transfer_belongs_to_active_peer(key)) {
            render_transfer_snapshot(key, snapshot);
        }
    }
    transfer_render_timer_.restart();
}

void MainWindow::show_log(QString text) {
    QMetaObject::invokeMethod(this, [this, text = std::move(text)] {
        log_event(text);
    }, Qt::QueuedConnection);
}

void MainWindow::log_event(QString text) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, text = std::move(text)] {
            log_event(text);
        }, Qt::QueuedConnection);
        return;
    }

    const auto line = QString("[%1] %2").arg(QTime::currentTime().toString("HH:mm:ss"), text);
    log_lines_.append(line);
    while (log_lines_.size() > 120) {
        log_lines_.removeFirst();
    }

    if (debug_log_view_ != nullptr) {
        debug_log_view_->appendPlainText(line);
        auto* bar = debug_log_view_->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

}  // namespace lan::gui

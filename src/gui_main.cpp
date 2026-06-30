#include <QApplication>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "lan/app/receiver_config.h"
#include "lan/app/receiver_server.h"
#include "lan/app/sender_config.h"
#include "lan/app/sender_transfer.h"
#include "lan/app/transfer_snapshot.h"
#include "lan/common/error.h"
#include "lan/common/size.h"

namespace {

constexpr quint16 kDiscoveryPort = 39124;
constexpr std::uint16_t kTransferPort = 39123;
constexpr const char* kProtocol = "lan-file-transfer/1";

QString to_qstring(const std::string& text) {
    return QString::fromStdString(text);
}

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

std::string to_string(const QString& text) {
    return text.trimmed().toStdString();
}

QString default_receive_dir() {
    return QDir::homePath() + "/Downloads/reviewdir";
}

QString machine_name() {
    const auto name = QHostInfo::localHostName();
    return name.isEmpty() ? "unknown" : name;
}

QString snapshot_key(const lan::TransferSnapshot& snapshot) {
    const auto direction = snapshot.direction == lan::TransferDirection::receive ? "receive" : "send";
    return QString("%1:%2").arg(direction).arg(snapshot.transfer_id);
}

QString state_text(lan::TransferState state) {
    return to_qstring(std::string(lan::transfer_state_name(state)));
}

QString kind_text(lan::TransferKind kind) {
    return kind == lan::TransferKind::directory ? "directory" : "file";
}

QString detail_text(const lan::TransferSnapshot& snapshot) {
    if (snapshot.error) {
        return to_qstring(lan::format_error(snapshot.error.value()));
    }
    if (snapshot.kind == lan::TransferKind::directory) {
        return QString("files %1/%2  skipped %3  full %4  delta %5")
            .arg(snapshot.processed_files)
            .arg(snapshot.total_files)
            .arg(snapshot.skipped_files)
            .arg(snapshot.full_files)
            .arg(snapshot.delta_files);
    }
    return QString("%1 / %2")
        .arg(to_qstring(lan::format_size(snapshot.current_bytes)))
        .arg(to_qstring(lan::format_size(snapshot.total_bytes)));
}

int progress_percent(const lan::TransferSnapshot& snapshot) {
    if (snapshot.state == lan::TransferState::completed) {
        return 100;
    }
    if (snapshot.kind == lan::TransferKind::directory && snapshot.total_files > 0) {
        return static_cast<int>((snapshot.processed_files * 100) / snapshot.total_files);
    }
    if (snapshot.total_bytes > 0) {
        return static_cast<int>((snapshot.current_bytes * 100) / snapshot.total_bytes);
    }
    return 0;
}

struct Peer {
    QString id;
    QString name;
    QString host;
    std::uint16_t port = kTransferPort;
};

class GuiSenderEvents final : public lan::SenderTransferEvents {
public:
    explicit GuiSenderEvents(std::function<void(lan::TransferSnapshotStore)> on_change)
        : on_change_(std::move(on_change)) {}

    void on_transfer_started(const lan::TransferStarted& started) override {
        update([&] {
            snapshots_.apply(started);
        });
    }

    void on_transfer_progress(const lan::TransferProgress& progress) override {
        update([&] {
            snapshots_.apply(progress);
        });
    }

    void on_transfer_completed(const lan::TransferCompleted& completed) override {
        update([&] {
            snapshots_.apply(completed);
        });
    }

    void on_transfer_failed(const lan::TransferFailed& failed) override {
        update([&] {
            snapshots_.apply(failed);
        });
    }

    void on_transfer_cancelled(const lan::TransferCancelled& cancelled) override {
        update([&] {
            snapshots_.apply(cancelled);
        });
    }

private:
    void update(const std::function<void()>& apply) {
        std::lock_guard<std::mutex> lock(mutex_);
        apply();
        on_change_(snapshots_);
    }

    std::mutex mutex_;
    lan::TransferSnapshotStore snapshots_;
    std::function<void(lan::TransferSnapshotStore)> on_change_;
};

class GuiReceiverEvents final : public lan::ReceiverServerEvents {
public:
    GuiReceiverEvents(std::function<void(lan::TransferSnapshotStore)> on_change,
                      std::function<void(QString)> on_log)
        : on_change_(std::move(on_change)), on_log_(std::move(on_log)) {}

    void on_listening(const lan::ReceiverConfig&) override {}

    void on_transfer_started(const lan::TransferStarted& started) override {
        update([&] {
            snapshots_.apply(started);
        });
    }

    void on_transfer_progress(const lan::TransferProgress& progress) override {
        update([&] {
            snapshots_.apply(progress);
        });
    }

    void on_transfer_completed(const lan::TransferCompleted& completed) override {
        update([&] {
            snapshots_.apply(completed);
        });
    }

    void on_transfer_failed(const lan::TransferFailed& failed) override {
        update([&] {
            snapshots_.apply(failed);
        });
    }

    void on_transfer_cancelled(const lan::TransferCancelled& cancelled) override {
        update([&] {
            snapshots_.apply(cancelled);
        });
    }

    void on_file_progress(const lan::ReceiveFileProgress&) override {}

    void on_file_received(const lan::ReceiveFileReport& report) override {
        on_log_(QString("received %1").arg(to_qstring(report.target_path)));
    }

    void on_directory_progress(const lan::ReceiveSyncProgress&) override {}

    void on_directory_synced(const lan::ReceiveSyncReport& report) override {
        on_log_(QString("synced directory: %1 files").arg(report.manifest_files));
    }

    void on_client_error(const lan::Error& error) override {
        on_log_(to_qstring(lan::format_error(error)));
    }

private:
    void update(const std::function<void()>& apply) {
        std::lock_guard<std::mutex> lock(mutex_);
        apply();
        on_change_(snapshots_);
    }

    std::mutex mutex_;
    lan::TransferSnapshotStore snapshots_;
    std::function<void(lan::TransferSnapshotStore)> on_change_;
    std::function<void(QString)> on_log_;
};

class DropPanel final : public QFrame {
public:
    explicit DropPanel(QWidget* parent = nullptr) : QFrame(parent) {
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

    std::function<void(QStringList)> on_drop;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent* event) override {
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
};

class MainWindow final : public QWidget {
public:
    MainWindow() {
        setWindowTitle("LAN File Transfer");
        resize(400, 600);
        setMinimumSize(380, 560);
        setObjectName("app");
        build_ui();
        setup_discovery();
    }

    ~MainWindow() override {
        stop_receiver();
        stop_sender();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        stop_receiver();
        stop_sender();
        event->accept();
    }

private:
    void build_ui() {
        stack_ = new QStackedWidget(this);
        stack_->addWidget(build_setup_page());
        stack_->addWidget(build_peer_page());
        stack_->addWidget(build_transfer_page());

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->addWidget(stack_);
        apply_style();
    }

    QWidget* build_setup_page() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(28, 28, 28, 28);
        layout->setSpacing(0);

        auto* card = new QFrame(page);
        card->setObjectName("setupCard");
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(24, 24, 24, 24);
        card_layout->setSpacing(14);

        auto* title = new QLabel("Receiving folder", page);
        title->setObjectName("title");
        auto* subtitle = new QLabel("Files sent to this machine will be saved here.", page);
        subtitle->setObjectName("mutedText");

        auto* path_box = new QFrame(page);
        path_box->setObjectName("pathBox");
        auto* row = new QHBoxLayout(path_box);
        row->setContentsMargins(12, 10, 10, 10);
        row->setSpacing(10);
        receive_dir_ = new QLabel(default_receive_dir(), page);
        receive_dir_->setObjectName("pathLabel");
        receive_dir_->setWordWrap(true);
        auto* choose = new QPushButton("Choose", page);
        choose->setObjectName("secondaryButton");
        row->addWidget(receive_dir_, 1);
        row->addWidget(choose);

        auto* continue_button = new QPushButton("Start", page);
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
            const auto dir = QFileDialog::getExistingDirectory(this, "Receiving folder", receive_dir_->text());
            if (!dir.isEmpty()) {
                receive_dir_->setText(dir);
            }
        });
        connect(continue_button, &QPushButton::clicked, this, [this] {
            if (start_receiver()) {
                search_peers();
                stack_->setCurrentWidget(peer_page_);
            }
        });
        return page;
    }

    QWidget* build_peer_page() {
        peer_page_ = new QWidget(this);
        auto* layout = new QVBoxLayout(peer_page_);
        layout->setContentsMargins(24, 22, 24, 18);
        layout->setSpacing(12);

        auto* title = new QLabel("Nearby machines", peer_page_);
        title->setObjectName("title");
        auto* search = new QPushButton("Refresh", peer_page_);
        search->setObjectName("secondaryButton");
        auto* header = new QHBoxLayout();
        header->setSpacing(12);
        header->addWidget(title, 1);
        header->addWidget(search);

        peer_filter_ = new QLineEdit(peer_page_);
        peer_filter_->setObjectName("searchInput");
        peer_filter_->setPlaceholderText("Search device name or IP");

        peer_list_ = new QListWidget(peer_page_);
        peer_list_->setObjectName("peerList");
        peer_list_->setFrameShape(QFrame::NoFrame);
        peer_list_->setSpacing(10);
        peer_list_->setSelectionMode(QAbstractItemView::NoSelection);
        peer_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        status_ = new QLabel("Ready to find machines.", peer_page_);
        status_->setObjectName("mutedText");

        layout->addLayout(header);
        layout->addWidget(peer_filter_);
        layout->addWidget(peer_list_, 1);
        layout->addWidget(status_);

        connect(search, &QPushButton::clicked, this, [this] {
            search_peers();
        });
        connect(peer_filter_, &QLineEdit::textChanged, this, [this] {
            refresh_peer_list();
        });
        connect(peer_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            link_peer(item);
        });
        return peer_page_;
    }

    QWidget* build_transfer_page() {
        transfer_page_ = new QWidget(this);
        auto* layout = new QVBoxLayout(transfer_page_);
        layout->setContentsMargins(24, 22, 24, 18);
        layout->setSpacing(12);

        linked_label_ = new QLabel("Not linked", transfer_page_);
        linked_label_->setObjectName("title");
        auto* back = new QPushButton("Change Machine", transfer_page_);
        back->setObjectName("secondaryButton");
        auto* header = new QHBoxLayout();
        header->addWidget(linked_label_, 1);
        header->addWidget(back);

        drop_panel_ = new DropPanel(transfer_page_);
        drop_panel_->setMinimumHeight(150);
        transfers_ = new QTableWidget(0, 6, transfer_page_);
        transfers_->setObjectName("transferTable");
        transfers_->setHorizontalHeaderLabels({"ID", "Kind", "State", "Name", "Progress", "Detail"});
        transfers_->horizontalHeader()->setStretchLastSection(true);
        transfers_->verticalHeader()->setVisible(false);
        transfers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        transfers_->setSelectionMode(QAbstractItemView::NoSelection);
        transfers_->setShowGrid(false);

        log_ = new QLabel("", transfer_page_);
        log_->setObjectName("mutedText");

        layout->addLayout(header);
        layout->addWidget(drop_panel_);
        layout->addWidget(transfers_, 1);
        layout->addWidget(log_);

        connect(back, &QPushButton::clicked, this, [this] {
            stack_->setCurrentWidget(peer_page_);
        });
        drop_panel_->on_drop = [this](const QStringList& paths) {
            send_paths(paths);
        };
        return transfer_page_;
    }

    void apply_style() {
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
            #dropPanel {
                border: 1px dashed #97a3b6;
                border-radius: 8px;
                background: #ffffff;
            }
            #dropTitle {
                font-size: 18px;
                font-weight: 700;
            }
            #searchInput {
                min-height: 38px;
                padding: 0 12px;
                border: 1px solid #d9dee8;
                border-radius: 8px;
                background: #ffffff;
                color: #1e2430;
            }
            #peerList {
                background: transparent;
                border: none;
                outline: none;
            }
            #peerCard {
                background: #ffffff;
                border: 1px solid #e2e7ef;
                border-radius: 8px;
            }
            #peerIcon {
                background: #e9f1ff;
                color: #2563eb;
                border-radius: 8px;
                font-size: 20px;
                font-weight: 700;
            }
            #peerName {
                color: #1e2430;
                font-size: 15px;
                font-weight: 700;
            }
            #peerMeta {
                color: #717b8b;
                font-size: 12px;
            }
            #onlineBadge {
                color: #087443;
                background: #e7f8ef;
                border-radius: 7px;
                padding: 3px 8px;
                font-size: 12px;
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
            }
            #primaryButton {
                background: #2563eb;
                border-color: #2563eb;
                color: #ffffff;
                font-weight: 700;
            }
            #primaryButton:focus {
                border-color: #2563eb;
            }
            #primaryButton:hover {
                background: #1d4ed8;
                border-color: #1d4ed8;
            }
            #primaryButton:pressed {
                background: #1e40af;
                border-color: #1e40af;
            }
            #secondaryButton {
                background: #ffffff;
                border-color: #d8dee8;
                color: #273549;
            }
            #secondaryButton:focus {
                border-color: #d8dee8;
            }
            #linkButton {
                min-height: 32px;
                background: #2563eb;
                border-color: #2563eb;
                color: #ffffff;
                font-weight: 700;
            }
            #linkButton:focus {
                border-color: #2563eb;
            }
            #linkButton:hover {
                background: #1d4ed8;
                border-color: #1d4ed8;
            }
            #linkButton:pressed {
                background: #1e40af;
                border-color: #1e40af;
            }
            #transferTable {
                background: #ffffff;
                border: 1px solid #e2e7ef;
                border-radius: 8px;
                gridline-color: transparent;
                selection-background-color: transparent;
            }
            QHeaderView::section {
                background: #f8fafc;
                color: #5d6675;
                border: none;
                border-bottom: 1px solid #e2e7ef;
                padding: 8px;
                font-size: 12px;
                font-weight: 700;
            }
            QTableWidget::item {
                padding: 8px;
                border-bottom: 1px solid #edf0f5;
            }
        )");
    }

    void setup_discovery() {
        discovery_ = std::make_unique<QUdpSocket>();
        discovery_->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                         QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        connect(discovery_.get(), &QUdpSocket::readyRead, this, [this] {
            read_discovery();
        });
    }

    bool start_receiver() {
        lan::ReceiverConfig config;
        config.bind_address = "0.0.0.0";
        config.port = kTransferPort;
        config.receive_dir = std::filesystem::path(to_string(receive_dir_->text()));
        config.allow_overwrite = false;
        config.once = false;

        auto validated = lan::validate_receiver_config(std::move(config));
        if (!validated) {
            status_->setText(to_qstring(lan::format_error(validated.error())));
            return false;
        }

        receiver_events_ = std::make_unique<GuiReceiverEvents>(
            [this](lan::TransferSnapshotStore store) {
                merge_snapshots(std::move(store));
            },
            [this](QString line) {
                show_log(std::move(line));
            });
        receiver_runner_ = std::make_unique<lan::ReceiverServerRunner>();
        auto started = receiver_runner_->start(validated.value(), *receiver_events_);
        if (!started) {
            receiver_runner_.reset();
            receiver_events_.reset();
            status_->setText(to_qstring(lan::format_error(started.error())));
            return false;
        }
        return true;
    }

    void stop_receiver() {
        if (receiver_runner_ == nullptr) {
            return;
        }
        receiver_runner_->stop();
        receiver_runner_->join();
        receiver_runner_.reset();
        receiver_events_.reset();
    }

    void search_peers() {
        peers_.clear();
        refresh_peer_list();
        status_->setText("Searching...");
        const auto message = QJsonDocument(QJsonObject{
            {"protocol", kProtocol},
            {"type", "discover"},
            {"id", node_id_},
            {"name", machine_name()},
            {"port", static_cast<int>(kTransferPort)},
        }).toJson(QJsonDocument::Compact);
        discovery_->writeDatagram(message, QHostAddress::Broadcast, kDiscoveryPort);
        QTimer::singleShot(1200, this, [this] {
            if (peers_.isEmpty()) {
                status_->setText("No machines found.");
            } else {
                status_->setText(QString("%1 machine(s) found.").arg(peers_.size()));
            }
        });
    }

    void read_discovery() {
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
                    reply_to_discovery(sender, sender_port);
                }
            } else if (type == "announce") {
                add_peer(sender, obj);
            }
        }
    }

    void reply_to_discovery(const QHostAddress& target, quint16 port) {
        const auto message = QJsonDocument(QJsonObject{
            {"protocol", kProtocol},
            {"type", "announce"},
            {"id", node_id_},
            {"name", machine_name()},
            {"port", static_cast<int>(kTransferPort)},
        }).toJson(QJsonDocument::Compact);
        discovery_->writeDatagram(message, target, port);
    }

    void add_peer(const QHostAddress& address, const QJsonObject& obj) {
        if (obj.value("id").toString() == node_id_) {
            return;
        }
        const auto host = address.toString();
        const auto port = static_cast<std::uint16_t>(obj.value("port").toInt(kTransferPort));
        const auto id = host + ":" + QString::number(port);
        if (peers_.contains(id)) {
            return;
        }

        Peer peer{
            .id = id,
            .name = obj.value("name").toString("Unknown"),
            .host = host,
            .port = port,
        };
        peers_.insert(id, peer);
        refresh_peer_list();
    }

    bool peer_matches_filter(const Peer& peer) const {
        if (peer_filter_ == nullptr || peer_filter_->text().trimmed().isEmpty()) {
            return true;
        }
        const auto needle = peer_filter_->text().trimmed();
        return peer.name.contains(needle, Qt::CaseInsensitive) ||
               peer.host.contains(needle, Qt::CaseInsensitive) ||
               peer.id.contains(needle, Qt::CaseInsensitive);
    }

    void refresh_peer_list() {
        if (peer_list_ == nullptr) {
            return;
        }
        peer_list_->clear();
        int visible = 0;
        for (const auto& peer : peers_) {
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
            const auto text = peers_.isEmpty() ? QString("No machines yet. Refresh to search the LAN.")
                                               : QString("No matching machines.");
            auto* item = new QListWidgetItem();
            item->setFlags(Qt::NoItemFlags);
            item->setSizeHint(QSize(0, 86));
            peer_list_->addItem(item);
            peer_list_->setItemWidget(item, make_empty_peer_card(text));
        }
    }

    QWidget* make_empty_peer_card(const QString& text) {
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

    QWidget* make_peer_card(const Peer& peer) {
        auto* card = new QFrame(peer_list_);
        card->setObjectName("peerCard");
        auto* row = new QHBoxLayout(card);
        row->setContentsMargins(12, 10, 12, 10);
        row->setSpacing(12);

        auto* icon = new QLabel(peer.name.left(1).toUpper(), card);
        icon->setObjectName("peerIcon");
        icon->setFixedSize(44, 44);
        icon->setAlignment(Qt::AlignCenter);

        auto* text_box = new QVBoxLayout();
        text_box->setContentsMargins(0, 0, 0, 0);
        text_box->setSpacing(3);
        auto* name = new QLabel(peer.name, card);
        name->setObjectName("peerName");
        auto* meta = new QLabel(QString("%1:%2").arg(peer.host).arg(peer.port), card);
        meta->setObjectName("peerMeta");
        text_box->addWidget(name);
        text_box->addWidget(meta);

        auto* badge = new QLabel("online", card);
        badge->setObjectName("onlineBadge");
        badge->setAlignment(Qt::AlignCenter);
        badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        auto* link = new QPushButton("Link", card);
        link->setObjectName("linkButton");
        connect(link, &QPushButton::clicked, this, [this, id = peer.id] {
            link_peer_by_id(id);
        });

        row->addWidget(icon);
        row->addLayout(text_box, 1);
        row->addWidget(badge);
        row->addWidget(link);
        return card;
    }

    void link_peer(QListWidgetItem* item) {
        if (item == nullptr) {
            return;
        }
        const auto id = item->data(Qt::UserRole).toString();
        link_peer_by_id(id);
    }

    void link_peer_by_id(const QString& id) {
        if (!peers_.contains(id)) {
            return;
        }
        linked_peer_ = peers_.value(id);
        linked_label_->setText(QString("Linked to %1").arg(linked_peer_.name));
        log_->clear();
        stack_->setCurrentWidget(transfer_page_);
    }

    void send_paths(const QStringList& paths) {
        if (linked_peer_.id.isEmpty()) {
            show_log("No linked machine.");
            return;
        }
        for (const auto& path : paths) {
            start_sender(path);
        }
    }

    void start_sender(const QString& path) {
        if (sender_thread_.joinable()) {
            show_log("A send is already running.");
            return;
        }

        lan::SenderConfig config;
        config.target.host = to_string(linked_peer_.host);
        config.target.port = linked_peer_.port;
        config.source_path = std::filesystem::path(to_string(path));
        config.resume = true;

        auto validated = lan::validate_sender_config(std::move(config));
        if (!validated) {
            show_log(to_qstring(lan::format_error(validated.error())));
            return;
        }

        sender_runner_ = std::make_unique<lan::SenderTransferRunner>();
        auto events = std::make_shared<GuiSenderEvents>([this](lan::TransferSnapshotStore store) {
            merge_snapshots(std::move(store));
        });
        sender_events_ = events;

        sender_thread_ = std::thread([this, config = std::move(validated).value(), events] {
            auto result = sender_runner_->run(config, *events);
            QMetaObject::invokeMethod(this, [this, result = std::move(result)] {
                if (!result) {
                    show_log(to_qstring(lan::format_error(result.error())));
                }
                stop_sender();
            }, Qt::QueuedConnection);
        });
    }

    void stop_sender() {
        if (sender_runner_ != nullptr) {
            sender_runner_->cancel();
        }
        if (sender_thread_.joinable()) {
            sender_thread_.join();
        }
        sender_runner_.reset();
        sender_events_.reset();
    }

    void merge_snapshots(lan::TransferSnapshotStore store) {
        QMetaObject::invokeMethod(this, [this, store = std::move(store)] {
            for (const auto& snapshot : store.snapshots()) {
                upsert_snapshot(snapshot);
            }
        }, Qt::QueuedConnection);
    }

    void upsert_snapshot(const lan::TransferSnapshot& snapshot) {
        const auto key = snapshot_key(snapshot);
        int row = -1;
        for (int i = 0; i < transfers_->rowCount(); ++i) {
            auto* item = transfers_->item(i, 0);
            if (item != nullptr && item->text() == key) {
                row = i;
                break;
            }
        }
        if (row < 0) {
            row = transfers_->rowCount();
            transfers_->insertRow(row);
            for (int col = 0; col < transfers_->columnCount(); ++col) {
                transfers_->setItem(row, col, new QTableWidgetItem());
            }
        }
        transfers_->item(row, 0)->setText(key);
        transfers_->item(row, 1)->setText(kind_text(snapshot.kind));
        transfers_->item(row, 2)->setText(state_text(snapshot.state));
        transfers_->item(row, 3)->setText(QString::fromStdString(snapshot.name));
        transfers_->item(row, 4)->setText(QString("%1%").arg(progress_percent(snapshot)));
        transfers_->item(row, 5)->setText(detail_text(snapshot));
    }

    void show_log(QString text) {
        QMetaObject::invokeMethod(this, [this, text = std::move(text)] {
            log_->setText(text);
        }, Qt::QueuedConnection);
    }

    QStackedWidget* stack_ = nullptr;
    QWidget* peer_page_ = nullptr;
    QWidget* transfer_page_ = nullptr;
    QLabel* receive_dir_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* peer_filter_ = nullptr;
    QListWidget* peer_list_ = nullptr;
    QLabel* linked_label_ = nullptr;
    DropPanel* drop_panel_ = nullptr;
    QTableWidget* transfers_ = nullptr;
    QLabel* log_ = nullptr;

    std::unique_ptr<QUdpSocket> discovery_;
    QMap<QString, Peer> peers_;
    Peer linked_peer_;
    QString node_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::unique_ptr<lan::ReceiverServerRunner> receiver_runner_;
    std::unique_ptr<GuiReceiverEvents> receiver_events_;
    std::unique_ptr<lan::SenderTransferRunner> sender_runner_;
    std::shared_ptr<GuiSenderEvents> sender_events_;
    std::thread sender_thread_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}

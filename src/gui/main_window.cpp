#include "gui/main_window.h"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>
#include <utility>

#include "gui/drop_panel.h"
#include "gui/qt_utils.h"
#include "gui/transfer_events.h"
#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/common/error.h"
#include "lan/common/size.h"

namespace lan::gui {
namespace {

QString snapshot_key(const TransferSnapshot& snapshot) {
    const auto direction = snapshot.direction == TransferDirection::receive ? "receive" : "send";
    return QString("%1:%2").arg(direction).arg(snapshot.transfer_id);
}

QString state_text(TransferState state) {
    return to_qstring(std::string(transfer_state_name(state)));
}

QString direction_text(TransferDirection direction) {
    return direction == TransferDirection::receive ? "Receive" : "Send";
}

QString kind_text(TransferKind kind) {
    return kind == TransferKind::directory ? "directory" : "file";
}

QString detail_text(const TransferSnapshot& snapshot) {
    if (snapshot.error) {
        return to_qstring(format_error(snapshot.error.value()));
    }
    if (snapshot.kind == TransferKind::directory) {
        return QString("files %1/%2  skipped %3  full %4  delta %5")
            .arg(snapshot.processed_files)
            .arg(snapshot.total_files)
            .arg(snapshot.skipped_files)
            .arg(snapshot.full_files)
            .arg(snapshot.delta_files);
    }
    return QString("%1 / %2")
        .arg(to_qstring(format_size(snapshot.current_bytes)))
        .arg(to_qstring(format_size(snapshot.total_bytes)));
}

int progress_percent(const TransferSnapshot& snapshot) {
    if (snapshot.state == TransferState::completed) {
        return 100;
    }
    if (snapshot.kind == TransferKind::directory && snapshot.total_files > 0) {
        return static_cast<int>((snapshot.processed_files * 100) / snapshot.total_files);
    }
    if (snapshot.total_bytes > 0) {
        return static_cast<int>((snapshot.current_bytes * 100) / snapshot.total_bytes);
    }
    return 0;
}

QString make_link_code() {
    const auto value = QRandomGenerator::global()->bounded(100000, 1000000);
    return QString::number(value);
}

}  // namespace

MainWindow::MainWindow() : node_id_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    setWindowTitle("LAN File Transfer");
    resize(400, 600);
    setMinimumSize(380, 560);
    setObjectName("app");
    build_ui();
    setup_discovery();
}

MainWindow::~MainWindow() {
    stop_receiver();
    stop_sender();
}

void MainWindow::closeEvent(QCloseEvent* event) {
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

QWidget* MainWindow::build_peer_page() {
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

QWidget* MainWindow::build_transfer_page() {
    transfer_page_ = new QWidget(this);
    auto* layout = new QVBoxLayout(transfer_page_);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(12);

    linked_label_ = new QLabel("Not linked", transfer_page_);
    linked_label_->setObjectName("title");
    auto* back = new QPushButton("Change", transfer_page_);
    back->setObjectName("secondaryButton");

    auto* header = new QHBoxLayout();
    header->addWidget(linked_label_, 1);
    header->addWidget(back);

    drop_panel_ = new DropPanel(transfer_page_);
    drop_panel_->setMinimumHeight(150);

    auto* scroll = new QScrollArea(transfer_page_);
    scroll->setObjectName("transferScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* transfer_list = new QWidget(scroll);
    transfers_layout_ = new QVBoxLayout(transfer_list);
    transfers_layout_->setContentsMargins(0, 0, 0, 0);
    transfers_layout_->setSpacing(8);

    empty_transfer_label_ = new QLabel("Transfer list is empty.", transfer_list);
    empty_transfer_label_->setObjectName("emptyTransfer");
    empty_transfer_label_->setAlignment(Qt::AlignCenter);
    empty_transfer_label_->setMinimumHeight(170);
    transfers_layout_->addWidget(empty_transfer_label_);
    transfers_layout_->addStretch(1);
    scroll->setWidget(transfer_list);

    log_ = new QLabel("", transfer_page_);
    log_->setObjectName("mutedText");

    layout->addLayout(header);
    layout->addWidget(drop_panel_);
    layout->addWidget(scroll, 1);
    layout->addWidget(log_);

    connect(back, &QPushButton::clicked, this, [this] {
        stack_->setCurrentWidget(peer_page_);
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
            background: #e9f1ff;
            color: #2563eb;
            border-radius: 8px;
            font-size: 20px;
            font-weight: 700;
        }
        #peerName, #transferName {
            color: #1e2430;
            font-size: 15px;
            font-weight: 700;
        }
        #peerMeta, #transferMeta {
            color: #717b8b;
            font-size: 12px;
        }
        #onlineBadge, #stateBadge {
            color: #087443;
            background: #e7f8ef;
            border-radius: 7px;
            padding: 3px 8px;
            font-size: 12px;
        }
        #emptyTransfer {
            color: #8a93a3;
            background: #ffffff;
            border: 1px solid #e2e7ef;
            border-radius: 8px;
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
        #secondaryButton {
            background: #ffffff;
            border-color: #d8dee8;
            color: #273549;
        }
        #secondaryButton:focus {
            border-color: #d8dee8;
        }
    )");
}

void MainWindow::setup_discovery() {
    discovery_ = std::make_unique<QUdpSocket>();
    discovery_->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    connect(discovery_.get(), &QUdpSocket::readyRead, this, [this] {
        read_discovery();
    });
}

bool MainWindow::start_receiver() {
    ReceiverConfig config;
    config.bind_address = "0.0.0.0";
    config.port = kTransferPort;
    config.receive_dir = std::filesystem::path(to_string(receive_dir_->text()));
    config.allow_overwrite = false;
    config.once = false;

    auto validated = validate_receiver_config(std::move(config));
    if (!validated) {
        status_->setText(to_qstring(format_error(validated.error())));
        return false;
    }

    receiver_events_ = std::make_unique<GuiReceiverEvents>(
        [this](TransferSnapshotStore store) {
            merge_snapshots(std::move(store));
        },
        [this](QString line) {
            show_log(std::move(line));
        });
    receiver_runner_ = std::make_unique<ReceiverServerRunner>();
    auto started = receiver_runner_->start(validated.value(), *receiver_events_);
    if (!started) {
        receiver_runner_.reset();
        receiver_events_.reset();
        status_->setText(to_qstring(format_error(started.error())));
        return false;
    }
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
}

void MainWindow::search_peers() {
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
                reply_to_discovery(sender, sender_port);
            }
        } else if (type == "announce") {
            add_peer(sender, obj);
        } else if (type == "link_request") {
            receive_link_request(sender, obj);
        } else if (type == "link_accept") {
            receive_link_response(sender, obj, true);
        } else if (type == "link_reject") {
            receive_link_response(sender, obj, false);
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
}

void MainWindow::add_peer(const QHostAddress& address, const QJsonObject& obj) {
    if (obj.value("id").toString() == node_id_) {
        return;
    }
    const auto host = address.toString();
    const auto port = static_cast<std::uint16_t>(obj.value("port").toInt(kTransferPort));
    const auto id = obj.value("id").toString(host + ":" + QString::number(port));
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
        request_link(id);
    });

    row->addWidget(icon);
    row->addLayout(text_box, 1);
    row->addWidget(badge);
    row->addWidget(link);
    return card;
}

QWidget* MainWindow::make_transfer_card(const TransferSnapshot& snapshot) {
    auto* card = new QFrame(transfer_page_);
    card->setObjectName("transferCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(5);

    auto* top = new QHBoxLayout();
    auto* name = new QLabel(QString::fromStdString(snapshot.name), card);
    name->setObjectName("transferName");
    name->setWordWrap(true);
    auto* state = new QLabel(state_text(snapshot.state), card);
    state->setObjectName("stateBadge");
    state->setAlignment(Qt::AlignCenter);
    top->addWidget(name, 1);
    top->addWidget(state);

    auto* meta = new QLabel(
        QString("%1  %2  %3%").arg(direction_text(snapshot.direction), kind_text(snapshot.kind))
            .arg(progress_percent(snapshot)),
        card);
    meta->setObjectName("transferMeta");
    auto* detail = new QLabel(detail_text(snapshot), card);
    detail->setObjectName("transferMeta");
    detail->setWordWrap(true);

    layout->addLayout(top);
    layout->addWidget(meta);
    layout->addWidget(detail);
    return card;
}

void MainWindow::request_link(const QString& id) {
    if (!peers_.contains(id)) {
        return;
    }
    pending_link_id_ = id;
    pending_link_code_ = make_link_code();
    const auto peer = peers_.value(id);
    status_->setText(QString("Waiting for %1 to accept code %2...").arg(peer.name, pending_link_code_));
    send_control(peer, "link_request", QJsonObject{{"code", pending_link_code_}});
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
    const auto answer = QMessageBox::question(
        this,
        "Link request",
        QString("%1 wants to link with this machine.\nCode: %2").arg(peer.name, code),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        send_control(peer, "link_accept", QJsonObject{{"code", code}});
        set_linked_peer(peer);
    } else {
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
    if (id != pending_link_id_ || code != pending_link_code_) {
        return;
    }
    pending_link_id_.clear();
    pending_link_code_.clear();
    if (!accepted) {
        status_->setText("Link request rejected.");
        return;
    }
    set_linked_peer(peers_.value(id));
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
}

void MainWindow::link_peer(QListWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    request_link(item->data(Qt::UserRole).toString());
}

void MainWindow::set_linked_peer(const Peer& peer) {
    linked_peer_ = peer;
    linked_label_->setText(QString("Linked to %1").arg(linked_peer_.name));
    log_->clear();
    status_->setText(QString("Linked to %1.").arg(linked_peer_.name));
    stack_->setCurrentWidget(transfer_page_);
}

void MainWindow::send_paths(const QStringList& paths) {
    if (linked_peer_.id.isEmpty()) {
        show_log("No linked machine.");
        return;
    }
    for (const auto& path : paths) {
        start_sender(path);
    }
}

void MainWindow::start_sender(const QString& path) {
    if (sender_thread_.joinable()) {
        show_log("A send is already running.");
        return;
    }

    SenderConfig config;
    config.target.host = to_string(linked_peer_.host);
    config.target.port = linked_peer_.port;
    config.source_path = std::filesystem::path(to_string(path));
    config.resume = true;

    auto validated = validate_sender_config(std::move(config));
    if (!validated) {
        show_log(to_qstring(format_error(validated.error())));
        return;
    }

    sender_runner_ = std::make_unique<SenderTransferRunner>();
    auto events = std::make_shared<GuiSenderEvents>([this](TransferSnapshotStore store) {
        merge_snapshots(std::move(store));
    });
    sender_events_ = events;

    sender_thread_ = std::thread([this, config = std::move(validated).value(), events] {
        auto result = sender_runner_->run(config, *events);
        QMetaObject::invokeMethod(this, [this, result = std::move(result)] {
            if (!result) {
                show_log(to_qstring(format_error(result.error())));
            }
            stop_sender();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::stop_sender() {
    if (sender_runner_ != nullptr) {
        sender_runner_->cancel();
    }
    if (sender_thread_.joinable()) {
        sender_thread_.join();
    }
    sender_runner_.reset();
    sender_events_.reset();
}

void MainWindow::merge_snapshots(TransferSnapshotStore store) {
    QMetaObject::invokeMethod(this, [this, store = std::move(store)] {
        for (const auto& snapshot : store.snapshots()) {
            upsert_snapshot(snapshot);
        }
    }, Qt::QueuedConnection);
}

void MainWindow::upsert_snapshot(const TransferSnapshot& snapshot) {
    if (empty_transfer_label_ != nullptr) {
        empty_transfer_label_->hide();
    }
    const auto key = snapshot_key(snapshot);
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
        log_->setText(text);
    }, Qt::QueuedConnection);
}

}  // namespace lan::gui

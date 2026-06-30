#pragma once

#include <QMap>
#include <QHostAddress>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <memory>
#include <thread>

#include "gui/types.h"
#include "lan/app/receiver_server.h"
#include "lan/app/sender_transfer.h"
#include "lan/app/transfer_snapshot.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QStackedWidget;
class QSystemTrayIcon;
class QToolButton;
class QVBoxLayout;
class QUdpSocket;

namespace lan::gui {

class DropPanel;
class GuiReceiverEvents;
class GuiSenderEvents;

class MainWindow final : public QWidget {
public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void build_ui();
    QWidget* build_setup_page();
    QWidget* build_peer_page();
    QWidget* build_transfer_page();
    void apply_style();
    void setup_tray();
    void toggle_window_visibility();
    void quit_from_tray();

    void setup_discovery();
    bool start_receiver();
    void stop_receiver();

    void search_peers();
    void read_discovery();
    void reply_to_discovery(const QHostAddress& target, quint16 port);
    void add_peer(const QHostAddress& address, const QJsonObject& obj);
    bool peer_matches_filter(const Peer& peer) const;
    void refresh_peer_list();
    QWidget* make_empty_peer_card(const QString& text);
    QWidget* make_peer_card(const Peer& peer);
    QWidget* make_transfer_card(const TransferSnapshot& snapshot);
    QLabel* make_metric_label(const QString& title, const QString& value, QWidget* parent);
    QToolButton* make_task_tool_button(const QIcon& icon, const QString& tooltip, QWidget* parent);
    QString transfer_rate_text(const TransferSnapshot& snapshot) const;
    QString transfer_size_text(const TransferSnapshot& snapshot) const;
    bool can_stop_transfer(const TransferSnapshot& snapshot) const;
    bool can_clear_transfer(const TransferSnapshot& snapshot) const;
    void stop_transfer(const QString& key);
    void remove_transfer_card(const QString& key);

    void request_link(const QString& id);
    void receive_link_request(const QHostAddress& address, const QJsonObject& obj);
    void receive_link_response(const QHostAddress& address, const QJsonObject& obj, bool accepted);
    void send_control(const Peer& peer, const QString& type, const QJsonObject& fields = {});
    void link_peer(QListWidgetItem* item);
    void set_linked_peer(const Peer& peer);

    void send_paths(const QStringList& paths);
    void start_sender(const QString& path);
    void stop_sender();

    void merge_snapshots(TransferSnapshotStore store);
    void upsert_snapshot(const TransferSnapshot& snapshot);
    void show_log(QString text);
    void log_event(QString text);

    QStackedWidget* stack_ = nullptr;
    QWidget* peer_page_ = nullptr;
    QWidget* transfer_page_ = nullptr;
    QLabel* receive_dir_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* peer_filter_ = nullptr;
    QListWidget* peer_list_ = nullptr;
    QLabel* linked_label_ = nullptr;
    DropPanel* drop_panel_ = nullptr;
    QVBoxLayout* transfers_layout_ = nullptr;
    QLabel* empty_transfer_label_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QSystemTrayIcon* tray_icon_ = nullptr;

    std::unique_ptr<QUdpSocket> discovery_;
    QMap<QString, Peer> peers_;
    QMap<QString, QWidget*> transfer_cards_;
    QMap<QString, TransferSnapshot> transfer_snapshots_;
    Peer linked_peer_;
    QString pending_link_id_;
    QString pending_link_code_;
    QString node_id_;
    bool receiver_ready_ = false;
    bool force_quit_ = false;
    bool tray_message_shown_ = false;
    std::unique_ptr<ReceiverServerRunner> receiver_runner_;
    std::unique_ptr<GuiReceiverEvents> receiver_events_;
    std::unique_ptr<SenderTransferRunner> sender_runner_;
    std::shared_ptr<GuiSenderEvents> sender_events_;
    std::thread sender_thread_;
};

}  // namespace lan::gui

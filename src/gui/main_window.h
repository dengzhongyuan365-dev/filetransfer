#pragma once

#include <QMap>
#include <QHostAddress>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <memory>

#include "gui/types.h"
#include "lan/app/receiver_server.h"
#include "lan/app/transfer_scheduler.h"
#include "lan/app/transfer_snapshot.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class QToolButton;
class QVBoxLayout;
class QUdpSocket;

namespace lan::gui {

class DropPanel;
class GuiReceiverEvents;

enum class CloseAction {
    cancel,
    minimizeToTray,
    quit,
};

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
    void setup_scheduler();
    void toggle_window_visibility();
    void quit_from_tray();
    CloseAction ask_close_action();

    void setup_discovery();
    QString load_or_create_node_id();
    void load_remembered_peers();
    QString find_peer_id_by_endpoint(const QString& host, std::uint16_t port) const;
    void remember_peer(const Peer& peer);
    void save_remembered_peers();
    bool start_receiver();
    void stop_receiver();
    void show_settings();

    void search_peers();
    void add_manual_peer_from_filter();
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
    QString transfer_detail_text(const TransferSnapshot& snapshot) const;
    bool can_stop_transfer(const TransferSnapshot& snapshot) const;
    bool can_clear_transfer(const TransferSnapshot& snapshot) const;
    bool can_open_transfer_dir(const TransferSnapshot& snapshot) const;
    bool can_resume_transfer(const TransferSnapshot& snapshot) const;
    bool can_change_transfer_target(const TransferSnapshot& snapshot) const;
    QString transfer_open_dir(const TransferSnapshot& snapshot) const;
    void open_transfer_dir(const QString& key);
    void show_receive_history();
    void record_receive_history(const TransferSnapshot& snapshot);
    void resume_transfer(const QString& key);
    void change_transfer_target(const QString& key);
    void stop_transfer(const QString& key);
    void remove_transfer_card(const QString& key);
    void remove_transfer_snapshot(const QString& key);
    bool transfer_belongs_to_active_peer(const QString& key) const;
    void clear_transfer_cards();
    void refresh_transfer_list();

    void request_link(const QString& id);
    void receive_link_request(const QHostAddress& address, const QJsonObject& obj);
    void receive_link_response(const QHostAddress& address, const QJsonObject& obj, bool accepted);
    void receive_link_disconnect(const QHostAddress& address, const QJsonObject& obj);
    void send_control(const Peer& peer, const QString& type, const QJsonObject& fields = {});
    void link_peer(QListWidgetItem* item);
    void open_transfer_page();
    void set_linked_peer(const Peer& peer, bool activate);
    void set_active_peer(const QString& id);
    void disconnect_peer(bool notify_peer = true, bool confirm_active_sends = true);
    void disconnect_peer(const QString& id, bool notify_peer = true, bool confirm_active_sends = true);
    bool is_linked_peer(const Peer& peer) const;
    bool has_active_peer() const;
    Peer active_peer() const;
    int linked_peer_count() const;
    QStringList linked_peer_ids() const;
    QStringList send_target_peer_ids() const;
    void reset_send_targets_to_active();
    void show_send_targets();
    void update_linked_header();

    void send_paths(const QStringList& paths);
    void paste_paths_from_clipboard();
    void stop_sender();
    void sync_scheduler_peer(const Peer& peer);
    void handle_scheduler_snapshot(SchedulerSnapshot snapshot);
    void handle_scheduler_log(std::string line);
    void wake_scheduler();

    void merge_snapshots(TransferSnapshotStore store, const QString& peer_id = {});
    void upsert_snapshot(const TransferSnapshot& snapshot, const QString& peer_id = {});
    void show_log(QString text);
    void log_event(QString text);

    QStackedWidget* stack_ = nullptr;
    QWidget* peer_page_ = nullptr;
    QWidget* transfer_page_ = nullptr;
    QLabel* receive_dir_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* peer_filter_ = nullptr;
    QListWidget* peer_list_ = nullptr;
    QPushButton* back_to_transfer_ = nullptr;
    QPushButton* target_button_ = nullptr;
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
    QMap<QString, QString> transfer_peer_ids_;
    QSet<QString> dismissed_transfer_keys_;
    QSet<QString> recorded_history_keys_;
    QSet<QString> send_target_peer_ids_;
    QString active_peer_id_;
    QMap<QString, QString> pending_link_codes_;
    QString node_id_;
    bool receiver_ready_ = false;
    bool force_quit_ = false;
    bool tray_message_shown_ = false;
    std::unique_ptr<ReceiverServerRunner> receiver_runner_;
    std::unique_ptr<GuiReceiverEvents> receiver_events_;
    std::unique_ptr<TransferScheduler> scheduler_;
};

}  // namespace lan::gui

#pragma once

#include <cstdint>

#include <QString>

#include "gui/transfer_list_model.h"

namespace lan::gui {

struct TransferResendRequest {
    QString peer_id;
    QString name;
    TransferKind kind = TransferKind::file;
    std::uint64_t total_bytes = 0;
    std::uint64_t total_files = 0;
};

bool transfer_matches_resend_request(const TransferListEntry& entry,
                                     const QString& entry_peer_id,
                                     const QString& entry_display_name,
                                     const TransferResendRequest& request);

}  // namespace lan::gui

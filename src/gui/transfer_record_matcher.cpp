#include "gui/transfer_record_matcher.h"

namespace lan::gui {

bool transfer_matches_resend_request(const TransferListEntry& entry,
                                     const QString& entry_peer_id,
                                     const QString& entry_display_name,
                                     const TransferResendRequest& request) {
    if (request.peer_id.isEmpty() || request.name.isEmpty()) {
        return false;
    }
    if (entry_peer_id != request.peer_id ||
        entry.snapshot.direction != TransferDirection::send ||
        entry.snapshot.kind != request.kind ||
        entry_display_name != request.name) {
        return false;
    }
    if (request.total_bytes > 0 &&
        entry.snapshot.total_bytes > 0 &&
        entry.snapshot.total_bytes != request.total_bytes) {
        return false;
    }
    if (request.total_files > 0 &&
        entry.snapshot.total_files > 0 &&
        entry.snapshot.total_files != request.total_files) {
        return false;
    }
    return true;
}

}  // namespace lan::gui

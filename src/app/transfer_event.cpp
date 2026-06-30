#include "lan/app/transfer_event.h"

namespace lan {

void TransferEvents::on_transfer_started(const TransferStarted&) {}

void TransferEvents::on_transfer_progress(const TransferProgress&) {}

void TransferEvents::on_transfer_completed(const TransferCompleted&) {}

void TransferEvents::on_transfer_failed(const TransferFailed&) {}

void TransferEvents::on_transfer_cancelled(const TransferCancelled&) {}

}  // namespace lan

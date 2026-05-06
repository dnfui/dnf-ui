// -----------------------------------------------------------------------------
// transaction_service_format.cpp
// Converts internal transaction service values into D-Bus reply values.
// This includes preview arrays and stable state names returned to the GUI.
// -----------------------------------------------------------------------------
#include "transaction_service_internal.hpp"

// -----------------------------------------------------------------------------
// Copy one preview section into a D-Bus string array builder.
// -----------------------------------------------------------------------------
void
append_transaction_preview_array(GVariantBuilder &builder, const std::vector<std::string> &items)
{
  for (const auto &item : items) {
    g_variant_builder_add(&builder, "s", item.c_str());
  }
}

// -----------------------------------------------------------------------------
// Map one internal transaction stage to its D-Bus state string.
// -----------------------------------------------------------------------------
const char *
transaction_stage_name(TransactionStage stage)
{
  switch (stage) {
  case TransactionStage::PREVIEW_RUNNING:
    return "preview-running";
  case TransactionStage::PREVIEW_READY:
    return "preview-ready";
  case TransactionStage::PREVIEW_FAILED:
    return "preview-failed";
  case TransactionStage::APPLY_RUNNING:
    return "apply-running";
  case TransactionStage::APPLY_SUCCEEDED:
    return "apply-succeeded";
  case TransactionStage::APPLY_FAILED:
    return "apply-failed";
  case TransactionStage::CANCELLED:
    return "cancelled";
  }

  return "unknown";
}

// -----------------------------------------------------------------------------
// Reset one transaction request object to a running state before new work starts.
// -----------------------------------------------------------------------------
void
set_transaction_running(TransactionSession *session, TransactionStage stage)
{
  if (!session) {
    return;
  }

  std::lock_guard<std::mutex> lock(session->state_mutex);
  session->stage = stage;
  session->finished = false;
  session->success = false;
  session->details.clear();
}

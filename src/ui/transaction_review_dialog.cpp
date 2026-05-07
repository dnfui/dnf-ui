// -----------------------------------------------------------------------------
// Transaction review dialog helpers
//
// Owns the confirmation and error dialogs used around transaction apply.
// The live progress window is handled in transaction_progress.cpp.
// -----------------------------------------------------------------------------
#include "transaction_review_dialog.hpp"

#include "dnf_backend/dnf_backend.hpp"
#include "i18n.hpp"
#include "widgets.hpp"

struct SummaryDialogApplyData {
  SearchWidgets *widgets;
  TransactionApplyCallback on_apply;
  TransactionApplyCallback on_cancel;
  bool apply_requested;
};

// -----------------------------------------------------------------------------
// Free data owned by the transaction summary dialog callback.
// -----------------------------------------------------------------------------
static void
summary_dialog_apply_data_free(gpointer p)
{
  SummaryDialogApplyData *data = static_cast<SummaryDialogApplyData *>(p);
  delete data;
}

// -----------------------------------------------------------------------------
// Format the resolved disk space change for the transaction summary dialog.
// -----------------------------------------------------------------------------
static std::string
format_transaction_space_change(long long delta_bytes)
{
  if (delta_bytes == 0) {
    return _("Disk space usage will be unchanged.");
  }

  unsigned long long abs_bytes =
      delta_bytes > 0 ? static_cast<unsigned long long>(delta_bytes) : static_cast<unsigned long long>(-delta_bytes);
  char *formatted = g_format_size(abs_bytes);
  std::string line;

  if (delta_bytes > 0) {
    line = dnfui_i18n_format(_("%s extra disk space will be used."), formatted);
  } else {
    line = dnfui_i18n_format(_("%s of disk space will be freed."), formatted);
  }

  g_free(formatted);
  return line;
}

// -----------------------------------------------------------------------------
// Append one resolved transaction section to the confirmation dialog.
// -----------------------------------------------------------------------------
static void
append_transaction_summary_section(GtkBox *parent, const char *title, const std::vector<std::string> &items)
{
  if (!parent || !title || items.empty()) {
    return;
  }

  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(parent, section);

  GtkWidget *heading = gtk_label_new(nullptr);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(heading), markup);
  g_free(markup);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(section), heading);

  for (const auto &item : items) {
    GtkWidget *label = gtk_label_new(item.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_focusable(label, FALSE);
    gtk_box_append(GTK_BOX(section), label);
  }
}

// -----------------------------------------------------------------------------
// Show the final confirmation dialog before starting the package transaction.
// -----------------------------------------------------------------------------
void
transaction_review_show_summary_dialog(SearchWidgets *widgets,
                                       const TransactionPreview &preview,
                                       TransactionApplyCallback on_apply,
                                       TransactionApplyCallback on_cancel)
{
  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, _("Summary"));
  gtk_window_set_default_size(dialog, 760, 520);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(dialog, app);
    }
    gtk_window_set_transient_for(dialog, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *title = gtk_label_new(nullptr);
  gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", _("Summary"));
  gtk_label_set_markup(GTK_LABEL(title), title_markup);
  g_free(title_markup);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_box_append(GTK_BOX(outer), title);

  GtkWidget *question = gtk_label_new(_("Apply the following changes?"));
  gtk_label_set_xalign(GTK_LABEL(question), 0.0f);
  gtk_box_append(GTK_BOX(outer), question);

  GtkWidget *intro = gtk_label_new(
      _("This is your last opportunity to look through the list of marked changes before they are applied."));
  gtk_label_set_xalign(GTK_LABEL(intro), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
  gtk_box_append(GTK_BOX(outer), intro);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(contents, 6);
  gtk_widget_set_margin_end(contents, 6);
  gtk_widget_set_margin_top(contents, 6);
  gtk_widget_set_margin_bottom(contents, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), contents);

  // Show the resolved backend changes, not only the packages marked manually.
  append_transaction_summary_section(GTK_BOX(contents), _("To be installed"), preview.install);
  append_transaction_summary_section(GTK_BOX(contents), _("To be upgraded"), preview.upgrade);
  append_transaction_summary_section(GTK_BOX(contents), _("To be downgraded"), preview.downgrade);
  append_transaction_summary_section(GTK_BOX(contents), _("To be reinstalled"), preview.reinstall);
  append_transaction_summary_section(GTK_BOX(contents), _("To be removed"), preview.remove);

  GtkWidget *summary_heading = gtk_label_new(nullptr);
  gchar *summary_markup = g_markup_printf_escaped("<b>%s</b>", _("Summary"));
  gtk_label_set_markup(GTK_LABEL(summary_heading), summary_markup);
  g_free(summary_markup);
  gtk_label_set_xalign(GTK_LABEL(summary_heading), 0.0f);
  gtk_box_append(GTK_BOX(outer), summary_heading);

  GtkWidget *summary_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append(GTK_BOX(outer), summary_box);

  auto append_summary_line = [&](const std::string &line) {
    GtkWidget *label = gtk_label_new(line.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(summary_box), label);
  };

  auto append_count_line = [&](size_t count, const char *singular, const char *plural) {
    if (count == 0) {
      return;
    }

    append_summary_line(dnfui_i18n_format_count(count, singular, plural));
  };

  append_count_line(preview.install.size(), "%zu package will be installed.", "%zu packages will be installed.");
  append_count_line(preview.upgrade.size(), "%zu package will be upgraded.", "%zu packages will be upgraded.");
  append_count_line(preview.downgrade.size(), "%zu package will be downgraded.", "%zu packages will be downgraded.");
  append_count_line(preview.reinstall.size(), "%zu package will be reinstalled.", "%zu packages will be reinstalled.");
  append_count_line(preview.remove.size(), "%zu package will be removed.", "%zu packages will be removed.");
  append_summary_line(format_transaction_space_change(preview.disk_space_delta));

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *cancel_button = gtk_button_new_with_label(_("Cancel"));
  gtk_box_append(GTK_BOX(button_box), cancel_button);

  GtkWidget *apply_button = gtk_button_new_with_label(_("Apply"));
  gtk_widget_add_css_class(apply_button, "suggested-action");
  gtk_box_append(GTK_BOX(button_box), apply_button);

  auto *apply_data = new SummaryDialogApplyData { widgets, on_apply, on_cancel, false };

  g_object_set_data_full(G_OBJECT(dialog), "summary-dialog-apply-data", apply_data, summary_dialog_apply_data_free);

  g_signal_connect(cancel_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  g_signal_connect(dialog,
                   "destroy",
                   G_CALLBACK(+[](GtkWidget *widget, gpointer) {
                     SummaryDialogApplyData *data = static_cast<SummaryDialogApplyData *>(
                         g_object_get_data(G_OBJECT(widget), "summary-dialog-apply-data"));
                     if (!data || data->apply_requested || !data->widgets || !data->on_cancel) {
                       return;
                     }

                     data->on_cancel(data->widgets);
                   }),
                   nullptr);

  g_signal_connect(apply_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer user_data) {
                     SummaryDialogApplyData *data = static_cast<SummaryDialogApplyData *>(user_data);
                     SearchWidgets *widgets = data ? data->widgets : nullptr;
                     TransactionApplyCallback on_apply = data ? data->on_apply : nullptr;
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (data) {
                       data->apply_requested = true;
                     }
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                     if (widgets && on_apply) {
                       on_apply(widgets);
                     }
                   }),
                   apply_data);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// Show a modal dialog with selectable transaction error details.
// -----------------------------------------------------------------------------
void
transaction_review_show_error_dialog(SearchWidgets *widgets,
                                     const char *title,
                                     const char *intro,
                                     const std::string &details)
{
  if (!widgets || !title || !intro) {
    return;
  }

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(dialog, title);
  gtk_window_set_default_size(dialog, 760, 420);
  gtk_window_set_modal(dialog, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(widgets->query.entry));
  if (root && GTK_IS_WINDOW(root)) {
    GtkWindow *parent = GTK_WINDOW(root);
    if (GtkApplication *app = gtk_window_get_application(parent)) {
      gtk_window_set_application(dialog, app);
    }
    gtk_window_set_transient_for(dialog, parent);
  }

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *heading = gtk_label_new(nullptr);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(heading), markup);
  g_free(markup);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_box_append(GTK_BOX(outer), heading);

  GtkWidget *message = gtk_label_new(intro);
  gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_box_append(GTK_BOX(outer), message);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_append(GTK_BOX(outer), scroller);

  // Use a text view so solver and transaction output can be selected and copied.
  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), details.c_str(), -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);

  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(outer), button_box);

  GtkWidget *close_button = gtk_button_new_with_label(_("Close"));
  gtk_box_append(GTK_BOX(button_box), close_button);

  g_signal_connect(close_button,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *button, gpointer) {
                     GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
                     if (root && GTK_IS_WINDOW(root)) {
                       gtk_window_destroy(GTK_WINDOW(root));
                     }
                   }),
                   nullptr);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// DNF5 command reference dialog
// -----------------------------------------------------------------------------
#include "ui/window/dnf_command_reference.hpp"

#include "i18n.hpp"

#include <initializer_list>

namespace {

struct CommandEntry {
  const char *command;
  const char *description;
};

GtkWindow *g_command_reference_window = nullptr;

void
append_command_group(GtkBox *content, const char *title, std::initializer_list<CommandEntry> commands)
{
  GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_box_append(content, group);

  GtkWidget *heading = gtk_label_new(nullptr);
  gchar *heading_markup = g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(heading), heading_markup);
  g_free(heading_markup);
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_box_append(GTK_BOX(group), heading);

  for (const auto &entry : commands) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(group), row);

    GtkWidget *command = gtk_label_new(entry.command);
    gtk_label_set_xalign(GTK_LABEL(command), 0);
    gtk_label_set_selectable(GTK_LABEL(command), TRUE);
    PangoAttrList *attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_family_new("monospace"));
    gtk_label_set_attributes(GTK_LABEL(command), attributes);
    pango_attr_list_unref(attributes);
    gtk_box_append(GTK_BOX(row), command);

    GtkWidget *description = gtk_label_new(entry.description);
    gtk_label_set_xalign(GTK_LABEL(description), 0);
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_widget_add_css_class(description, "dim-label");
    gtk_box_append(GTK_BOX(row), description);
  }
}

} // namespace

void
dnf_command_reference_show(GtkWindow *parent)
{
  if (g_command_reference_window) {
    gtk_window_present(g_command_reference_window);
    return;
  }
  if (!parent) {
    return;
  }

  GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
  g_command_reference_window = dialog;
  gtk_window_set_title(dialog, _("DNF5 Command Reference"));
  gtk_window_set_default_size(dialog, 760, 620);
  if (GtkApplication *app = gtk_window_get_application(parent)) {
    gtk_window_set_application(dialog, app);
  }
  g_signal_connect(
      dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer) { g_command_reference_window = nullptr; }), nullptr);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(outer, 12);
  gtk_widget_set_margin_end(outer, 12);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 12);
  gtk_window_set_child(dialog, outer);

  GtkWidget *intro = gtk_label_new(_("Select command text and press Ctrl+C to copy it. Replace example package names "
                                     "and review proposed changes before confirming."));
  gtk_label_set_xalign(GTK_LABEL(intro), 0);
  gtk_label_set_wrap(GTK_LABEL(intro), TRUE);
  gtk_box_append(GTK_BOX(outer), intro);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_box_append(GTK_BOX(outer), scroller);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), content);

  append_command_group(GTK_BOX(content),
                       _("Inspect packages"),
                       {
                           { "dnf5 search firefox", _("Search packages by name or description.") },
                           { "dnf5 info firefox", _("Show package information.") },
                           { "dnf5 provides /usr/bin/bash", _("Find packages providing a file or capability.") },
                           { "dnf5 repoquery --installed", _("List installed packages.") },
                           { "dnf5 repoquery --unneeded", _("List installed dependencies no longer needed.") },
                           { "dnf5 check-upgrade", _("Check for available package upgrades.") },
                           { "dnf5 repo list", _("List configured repositories.") },
                       });

  append_command_group(
      GTK_BOX(content),
      _("Manage packages"),
      {
          { "sudo dnf5 install firefox", _("Install a package and its dependencies.") },
          { "sudo dnf5 remove firefox", _("Remove a package. Check which dependencies will also be removed.") },
          { "sudo dnf5 upgrade", _("Upgrade installed packages.") },
          { "sudo dnf5 reinstall firefox", _("Reinstall an installed package.") },
          { "sudo dnf5 downgrade firefox", _("Downgrade when an older version is available.") },
          { "sudo dnf5 autoremove", _("Remove unneeded dependencies. Review the proposed removals.") },
      });

  append_command_group(
      GTK_BOX(content),
      _("Upgrade to a new distribution release"),
      {
          { "sudo dnf5 --refresh upgrade",
            _("Update packages in the current release. Back up important data before a release upgrade.") },
          { "sudo dnf5 system-upgrade download --releasever=NN",
            _("Replace NN with the target release number. Use system-upgrade only if your distribution supports it.") },
          { "sudo dnf5 system-upgrade reboot",
            _("Reboot to install the downloaded release upgrade in the offline environment.") },
      });

  append_command_group(GTK_BOX(content),
                       _("Transaction history"),
                       {
                           { "dnf5 history list", _("List recorded transactions.") },
                           { "dnf5 history info 123", _("Show details of transaction 123.") },
                           { "sudo dnf5 history undo 123",
                             _("Attempt to reverse one transaction. Required versions may be unavailable.") },
                           { "sudo dnf5 history rollback 123",
                             _("Attempt to reverse all transactions after 123. This is not a system restore.") },
                       });

  append_command_group(
      GTK_BOX(content),
      _("Advanced CLI operations"),
      {
          { "dnf5 repoquery --whatrequires bash", _("Find packages that require a capability.") },
          { "sudo dnf5 distro-sync", _("Align installed versions with repositories. This may downgrade packages.") },
          { "sudo dnf5 mark user firefox", _("Mark an installed package as explicitly installed by the user.") },
          { "sudo dnf5 mark dependency firefox",
            _("Mark an installed package as a dependency. It may become eligible for automatic removal.") },
          { "sudo dnf5 clean all", _("Remove cached DNF data. Future operations may need to download it again.") },
      });

  GtkWidget *close = gtk_button_new_with_label(_("Close"));
  gtk_widget_set_halign(close, GTK_ALIGN_END);
  g_signal_connect(close,
                   "clicked",
                   G_CALLBACK(+[](GtkButton *, gpointer user_data) { gtk_window_destroy(GTK_WINDOW(user_data)); }),
                   dialog);
  gtk_box_append(GTK_BOX(outer), close);

  gtk_window_present(dialog);
}

// -----------------------------------------------------------------------------
// resource_monitor.cpp
// A separate, low-overhead window for monitoring DNF UI's own resources.
// -----------------------------------------------------------------------------
#include "ui/diagnostics/resource_monitor.hpp"

#include "i18n.hpp"
#include "ui/diagnostics/process_resources.hpp"
#include "ui/diagnostics/system_info.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

struct ResourceMonitorState {
  GtkWindow *window = nullptr;
  GtkLabel *dnfui_version = nullptr;
  GtkLabel *snapshot_time = nullptr;
  GtkLabel *pid = nullptr;
  GtkLabel *os_version = nullptr;
  GtkLabel *kernel_version = nullptr;
  GtkLabel *architecture = nullptr;
  GtkLabel *total_ram = nullptr;
  GtkLabel *cpu_model = nullptr;
  GtkLabel *logical_processors = nullptr;
  GtkLabel *resident = nullptr;
  GtkLabel *peak = nullptr;
  GtkLabel *anonymous = nullptr;
  GtkLabel *file = nullptr;
  GtkLabel *shared = nullptr;
  GtkLabel *memory_scale_label = nullptr;
  GtkProgressBar *memory_bar = nullptr;
  GtkLabel *cpu = nullptr;
  GtkProgressBar *cpu_bar = nullptr;
  ProcessCpuSample previous_cpu;
  bool have_previous_cpu = false;
  guint timer_id = 0;
  std::uint64_t memory_scale = 256ULL * 1024 * 1024;
};

static GtkWindow *g_resource_monitor_window = nullptr;

// -----------------------------------------------------------------------------
// Append a labelled value to a vertical section.
// -----------------------------------------------------------------------------
static GtkLabel *
append_resource_value(GtkWidget *section, const char *title)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *name = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_box_append(GTK_BOX(row), name);

  GtkWidget *value = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(value), 1.0f);
  gtk_box_append(GTK_BOX(row), value);
  gtk_box_append(GTK_BOX(section), row);
  return GTK_LABEL(value);
}

// -----------------------------------------------------------------------------
// Format a byte count using binary units.
// -----------------------------------------------------------------------------
static void
set_memory_value(GtkLabel *label, std::uint64_t bytes)
{
  gchar *text = g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
  gtk_label_set_text(label, text);
  g_free(text);
}

// -----------------------------------------------------------------------------
// Read and update the two bars. This runs only while the window is open.
// -----------------------------------------------------------------------------
static gboolean
update_resource_monitor(gpointer user_data)
{
  auto *state = static_cast<ResourceMonitorState *>(user_data);

  ProcessMemorySample memory;
  if (process_resources_read_memory(memory)) {
    while (state->memory_scale < memory.resident_bytes &&
           state->memory_scale <= std::numeric_limits<std::uint64_t>::max() / 2) {
      state->memory_scale *= 2;
    }

    set_memory_value(state->resident, memory.resident_bytes);
    set_memory_value(state->peak, memory.peak_bytes);
    set_memory_value(state->anonymous, memory.anonymous_bytes);
    set_memory_value(state->file, memory.file_bytes);
    set_memory_value(state->shared, memory.shared_bytes);
    set_memory_value(state->memory_scale_label, state->memory_scale);
    gtk_progress_bar_set_fraction(state->memory_bar,
                                  std::min(1.0, static_cast<double>(memory.resident_bytes) / state->memory_scale));
  } else {
    gtk_label_set_text(state->resident, _("Unavailable"));
    gtk_label_set_text(state->peak, _("Unavailable"));
    gtk_label_set_text(state->anonymous, _("Unavailable"));
    gtk_label_set_text(state->file, _("Unavailable"));
    gtk_label_set_text(state->shared, _("Unavailable"));
    gtk_progress_bar_set_fraction(state->memory_bar, 0.0);
  }

  ProcessCpuSample cpu;
  if (process_resources_read_cpu(cpu)) {
    if (state->have_previous_cpu) {
      const double usage =
          process_resources_cpu_fraction(state->previous_cpu, cpu, static_cast<unsigned int>(g_get_num_processors()));
      gchar *text = g_strdup_printf("%.1f%%", usage * 100.0);
      gtk_label_set_text(state->cpu, text);
      g_free(text);
      gtk_progress_bar_set_fraction(state->cpu_bar, usage);
    } else {
      gtk_label_set_text(state->cpu, _("Measuring..."));
    }
    state->previous_cpu = cpu;
    state->have_previous_cpu = true;
  } else {
    state->have_previous_cpu = false;
    gtk_label_set_text(state->cpu, _("Unavailable"));
    gtk_progress_bar_set_fraction(state->cpu_bar, 0.0);
  }

  GDateTime *now = g_date_time_new_now_local();
  gchar *timestamp = g_date_time_format_iso8601(now);
  gtk_label_set_text(state->snapshot_time, timestamp);
  g_free(timestamp);
  g_date_time_unref(now);

  return G_SOURCE_CONTINUE;
}

// -----------------------------------------------------------------------------
// Copy the readings currently shown in the window.
// -----------------------------------------------------------------------------
static void
on_resource_monitor_copy_clicked(GtkButton *button, gpointer user_data)
{
  auto *state = static_cast<ResourceMonitorState *>(user_data);
  GString *snapshot = g_string_new(_("Resource Monitor"));

  auto append_reading = [snapshot](const char *name, GtkLabel *value) {
    g_string_append_printf(snapshot, "%s: %s\n", name, gtk_label_get_text(value));
  };

  g_string_append_printf(snapshot, "\n\n%s\n", _("System Information"));
  append_reading(_("DNF UI version"), state->dnfui_version);
  append_reading(_("Snapshot time"), state->snapshot_time);
  append_reading(_("PID"), state->pid);
  append_reading(_("OS version"), state->os_version);
  append_reading(_("Kernel version"), state->kernel_version);
  append_reading(_("Architecture"), state->architecture);
  append_reading(_("Total RAM"), state->total_ram);
  append_reading(_("CPU model"), state->cpu_model);
  append_reading(_("Logical processors"), state->logical_processors);

  g_string_append_printf(snapshot, "\n%s\n", _("Memory (DNF UI only)"));
  append_reading(_("Resident memory (RSS)"), state->resident);
  append_reading(_("Peak RSS"), state->peak);
  append_reading(_("Anonymous memory"), state->anonymous);
  append_reading(_("File-backed memory"), state->file);
  append_reading(_("Shared memory"), state->shared);
  g_string_append_printf(snapshot, "\n%s\n", _("CPU"));
  append_reading(_("Process CPU"), state->cpu);
  g_string_append_printf(snapshot, "%s\n", _("Percentage of total logical CPU capacity."));

  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), snapshot->str);
  g_string_free(snapshot, TRUE);
}

// -----------------------------------------------------------------------------
// Stop periodic sampling before the window state is released.
// -----------------------------------------------------------------------------
static void
on_resource_monitor_destroy(GtkWidget *, gpointer user_data)
{
  auto *state = static_cast<ResourceMonitorState *>(user_data);
  if (state->timer_id != 0) {
    g_source_remove(state->timer_id);
    state->timer_id = 0;
  }
  if (g_resource_monitor_window == state->window) {
    g_resource_monitor_window = nullptr;
  }
}

// -----------------------------------------------------------------------------
// Show a single non-modal monitor alongside the main window.
// -----------------------------------------------------------------------------
void
resource_monitor_show_window(GtkWindow *parent)
{
  if (g_resource_monitor_window) {
    gtk_window_present(g_resource_monitor_window);
    return;
  }

  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  auto *state = new ResourceMonitorState;
  state->window = window;
  g_resource_monitor_window = window;

  gtk_window_set_title(window, _("Resource Monitor"));
  gtk_window_set_default_size(window, 460, 540);
  gtk_window_set_modal(window, FALSE);
  gtk_window_set_resizable(window, TRUE);
  if (parent) {
    gtk_window_set_application(window, gtk_window_get_application(parent));
  }

  g_object_set_data_full(G_OBJECT(window), "dnfui-resource-monitor-state", state, [](gpointer p) {
    delete static_cast<ResourceMonitorState *>(p);
  });
  g_signal_connect(window, "destroy", G_CALLBACK(on_resource_monitor_destroy), state);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_hexpand(root, TRUE);
  gtk_widget_set_vexpand(root, TRUE);
  gtk_widget_set_margin_start(root, 16);
  gtk_widget_set_margin_end(root, 16);
  gtk_widget_set_margin_top(root, 16);
  gtk_widget_set_margin_bottom(root, 16);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *window_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(window, window_box);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_box_append(GTK_BOX(window_box), scrolled);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), root);

  const SystemInfo system = system_info_read();
  GtkWidget *system_frame = gtk_frame_new(_("System Information"));
  gtk_box_append(GTK_BOX(root), system_frame);
  GtkWidget *system_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(system_box, 12);
  gtk_widget_set_margin_end(system_box, 12);
  gtk_widget_set_margin_top(system_box, 12);
  gtk_widget_set_margin_bottom(system_box, 12);
  gtk_frame_set_child(GTK_FRAME(system_frame), system_box);

  state->dnfui_version = append_resource_value(system_box, _("DNF UI version"));
  state->snapshot_time = append_resource_value(system_box, _("Snapshot time"));
  state->pid = append_resource_value(system_box, _("PID"));
  state->os_version = append_resource_value(system_box, _("OS version"));
  state->kernel_version = append_resource_value(system_box, _("Kernel version"));
  state->architecture = append_resource_value(system_box, _("Architecture"));
  state->total_ram = append_resource_value(system_box, _("Total RAM"));
  state->cpu_model = append_resource_value(system_box, _("CPU model"));
  state->logical_processors = append_resource_value(system_box, _("Logical processors"));

  gtk_label_set_wrap(state->os_version, TRUE);
  gtk_label_set_max_width_chars(state->os_version, 36);
  gtk_label_set_wrap(state->cpu_model, TRUE);
  gtk_label_set_max_width_chars(state->cpu_model, 36);

  gtk_label_set_text(state->dnfui_version, DNFUI_VERSION);
  const std::string pid = std::to_string(system.pid);
  gtk_label_set_text(state->pid, pid.c_str());
  gtk_label_set_text(state->os_version, system.os_version.empty() ? _("Unavailable") : system.os_version.c_str());
  gtk_label_set_text(state->kernel_version,
                     system.kernel_version.empty() ? _("Unavailable") : system.kernel_version.c_str());
  gtk_label_set_text(state->architecture, system.architecture.empty() ? _("Unavailable") : system.architecture.c_str());
  if (system.total_ram_bytes != 0) {
    set_memory_value(state->total_ram, system.total_ram_bytes);
  } else {
    gtk_label_set_text(state->total_ram, _("Unavailable"));
  }
  gtk_label_set_text(state->cpu_model, system.cpu_model.empty() ? _("Unavailable") : system.cpu_model.c_str());
  if (system.logical_processors != 0) {
    const std::string count = std::to_string(system.logical_processors);
    gtk_label_set_text(state->logical_processors, count.c_str());
  } else {
    gtk_label_set_text(state->logical_processors, _("Unavailable"));
  }

  GtkWidget *memory_frame = gtk_frame_new(_("Memory (DNF UI only)"));
  gtk_box_append(GTK_BOX(root), memory_frame);
  GtkWidget *memory_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(memory_box, 12);
  gtk_widget_set_margin_end(memory_box, 12);
  gtk_widget_set_margin_top(memory_box, 12);
  gtk_widget_set_margin_bottom(memory_box, 12);
  gtk_frame_set_child(GTK_FRAME(memory_frame), memory_box);

  state->resident = append_resource_value(memory_box, _("Resident memory (RSS)"));
  state->memory_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
  gtk_box_append(GTK_BOX(memory_box), GTK_WIDGET(state->memory_bar));
  state->memory_scale_label = append_resource_value(memory_box, _("Memory scale"));
  state->peak = append_resource_value(memory_box, _("Peak RSS"));
  state->anonymous = append_resource_value(memory_box, _("Anonymous memory"));
  state->file = append_resource_value(memory_box, _("File-backed memory"));
  state->shared = append_resource_value(memory_box, _("Shared memory"));

  GtkWidget *cpu_frame = gtk_frame_new(_("CPU"));
  gtk_box_append(GTK_BOX(root), cpu_frame);
  GtkWidget *cpu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(cpu_box, 12);
  gtk_widget_set_margin_end(cpu_box, 12);
  gtk_widget_set_margin_top(cpu_box, 12);
  gtk_widget_set_margin_bottom(cpu_box, 12);
  gtk_frame_set_child(GTK_FRAME(cpu_frame), cpu_box);

  state->cpu = append_resource_value(cpu_box, _("Process CPU"));
  state->cpu_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
  gtk_box_append(GTK_BOX(cpu_box), GTK_WIDGET(state->cpu_bar));
  GtkWidget *cpu_note = gtk_label_new(_("Percentage of total logical CPU capacity."));
  gtk_label_set_xalign(GTK_LABEL(cpu_note), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(cpu_note), TRUE);
  gtk_box_append(GTK_BOX(cpu_box), cpu_note);

  GtkWidget *copy_button = gtk_button_new_with_label(_("Copy Snapshot"));
  gtk_widget_set_halign(copy_button, GTK_ALIGN_END);
  gtk_widget_set_margin_end(copy_button, 16);
  gtk_widget_set_margin_bottom(copy_button, 16);
  g_signal_connect(copy_button, "clicked", G_CALLBACK(on_resource_monitor_copy_clicked), state);
  gtk_box_append(GTK_BOX(window_box), copy_button);

  update_resource_monitor(state);
  state->timer_id = g_timeout_add(500, update_resource_monitor, state);
  gtk_window_present(window);
}

// -----------------------------------------------------------------------------
// Close the resource monitor when the main application window closes.
// -----------------------------------------------------------------------------
void
resource_monitor_close_window()
{
  if (g_resource_monitor_window) {
    gtk_window_close(g_resource_monitor_window);
  }
}

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------

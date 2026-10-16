#include "ui/common/color_scheme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

// -----------------------------------------------------------------------------
// Stop a notification loop before it can hang the test process.
// -----------------------------------------------------------------------------
static void
count_color_scheme_notifications(GObject *, GParamSpec *, gpointer user_data)
{
  auto *count = static_cast<unsigned *>(user_data);
  ++*count;
  g_assert_cmpuint(*count, <, 128);
}

// -----------------------------------------------------------------------------
// Check the actual themed window background, including its opacity.
// -----------------------------------------------------------------------------
static void
check_window_background(GtkWidget *window, bool dark)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gtk_snapshot_render_background(snapshot, gtk_widget_get_style_context(window), 0, 0, 100, 100);
  G_GNUC_END_IGNORE_DEPRECATIONS
  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
  REQUIRE(node != nullptr);

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
  cairo_t *cr = cairo_create(surface);
  gsk_render_node_draw(node, cr);
  cairo_surface_flush(surface);
  const auto *pixels = reinterpret_cast<const guint32 *>(cairo_image_surface_get_data(surface));
  const guint32 pixel = pixels[50 * cairo_image_surface_get_stride(surface) / 4 + 50];
  const unsigned alpha = (pixel >> 24) & 0xff;
  const unsigned red = (pixel >> 16) & 0xff;
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  gsk_render_node_unref(node);

  CHECK(alpha == 255);
  CHECK((red < 128) == dark);
}

// -----------------------------------------------------------------------------
// Reproduce a system notification separately from the GNOME preference change.
// -----------------------------------------------------------------------------
static void
set_system_color_scheme(GtkSettings *settings, const char *scheme)
{
#if GTK_CHECK_VERSION(4, 20, 0)
  GtkInterfaceColorScheme color_scheme = GTK_INTERFACE_COLOR_SCHEME_LIGHT;
  if (g_strcmp0(scheme, "prefer-dark") == 0) {
    color_scheme = GTK_INTERFACE_COLOR_SCHEME_DARK;
  }
  g_object_set(settings, "gtk-interface-color-scheme", color_scheme, nullptr);
#else
  (void)settings;
  (void)scheme;
#endif
}

TEST_CASE("GNOME color scheme setup returns and follows appearance changes", "[color-scheme]")
{
  if (!gtk_init_check()) {
    REQUIRE(g_getenv("DNFUI_TEST_REQUIRE_DISPLAY") == nullptr);
    SKIP("A GTK display is required for color scheme tests.");
  }

  GtkSettings *gtk_settings = gtk_settings_get_default();
  g_object_set(gtk_settings, "gtk-theme-name", "Adwaita", "gtk-enable-animations", FALSE, nullptr);
  GSettings *settings = g_settings_new("org.gnome.desktop.interface");
  const char *initial_scheme = "prefer-dark";
  bool system_notification_first = false;
  SECTION("Start with a dark preference")
  {
    initial_scheme = "prefer-dark";
  }
  SECTION("Start with a light preference")
  {
    initial_scheme = "prefer-light";
  }
  SECTION("GTK notification arrives first")
  {
    system_notification_first = true;
  }
  g_settings_set_string(settings, "color-scheme", initial_scheme);

  unsigned notifications = 0;
  gulong handler = g_signal_connect(
      gtk_settings, "notify::gtk-interface-color-scheme", G_CALLBACK(count_color_scheme_notifications), &notifications);
  GtkApplication *app = gtk_application_new("io.github.dnfui.themetest", G_APPLICATION_NON_UNIQUE);
  setup_color_scheme(app);
  GtkWidget *window = gtk_window_new();
  check_window_background(window, g_strcmp0(initial_scheme, "prefer-dark") == 0);

  for (unsigned cycle = 0; cycle < 5; ++cycle) {
    for (const char *scheme : { "prefer-light", "prefer-dark", "default" }) {
      if (system_notification_first) {
        set_system_color_scheme(gtk_settings, scheme);
      }
      g_settings_set_string(settings, "color-scheme", scheme);
      if (!system_notification_first) {
        set_system_color_scheme(gtk_settings, scheme);
      }
      while (g_main_context_iteration(nullptr, FALSE)) { }
      check_window_background(window, g_strcmp0(scheme, "prefer-dark") == 0);
    }
  }

  gtk_window_destroy(GTK_WINDOW(window));
  g_object_unref(app);
  g_object_unref(settings);
  g_signal_handler_disconnect(gtk_settings, handler);
}

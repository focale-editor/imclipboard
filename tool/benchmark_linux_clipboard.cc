// Component benchmark using a private X11 selection, never the user's
// clipboard. Build/run commands and measurement limits are in tool/README.md.
#include <gdk/gdkx.h>

#include <cstdio>
#include <cstring>

#include "clipboard_service.h"

namespace {
void Wait(const std::function<bool()>& ready) {
  while (!ready()) g_main_context_iteration(nullptr, TRUE);
}

struct Heartbeat {
  gint64 previous = g_get_monotonic_time();
  gint64 maximum_gap = 0;
  guint ticks = 0;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || !gtk_init_check(nullptr, nullptr) ||
      !GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
    std::fprintf(
        stderr,
        "Usage: GDK_BACKEND=x11 benchmark_linux_clipboard image-file\n");
    return 1;
  }
  g_autoptr(GError) error = nullptr;
  g_autoptr(GdkPixbuf) source = gdk_pixbuf_new_from_file(argv[1], &error);
  if (!source) {
    std::fprintf(stderr, "%s\n", error->message);
    return 1;
  }
  g_autoptr(GdkPixbuf) rgba = gdk_pixbuf_add_alpha(source, FALSE, 0, 0, 0);
  gchar* encoded = nullptr;
  gsize length = 0;
  if (!gdk_pixbuf_save_to_buffer(rgba, &encoded, &length, "png", &error,
                                 "compression", "1", nullptr))
    return 1;
  imclipboard::Bytes input(g_bytes_new_take(encoded, length), g_bytes_unref);
  std::printf("dimensions=%dx%d png_bytes=%zu\n", gdk_pixbuf_get_width(rgba),
              gdk_pixbuf_get_height(rgba), length);
  const std::string name =
      "IMCLIPBOARD_BENCH_" + std::to_string(g_get_monotonic_time());
  GtkClipboard* clipboard =
      gtk_clipboard_get(gdk_atom_intern(name.c_str(), FALSE));
  auto service = std::make_shared<imclipboard::ClipboardService>(clipboard);
  for (int round = 0; round < 3; ++round) {
    // Original Linux path: decode PNG, then re-encode when GTK supplies a
    // target.
    const gint64 before = g_get_monotonic_time();
    g_autoptr(GdkPixbufLoader) loader =
        gdk_pixbuf_loader_new_with_type("png", nullptr);
    if (!gdk_pixbuf_loader_write(loader, reinterpret_cast<guint8*>(encoded),
                                 length, &error) ||
        !gdk_pixbuf_loader_close(loader, &error))
      return 1;
    gchar* output = nullptr;
    gsize output_length = 0;
    if (!gdk_pixbuf_save_to_buffer(gdk_pixbuf_loader_get_pixbuf(loader),
                                   &output, &output_length, "png", &error,
                                   nullptr))
      return 1;
    const double old_ms = (g_get_monotonic_time() - before) / 1000.;
    g_free(output);
    g_clear_object(&loader);

    Heartbeat heartbeat;
    const guint timer = g_timeout_add(
        1,
        [](gpointer data) -> gboolean {
          auto* beat = static_cast<Heartbeat*>(data);
          const gint64 now = g_get_monotonic_time();
          beat->maximum_gap = std::max(beat->maximum_gap, now - beat->previous);
          beat->previous = now;
          ++beat->ticks;
          return G_SOURCE_CONTINUE;
        },
        &heartbeat);
    const gint64 start = g_get_monotonic_time();
    bool done = false;
    bool valid = true;
    // Include the native method-channel input's owned copy in the measurement.
    service->Write(
        imclipboard::Bytes(g_bytes_new(encoded, length), g_bytes_unref),
        "benchmark", [&](std::string failure) {
          valid = failure.empty();
          done = true;
        });
    Wait([&] { return done; });
    if (!valid) return 1;
    const double write_ms = (g_get_monotonic_time() - start) / 1000.;
    const gint64 supply_start = g_get_monotonic_time();
    GtkSelectionData* supplied = gtk_clipboard_wait_for_contents(
        clipboard, gdk_atom_intern_static_string("image/png"));
    if (supplied == nullptr ||
        gtk_selection_data_get_length(supplied) != static_cast<int>(length) ||
        std::memcmp(gtk_selection_data_get_data(supplied), encoded, length) !=
            0)
      return 1;
    const double supply_ms = (g_get_monotonic_time() - supply_start) / 1000.;
    gtk_selection_data_free(supplied);
    g_source_remove(timer);
    const gint64 info_start = g_get_monotonic_time();
    done = false;
    service->Read(false, [&](imclipboard::ImageReply reply) {
      valid = reply.image != nullptr;
      done = true;
    });
    Wait([&] { return done; });
    if (!valid) return 1;
    const double info_ms = (g_get_monotonic_time() - info_start) / 1000.;
    std::printf(
        "round=%d old_decode_encode_ms=%.3f write_ms=%.3f supply_ms=%.3f "
        "info_ms=%.3f heartbeat_max_gap_ms=%.3f ticks=%u\n",
        round, old_ms, write_ms, supply_ms, info_ms,
        heartbeat.maximum_gap / 1000., heartbeat.ticks);
    std::fflush(stdout);
  }
  gtk_clipboard_clear(clipboard);
  return 0;
}

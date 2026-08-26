#include "include/imclipboard/imclipboard_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>

#define IMCLIPBOARD_PLUGIN(obj)                                      \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), imclipboard_plugin_get_type(), \
                              ImclipboardPlugin))

namespace {

constexpr char kChannelName[] = "app.focaleeditor.imclipboard/image_clipboard";
constexpr char kTokenTarget[] = "application/x-imclipboard-token";
constexpr gsize kMaximumEncodedBytes = 512ULL * 1024ULL * 1024ULL;
constexpr gsize kMaximumFileCount = 32;
constexpr gsize kMaximumFilePathBytes = 32ULL * 1024ULL;

struct ClipboardPayload {
  GdkPixbuf* pixbuf;
  gchar* token;
};

GtkClipboard* SystemClipboard() {
  return gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
}

gchar* ReadToken(GtkClipboard* clipboard) {
  const GdkAtom target = gdk_atom_intern_static_string(kTokenTarget);
  GtkSelectionData* selection =
      gtk_clipboard_wait_for_contents(clipboard, target);
  if (selection == nullptr) {
    return nullptr;
  }
  const gint length = gtk_selection_data_get_length(selection);
  const guchar* data = gtk_selection_data_get_data(selection);
  gchar* token =
      length > 0 && length <= 1024 && data != nullptr
          ? g_strndup(reinterpret_cast<const gchar*>(data), length)
          : nullptr;
  gtk_selection_data_free(selection);
  return token;
}

FlValue* ImageResult(GdkPixbuf* pixbuf,
                     const gchar* token,
                     const guint8* png_data = nullptr,
                     gsize png_length = 0) {
  FlValue* result = fl_value_new_map();
  fl_value_set_string_take(
      result, "width", fl_value_new_int(gdk_pixbuf_get_width(pixbuf)));
  fl_value_set_string_take(
      result, "height", fl_value_new_int(gdk_pixbuf_get_height(pixbuf)));
  if (token != nullptr) {
    fl_value_set_string_take(result, "token", fl_value_new_string(token));
  }
  if (png_data != nullptr && png_length > 0) {
    fl_value_set_string_take(
        result, "bytes", fl_value_new_uint8_list(png_data, png_length));
  }
  return result;
}

// Resolves one local regular-file URI to filesystem and UTF-8 paths.
gboolean LocalFileFromUri(const gchar* uri,
                          gchar** filename_out,
                          gchar** utf8_filename_out) {
  g_autofree gchar* hostname = nullptr;
  g_autofree gchar* filename = g_filename_from_uri(uri, &hostname, nullptr);
  const gboolean local_host =
      hostname == nullptr || hostname[0] == '\0' ||
      g_ascii_strcasecmp(hostname, "localhost") == 0;
  g_autofree gchar* utf8_filename =
      filename == nullptr
          ? nullptr
          : g_filename_to_utf8(filename, -1, nullptr, nullptr, nullptr);
  if (filename == nullptr || utf8_filename == nullptr || !local_host ||
      !g_path_is_absolute(filename) ||
      std::strlen(utf8_filename) > kMaximumFilePathBytes ||
      !g_file_test(filename, G_FILE_TEST_IS_REGULAR)) {
    return FALSE;
  }
  *filename_out = g_steal_pointer(&filename);
  *utf8_filename_out = g_steal_pointer(&utf8_filename);
  return TRUE;
}

FlValue* ReadLocalFiles(GtkClipboard* clipboard) {
  FlValue* result = fl_value_new_list();
  g_auto(GStrv) uris = gtk_clipboard_wait_for_uris(clipboard);
  if (uris == nullptr) {
    return result;
  }

  for (gsize index = 0;
       uris[index] != nullptr && fl_value_get_length(result) < kMaximumFileCount;
       ++index) {
    g_autofree gchar* filename = nullptr;
    g_autofree gchar* utf8_filename = nullptr;
    if (!LocalFileFromUri(uris[index], &filename, &utf8_filename)) {
      continue;
    }
    fl_value_append_take(result, fl_value_new_string(utf8_filename));
  }
  return result;
}

// Reads an advertised image or loads the first readable copied image file.
GdkPixbuf* ReadClipboardImage(GtkClipboard* clipboard) {
  GdkPixbuf* pixbuf = gtk_clipboard_wait_for_image(clipboard);
  if (pixbuf != nullptr) {
    return pixbuf;
  }

  g_auto(GStrv) uris = gtk_clipboard_wait_for_uris(clipboard);
  if (uris == nullptr) {
    return nullptr;
  }
  for (gsize index = 0;
       uris[index] != nullptr && index < kMaximumFileCount; ++index) {
    g_autofree gchar* filename = nullptr;
    g_autofree gchar* utf8_filename = nullptr;
    if (!LocalFileFromUri(uris[index], &filename, &utf8_filename)) {
      continue;
    }
    g_autoptr(GError) error = nullptr;
    pixbuf = gdk_pixbuf_new_from_file(filename, &error);
    if (pixbuf != nullptr) {
      return pixbuf;
    }
  }
  return nullptr;
}

void ProvideClipboardData(GtkClipboard* clipboard,
                          GtkSelectionData* selection,
                          guint info,
                          gpointer user_data) {
  auto* payload = static_cast<ClipboardPayload*>(user_data);
  if (info == 0) {
    gtk_selection_data_set_pixbuf(selection, payload->pixbuf);
    return;
  }
  if (payload->token != nullptr) {
    gtk_selection_data_set(
        selection, gtk_selection_data_get_target(selection), 8,
        reinterpret_cast<const guchar*>(payload->token),
        static_cast<gint>(std::strlen(payload->token)));
  }
}

void ClearClipboardData(GtkClipboard* clipboard, gpointer user_data) {
  auto* payload = static_cast<ClipboardPayload*>(user_data);
  g_object_unref(payload->pixbuf);
  g_free(payload->token);
  delete payload;
}

GdkPixbuf* DecodePng(FlValue* bytes, GError** error) {
  if (bytes == nullptr ||
      fl_value_get_type(bytes) != FL_VALUE_TYPE_UINT8_LIST) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Missing PNG bytes");
    return nullptr;
  }
  g_autoptr(GdkPixbufLoader) loader =
      gdk_pixbuf_loader_new_with_type("png", error);
  if (loader == nullptr) {
    return nullptr;
  }
  const guint8* data = fl_value_get_uint8_list(bytes);
  const gsize length = fl_value_get_length(bytes);
  if (length == 0 || length > kMaximumEncodedBytes ||
      !gdk_pixbuf_loader_write(loader, data, length, error) ||
      !gdk_pixbuf_loader_close(loader, error)) {
    return nullptr;
  }
  GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
  return pixbuf == nullptr ? nullptr : GDK_PIXBUF(g_object_ref(pixbuf));
}

void RespondError(FlMethodCall* call, const gchar* code, GError* error) {
  fl_method_call_respond_error(
      call, code,
      error == nullptr ? "System clipboard operation failed" : error->message,
      nullptr, nullptr);
}

void HandleMethodCall(FlMethodCall* call) {
  const gchar* method = fl_method_call_get_name(call);
  if (std::strcmp(method, "isSupported") == 0) {
    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    fl_method_call_respond_success(call, result, nullptr);
    return;
  }

  GtkClipboard* clipboard = SystemClipboard();
  if (std::strcmp(method, "readImageFiles") == 0) {
    g_autoptr(FlValue) result = ReadLocalFiles(clipboard);
    fl_method_call_respond_success(call, result, nullptr);
    return;
  }

  if (std::strcmp(method, "readImageInfo") == 0 ||
      std::strcmp(method, "readImage") == 0) {
    g_autoptr(GdkPixbuf) pixbuf = ReadClipboardImage(clipboard);
    if (pixbuf == nullptr) {
      fl_method_call_respond_success(call, nullptr, nullptr);
      return;
    }
    g_autofree gchar* token = ReadToken(clipboard);
    if (std::strcmp(method, "readImageInfo") == 0) {
      g_autoptr(FlValue) result = ImageResult(pixbuf, token);
      fl_method_call_respond_success(call, result, nullptr);
      return;
    }

    g_autofree gchar* png_data = nullptr;
    gsize png_length = 0;
    g_autoptr(GError) error = nullptr;
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &png_data, &png_length, "png",
                                   &error, nullptr)) {
      RespondError(call, "encode_failed", error);
      return;
    }
    if (png_length > kMaximumEncodedBytes) {
      fl_method_call_respond_error(call, "image_too_large",
                                   "The clipboard PNG exceeds 512 MiB", nullptr,
                                   nullptr);
      return;
    }
    g_autoptr(FlValue) result =
        ImageResult(pixbuf, token,
                    reinterpret_cast<const guint8*>(png_data), png_length);
    fl_method_call_respond_success(call, result, nullptr);
    return;
  }

  if (std::strcmp(method, "writeImage") == 0) {
    FlValue* arguments = fl_method_call_get_args(call);
    if (arguments == nullptr ||
        fl_value_get_type(arguments) != FL_VALUE_TYPE_MAP) {
      fl_method_call_respond_error(call, "invalid_arguments",
                                   "Expected a map of clipboard arguments",
                                   nullptr, nullptr);
      return;
    }
    FlValue* bytes = fl_value_lookup_string(arguments, "bytes");
    FlValue* token = fl_value_lookup_string(arguments, "token");
    const gchar* token_string = nullptr;
    if (token != nullptr) {
      if (fl_value_get_type(token) != FL_VALUE_TYPE_STRING ||
          std::strlen(fl_value_get_string(token)) == 0 ||
          std::strlen(fl_value_get_string(token)) > 1024) {
        fl_method_call_respond_error(call, "invalid_arguments",
                                     "The clipboard token is invalid", nullptr,
                                     nullptr);
        return;
      }
      token_string = fl_value_get_string(token);
    }

    g_autoptr(GError) error = nullptr;
    GdkPixbuf* pixbuf = DecodePng(bytes, &error);
    if (pixbuf == nullptr) {
      RespondError(call, "decode_failed", error);
      return;
    }
    auto* payload =
        new ClipboardPayload{pixbuf, g_strdup(token_string)};
    static GtkTargetEntry targets[] = {
        {const_cast<gchar*>("image/png"), 0, 0},
        {const_cast<gchar*>(kTokenTarget), 0, 1},
    };
    const guint target_count = token_string == nullptr ? 1 : 2;
    if (!gtk_clipboard_set_with_data(
            clipboard, targets, target_count, ProvideClipboardData,
            ClearClipboardData, payload)) {
      ClearClipboardData(clipboard, payload);
      fl_method_call_respond_error(call, "write_failed",
                                   "GTK refused clipboard ownership", nullptr,
                                   nullptr);
      return;
    }
    gtk_clipboard_set_can_store(clipboard, targets, target_count);
    gtk_clipboard_store(clipboard);
    fl_method_call_respond_success(call, nullptr, nullptr);
    return;
  }

  fl_method_call_respond_not_implemented(call, nullptr);
}

}  // namespace

struct _ImclipboardPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(ImclipboardPlugin, imclipboard_plugin, g_object_get_type())

static void imclipboard_plugin_class_init(ImclipboardPluginClass* klass) {}

static void imclipboard_plugin_init(ImclipboardPlugin* self) {}

static void MethodCallCallback(FlMethodChannel* channel,
                               FlMethodCall* method_call,
                               gpointer user_data) {
  HandleMethodCall(method_call);
}

void imclipboard_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  ImclipboardPlugin* plugin = IMCLIPBOARD_PLUGIN(
      g_object_new(imclipboard_plugin_get_type(), nullptr));
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar), kChannelName,
      FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      channel, MethodCallCallback, g_object_ref(plugin), g_object_unref);
  g_object_unref(plugin);
}

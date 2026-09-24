#include "include/imclipboard/imclipboard_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>
#include <memory>

#include "clipboard_service.h"

#define IMCLIPBOARD_PLUGIN(obj)                                     \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), imclipboard_plugin_get_type(), \
                              ImclipboardPlugin))

struct _ImclipboardPlugin {
  GObject parent_instance;
  std::shared_ptr<imclipboard::ClipboardService>* service;
};

G_DEFINE_TYPE(ImclipboardPlugin, imclipboard_plugin, G_TYPE_OBJECT)

namespace {
constexpr char kChannelName[] = "app.focaleeditor.imclipboard/image_clipboard";
constexpr gsize kMaximumEncodedBytes = 512ULL * 1024ULL * 1024ULL;

// Keeps a pending method response alive until its asynchronous operation ends.
using Call = std::shared_ptr<FlMethodCall>;

void HandleMethodCall(ImclipboardPlugin* plugin, FlMethodCall* raw_call) {
  const gchar* method = fl_method_call_get_name(raw_call);
  if (std::strcmp(method, "isSupported") == 0) {
    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    fl_method_call_respond_success(raw_call, result, nullptr);
    return;
  }
  if (!*plugin->service) {
    *plugin->service = std::make_shared<imclipboard::ClipboardService>(
        gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
  }
  Call call(FL_METHOD_CALL(g_object_ref(raw_call)),
            [](FlMethodCall* call) { g_object_unref(call); });
  if (std::strcmp(method, "readFiles") == 0) {
    (*plugin->service)->ReadFiles([call](std::vector<std::string> files) {
      g_autoptr(FlValue) result = fl_value_new_list();
      for (const auto& path : files)
        fl_value_append_take(result, fl_value_new_string(path.c_str()));
      fl_method_call_respond_success(call.get(), result, nullptr);
    });
    return;
  }
  if (std::strcmp(method, "readImageInfo") == 0 ||
      std::strcmp(method, "readImage") == 0) {
    const bool include_png = std::strcmp(method, "readImage") == 0;
    (*plugin->service)
        ->Read(include_png, [call, include_png](imclipboard::ImageReply reply) {
          if (!reply.error.empty()) {
            fl_method_call_respond_error(call.get(), "read_failed",
                                         reply.error.c_str(), nullptr, nullptr);
            return;
          }
          if (!reply.image) {
            fl_method_call_respond_success(call.get(), nullptr, nullptr);
            return;
          }
          g_autoptr(FlValue) result = fl_value_new_map();
          fl_value_set_string_take(result, "width",
                                   fl_value_new_int(reply.image->width));
          fl_value_set_string_take(result, "height",
                                   fl_value_new_int(reply.image->height));
          if (!reply.image->token.empty())
            fl_value_set_string_take(
                result, "token",
                fl_value_new_string(reply.image->token.c_str()));
          if (include_png) {
            gsize length = 0;
            const auto* data = static_cast<const guint8*>(
                g_bytes_get_data(reply.image->png.get(), &length));
            fl_value_set_string_take(result, "bytes",
                                     fl_value_new_uint8_list(data, length));
          }
          fl_method_call_respond_success(call.get(), result, nullptr);
        });
    return;
  }
  if (std::strcmp(method, "writeImage") == 0) {
    FlValue* arguments = fl_method_call_get_args(raw_call);
    if (arguments == nullptr ||
        fl_value_get_type(arguments) != FL_VALUE_TYPE_MAP) {
      fl_method_call_respond_error(raw_call, "invalid_arguments",
                                   "Expected clipboard arguments", nullptr,
                                   nullptr);
      return;
    }
    FlValue* bytes = fl_value_lookup_string(arguments, "bytes");
    FlValue* token = fl_value_lookup_string(arguments, "token");
    if (bytes == nullptr ||
        fl_value_get_type(bytes) != FL_VALUE_TYPE_UINT8_LIST ||
        fl_value_get_length(bytes) == 0 ||
        fl_value_get_length(bytes) > kMaximumEncodedBytes) {
      fl_method_call_respond_error(raw_call, "invalid_arguments",
                                   "Expected a PNG smaller than 512 MiB",
                                   nullptr, nullptr);
      return;
    }
    if (token != nullptr && (fl_value_get_type(token) != FL_VALUE_TYPE_STRING ||
                             std::strlen(fl_value_get_string(token)) == 0 ||
                             std::strlen(fl_value_get_string(token)) > 1024)) {
      fl_method_call_respond_error(raw_call, "invalid_arguments",
                                   "The clipboard token is invalid", nullptr,
                                   nullptr);
      return;
    }
    FlValue* generated = fl_value_lookup_string(arguments, "generatedPng");
    const bool generated_png = generated != nullptr && fl_value_get_type(generated) == FL_VALUE_TYPE_BOOL && fl_value_get_bool(generated);
    imclipboard::Bytes png(
        g_bytes_new(fl_value_get_uint8_list(bytes), fl_value_get_length(bytes)),
        g_bytes_unref);
    (*plugin->service)
        ->Write(
            std::move(png), token == nullptr ? "" : fl_value_get_string(token),
            [call](std::string error) {
              if (error.empty())
                fl_method_call_respond_success(call.get(), nullptr, nullptr);
              else
                fl_method_call_respond_error(call.get(), "write_failed",
                                             error.c_str(), nullptr, nullptr);
            }, generated_png);
    return;
  }
  fl_method_call_respond_not_implemented(raw_call, nullptr);
}
}  // namespace

static void imclipboard_plugin_dispose(GObject* object) {
  auto* plugin = IMCLIPBOARD_PLUGIN(object);
  delete plugin->service;
  plugin->service = nullptr;
  G_OBJECT_CLASS(imclipboard_plugin_parent_class)->dispose(object);
}

static void imclipboard_plugin_class_init(ImclipboardPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = imclipboard_plugin_dispose;
}

static void imclipboard_plugin_init(ImclipboardPlugin* plugin) {
  plugin->service = new std::shared_ptr<imclipboard::ClipboardService>();
}

void imclipboard_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  ImclipboardPlugin* plugin =
      IMCLIPBOARD_PLUGIN(g_object_new(imclipboard_plugin_get_type(), nullptr));
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            kChannelName, FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      channel,
      [](FlMethodChannel*, FlMethodCall* call, gpointer data) {
        HandleMethodCall(IMCLIPBOARD_PLUGIN(data), call);
      },
      g_object_ref(plugin), g_object_unref);
  g_object_unref(plugin);
}

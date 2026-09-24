#include "clipboard_service.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace imclipboard {
namespace {
constexpr gsize kMaximumBytes = 512ULL * 1024ULL * 1024ULL;
constexpr guint64 kMaximumPixels = 100000000;
constexpr char kTokenTarget[] = "application/x-imclipboard-token";
constexpr char kPayloadKey[] = "imclipboard-png";

// One worker's data is destroyed on the originating main context.
struct Work {
  std::function<void()> run;
  std::function<void()> complete;
};

void Background(std::function<void()> run, std::function<void()> complete) {
  auto* work = new Work{std::move(run), std::move(complete)};
  GTask* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        auto* work = static_cast<Work*>(g_task_get_task_data(G_TASK(result)));
        work->complete();
      },
      nullptr);
  g_task_set_task_data(task, work,
                       [](gpointer data) { delete static_cast<Work*>(data); });
  g_task_run_in_thread(task,
                       [](GTask* task, gpointer, gpointer data, GCancellable*) {
                         static_cast<Work*>(data)->run();
                         g_task_return_boolean(task, TRUE);
                       });
  g_object_unref(task);
}

bool ValidSize(int width, int height) {
  return width > 0 && height > 0 &&
         static_cast<guint64>(width) * height <= kMaximumPixels;
}

// PNG metadata needs only the complete IHDR, not an allocated pixel surface.
bool PngInfo(const guint8* bytes, gsize length, Image* image) {
  static const guint8 signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (length < 33 || length > kMaximumBytes ||
      std::memcmp(bytes, signature, 8) != 0 ||
      std::memcmp(bytes + 8, "\0\0\0\rIHDR", 8) != 0)
    return false;
  const auto number = [](const guint8* p) -> guint32 {
    return (guint32(p[0]) << 24) | (guint32(p[1]) << 16) |
           (guint32(p[2]) << 8) | p[3];
  };
  const guint32 width = number(bytes + 16);
  const guint32 height = number(bytes + 20);
  const int depth = bytes[24];
  const int color = bytes[25];
  const bool valid_depth =
      color == 0   ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
                      depth == 16)
      : color == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                   : (color == 2 || color == 4 || color == 6) &&
                         (depth == 8 || depth == 16);
  if (width > G_MAXINT || height > G_MAXINT || !ValidSize(width, height) ||
      !valid_depth || bytes[26] != 0 || bytes[27] != 0 || bytes[28] > 1)
    return false;
  guint32 crc = 0xffffffff;
  for (int i = 12; i < 29; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  if ((crc ^ 0xffffffff) != number(bytes + 29)) return false;
  image->width = width;
  image->height = height;
  return true;
}

// Only for encoder-owned PNGs: bound chunks without decoding compressed pixels.
// The ordinary write path continues to fully decode all external input.
ImageReply GeneratedPng(Bytes bytes) {
  auto image = std::make_shared<Image>();
  gsize length = 0;
  const auto* data = static_cast<const guint8*>(g_bytes_get_data(bytes.get(), &length));
  if (!PngInfo(data, length, image.get())) return {nullptr, "Invalid generated PNG header"};
  bool pixels = false;
  for (gsize offset = 33; offset + 12 <= length;) {
    const guint32 count = (guint32(data[offset]) << 24) | (guint32(data[offset + 1]) << 16) |
                          (guint32(data[offset + 2]) << 8) | data[offset + 3];
    if (count > length - offset - 12) break;
    const auto* type = data + offset + 4;
    if (std::memcmp(type, "IHDR", 4) == 0) break;
    if (std::memcmp(type, "IDAT", 4) == 0 && count > 0) pixels = true;
    if (std::memcmp(type, "IEND", 4) == 0) {
      if (!pixels || count != 0 || offset + 12 != length) break;
      image->png = std::move(bytes);
      return {image, {}};
    }
    offset += gsize(count) + 12;
  }
  return {nullptr, "Incomplete generated PNG"};
}

// Reject enormous decoded images before the loader allocates their full
// surface.
struct DecodeSize {
  int width = 0;
  int height = 0;
  bool metadata_only;
};

ImageReply Decode(Bytes bytes, bool include_png, bool require_png) {
  ImageReply reply;
  auto image = std::make_shared<Image>();
  gsize length = 0;
  const auto* data =
      static_cast<const guint8*>(g_bytes_get_data(bytes.get(), &length));
  const bool png = PngInfo(data, length, image.get());
  if (length == 0 || length > kMaximumBytes || (require_png && !png)) {
    reply.error = "Invalid PNG header, dimensions or encoded size";
    return reply;
  }
  if (png && !include_png) {
    reply.image = image;
    return reply;
  }
  g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
  DecodeSize size{0, 0, !include_png};
  g_signal_connect(loader, "size-prepared",
                   G_CALLBACK(+[](GdkPixbufLoader* loader, gint width,
                                  gint height, gpointer user_data) {
                     auto* size = static_cast<DecodeSize*>(user_data);
                     size->width = width;
                     size->height = height;
                     if (size->metadata_only || !ValidSize(width, height))
                       gdk_pixbuf_loader_set_size(loader, 1, 1);
                   }),
                   &size);
  g_autoptr(GError) error = nullptr;
  bool valid = true;
  for (gsize offset = 0; offset < length; offset += 65536) {
    if (!gdk_pixbuf_loader_write(loader, data + offset,
                                 std::min<gsize>(65536, length - offset),
                                 &error)) {
      valid = false;
      break;
    }
    if (size.width != 0 &&
        (!ValidSize(size.width, size.height) || !include_png))
      break;
  }
  const bool closed = gdk_pixbuf_loader_close(loader, valid ? &error : nullptr);
  if (!ValidSize(size.width, size.height) ||
      (include_png && (!valid || !closed))) {
    reply.error =
        error == nullptr
            ? "Invalid image or decoded image exceeds 100 million pixels"
            : error->message;
    return reply;
  }
  image->width = size.width;
  image->height = size.height;
  if (include_png) {
    if (png) {
      image->png = std::move(bytes);
    } else {
      GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
      gchar* output = nullptr;
      gsize output_length = 0;
      if (pixbuf == nullptr ||
          !gdk_pixbuf_save_to_buffer(pixbuf, &output, &output_length, "png",
                                     &error, "compression", "1", nullptr)) {
        reply.error = error == nullptr ? "Could not encode clipboard image"
                                       : error->message;
        return reply;
      }
      if (output_length > kMaximumBytes) {
        g_free(output);
        reply.error = "Clipboard PNG exceeds 512 MiB";
        return reply;
      }
      image->png =
          Bytes(g_bytes_new_take(output, output_length), g_bytes_unref);
    }
  }
  reply.image = std::move(image);
  return reply;
}

// Returns only local regular files, preserving the existing URI policy.
std::string LocalFile(const std::string& uri, bool utf8) {
  g_autofree gchar* hostname = nullptr;
  g_autofree gchar* path = g_filename_from_uri(uri.c_str(), &hostname, nullptr);
  if (path == nullptr ||
      (hostname != nullptr && hostname[0] != '\0' &&
       g_ascii_strcasecmp(hostname, "localhost") != 0) ||
      !g_path_is_absolute(path) || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
    return {};
  g_autofree gchar* text =
      g_filename_to_utf8(path, -1, nullptr, nullptr, nullptr);
  if (text == nullptr || std::strlen(text) > 32768) return {};
  return utf8 ? text : path;
}

// A bounded stream also handles files that grow after their initial stat.
Bytes ReadFile(const std::string& path) {
  g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
  g_autoptr(GFileInputStream) stream = g_file_read(file, nullptr, nullptr);
  if (stream == nullptr) return {};
  g_autoptr(GByteArray) bytes = g_byte_array_new();
  guint8 chunk[65536];
  while (true) {
    const gssize count = g_input_stream_read(G_INPUT_STREAM(stream), chunk,
                                             sizeof(chunk), nullptr, nullptr);
    if (count < 0 || bytes->len + static_cast<guint64>(count) > kMaximumBytes)
      return {};
    if (count == 0) break;
    g_byte_array_append(bytes, chunk, count);
  }
  return Bytes(g_byte_array_free_to_bytes(g_steal_pointer(&bytes)),
               g_bytes_unref);
}

// Transfers URI strings out of GTK's callback lifetime.
void RequestUris(GtkClipboard* clipboard,
                 std::function<void(std::vector<std::string>)> complete) {
  auto* callback =
      new std::function<void(std::vector<std::string>)>(std::move(complete));
  gtk_clipboard_request_uris(
      clipboard,
      [](GtkClipboard*, gchar** uris, gpointer data) {
        std::unique_ptr<std::function<void(std::vector<std::string>)>> callback(
            static_cast<std::function<void(std::vector<std::string>)>*>(data));
        std::vector<std::string> result;
        if (uris != nullptr)
          for (int i = 0; uris[i] != nullptr; ++i) {
            if (std::strlen(uris[i]) <= 3 * 32768) result.emplace_back(uris[i]);
          }
        (*callback)(std::move(result));
      },
      callback);
}

// GTK receives the original PNG bytes, never a pixbuf to re-encode.
void Provide(GtkClipboard*, GtkSelectionData* selection, guint info,
             gpointer object) {
  auto* payload = static_cast<std::shared_ptr<Image>*>(
      g_object_get_data(G_OBJECT(object), kPayloadKey));
  if (payload == nullptr) return;
  const std::shared_ptr<Image> image = *payload;
  if (info == 0) {
    gsize length = 0;
    const auto* bytes =
        static_cast<const guint8*>(g_bytes_get_data(image->png.get(), &length));
    gtk_selection_data_set(selection, gtk_selection_data_get_target(selection),
                           8, bytes, length);
  } else {
    gtk_selection_data_set(selection, gtk_selection_data_get_target(selection),
                           8,
                           reinterpret_cast<const guchar*>(image->token.data()),
                           image->token.size());
  }
}

void Clear(GtkClipboard*, gpointer object) {
  g_object_set_data(G_OBJECT(object), kPayloadKey, nullptr);
}

}  // namespace

struct ClipboardService::ReadRequest {
  std::shared_ptr<ClipboardService> service;
  guint64 revision;
  bool include_png;
  std::function<void(ImageReply)> complete;
  ImageReply reply;
};

ClipboardService::ClipboardService(GtkClipboard* clipboard)
    : clipboard_(GTK_CLIPBOARD(g_object_ref(clipboard))) {
  owner_changed_ = g_signal_connect(
      clipboard_, "owner-change",
      G_CALLBACK(+[](GtkClipboard*, GdkEventOwnerChange*, gpointer data) {
        ++static_cast<ClipboardService*>(data)->revision_;
      }),
      this);
}

ClipboardService::~ClipboardService() {
  g_signal_handler_disconnect(clipboard_, owner_changed_);
  g_clear_object(&owner_);
  g_object_unref(clipboard_);
}

void ClipboardService::Enqueue(std::function<void()> operation) {
  pending_.push_back(std::move(operation));
  if (!running_) Next();
}

void ClipboardService::Next() {
  running_ = !pending_.empty();
  if (!running_) return;
  auto operation = std::move(pending_.front());
  pending_.pop_front();
  operation();
}

void ClipboardService::Write(Bytes png, std::string token,
                             std::function<void(std::string)> complete, bool generated_png) {
  auto self = shared_from_this();
  Enqueue([self, png, token, complete, generated_png] {
    auto reply = std::make_shared<ImageReply>();
    Background(
        [png, reply, generated_png] { *reply = generated_png ? GeneratedPng(png) : Decode(png, true, true); },
        [self, token, reply, complete] {
          if (!reply->image) {
            complete(reply->error);
            self->Next();
            return;
          }
          reply->image->token = token;
          GObject* owner = G_OBJECT(g_object_new(G_TYPE_OBJECT, nullptr));
          g_object_set_data_full(
              owner, kPayloadKey, new std::shared_ptr<Image>(reply->image),
              [](gpointer data) {
                delete static_cast<std::shared_ptr<Image>*>(data);
              });
          GtkTargetEntry targets[] = {{const_cast<gchar*>("image/png"), 0, 0},
                                      {const_cast<gchar*>(kTokenTarget), 0, 1}};
          const guint count = token.empty() ? 1 : 2;
          if (!gtk_clipboard_set_with_owner(self->clipboard_, targets, count,
                                            Provide, Clear, owner)) {
            g_object_unref(owner);
            complete("GTK refused clipboard ownership");
          } else {
            g_clear_object(&self->owner_);
            self->owner_ = owner;
            gtk_clipboard_set_can_store(self->clipboard_, targets, count);
            // Keep the existing persistence contract, now serving compressed
            // bytes.
            gtk_clipboard_store(self->clipboard_);
            complete({});
          }
          self->Next();
        });
  });
}

void ClipboardService::Read(bool include_png,
                            std::function<void(ImageReply)> complete) {
  auto self = shared_from_this();
  Enqueue([self, include_png, complete] {
    if (self->owner_ != nullptr &&
        gtk_clipboard_get_owner(self->clipboard_) == self->owner_) {
      auto* image = static_cast<std::shared_ptr<Image>*>(
          g_object_get_data(self->owner_, kPayloadKey));
      if (image != nullptr) {
        complete({*image, {}});
        self->Next();
        return;
      }
    }
    auto request = std::make_shared<ReadRequest>(
        ReadRequest{self, self->revision_, include_png, complete, {}});
    self->ReadTargets(request);
  });
}

void ClipboardService::ReadTargets(
    const std::shared_ptr<ReadRequest>& request) {
  auto* held = new std::shared_ptr<ReadRequest>(request);
  gtk_clipboard_request_targets(
      clipboard_,
      [](GtkClipboard* clipboard, GdkAtom* targets, gint count, gpointer data) {
        std::unique_ptr<std::shared_ptr<ReadRequest>> held(
            static_cast<std::shared_ptr<ReadRequest>*>(data));
        auto request = *held;
        if (request->revision != request->service->revision_) {
          request->service->CompleteRead(request);
          return;
        }
        GdkAtom selected = GDK_NONE;
        const GdkAtom png = gdk_atom_intern_static_string("image/png");
        for (int i = 0; i < count; ++i)
          if (targets[i] == png) selected = png;
        if (selected == GDK_NONE) {
          GSList* formats = gdk_pixbuf_get_formats();
          for (GSList* item = formats; item != nullptr && selected == GDK_NONE;
               item = item->next) {
            g_auto(GStrv) types = gdk_pixbuf_format_get_mime_types(
                static_cast<GdkPixbufFormat*>(item->data));
            for (int t = 0; types[t] != nullptr && selected == GDK_NONE; ++t) {
              const GdkAtom atom = gdk_atom_intern(types[t], FALSE);
              for (int i = 0; i < count; ++i)
                if (targets[i] == atom) selected = atom;
            }
          }
          g_slist_free(formats);
        }
        if (selected == GDK_NONE) {
          request->service->ReadUris(request);
          return;
        }
        auto* transfer = new std::shared_ptr<ReadRequest>(request);
        gtk_clipboard_request_contents(
            clipboard, selected,
            [](GtkClipboard*, GtkSelectionData* selection, gpointer data) {
              std::unique_ptr<std::shared_ptr<ReadRequest>> held(
                  static_cast<std::shared_ptr<ReadRequest>*>(data));
              auto request = *held;
              if (request->revision != request->service->revision_) {
                request->service->CompleteRead(request);
                return;
              }
              const gint length = gtk_selection_data_get_length(selection);
              const guchar* bytes = gtk_selection_data_get_data(selection);
              if (length <= 0 || bytes == nullptr) {
                request->service->ReadUris(request);
                return;
              }
              if (static_cast<gsize>(length) > kMaximumBytes) {
                request->reply.error = "Clipboard image exceeds 512 MiB";
                request->service->CompleteRead(request);
                return;
              }
              if (!request->include_png) {
                auto image = std::make_shared<Image>();
                if (PngInfo(bytes, length, image.get())) {
                  request->reply.image = image;
                  request->service->ReadToken(request);
                  return;
                }
              }
              Bytes input(g_bytes_new(bytes, length), g_bytes_unref);
              Background(
                  [request, input] {
                    request->reply = Decode(input, request->include_png, false);
                  },
                  [request] { request->service->ReadToken(request); });
            },
            transfer);
      },
      held);
}

void ClipboardService::ReadUris(const std::shared_ptr<ReadRequest>& request) {
  RequestUris(clipboard_, [request](std::vector<std::string> uris) {
    if (request->revision != request->service->revision_) {
      request->service->CompleteRead(request);
      return;
    }
    Background(
        [request, uris] {
          for (size_t index = 0; index < uris.size() && index < 32; ++index) {
            const std::string path = LocalFile(uris[index], false);
            if (path.empty()) continue;
            if (!request->include_png) {
              auto image = std::make_shared<Image>();
              if (gdk_pixbuf_get_file_info(path.c_str(), &image->width,
                                           &image->height) != nullptr &&
                  ValidSize(image->width, image->height)) {
                request->reply.image = image;
                return;
              }
            } else {
              Bytes bytes = ReadFile(path);
              if (!bytes) continue;
              ImageReply reply = Decode(bytes, true, false);
              if (reply.image) {
                request->reply = std::move(reply);
                return;
              }
            }
          }
        },
        [request] { request->service->ReadToken(request); });
  });
}

void ClipboardService::ReadToken(const std::shared_ptr<ReadRequest>& request) {
  if (!request->reply.image || request->revision != revision_) {
    CompleteRead(request);
    return;
  }
  auto* held = new std::shared_ptr<ReadRequest>(request);
  gtk_clipboard_request_contents(
      clipboard_, gdk_atom_intern_static_string(kTokenTarget),
      [](GtkClipboard*, GtkSelectionData* selection, gpointer data) {
        std::unique_ptr<std::shared_ptr<ReadRequest>> held(
            static_cast<std::shared_ptr<ReadRequest>*>(data));
        auto request = *held;
        const gint length = gtk_selection_data_get_length(selection);
        const guchar* bytes = gtk_selection_data_get_data(selection);
        if (length > 0 && length <= 1024 && bytes != nullptr &&
            std::memchr(bytes, 0, length) == nullptr &&
            g_utf8_validate(reinterpret_cast<const gchar*>(bytes), length,
                            nullptr))
          request->reply.image->token.assign(
              reinterpret_cast<const char*>(bytes), length);
        request->service->CompleteRead(request);
      },
      held);
}

void ClipboardService::CompleteRead(
    const std::shared_ptr<ReadRequest>& request) {
  if (request->revision != revision_)
    request->reply = {nullptr, "Clipboard changed while reading"};
  request->complete(request->reply);
  Next();
}

void ClipboardService::ReadFiles(
    std::function<void(std::vector<std::string>)> complete) {
  auto self = shared_from_this();
  Enqueue([self, complete] {
    const guint64 revision = self->revision_;
    RequestUris(self->clipboard_, [self, complete,
                                   revision](std::vector<std::string> uris) {
      auto files = std::make_shared<std::vector<std::string>>();
      Background(
          [uris, files] {
            for (const auto& uri : uris) {
              const std::string path = LocalFile(uri, true);
              if (!path.empty()) files->push_back(path);
              if (files->size() == 32) break;
            }
          },
          [self, complete, files, revision] {
            complete(revision == self->revision_ ? *files
                                                 : std::vector<std::string>{});
            self->Next();
          });
    });
  });
}
}  // namespace imclipboard

#include "clipboard_service.h"

#include <gtest/gtest.h>

#include <cstring>

namespace imclipboard {
namespace {

// Uses a private X11 selection, never CLIPBOARD or PRIMARY on the user's
// desktop.
class ClipboardServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    if (!gtk_init_check(nullptr, nullptr))
      GTEST_SKIP() << "A GTK display is required";
    if (std::strcmp(G_OBJECT_TYPE_NAME(gdk_display_get_default()),
                    "GdkX11Display") != 0)
      GTEST_SKIP() << "Private selections require X11";
    const std::string name =
        "IMCLIPBOARD_TEST_" + std::to_string(g_get_monotonic_time());
    clipboard = gtk_clipboard_get(gdk_atom_intern(name.c_str(), FALSE));
    service = std::make_shared<ClipboardService>(clipboard);
  }
  void TearDown() override {
    if (clipboard != nullptr) gtk_clipboard_clear(clipboard);
    service.reset();
    Drain();
  }
  static void Drain() {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }
  static bool Wait(const std::function<bool()>& done) {
    const gint64 limit = g_get_monotonic_time() + 10000000;
    while (!done() && g_get_monotonic_time() < limit) {
      Drain();
      g_usleep(1000);
    }
    return done();
  }
  static Bytes Png(guint32 color = 0x22448880) {
    g_autoptr(GdkPixbuf) image =
        gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 3, 2);
    gdk_pixbuf_fill(image, color);
    gchar* data = nullptr;
    gsize length = 0;
    gdk_pixbuf_save_to_buffer(image, &data, &length, "png", nullptr,
                              "compression", "9", nullptr);
    return Bytes(g_bytes_new_take(data, length), g_bytes_unref);
  }
  /// Creates an alternative image with dimensions distinct from Png().
  static Bytes Jpeg() {
    g_autoptr(GdkPixbuf) image =
        gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8, 9);
    gdk_pixbuf_fill(image, 0x224488ff);
    gchar* data = nullptr;
    gsize length = 0;
    gdk_pixbuf_save_to_buffer(image, &data, &length, "jpeg", nullptr, nullptr);
    return Bytes(g_bytes_new_take(data, length), g_bytes_unref);
  }
  /// Publishes independent representations and records which GTK requests.
  void PublishTargets(std::vector<std::pair<std::string, Bytes>> values,
                      std::vector<std::string>* requested) {
    /// Retains representation data until GTK releases this test selection.
    struct Payload {
      /// Encoded bytes indexed by the advertised MIME target.
      std::vector<std::pair<std::string, Bytes>> values;
      /// Requests observed while the test owns the selection.
      std::vector<std::string>* requested;
    };
    auto* payload = new Payload{std::move(values), requested};
    std::vector<GtkTargetEntry> targets;
    for (size_t index = 0; index < payload->values.size(); ++index)
      targets.push_back({const_cast<gchar*>(payload->values[index].first.c_str()),
                         0, static_cast<guint>(index)});
    ASSERT_TRUE(gtk_clipboard_set_with_data(
        clipboard, targets.data(), targets.size(),
        [](GtkClipboard*, GtkSelectionData* selection, guint index,
           gpointer user_data) {
          const auto* payload = static_cast<Payload*>(user_data);
          const auto& value = payload->values[index];
          payload->requested->push_back(value.first);
          if (!value.second) return;
          gsize length;
          const auto* data = static_cast<const guint8*>(
              g_bytes_get_data(value.second.get(), &length));
          gtk_selection_data_set(selection,
                                 gtk_selection_data_get_target(selection), 8,
                                 data, length);
        },
        [](GtkClipboard*, gpointer user_data) {
          delete static_cast<Payload*>(user_data);
        },
        payload));
    Drain();
  }
  void Publish(Bytes bytes, const char* type = "image/png") {
    auto* retained = new Bytes(std::move(bytes));
    GtkTargetEntry targets[] = {{const_cast<gchar*>(type), 0, 0}};
    ASSERT_TRUE(gtk_clipboard_set_with_data(
        clipboard, targets, 1,
        [](GtkClipboard*, GtkSelectionData* selection, guint,
           gpointer user_data) {
          auto bytes = *static_cast<Bytes*>(user_data);
          gsize length;
          const auto* data = static_cast<const guint8*>(
              g_bytes_get_data(bytes.get(), &length));
          gtk_selection_data_set(selection,
                                 gtk_selection_data_get_target(selection), 8,
                                 data, length);
        },
        [](GtkClipboard*, gpointer user_data) {
          delete static_cast<Bytes*>(user_data);
        },
        retained));
    Drain();
  }
  ImageReply Read(bool pixels) {
    bool done = false;
    ImageReply reply;
    service->Read(pixels, [&](ImageReply value) {
      reply = std::move(value);
      done = true;
    });
    EXPECT_TRUE(Wait([&] { return done; }));
    return reply;
  }
  GtkClipboard* clipboard = nullptr;
  std::shared_ptr<ClipboardService> service;
};

TEST_F(ClipboardServiceTest, WritesOriginalPngAndTokenWithoutReencoding) {
  const Bytes png = Png();
  bool done = false;
  service->Write(png, "original", [&](std::string error) {
    EXPECT_TRUE(error.empty()) << error;
    done = true;
  });
  ASSERT_TRUE(Wait([&] { return done; }));
  const ImageReply info = Read(false);
  ASSERT_NE(info.image, nullptr);
  EXPECT_EQ(info.image->width, 3);
  EXPECT_EQ(info.image->height, 2);
  EXPECT_EQ(info.image->token, "original");
  const ImageReply image = Read(true);
  ASSERT_NE(image.image, nullptr);
  EXPECT_TRUE(g_bytes_equal(png.get(), image.image->png.get()));
  // Exercise the actual GTK supply callback, not only the in-process cache.
  GtkSelectionData* transfer = gtk_clipboard_wait_for_contents(
      clipboard, gdk_atom_intern_static_string("image/png"));
  ASSERT_NE(transfer, nullptr);
  gsize length;
  const void* data = g_bytes_get_data(png.get(), &length);
  EXPECT_EQ(gtk_selection_data_get_length(transfer), static_cast<int>(length));
  EXPECT_EQ(std::memcmp(gtk_selection_data_get_data(transfer), data, length),
            0);
  gtk_selection_data_free(transfer);
}

TEST_F(ClipboardServiceTest, GeneratedPngPublishesOriginalBytesAndRejectsTruncation) {
  const Bytes png = Png();
  bool done = false;
  service->Write(png, "generated", [&](std::string error) {
    EXPECT_TRUE(error.empty()) << error;
    done = true;
  }, true);
  ASSERT_TRUE(Wait([&] { return done; }));
  const ImageReply image = Read(true);
  ASSERT_NE(image.image, nullptr);
  EXPECT_EQ(image.image->token, "generated");
  EXPECT_TRUE(g_bytes_equal(png.get(), image.image->png.get()));
  GtkSelectionData* transfer = gtk_clipboard_wait_for_contents(
      clipboard, gdk_atom_intern_static_string("image/png"));
  ASSERT_NE(transfer, nullptr);
  gsize length;
  const void* data = g_bytes_get_data(png.get(), &length);
  EXPECT_EQ(gtk_selection_data_get_length(transfer), static_cast<int>(length));
  EXPECT_EQ(std::memcmp(gtk_selection_data_get_data(transfer), data, length), 0);
  gtk_selection_data_free(transfer);
  for (gsize truncated : {gsize(0), gsize(33), length - 1}) {
    done = false;
    service->Write(Bytes(g_bytes_new(data, truncated), g_bytes_unref), "bad",
      [&](std::string error) {
        EXPECT_FALSE(error.empty());
        done = true;
      }, true);
    ASSERT_TRUE(Wait([&] { return done; }));
    const ImageReply retained = Read(false);
    ASSERT_NE(retained.image, nullptr);
    EXPECT_EQ(retained.image->token, "generated");
  }
}

TEST_F(ClipboardServiceTest,
       ForeignPngPassesThroughAndOwnCacheDoesNotSurviveReplacement) {
  bool done = false;
  service->Write(Png(), "old", [&](std::string error) {
    EXPECT_TRUE(error.empty());
    done = true;
  });
  ASSERT_TRUE(Wait([&] { return done; }));
  const Bytes replacement = Png(0x887766ff);
  Publish(replacement);
  const ImageReply reply = Read(true);
  ASSERT_NE(reply.image, nullptr) << reply.error;
  EXPECT_TRUE(reply.image->token.empty());
  EXPECT_TRUE(g_bytes_equal(replacement.get(), reply.image->png.get()));
}

TEST_F(ClipboardServiceTest, PngMetadataDoesNotRequirePixelDecoding) {
  const Bytes png = Png();
  gsize length;
  const void* data = g_bytes_get_data(png.get(), &length);
  // The complete IHDR is valid; no compressed rows are provided.
  Publish(Bytes(g_bytes_new(data, 33), g_bytes_unref));
  const ImageReply info = Read(false);
  ASSERT_NE(info.image, nullptr) << info.error;
  EXPECT_EQ(info.image->width, 3);
  const ImageReply pixels = Read(true);
  EXPECT_EQ(pixels.image, nullptr);
  EXPECT_FALSE(pixels.error.empty());
}

TEST_F(ClipboardServiceTest, InvalidWritePreservesPreviousClipboard) {
  bool done = false;
  service->Write(Png(), "previous", [&](std::string error) {
    EXPECT_TRUE(error.empty());
    done = true;
  });
  ASSERT_TRUE(Wait([&] { return done; }));
  done = false;
  service->Write(Bytes(g_bytes_new_static("invalid", 7), g_bytes_unref), "bad",
                 [&](std::string error) {
                   EXPECT_FALSE(error.empty());
                   done = true;
                 });
  ASSERT_TRUE(Wait([&] { return done; }));
  const ImageReply info = Read(false);
  ASSERT_NE(info.image, nullptr);
  EXPECT_EQ(info.image->token, "previous");
}

TEST_F(ClipboardServiceTest, QueuedWritesRetainInvocationOrder) {
  std::vector<int> completed;
  service->Write(Png(), "first", [&](std::string error) {
    EXPECT_TRUE(error.empty());
    completed.push_back(1);
  });
  service->Write(Png(), "second", [&](std::string error) {
    EXPECT_TRUE(error.empty());
    completed.push_back(2);
  });
  ASSERT_TRUE(Wait([&] { return completed.size() == 2; }));
  EXPECT_EQ(completed, (std::vector<int>{1, 2}));
  const ImageReply info = Read(false);
  ASSERT_NE(info.image, nullptr);
  EXPECT_EQ(info.image->token, "second");
}

TEST_F(ClipboardServiceTest, ReadsForeignJpegThroughWorkerFallback) {
  g_autoptr(GdkPixbuf) image =
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 8, 9);
  gdk_pixbuf_fill(image, 0x224488ff);
  gchar* data = nullptr;
  gsize length = 0;
  ASSERT_TRUE(gdk_pixbuf_save_to_buffer(image, &data, &length, "jpeg", nullptr,
                                        nullptr));
  Publish(Bytes(g_bytes_new_take(data, length), g_bytes_unref), "image/jpeg");
  const ImageReply reply = Read(true);
  ASSERT_NE(reply.image, nullptr) << reply.error;
  EXPECT_EQ(reply.image->width, 8);
  EXPECT_EQ(reply.image->height, 9);
  EXPECT_NE(reply.image->png, nullptr);
}

TEST_F(ClipboardServiceTest, PrefersValidPngWithoutRequestingAlternatives) {
  const Bytes png = Png();
  std::vector<std::string> requested;
  PublishTargets({{"image/jpeg", Jpeg()}, {"image/png", png}}, &requested);

  const ImageReply reply = Read(true);

  ASSERT_NE(reply.image, nullptr) << reply.error;
  EXPECT_TRUE(reply.error.empty());
  EXPECT_TRUE(g_bytes_equal(png.get(), reply.image->png.get()));
  EXPECT_EQ(requested, (std::vector<std::string>{"image/png"}));
}

TEST_F(ClipboardServiceTest, InvalidPngFallsBackToJpegForMetadataAndPixels) {
  std::vector<std::string> requested;
  PublishTargets({{"image/png", Bytes(g_bytes_new_static("invalid", 7), g_bytes_unref)},
                  {"image/jpeg", Jpeg()},
                  {"application/x-imclipboard-token",
                   Bytes(g_bytes_new_static("fallback-token", 14), g_bytes_unref)}},
                 &requested);

  for (const bool pixels : {false, true}) {
    requested.clear();
    const ImageReply reply = Read(pixels);

    ASSERT_NE(reply.image, nullptr) << reply.error;
    EXPECT_TRUE(reply.error.empty());
    EXPECT_TRUE(reply.diagnostics.target.empty());
    EXPECT_EQ(reply.image->width, 8);
    EXPECT_EQ(reply.image->height, 9);
    EXPECT_EQ(reply.image->token, "fallback-token");
    EXPECT_EQ(reply.image->png != nullptr, pixels);
    EXPECT_EQ(requested, (std::vector<std::string>{
                             "image/png", "image/jpeg",
                             "application/x-imclipboard-token"}));
  }
}

TEST_F(ClipboardServiceTest, UnavailableAndEmptyPngFallBackToJpeg) {
  for (const Bytes& png : {Bytes{}, Bytes(g_bytes_new_static("", 0), g_bytes_unref)}) {
    std::vector<std::string> requested;
    PublishTargets({{"image/png", png}, {"image/jpeg", Jpeg()}}, &requested);

    const ImageReply reply = Read(true);

    ASSERT_NE(reply.image, nullptr) << reply.error;
    EXPECT_TRUE(reply.error.empty());
    EXPECT_EQ(reply.image->width, 8);
    EXPECT_EQ(requested, (std::vector<std::string>{"image/png", "image/jpeg"}));
    gtk_clipboard_clear(clipboard);
  }
}

TEST_F(ClipboardServiceTest, TruncatedPngFallsBackAfterSuccessfulMetadataRead) {
  const Bytes png = Png();
  const Bytes header(g_bytes_new(g_bytes_get_data(png.get(), nullptr), 33),
                     g_bytes_unref);
  std::vector<std::string> requested;
  PublishTargets({{"image/png", header}, {"image/jpeg", Jpeg()}}, &requested);

  const ImageReply info = Read(false);
  ASSERT_NE(info.image, nullptr) << info.error;
  EXPECT_EQ(info.image->width, 3);
  EXPECT_EQ(requested, (std::vector<std::string>{"image/png"}));
  requested.clear();
  const ImageReply reply = Read(true);

  ASSERT_NE(reply.image, nullptr) << reply.error;
  EXPECT_TRUE(reply.error.empty());
  EXPECT_EQ(reply.image->width, 8);
  EXPECT_EQ(requested, (std::vector<std::string>{"image/png", "image/jpeg"}));
}

TEST_F(ClipboardServiceTest, ExhaustedTargetsPreserveFirstErrorAndReleaseQueue) {
  const Bytes png = Png();
  std::vector<std::string> requested;
  PublishTargets({{"image/png", Bytes(g_bytes_new_static("invalid", 7), g_bytes_unref)},
                  {"image/jpeg", Bytes(g_bytes_new(g_bytes_get_data(png.get(), nullptr), 33),
                                        g_bytes_unref)}}, &requested);

  const ImageReply reply = Read(true);

  EXPECT_EQ(reply.image, nullptr);
  EXPECT_FALSE(reply.error.empty());
  EXPECT_EQ(reply.diagnostics.target, "image/png");
  EXPECT_EQ(reply.diagnostics.encoded_bytes, 7U);
  EXPECT_EQ(reply.diagnostics.error_domain, g_quark_to_string(GDK_PIXBUF_ERROR));
  EXPECT_EQ(reply.diagnostics.error_code, GDK_PIXBUF_ERROR_UNKNOWN_TYPE);
  EXPECT_EQ(reply.diagnostics.attempted_targets,
            (std::vector<std::string>{"image/png", "image/jpeg"}));
  EXPECT_EQ(requested, reply.diagnostics.attempted_targets);
  Publish(png);
  EXPECT_NE(Read(true).image, nullptr);
}

TEST_F(ClipboardServiceTest, MissingPngDecoderReportsNativeErrorAfterMetadata) {
  const Bytes png = Png();
  Publish(png);
  std::vector<GdkPixbufFormat*> disabled;
  GSList* formats = gdk_pixbuf_get_formats();
  for (GSList* item = formats; item != nullptr; item = item->next) {
    auto* format = static_cast<GdkPixbufFormat*>(item->data);
    g_auto(GStrv) types = gdk_pixbuf_format_get_mime_types(format);
    for (int index = 0; types[index] != nullptr; ++index) {
      if (std::strcmp(types[index], "image/png") == 0 &&
          !gdk_pixbuf_format_is_disabled(format)) {
        disabled.push_back(format);
        gdk_pixbuf_format_set_disabled(format, TRUE);
        break;
      }
    }
  }

  const ImageReply info = Read(false);
  const ImageReply reply = Read(true);

  for (GdkPixbufFormat* format : disabled)
    gdk_pixbuf_format_set_disabled(format, FALSE);
  g_slist_free(formats);
  EXPECT_NE(info.image, nullptr);
  EXPECT_TRUE(info.error.empty());
  EXPECT_EQ(reply.image, nullptr);
  EXPECT_FALSE(reply.error.empty());
  EXPECT_EQ(reply.diagnostics.target, "image/png");
  EXPECT_EQ(reply.diagnostics.encoded_bytes, g_bytes_get_size(png.get()));
  EXPECT_EQ(reply.diagnostics.error_domain, g_quark_to_string(GDK_PIXBUF_ERROR));
  EXPECT_EQ(reply.diagnostics.error_code, GDK_PIXBUF_ERROR_UNKNOWN_TYPE);
  EXPECT_EQ(reply.diagnostics.attempted_targets,
            (std::vector<std::string>{"image/png"}));
}

TEST_F(ClipboardServiceTest, OwnerChangeDuringFailureStopsFallbackAndReleasesQueue) {
  std::vector<std::string> requested;
  PublishTargets({{"image/png", Bytes(g_bytes_new_static("invalid", 7), g_bytes_unref)},
                  {"image/jpeg", Jpeg()}}, &requested);
  int completed = 0;
  ImageReply reply;
  service->Read(true, [&](ImageReply value) {
    reply = std::move(value);
    ++completed;
  });
  Publish(Png());
  ImageReply next;
  service->Read(true, [&](ImageReply value) {
    next = std::move(value);
    ++completed;
  });

  ASSERT_TRUE(Wait([&] { return completed == 2; }));

  EXPECT_EQ(reply.image, nullptr);
  EXPECT_EQ(reply.error, "Clipboard changed while reading");
  EXPECT_TRUE(reply.diagnostics.target.empty());
  EXPECT_EQ(requested, (std::vector<std::string>{"image/png"}));
  ASSERT_NE(next.image, nullptr) << next.error;
  EXPECT_EQ(next.image->width, 3);
}

TEST_F(ClipboardServiceTest, OwnerChangeDuringDecodeRejectsStaleImage) {
  Publish(Png());
  bool done = false;
  ImageReply reply;
  service->Read(true, [&](ImageReply value) {
    reply = std::move(value);
    done = true;
  });
  Publish(Png(0xffffffff));
  ASSERT_TRUE(Wait([&] { return done; }));
  EXPECT_EQ(reply.image, nullptr);
  EXPECT_FALSE(reply.error.empty());
  EXPECT_NE(Read(true).image, nullptr);
}

TEST_F(ClipboardServiceTest, ReadsOriginalPngFromFileUri) {
  gchar* path = nullptr;
  const int descriptor =
      g_file_open_tmp("imclipboard-test-XXXXXX", &path, nullptr);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  g_autofree gchar* filename = path;
  const Bytes png = Png();
  gsize length;
  const auto* data =
      static_cast<const gchar*>(g_bytes_get_data(png.get(), &length));
  ASSERT_TRUE(g_file_set_contents(filename, data, length, nullptr));
  g_autofree gchar* uri = g_filename_to_uri(filename, nullptr, nullptr);
  const std::string payload = std::string(uri) + "\r\n";
  Publish(Bytes(g_bytes_new(payload.data(), payload.size()), g_bytes_unref),
          "text/uri-list");
  const ImageReply info = Read(false);
  ASSERT_NE(info.image, nullptr) << info.error;
  EXPECT_EQ(info.image->width, 3);
  const ImageReply reply = Read(true);
  ASSERT_NE(reply.image, nullptr) << reply.error;
  EXPECT_TRUE(g_bytes_equal(png.get(), reply.image->png.get()));
  // Rejected URI entries must not consume the 32 returned-file slots.
  std::string mixed;
  for (int index = 0; index < 33; ++index)
    mixed += "https://example.invalid/image.png\r\n";
  mixed += payload;
  Publish(Bytes(g_bytes_new(mixed.data(), mixed.size()), g_bytes_unref),
          "text/uri-list");
  bool done = false;
  service->ReadFiles([&](std::vector<std::string> files) {
    EXPECT_EQ(files, (std::vector<std::string>{filename}));
    done = true;
  });
  EXPECT_TRUE(Wait([&] { return done; }));
  EXPECT_EQ(std::remove(filename), 0);
}

TEST_F(ClipboardServiceTest, InvalidImageTargetsFallBackToLocalFile) {
  gchar* path = nullptr;
  const int descriptor =
      g_file_open_tmp("imclipboard-fallback-XXXXXX", &path, nullptr);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  g_autofree gchar* filename = path;
  const Bytes png = Png();
  gsize length;
  const auto* data =
      static_cast<const gchar*>(g_bytes_get_data(png.get(), &length));
  ASSERT_TRUE(g_file_set_contents(filename, data, length, nullptr));
  g_autofree gchar* uri = g_filename_to_uri(filename, nullptr, nullptr);
  const std::string payload = std::string(uri) + "\r\n";
  std::vector<std::string> requested;
  const Bytes invalid(g_bytes_new_static("invalid", 7), g_bytes_unref);
  PublishTargets({{"image/png", invalid}, {"image/jpeg", invalid},
                  {"text/uri-list", Bytes(g_bytes_new(payload.data(), payload.size()),
                                          g_bytes_unref)}}, &requested);

  for (const bool pixels : {false, true}) {
    requested.clear();
    const ImageReply reply = Read(pixels);

    ASSERT_NE(reply.image, nullptr) << reply.error;
    EXPECT_TRUE(reply.error.empty());
    EXPECT_TRUE(reply.diagnostics.target.empty());
    EXPECT_EQ(reply.image->width, 3);
    if (pixels) {
      EXPECT_TRUE(g_bytes_equal(png.get(), reply.image->png.get()));
    }
    EXPECT_EQ(requested, (std::vector<std::string>{
                             "image/png", "image/jpeg", "text/uri-list"}));
  }
  EXPECT_EQ(std::remove(filename), 0);
}

TEST_F(ClipboardServiceTest, EmptyClipboardCompletesWithoutImage) {
  const ImageReply reply = Read(true);
  EXPECT_TRUE(reply.error.empty()) << reply.error;
  EXPECT_EQ(reply.image, nullptr);
}

}  // namespace
}  // namespace imclipboard

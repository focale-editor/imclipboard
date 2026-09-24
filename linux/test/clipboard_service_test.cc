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

TEST_F(ClipboardServiceTest, EmptyClipboardCompletesWithoutImage) {
  const ImageReply reply = Read(true);
  EXPECT_TRUE(reply.error.empty()) << reply.error;
  EXPECT_EQ(reply.image, nullptr);
}

}  // namespace
}  // namespace imclipboard

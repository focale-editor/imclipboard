#ifndef IMCLIPBOARD_CLIPBOARD_SERVICE_H_
#define IMCLIPBOARD_CLIPBOARD_SERVICE_H_

#include <gtk/gtk.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace imclipboard {

// Immutable compressed bytes, independently retained across callbacks.
using Bytes = std::shared_ptr<GBytes>;

// Validated dimensions and optional PNG representation of one clipboard image.
struct Image {
  int width = 0;
  int height = 0;
  std::string token;
  Bytes png;
};

// Separates an empty clipboard from a failed or invalid transfer.
struct ImageReply {
  std::shared_ptr<Image> image;
  std::string error;
};

// GTK ownership stays on the main context; codecs and file I/O use workers.
// Operations are serialized so validation cannot reorder two writes.
class ClipboardService : public std::enable_shared_from_this<ClipboardService> {
 public:
  // Retains a clipboard on its originating main context.
  explicit ClipboardService(GtkClipboard* clipboard);
  ~ClipboardService();
  ClipboardService(const ClipboardService&) = delete;
  ClipboardService& operator=(const ClipboardService&) = delete;

  // Validates on a worker, then publishes the unchanged PNG and optional token.
  void Write(Bytes png, std::string token,
             std::function<void(std::string)> complete);
  // Reads one owner generation; metadata reads skip pixel decoding.
  void Read(bool include_png, std::function<void(ImageReply)> complete);
  // Resolves local regular file URIs on a worker.
  void ReadFiles(std::function<void(std::vector<std::string>)> complete);

 private:
  // Carries one transfer and the owner generation it belongs to.
  struct ReadRequest;
  // Serializes calls so asynchronous work preserves invocation order.
  void Enqueue(std::function<void()> operation);
  // Starts the next queued call once the current callback has completed.
  void Next();
  // Prefers native PNG before trying another image representation.
  void ReadTargets(const std::shared_ptr<ReadRequest>& request);
  // Falls back to local files when no image target is available.
  void ReadUris(const std::shared_ptr<ReadRequest>& request);
  // Obtains metadata only while the image owner remains unchanged.
  void ReadToken(const std::shared_ptr<ReadRequest>& request);
  // Rejects stale transfers before releasing the next queued call.
  void CompleteRead(const std::shared_ptr<ReadRequest>& request);

  // Retained GTK clipboard; all ownership access stays on the main context.
  GtkClipboard* clipboard_;
  // Latest publication identity, distinct for every write.
  GObject* owner_ = nullptr;
  // Signal subscription invalidating transfers after external replacement.
  gulong owner_changed_;
  // Generation captured at the start of each foreign read.
  guint64 revision_ = 0;
  // Includes work already dispatched to a worker or GTK callback.
  bool running_ = false;
  // Calls waiting for the active operation to finish.
  std::deque<std::function<void()>> pending_;
};

}  // namespace imclipboard
#endif

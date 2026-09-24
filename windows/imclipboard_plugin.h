#ifndef FLUTTER_PLUGIN_IMCLIPBOARD_PLUGIN_H_
#define FLUTTER_PLUGIN_IMCLIPBOARD_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <windows.h>

#include <memory>

namespace imclipboard {

// Serializes background bitmap preparation and main-thread publication.
class ClipboardWriter;

// Implements PNG image clipboard operations for Windows.
class ImclipboardPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  explicit ImclipboardPlugin(HWND window = nullptr);
  ~ImclipboardPlugin() override;

  ImclipboardPlugin(const ImclipboardPlugin&) = delete;
  ImclipboardPlugin& operator=(const ImclipboardPlugin&) = delete;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  HWND window_;
  std::unique_ptr<ClipboardWriter> writer_;
};

}  // namespace imclipboard

#endif  // FLUTTER_PLUGIN_IMCLIPBOARD_PLUGIN_H_

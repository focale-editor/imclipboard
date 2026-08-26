#include "include/imclipboard/imclipboard_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "imclipboard_plugin.h"

void ImclipboardPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  imclipboard::ImclipboardPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

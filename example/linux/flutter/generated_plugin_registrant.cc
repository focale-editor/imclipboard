//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <imclipboard/imclipboard_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) imclipboard_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "ImclipboardPlugin");
  imclipboard_plugin_register_with_registrar(imclipboard_registrar);
}

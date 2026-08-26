#include <flutter_linux/flutter_linux.h>
#include <gtest/gtest.h>

#include "include/imclipboard/imclipboard_plugin.h"

namespace imclipboard {
namespace test {

TEST(ImclipboardPlugin, ExposesPluginType) {
  EXPECT_NE(imclipboard_plugin_get_type(), G_TYPE_INVALID);
}

}  // namespace test
}  // namespace imclipboard

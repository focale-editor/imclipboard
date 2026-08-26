#include <flutter/method_call.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <memory>
#include <variant>

#include "imclipboard_plugin.h"

namespace imclipboard {
namespace test {

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResultFunctions;

}  // namespace

TEST(ImclipboardPlugin, ReportsSupport) {
  ImclipboardPlugin plugin;
  bool supported = false;
  plugin.HandleMethodCall(
      MethodCall("isSupported", std::make_unique<EncodableValue>()),
      std::make_unique<MethodResultFunctions<>>(
          [&supported](const EncodableValue* result) {
            supported = std::get<bool>(*result);
          },
          nullptr, nullptr));

  EXPECT_TRUE(supported);
}

}  // namespace test
}  // namespace imclipboard

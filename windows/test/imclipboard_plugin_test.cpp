#include "imclipboard_plugin.h"

#include <flutter/method_call.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <gtest/gtest.h>
#include <windows.h>

#include <memory>
#include <variant>

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

// Invalid PNGs exercise the real worker without touching the system clipboard.
TEST(ImclipboardPlugin, InvalidWritesCompleteInOrderOnThePlatformThread) {
  ImclipboardPlugin plugin;
  std::vector<int> completions;
  const DWORD platform_thread = ::GetCurrentThreadId();
  for (int index = 0; index < 2; ++index) {
    EncodableMap arguments = {{EncodableValue("bytes"),
                               EncodableValue(std::vector<uint8_t>{1, 2, 3})}};
    plugin.HandleMethodCall(
        MethodCall("writeImage", std::make_unique<EncodableValue>(arguments)),
        std::make_unique<MethodResultFunctions<>>(
            [](const EncodableValue*) {
              ADD_FAILURE() << "Invalid PNG accepted";
            },
            [&, index](const std::string& code, const std::string&,
                       const EncodableValue*) {
              EXPECT_EQ(::GetCurrentThreadId(), platform_thread);
              EXPECT_EQ(code, "write_failed");
              completions.push_back(index);
            },
            nullptr));
  }
  EXPECT_TRUE(completions.empty());
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  while (completions.size() < 2 && ::GetTickCount64() < deadline) {
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
    ::Sleep(1);
  }
  EXPECT_EQ(completions, (std::vector<int>{0, 1}));
}

TEST(ImclipboardPlugin, DestroyingPendingWriterDoesNotPublishOrCallBack) {
  bool called = false;
  {
    ImclipboardPlugin plugin;
    EncodableMap arguments = {{EncodableValue("bytes"),
                               EncodableValue(std::vector<uint8_t>{1, 2, 3})}};
    plugin.HandleMethodCall(
        MethodCall("writeImage", std::make_unique<EncodableValue>(arguments)),
        std::make_unique<MethodResultFunctions<>>(
            [&](const EncodableValue*) { called = true; },
            [&](const std::string&, const std::string&, const EncodableValue*) {
              called = true;
            },
            nullptr));
  }
  EXPECT_FALSE(called);
}

}  // namespace test
}  // namespace imclipboard

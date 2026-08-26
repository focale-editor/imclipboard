#include "imclipboard_plugin.h"

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace imclipboard {
namespace {

using Microsoft::WRL::ComPtr;

constexpr char kChannelName[] = "app.focaleeditor.imclipboard/image_clipboard";
constexpr wchar_t kPngFormatName[] = L"PNG";
constexpr wchar_t kTokenFormatName[] = L"application/x-imclipboard-token";
constexpr size_t kMaximumEncodedBytes = 512ULL * 1024ULL * 1024ULL;
constexpr UINT kMaximumFileCount = 32;
constexpr size_t kMaximumFilePathBytes = 32ULL * 1024ULL;

struct ClipboardImage {
  UINT width;
  UINT height;
  std::string token;
  std::vector<uint8_t> png;
};

class ScopedClipboard {
 public:
  explicit ScopedClipboard(HWND window) {
    for (int attempt = 0; attempt < 5 && !opened_; ++attempt) {
      opened_ = ::OpenClipboard(window) != FALSE;
      if (!opened_) {
        ::Sleep(5);
      }
    }
  }

  ~ScopedClipboard() {
    if (opened_) {
      ::CloseClipboard();
    }
  }

  ScopedClipboard(const ScopedClipboard&) = delete;
  ScopedClipboard& operator=(const ScopedClipboard&) = delete;

  bool opened() const { return opened_; }

 private:
  bool opened_ = false;
};

std::string HResultMessage(const char* operation, HRESULT result) {
  return std::string(operation) + " failed (HRESULT " +
         std::to_string(static_cast<unsigned long>(result)) + ")";
}

bool CreateFactory(ComPtr<IWICImagingFactory>* factory, std::string* error) {
  const HRESULT result = ::CoCreateInstance(
      CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(factory->ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    *error = HResultMessage("Creating the WIC factory", result);
    return false;
  }
  return true;
}

bool DecodePngFrame(IWICImagingFactory* factory,
                    const std::vector<uint8_t>& png,
                    ComPtr<IWICBitmapFrameDecode>* frame,
                    std::string* error) {
  if (png.empty() || png.size() > std::numeric_limits<DWORD>::max()) {
    *error = "The clipboard PNG is empty or too large";
    return false;
  }
  ComPtr<IWICStream> stream;
  HRESULT result = factory->CreateStream(&stream);
  if (SUCCEEDED(result)) {
    result = stream->InitializeFromMemory(
        const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()));
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (SUCCEEDED(result)) {
    result = factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
  }
  if (SUCCEEDED(result)) {
    result = decoder->GetFrame(0, frame->ReleaseAndGetAddressOf());
  }
  if (FAILED(result)) {
    *error = HResultMessage("Decoding the clipboard PNG", result);
    return false;
  }
  return true;
}

bool EncodePng(IWICImagingFactory* factory,
               IWICBitmapSource* source,
               std::vector<uint8_t>* png,
               std::string* error) {
  ComPtr<IStream> stream;
  HRESULT result = ::CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  ComPtr<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(result)) {
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(result)) {
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (SUCCEEDED(result)) {
    result = encoder->CreateNewFrame(&frame, &properties);
  }
  if (SUCCEEDED(result)) {
    result = frame->Initialize(properties.Get());
  }

  UINT width = 0;
  UINT height = 0;
  if (SUCCEEDED(result)) {
    result = source->GetSize(&width, &height);
  }
  if (SUCCEEDED(result)) {
    result = frame->SetSize(width, height);
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(result)) {
    result = frame->SetPixelFormat(&format);
  }
  if (SUCCEEDED(result)) {
    result = frame->WriteSource(source, nullptr);
  }
  if (SUCCEEDED(result)) {
    result = frame->Commit();
  }
  if (SUCCEEDED(result)) {
    result = encoder->Commit();
  }
  if (FAILED(result)) {
    *error = HResultMessage("Encoding the clipboard bitmap", result);
    return false;
  }

  HGLOBAL memory = nullptr;
  result = ::GetHGlobalFromStream(stream.Get(), &memory);
  const SIZE_T length = memory == nullptr ? 0 : ::GlobalSize(memory);
  if (FAILED(result) || length == 0 || length > kMaximumEncodedBytes) {
    *error = FAILED(result)
                 ? HResultMessage("Reading the encoded clipboard PNG", result)
                 : "The encoded clipboard PNG is empty or too large";
    return false;
  }
  const void* data = ::GlobalLock(memory);
  if (data == nullptr) {
    *error = "Could not lock the encoded clipboard PNG";
    return false;
  }
  png->assign(static_cast<const uint8_t*>(data),
              static_cast<const uint8_t*>(data) + length);
  ::GlobalUnlock(memory);
  return true;
}

std::vector<uint8_t> ReadGlobalBytes(UINT format) {
  HANDLE handle = ::GetClipboardData(format);
  if (handle == nullptr) {
    return {};
  }
  const SIZE_T length = ::GlobalSize(handle);
  if (length == 0 || length > kMaximumEncodedBytes) {
    return {};
  }
  const void* data = ::GlobalLock(handle);
  if (data == nullptr) {
    return {};
  }
  std::vector<uint8_t> result(static_cast<const uint8_t*>(data),
                              static_cast<const uint8_t*>(data) + length);
  ::GlobalUnlock(handle);
  return result;
}

std::string ReadToken(UINT token_format) {
  const std::vector<uint8_t> bytes = ReadGlobalBytes(token_format);
  if (bytes.empty() || bytes.size() > 1024) {
    return {};
  }
  const auto terminator = std::find(bytes.begin(), bytes.end(), uint8_t{0});
  return std::string(bytes.begin(), terminator);
}

bool IsLocalAbsolutePath(const std::wstring& path) {
  const auto is_slash = [](wchar_t character) {
    return character == L'\\' || character == L'/';
  };
  const auto is_drive_letter = [](wchar_t character) {
    return (character >= L'A' && character <= L'Z') ||
           (character >= L'a' && character <= L'z');
  };
  const bool drive_path =
      path.size() >= 3 && is_drive_letter(path[0]) && path[1] == L':' &&
      is_slash(path[2]);
  const bool extended_drive_path =
      path.size() >= 7 && path.compare(0, 4, L"\\\\?\\") == 0 &&
      is_drive_letter(path[4]) && path[5] == L':' && is_slash(path[6]);
  return drive_path || extended_drive_path;
}

std::optional<std::string> WideToUtf8(const std::wstring& value) {
  if (value.empty() || value.size() > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  const int length = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0 || static_cast<size_t>(length) > kMaximumFilePathBytes) {
    return std::nullopt;
  }
  std::string result(static_cast<size_t>(length), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            length, nullptr, nullptr) != length) {
    return std::nullopt;
  }
  return result;
}

bool ReadClipboardFiles(HWND window,
                        std::vector<std::string>* files,
                        std::string* error) {
  ScopedClipboard clipboard(window);
  if (!clipboard.opened()) {
    *error = "The Windows clipboard is busy";
    return false;
  }
  if (!::IsClipboardFormatAvailable(CF_HDROP)) {
    return true;
  }
  HDROP drop = static_cast<HDROP>(::GetClipboardData(CF_HDROP));
  if (drop == nullptr) {
    *error = "Windows could not read the clipboard file list";
    return false;
  }
  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT index = 0;
       index < count && files->size() < kMaximumFileCount; ++index) {
    const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
    if (length == 0 || length >= 32767) {
      continue;
    }
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    const UINT copied =
        ::DragQueryFileW(drop, index, path.data(), length + 1);
    if (copied == 0) {
      continue;
    }
    path.resize(copied);
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (!IsLocalAbsolutePath(path) || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    if (const auto utf8 = WideToUtf8(path); utf8.has_value()) {
      files->push_back(*utf8);
    }
  }
  return true;
}

bool ClipboardBitmapUsesAlpha() {
  if (!::IsClipboardFormatAvailable(CF_DIBV5)) {
    return false;
  }
  HANDLE handle = ::GetClipboardData(CF_DIBV5);
  if (handle == nullptr || ::GlobalSize(handle) < sizeof(BITMAPV5HEADER)) {
    return false;
  }
  const auto* header =
      static_cast<const BITMAPV5HEADER*>(::GlobalLock(handle));
  const bool uses_alpha =
      header != nullptr && header->bV5Size >= sizeof(BITMAPV5HEADER) &&
      header->bV5BitCount == 32 && header->bV5AlphaMask != 0;
  if (header != nullptr) {
    ::GlobalUnlock(handle);
  }
  return uses_alpha;
}

std::optional<ClipboardImage> ReadClipboardImage(HWND window,
                                                 bool include_png,
                                                 std::string* error) {
  ScopedClipboard clipboard(window);
  if (!clipboard.opened()) {
    *error = "The Windows clipboard is busy";
    return std::nullopt;
  }
  const UINT png_format = ::RegisterClipboardFormatW(kPngFormatName);
  const UINT token_format = ::RegisterClipboardFormatW(kTokenFormatName);
  const std::string token = ReadToken(token_format);

  ComPtr<IWICImagingFactory> factory;
  if (!CreateFactory(&factory, error)) {
    return std::nullopt;
  }
  if (png_format != 0 && ::IsClipboardFormatAvailable(png_format)) {
    std::vector<uint8_t> png = ReadGlobalBytes(png_format);
    ComPtr<IWICBitmapFrameDecode> frame;
    std::string png_error;
    if (DecodePngFrame(factory.Get(), png, &frame, &png_error)) {
      UINT width = 0;
      UINT height = 0;
      if (SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 &&
          height > 0) {
        return ClipboardImage{width, height, token,
                              include_png ? std::move(png)
                                          : std::vector<uint8_t>()};
      }
    }
  }

  if (!::IsClipboardFormatAvailable(CF_BITMAP) &&
      !::IsClipboardFormatAvailable(CF_DIB) &&
      !::IsClipboardFormatAvailable(CF_DIBV5)) {
    return std::nullopt;
  }
  HBITMAP bitmap = static_cast<HBITMAP>(::GetClipboardData(CF_BITMAP));
  if (bitmap == nullptr) {
    *error = "Windows could not synthesize a clipboard bitmap";
    return std::nullopt;
  }
  BITMAP description = {};
  if (::GetObject(bitmap, sizeof(description), &description) == 0 ||
      description.bmWidth < 1 || description.bmHeight < 1) {
    *error = "The clipboard bitmap has invalid dimensions";
    return std::nullopt;
  }

  std::vector<uint8_t> png;
  if (include_png) {
    ComPtr<IWICBitmap> wic_bitmap;
    const WICBitmapAlphaChannelOption alpha =
        ClipboardBitmapUsesAlpha() ? WICBitmapUseAlpha : WICBitmapIgnoreAlpha;
    HRESULT result = factory->CreateBitmapFromHBITMAP(
        bitmap, nullptr, alpha, &wic_bitmap);
    if (FAILED(result)) {
      *error = HResultMessage("Reading the clipboard bitmap", result);
      return std::nullopt;
    }
    if (!EncodePng(factory.Get(), wic_bitmap.Get(), &png, error)) {
      return std::nullopt;
    }
  }
  return ClipboardImage{static_cast<UINT>(description.bmWidth),
                        static_cast<UINT>(description.bmHeight), token,
                        std::move(png)};
}

HGLOBAL AllocateGlobal(const void* data, size_t length) {
  if (data == nullptr || length == 0) {
    return nullptr;
  }
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, length);
  if (memory == nullptr) {
    return nullptr;
  }
  void* destination = ::GlobalLock(memory);
  if (destination == nullptr) {
    ::GlobalFree(memory);
    return nullptr;
  }
  std::memcpy(destination, data, length);
  ::GlobalUnlock(memory);
  return memory;
}

HGLOBAL CreateDibV5(IWICImagingFactory* factory,
                    IWICBitmapSource* source,
                    std::string* error) {
  UINT width = 0;
  UINT height = 0;
  HRESULT result = source->GetSize(&width, &height);
  const uint64_t stride = static_cast<uint64_t>(width) * 4;
  const uint64_t pixel_bytes = stride * height;
  if (FAILED(result) || width == 0 || height == 0 ||
      width > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
      height > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
      pixel_bytes > std::numeric_limits<DWORD>::max() ||
      pixel_bytes + sizeof(BITMAPV5HEADER) >
          std::numeric_limits<SIZE_T>::max()) {
    *error = "The clipboard PNG dimensions are not supported";
    return nullptr;
  }

  ComPtr<IWICFormatConverter> converter;
  result = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(result)) {
    result = converter->Initialize(
        source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
        0, WICBitmapPaletteTypeCustom);
  }
  if (FAILED(result)) {
    *error = HResultMessage("Converting the clipboard PNG", result);
    return nullptr;
  }

  const SIZE_T allocation =
      sizeof(BITMAPV5HEADER) + static_cast<SIZE_T>(pixel_bytes);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, allocation);
  auto* header = memory == nullptr
                     ? nullptr
                     : static_cast<BITMAPV5HEADER*>(::GlobalLock(memory));
  if (header == nullptr) {
    if (memory != nullptr) {
      ::GlobalFree(memory);
    }
    *error = "Could not allocate the Windows clipboard bitmap";
    return nullptr;
  }

  header->bV5Size = sizeof(BITMAPV5HEADER);
  header->bV5Width = static_cast<LONG>(width);
  header->bV5Height = -static_cast<LONG>(height);
  header->bV5Planes = 1;
  header->bV5BitCount = 32;
  header->bV5Compression = BI_BITFIELDS;
  header->bV5SizeImage = static_cast<DWORD>(pixel_bytes);
  header->bV5RedMask = 0x00FF0000;
  header->bV5GreenMask = 0x0000FF00;
  header->bV5BlueMask = 0x000000FF;
  header->bV5AlphaMask = 0xFF000000;
  header->bV5CSType = LCS_sRGB;
  header->bV5Intent = LCS_GM_IMAGES;

  BYTE* pixels = reinterpret_cast<BYTE*>(header) + sizeof(BITMAPV5HEADER);
  result = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                 static_cast<UINT>(pixel_bytes), pixels);
  ::GlobalUnlock(memory);
  if (FAILED(result)) {
    ::GlobalFree(memory);
    *error = HResultMessage("Copying the clipboard PNG pixels", result);
    return nullptr;
  }
  return memory;
}

bool WriteClipboardImage(HWND window,
                         const std::vector<uint8_t>& png,
                         const std::optional<std::string>& token,
                         std::string* error) {
  if (png.empty() || png.size() > kMaximumEncodedBytes ||
      (token.has_value() &&
       (token->empty() || token->size() > 1024 ||
        token->find('\0') != std::string::npos))) {
    *error = "The clipboard image or token is invalid";
    return false;
  }

  ComPtr<IWICImagingFactory> factory;
  if (!CreateFactory(&factory, error)) {
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (!DecodePngFrame(factory.Get(), png, &frame, error)) {
    return false;
  }

  HGLOBAL dib = CreateDibV5(factory.Get(), frame.Get(), error);
  HGLOBAL png_memory = AllocateGlobal(png.data(), png.size());
  HGLOBAL token_memory = nullptr;
  if (token.has_value()) {
    const std::string terminated_token = *token + '\0';
    token_memory =
        AllocateGlobal(terminated_token.data(), terminated_token.size());
  }
  if (dib == nullptr || png_memory == nullptr ||
      (token.has_value() && token_memory == nullptr)) {
    if (dib != nullptr) ::GlobalFree(dib);
    if (png_memory != nullptr) ::GlobalFree(png_memory);
    if (token_memory != nullptr) ::GlobalFree(token_memory);
    if (error->empty()) *error = "Could not allocate Windows clipboard data";
    return false;
  }

  ScopedClipboard clipboard(window);
  if (!clipboard.opened()) {
    ::GlobalFree(dib);
    ::GlobalFree(png_memory);
    if (token_memory != nullptr) ::GlobalFree(token_memory);
    *error = "The Windows clipboard is busy";
    return false;
  }
  const UINT png_format = ::RegisterClipboardFormatW(kPngFormatName);
  const UINT token_format = token.has_value()
                                ? ::RegisterClipboardFormatW(kTokenFormatName)
                                : 0;
  if (png_format == 0 || (token.has_value() && token_format == 0) ||
      !::EmptyClipboard()) {
    ::GlobalFree(dib);
    ::GlobalFree(png_memory);
    if (token_memory != nullptr) ::GlobalFree(token_memory);
    *error = "Windows refused clipboard ownership";
    return false;
  }

  bool success = true;
  if (::SetClipboardData(png_format, png_memory) != nullptr) {
    png_memory = nullptr;
  } else {
    success = false;
  }
  if (success && ::SetClipboardData(CF_DIBV5, dib) != nullptr) {
    dib = nullptr;
  } else {
    success = false;
  }
  if (success && token.has_value()) {
    if (::SetClipboardData(token_format, token_memory) != nullptr) {
      token_memory = nullptr;
    } else {
      success = false;
    }
  }
  if (!success) {
    ::EmptyClipboard();
    if (dib != nullptr) ::GlobalFree(dib);
    if (png_memory != nullptr) ::GlobalFree(png_memory);
    if (token_memory != nullptr) ::GlobalFree(token_memory);
    *error = "Windows could not publish every clipboard format";
  }
  return success;
}

flutter::EncodableValue ImageResult(const ClipboardImage& image,
                                    bool include_png) {
  flutter::EncodableMap response = {
      {flutter::EncodableValue("width"),
       flutter::EncodableValue(static_cast<int32_t>(image.width))},
      {flutter::EncodableValue("height"),
       flutter::EncodableValue(static_cast<int32_t>(image.height))},
  };
  if (!image.token.empty()) {
    response[flutter::EncodableValue("token")] =
        flutter::EncodableValue(image.token);
  }
  if (include_png) {
    response[flutter::EncodableValue("bytes")] =
        flutter::EncodableValue(image.png);
  }
  return flutter::EncodableValue(response);
}

const flutter::EncodableValue* MapValue(const flutter::EncodableMap& map,
                                        const char* key) {
  const auto found = map.find(flutter::EncodableValue(key));
  return found == map.end() ? nullptr : &found->second;
}

}  // namespace

void ImclipboardPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), kChannelName,
          &flutter::StandardMethodCodec::GetInstance());
  HWND window = registrar->GetView() == nullptr
                    ? nullptr
                    : registrar->GetView()->GetNativeWindow();
  auto plugin = std::make_unique<ImclipboardPlugin>(window);
  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });
  registrar->AddPlugin(std::move(plugin));
}

ImclipboardPlugin::ImclipboardPlugin(HWND window) : window_(window) {}

ImclipboardPlugin::~ImclipboardPlugin() = default;

void ImclipboardPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name() == "isSupported") {
    result->Success(flutter::EncodableValue(true));
    return;
  }
  if (method_call.method_name() == "readImageFiles") {
    std::vector<std::string> files;
    std::string error;
    if (!ReadClipboardFiles(window_, &files, &error)) {
      result->Error("read_failed", error);
      return;
    }
    flutter::EncodableList response;
    response.reserve(files.size());
    for (const auto& path : files) {
      response.emplace_back(path);
    }
    result->Success(flutter::EncodableValue(response));
    return;
  }
  if (method_call.method_name() == "readImageInfo" ||
      method_call.method_name() == "readImage") {
    std::string error;
    const bool include_png = method_call.method_name() == "readImage";
    const std::optional<ClipboardImage> image =
        ReadClipboardImage(window_, include_png, &error);
    if (!image.has_value()) {
      if (error.empty()) {
        result->Success();
      } else {
        result->Error("read_failed", error);
      }
      return;
    }
    result->Success(ImageResult(*image, include_png));
    return;
  }
  if (method_call.method_name() == "writeImage") {
    const auto* arguments = method_call.arguments() == nullptr
                                ? nullptr
                                : std::get_if<flutter::EncodableMap>(
                                      method_call.arguments());
    const flutter::EncodableValue* bytes_value =
        arguments == nullptr ? nullptr : MapValue(*arguments, "bytes");
    const flutter::EncodableValue* token_value =
        arguments == nullptr ? nullptr : MapValue(*arguments, "token");
    const auto* bytes = bytes_value == nullptr
                            ? nullptr
                            : std::get_if<std::vector<uint8_t>>(bytes_value);
    if (bytes == nullptr || bytes->empty() ||
        bytes->size() > kMaximumEncodedBytes) {
      result->Error("invalid_arguments", "Expected valid PNG bytes");
      return;
    }

    std::optional<std::string> token;
    if (token_value != nullptr) {
      const auto* token_string = std::get_if<std::string>(token_value);
      if (token_string == nullptr || token_string->empty() ||
          token_string->size() > 1024 ||
          token_string->find('\0') != std::string::npos) {
        result->Error("invalid_arguments", "The clipboard token is invalid");
        return;
      }
      token = *token_string;
    }

    std::string error;
    if (!WriteClipboardImage(window_, *bytes, token, &error)) {
      result->Error("write_failed", error);
      return;
    }
    result->Success();
    return;
  }
  result->NotImplemented();
}

}  // namespace imclipboard

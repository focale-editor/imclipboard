# Imclipboard

Imclipboard is a focused Flutter plugin for reading and writing images through the system clipboard. Its desktop implementation was extracted from [Focale](https://github.com/focale-editor/focale), then generalized for reuse and extended to Android, iOS, and the web.

Clipboard reads and native writes use PNG as a stable cross-platform representation. On native platforms, encoded BMP, JPEG, JPEG XL, PNG, QOI, TGA, TIFF, and WebP input can also be copied: Imclipboard uses [Imcodec](https://pub.dev/packages/imcodec) to detect and normalize non-PNG input before publishing it. The plugin can inspect dimensions without transferring image bytes on native platforms, distinguish an empty clipboard from an unavailable implementation, and preserve an optional application token beside the interoperable image.

## Platform support

| Platform | Read/write images | Copied local file paths    | Notes                                                                                                                |
|----------|-------------------|----------------------------|----------------------------------------------------------------------------------------------------------------------|
| Android  | Yes               | No                         | Images are exchanged through a temporary, read-only content URI.                                                     |
| iOS      | Yes               | When supplied as file URLs | Read from an explicit paste action to follow iOS pasteboard privacy behavior.                                        |
| Linux    | Yes               | Yes                        | Uses GTK 3 and supports image/file clipboards on the active desktop session.                                         |
| macOS    | Yes               | Yes                        | Uses `NSPasteboard`; iOS and macOS share one `darwin` implementation.                                                |
| Web      | PNG               | No                         | Requires HTTPS or localhost, permission, and a user gesture. Copied file references are not exposed as local images. |
| Windows  | Yes               | Yes                        | Publishes both PNG and alpha-aware `CF_DIBV5` data.                                                                  |

## Usage

Add the dependency, then create a lightweight client:

```dart
import 'dart:typed_data';

import 'package:imclipboard/imclipboard.dart';

const ImClipboard clipboard = ImClipboard();

Future<void> copyPng(Uint8List pngBytes) async {
  await clipboard.writeImage(pngBytes, token: 'current-edit');
}

Future<void> copyEncodedImage(Uint8List encodedBytes) async {
  await clipboard.writeEncodedImage(encodedBytes);
}

Future<ClipboardImage?> pastePng() async {
  final ClipboardReadResult<ClipboardImage> result = await clipboard.readImage();
  if (!result.supported) {
    return null;
  }
  return result.value;
}
```

`writeEncodedImage` detects the format from the image signature. Pass `format: ClipboardImageFormat.jpeg`, for example, when the expected encoding should also be validated. PNG input is forwarded unchanged; other supported formats are decoded and re-encoded as PNG on native platforms. The Web implementation accepts PNG input only. Animated input uses its first frame, and encoded metadata is not preserved during transcoding.

Inspect dimensions without moving a potentially large PNG through a native method channel:

```dart
final ClipboardReadResult<ClipboardImageInfo> result = await clipboard.readImageInfo();
final ClipboardImageInfo? info = result.value;
```

On the web and iOS, call read methods from a visible paste button or another explicit user action. Browsers may reject an otherwise supported operation with `ImClipboardException` when clipboard permission or transient user activation is missing.

## Result and error semantics

- `supported: false` means the native plugin or browser API is unavailable.
- `supported: true, value: null` means the clipboard is accessible but contains no compatible image.
- Platform, permission, malformed-data, and size failures throw `ImClipboardException`.
- Encoded payloads and tokens are limited to 512 MiB and 1024 UTF-8 bytes respectively.
- Transcoding rejects images containing more than 100 million decoded pixels.

Tokens are optional and are not secrets. Desktop and Apple platforms store a private clipboard format, Android embeds the token in the plugin-owned content URI, and web matches it to the most recent same-page write with a local fingerprint.

See the [example application](example/lib/main.dart) for copy and paste buttons that also satisfy browser user-gesture requirements.

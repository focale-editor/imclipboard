# Imclipboard

Imclipboard is a focused Flutter plugin for reading and writing images through the system clipboard. Its desktop implementation was extracted from [Focale](https://github.com/focale-editor/focale), then generalized for reuse and extended to Android, iOS, and the web.

Images cross the Flutter boundary as PNG bytes. The plugin can inspect dimensions without transferring those bytes on native platforms, distinguish an empty clipboard from an unavailable implementation, and preserve an optional application token beside the interoperable image.

## Platform support

| Platform | Read/write images | Copied local file paths | Notes |
| --- | --- | --- | --- |
| Android | Yes | No | Images are exchanged through a temporary, read-only content URI. |
| iOS | Yes | When supplied as file URLs | Read from an explicit paste action to follow iOS pasteboard privacy behavior. |
| Linux | Yes | Yes | Uses GTK 3 and supports image/file clipboards on the active desktop session. |
| macOS | Yes | Yes | Uses `NSPasteboard`; iOS and macOS share one `darwin` implementation. |
| Web | PNG | No | Requires HTTPS or localhost, a compatible browser, permission, and normally a user gesture. |
| Windows | Yes | Yes | Publishes both PNG and alpha-aware `CF_DIBV5` data. |

`readImageFiles` intentionally returns only existing absolute paths. Android content URIs and browser `File` objects are consumed internally or omitted rather than being misrepresented as local paths.

## Usage

Add the dependency, then create a lightweight client:

```dart
import 'dart:typed_data';

import 'package:imclipboard/imclipboard.dart';

const ImClipboard clipboard = ImClipboard();

Future<void> copyPng(Uint8List pngBytes) async {
  await clipboard.writeImage(pngBytes, token: 'current-edit');
}

Future<ClipboardImage?> pastePng() async {
  final ClipboardReadResult<ClipboardImage> result = await clipboard.readImage();
  if (!result.supported) {
    return null;
  }
  return result.value;
}
```

Inspect dimensions without moving a potentially large PNG through a native method channel:

```dart
final ClipboardReadResult<ClipboardImageInfo> result =
    await clipboard.readImageInfo();
final ClipboardImageInfo? info = result.value;
```

On the web and iOS, call read methods from a visible paste button or another explicit user action. Browsers may reject an otherwise supported operation with `ImClipboardException` when clipboard permission or transient user activation is missing.

## Result and error semantics

- `supported: false` means the native plugin or browser API is unavailable.
- `supported: true, value: null` means the clipboard is accessible but contains no compatible image.
- Platform, permission, malformed-data, and size failures throw `ImClipboardException`.
- PNG payloads and tokens are limited to 512 MiB and 1024 UTF-8 bytes respectively.

Tokens are optional and are not secrets. Desktop and Apple platforms store a private clipboard format, Android embeds the token in the plugin-owned content URI, and web matches it to the most recent same-page write with a local fingerprint.

See the [example application](example/lib/main.dart) for copy and paste buttons that also satisfy browser user-gesture requirements.

## License

Imclipboard is available under the [MIT License](LICENSE).

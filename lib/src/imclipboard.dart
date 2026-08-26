import 'dart:typed_data';

import 'package:imclipboard/src/platform_interface.dart';

/// Reads and writes images through the operating system clipboard.
///
/// All images crossing the Dart boundary are PNG encoded. Clipboard reads
/// distinguish an unsupported platform from a supported but currently empty
/// clipboard through [ClipboardReadResult.supported].
final class ImClipboard {
  /// Creates an image clipboard client.
  const ImClipboard();

  /// Largest encoded PNG accepted by the plugin.
  static const int maxEncodedBytes = ImClipboardPlatform.maxEncodedBytes;

  /// Whether this platform provides an image clipboard implementation.
  Future<bool> isSupported() => ImClipboardPlatform.instance.isSupported();

  /// Reads image dimensions without transferring PNG bytes over native channels.
  Future<ClipboardReadResult<ClipboardImageInfo>> readImageInfo() => ImClipboardPlatform.instance.readImageInfo();

  /// Reads the first image on the clipboard as PNG.
  Future<ClipboardReadResult<ClipboardImage>> readImage() => ImClipboardPlatform.instance.readImage();

  /// Reads existing absolute file paths advertised by the clipboard.
  ///
  /// Desktop file managers commonly expose copied images as file references.
  /// Platforms without local path clipboards return an empty list while still
  /// reporting the plugin as supported.
  Future<ClipboardReadResult<List<String>>> readImageFiles() => ImClipboardPlatform.instance.readImageFiles();

  /// Replaces the clipboard with [pngBytes].
  ///
  /// [token] is optional application metadata. When supplied, supporting
  /// platforms return it from subsequent reads of the same clipboard entry.
  /// This can avoid decoding a flattened image when an application also keeps
  /// a richer in-process representation.
  ///
  /// Returns `false` only when no platform implementation is installed.
  Future<bool> writeImage(
    Uint8List pngBytes, {
    String? token,
  }) => ImClipboardPlatform.instance.writeImage(pngBytes, token: token);
}

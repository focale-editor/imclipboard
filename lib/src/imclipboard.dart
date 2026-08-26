import 'dart:typed_data';

import 'package:imclipboard/src/image_format.dart';
import 'package:imclipboard/src/image_transcoder.dart';
import 'package:imclipboard/src/platform_interface.dart';

/// Reads and writes images through the operating system clipboard.
///
/// All images crossing the Dart boundary are PNG encoded. Clipboard reads
/// distinguish an unsupported platform from a supported but currently empty
/// clipboard through [ClipboardReadResult.supported].
final class ImClipboard {
  /// Creates an image clipboard client.
  const ImClipboard();

  /// Largest encoded image accepted by the plugin.
  static const int maxEncodedBytes = ImClipboardPlatform.maxEncodedBytes;

  /// Largest image decoded while transcoding to PNG.
  static const int maxDecodedPixels = 100000000;

  /// Whether this platform provides an image clipboard implementation.
  Future<bool> isSupported() => ImClipboardPlatform.instance.isSupported();

  /// Reads image dimensions without transferring PNG bytes over native channels.
  Future<ClipboardReadResult<ClipboardImageInfo>> readImageInfo() => ImClipboardPlatform.instance.readImageInfo();

  /// Reads the first image on the clipboard as PNG.
  Future<ClipboardReadResult<ClipboardImage>> readImage() => ImClipboardPlatform.instance.readImage();

  /// Reads existing absolute file paths advertised by the clipboard.
  ///
  /// Desktop file managers commonly expose copied files by reference. Paths
  /// are returned regardless of file type so callers can apply their own
  /// import policy.
  /// Platforms without local path clipboards return an empty list while still
  /// reporting the plugin as supported.
  Future<ClipboardReadResult<List<String>>> readFiles() => ImClipboardPlatform.instance.readFiles();

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

  /// Replaces the clipboard with an image encoded in any supported [format].
  ///
  /// When [format] is omitted, the encoding is detected from [encodedBytes].
  /// PNG input is passed through unchanged. BMP, JPEG, JPEG XL, QOI, TGA,
  /// TIFF, and WebP input is decoded with `imcodec` and normalized to PNG for
  /// consistent clipboard interoperability. Animated inputs use their first
  /// frame, and transcoding does not preserve encoded metadata.
  ///
  /// Returns `false` only when no platform implementation is installed.
  Future<bool> writeEncodedImage(
    Uint8List encodedBytes, {
    ClipboardImageFormat? format,
    String? token,
  }) async {
    ImClipboardPlatform.validateImageArguments(
      encodedBytes,
      token,
      operation: 'writeEncodedImage',
    );
    if (!await isSupported()) {
      return false;
    }

    final Uint8List pngBytes;
    try {
      pngBytes = await normalizeImageToPng(
        encodedBytes,
        expectedFormat: format,
        maxDecodedPixels: maxDecodedPixels,
      );
    } on Object catch (error, stackTrace) {
      throw ImClipboardException(
        operation: 'writeEncodedImage',
        cause: error,
        stackTrace: stackTrace,
      );
    }

    return writeImage(pngBytes, token: token);
  }
}

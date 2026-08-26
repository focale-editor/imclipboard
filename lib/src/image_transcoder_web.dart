import 'dart:typed_data';

import 'package:imclipboard/src/image_format.dart';

/// Validates PNG input for the browser clipboard implementation.
Future<Uint8List> normalizeImageToPng(
  Uint8List encodedBytes, {
  required ClipboardImageFormat? expectedFormat,
  required int maxDecodedPixels,
}) {
  final bool isPng = _hasPngSignature(encodedBytes);
  if (isPng) {
    if (expectedFormat != null && expectedFormat != ClipboardImageFormat.png) {
      throw FormatException('Expected ${expectedFormat.name} data, found png.');
    }
    return Future<Uint8List>.value(encodedBytes);
  }
  if (expectedFormat == ClipboardImageFormat.png) {
    throw const FormatException('Expected png data, found an unknown format.');
  }
  throw UnsupportedError('Only PNG encoded input is supported by the Web clipboard implementation.');
}

/// Whether [bytes] begins with the complete PNG signature.
bool _hasPngSignature(Uint8List bytes) =>
    bytes.length >= 8 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4e && bytes[3] == 0x47 && bytes[4] == 0x0d && bytes[5] == 0x0a && bytes[6] == 0x1a && bytes[7] == 0x0a;

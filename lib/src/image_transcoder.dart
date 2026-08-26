import 'dart:typed_data';

import 'package:imclipboard/src/image_format.dart';
import 'package:imclipboard/src/image_transcoder_native.dart' if (dart.library.js_interop) 'package:imclipboard/src/image_transcoder_web.dart' as implementation;

/// Normalizes [encodedBytes] to PNG for the platform clipboard boundary.
Future<Uint8List> normalizeImageToPng(
  Uint8List encodedBytes, {
  required ClipboardImageFormat? expectedFormat,
  required int maxDecodedPixels,
}) => Future<Uint8List>.sync(
  () => implementation.normalizeImageToPng(
    encodedBytes,
    expectedFormat: expectedFormat,
    maxDecodedPixels: maxDecodedPixels,
  ),
);

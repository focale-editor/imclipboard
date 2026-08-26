import 'package:flutter/foundation.dart';
import 'package:imclipboard/src/image_format.dart';
import 'package:imcodec/imcodec.dart' as imcodec;

/// Normalizes a natively supported encoded image to PNG with Imcodec.
Future<Uint8List> normalizeImageToPng(
  Uint8List encodedBytes, {
  required ClipboardImageFormat? expectedFormat,
  required int maxDecodedPixels,
}) async {
  final imcodec.ImageFormat? detectedFormat = imcodec.ImageFormat.sniff(encodedBytes);
  if (detectedFormat == null) {
    throw const FormatException('The encoded image format is not supported.');
  }
  final imcodec.ImageFormat? expectedImcodecFormat = expectedFormat == null ? null : _toImcodecFormat(expectedFormat);
  if (expectedImcodecFormat != null && expectedImcodecFormat != detectedFormat) {
    throw FormatException('Expected ${expectedImcodecFormat.name} data, found ${detectedFormat.name}.');
  }
  if (detectedFormat == imcodec.ImageFormat.png) {
    return encodedBytes;
  }

  final imcodec.Image decodedImage = await imcodec.decodeImage(
    encodedBytes,
    maxPixels: maxDecodedPixels,
  );
  return compute(_encodePng, decodedImage, debugLabel: 'Imclipboard PNG encoding');
}

/// Encodes a decoded image in a worker isolate when the platform supports it.
Uint8List _encodePng(imcodec.Image image) => imcodec.encodePng(image);

/// Maps the public clipboard encoding to its Imcodec counterpart.
imcodec.ImageFormat _toImcodecFormat(ClipboardImageFormat format) => switch (format) {
  ClipboardImageFormat.bmp => imcodec.ImageFormat.bmp,
  ClipboardImageFormat.jpeg => imcodec.ImageFormat.jpeg,
  ClipboardImageFormat.jpegXl => imcodec.ImageFormat.jpegXl,
  ClipboardImageFormat.png => imcodec.ImageFormat.png,
  ClipboardImageFormat.qoi => imcodec.ImageFormat.qoi,
  ClipboardImageFormat.tga => imcodec.ImageFormat.tga,
  ClipboardImageFormat.tiff => imcodec.ImageFormat.tiff,
  ClipboardImageFormat.webp => imcodec.ImageFormat.webp,
};

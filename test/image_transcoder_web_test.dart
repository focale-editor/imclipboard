@TestOn('browser')
library;

import 'dart:convert';
import 'dart:js_interop';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard/src/image_format.dart';
import 'package:imclipboard/src/image_transcoder.dart';
import 'package:imclipboard/src/platform_interface.dart';
import 'package:web/web.dart' as web;

/// Verifies the browser-specific PNG normalization contract.
void main() {
  test('passes PNG input through unchanged', () async {
    final Uint8List pngBytes = Uint8List.fromList(<int>[0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

    final Uint8List normalizedBytes = await normalizeImageToPng(
      pngBytes,
      expectedFormat: ClipboardImageFormat.png,
      maxDecodedPixels: 1,
    );

    expect(normalizedBytes, same(pngBytes));
  });

  test('rejects non-PNG and mismatched input', () async {
    final Uint8List pngBytes = Uint8List.fromList(<int>[0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

    await expectLater(
      normalizeImageToPng(
        Uint8List.fromList(<int>[0xff, 0xd8, 0xff]),
        expectedFormat: ClipboardImageFormat.jpeg,
        maxDecodedPixels: 1,
      ),
      throwsUnsupportedError,
    );
    await expectLater(
      normalizeImageToPng(
        pngBytes,
        expectedFormat: ClipboardImageFormat.jpeg,
        maxDecodedPixels: 1,
      ),
      throwsFormatException,
    );
  });

  test('preserves and decodes PNG bytes through a browser Blob', () async {
    final Uint8List sourceBytes = base64Decode(
      'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=',
    );
    final JSArray<JSAny> parts = <JSAny>[sourceBytes.toJS].toJS;
    final web.Blob blob = web.Blob(parts, web.BlobPropertyBag(type: 'image/png'));

    final JSArrayBuffer buffer = await blob.arrayBuffer().toDart;
    final Uint8List blobBytes = JSUint8Array(buffer).toDart;
    final ClipboardImage clipboardImage = ClipboardImage(
      info: const ClipboardImageInfo(width: 1, height: 1),
      pngBytes: blobBytes,
    );
    final ui.Codec codec = await ui.instantiateImageCodec(clipboardImage.pngBytes);
    final ui.FrameInfo frame = await codec.getNextFrame();

    expect(blobBytes, sourceBytes);
    expect(frame.image.width, 1);
    expect(frame.image.height, 1);
    frame.image.dispose();
    codec.dispose();
  });
}

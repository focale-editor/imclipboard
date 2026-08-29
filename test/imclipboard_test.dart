import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard/imclipboard.dart';
import 'package:imclipboard/src/method_channel.dart';
import 'package:imclipboard/src/platform_interface.dart';
import 'package:imcodec/imcodec.dart' as imcodec;

/// In-memory platform used to verify the public facade.
final class _FakePlatform extends ImClipboardPlatform {
  /// Image returned by clipboard reads.
  final ClipboardImage image = ClipboardImage(
    info: const ClipboardImageInfo(width: 2, height: 3, token: 'fake'),
    pngBytes: Uint8List.fromList(<int>[1, 2, 3]),
  );

  /// Number of write calls received by the fake.
  int writeCount = 0;

  /// Most recent PNG bytes received by the fake.
  Uint8List? writtenPngBytes;

  /// Most recent ownership token received by the fake.
  String? writtenToken;

  @override
  Future<bool> isSupported() async => true;

  @override
  Future<ClipboardReadResult<ClipboardImage>> readImage() async => ClipboardReadResult(supported: true, value: image);

  @override
  Future<ClipboardReadResult<List<String>>> readFiles() async => const ClipboardReadResult(supported: true, value: <String>['/tmp/image.png']);

  @override
  Future<ClipboardReadResult<ClipboardImageInfo>> readImageInfo() async => ClipboardReadResult(supported: true, value: image.info);

  @override
  Future<bool> writeImage(
    Uint8List pngBytes, {
    String? token,
  }) async {
    writeCount += 1;
    writtenPngBytes = pngBytes;
    writtenToken = token;
    return true;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  final ImClipboardPlatform initialPlatform = ImClipboardPlatform.instance;

  tearDown(() => ImClipboardPlatform.instance = initialPlatform);

  test('uses the method channel implementation by default', () {
    expect(initialPlatform, isA<MethodChannelImClipboard>());
  });

  test('owns a defensive copy of clipboard image bytes', () {
    final Uint8List sourceBytes = Uint8List.fromList(<int>[1, 2, 3]);
    final ClipboardImage image = ClipboardImage(
      info: const ClipboardImageInfo(width: 1, height: 1),
      pngBytes: sourceBytes,
    );

    sourceBytes[0] = 9;

    expect(image.pngBytes, <int>[1, 2, 3]);
  });

  test('delegates every operation to the active platform', () async {
    final _FakePlatform fake = _FakePlatform();
    ImClipboardPlatform.instance = fake;
    const ImClipboard clipboard = ImClipboard();

    expect(await clipboard.isSupported(), isTrue);
    expect((await clipboard.readImageInfo()).value, fake.image.info);
    expect((await clipboard.readImage()).value, same(fake.image));
    expect((await clipboard.readFiles()).value, <String>['/tmp/image.png']);
    expect(await clipboard.writeImage(Uint8List.fromList(<int>[1]), token: 'copy'), isTrue);
    expect(fake.writeCount, 1);
  });

  test('passes PNG input through without transcoding', () async {
    final _FakePlatform fake = _FakePlatform();
    ImClipboardPlatform.instance = fake;
    const ImClipboard clipboard = ImClipboard();
    final imcodec.Image source = imcodec.Image.fromRgba(
      width: 1,
      height: 1,
      bytes: Uint8List.fromList(<int>[12, 34, 56, 78]),
    );
    final Uint8List pngBytes = imcodec.encodePng(source);

    final bool written = await clipboard.writeEncodedImage(
      pngBytes,
      format: ClipboardImageFormat.png,
      token: 'png',
    );

    expect(written, isTrue);
    expect(fake.writtenPngBytes, same(pngBytes));
    expect(fake.writtenToken, 'png');
  });

  test('transcodes every non-PNG imcodec format to PNG', () async {
    final _FakePlatform fake = _FakePlatform();
    ImClipboardPlatform.instance = fake;
    const ImClipboard clipboard = ImClipboard();
    final imcodec.Image source = imcodec.Image.fromRgba(
      width: 2,
      height: 1,
      bytes: Uint8List.fromList(<int>[255, 0, 0, 255, 0, 0, 255, 128]),
    );
    final List<(ClipboardImageFormat, Uint8List)> encodedImages = [
      (ClipboardImageFormat.bmp, imcodec.encodeBmp(source)),
      (ClipboardImageFormat.jpeg, imcodec.encodeJpg(source)),
      (ClipboardImageFormat.jpegXl, imcodec.encodeJpegXl(source)),
      (ClipboardImageFormat.qoi, imcodec.encodeQoi(source)),
      (ClipboardImageFormat.tga, imcodec.encodeTga(source)),
      (ClipboardImageFormat.tiff, imcodec.encodeTiff(source)),
      (ClipboardImageFormat.webp, imcodec.encodeWebP(source)),
    ];

    for (final (ClipboardImageFormat format, Uint8List encodedBytes) in encodedImages) {
      final bool written = await clipboard.writeEncodedImage(
        encodedBytes,
        format: format,
        token: format.name,
      );
      final Uint8List pngBytes = fake.writtenPngBytes ?? (throw StateError('The fake platform received no PNG bytes.'));
      final imcodec.Image decodedImage = imcodec.decodePng(pngBytes);

      expect(written, isTrue, reason: format.name);
      expect(imcodec.ImageFormat.sniff(pngBytes), imcodec.ImageFormat.png, reason: format.name);
      expect(decodedImage.width, source.width, reason: format.name);
      expect(decodedImage.height, source.height, reason: format.name);
      expect(fake.writtenToken, format.name);
    }
    expect(fake.writeCount, encodedImages.length);
  });

  test('rejects unknown or mismatched encoded formats', () async {
    final _FakePlatform fake = _FakePlatform();
    ImClipboardPlatform.instance = fake;
    const ImClipboard clipboard = ImClipboard();
    final imcodec.Image source = imcodec.Image.fromRgba(
      width: 1,
      height: 1,
      bytes: Uint8List.fromList(<int>[0, 0, 0, 255]),
    );

    await expectLater(
      clipboard.writeEncodedImage(Uint8List.fromList(<int>[1, 2, 3])),
      throwsA(isA<ImClipboardException>().having((exception) => exception.operation, 'operation', 'writeEncodedImage').having((exception) => exception.cause, 'cause', isA<FormatException>())),
    );
    await expectLater(
      clipboard.writeEncodedImage(
        imcodec.encodeBmp(source),
        format: ClipboardImageFormat.jpeg,
      ),
      throwsA(isA<ImClipboardException>().having((exception) => exception.cause, 'cause', isA<FormatException>())),
    );
    expect(fake.writeCount, 0);
  });
}

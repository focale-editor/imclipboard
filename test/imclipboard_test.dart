import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard/imclipboard.dart';
import 'package:imclipboard/src/method_channel.dart';
import 'package:imclipboard/src/platform_interface.dart';

/// In-memory platform used to verify the public facade.
final class _FakePlatform extends ImClipboardPlatform {
  /// Image returned by clipboard reads.
  final ClipboardImage image = ClipboardImage(
    info: const ClipboardImageInfo(width: 2, height: 3, token: 'fake'),
    pngBytes: Uint8List.fromList(<int>[1, 2, 3]),
  );

  /// Number of write calls received by the fake.
  int writeCount = 0;

  @override
  Future<bool> isSupported() async => true;

  @override
  Future<ClipboardReadResult<ClipboardImage>> readImage() async => ClipboardReadResult(supported: true, value: image);

  @override
  Future<ClipboardReadResult<List<String>>> readImageFiles() async => const ClipboardReadResult(supported: true, value: <String>['/tmp/image.png']);

  @override
  Future<ClipboardReadResult<ClipboardImageInfo>> readImageInfo() async => ClipboardReadResult(supported: true, value: image.info);

  @override
  Future<bool> writeImage(
    Uint8List pngBytes, {
    String? token,
  }) async {
    writeCount += 1;
    return true;
  }
}

void main() {
  final ImClipboardPlatform initialPlatform = ImClipboardPlatform.instance;

  tearDown(() => ImClipboardPlatform.instance = initialPlatform);

  test('uses the method channel implementation by default', () {
    expect(initialPlatform, isA<MethodChannelImClipboard>());
  });

  test('delegates every operation to the active platform', () async {
    final _FakePlatform fake = _FakePlatform();
    ImClipboardPlatform.instance = fake;
    const ImClipboard clipboard = ImClipboard();

    expect(await clipboard.isSupported(), isTrue);
    expect((await clipboard.readImageInfo()).value, fake.image.info);
    expect((await clipboard.readImage()).value, same(fake.image));
    expect((await clipboard.readImageFiles()).value, <String>['/tmp/image.png']);
    expect(await clipboard.writeImage(Uint8List.fromList(<int>[1]), token: 'copy'), isTrue);
    expect(fake.writeCount, 1);
  });
}

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard/src/method_channel.dart';
import 'package:imclipboard/src/platform_interface.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const MethodChannel channel = MethodChannel(MethodChannelImClipboard.channelName);
  final TestDefaultBinaryMessenger messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test('reports an absent native implementation as unsupported', () async {
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    expect(await clipboard.isSupported(), isFalse);
    expect((await clipboard.readImage()).supported, isFalse);
    expect((await clipboard.readFiles()).supported, isFalse);
    expect(await clipboard.writeImage(Uint8List.fromList(<int>[1])), isFalse);
  });

  test('reads metadata without requesting the PNG payload', () async {
    final List<String> calls = <String>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call.method);
      return switch (call.method) {
        'isSupported' => true,
        'readImageInfo' => <String, Object>{'width': 640, 'height': 480, 'token': 'ours'},
        _ => null,
      };
    });
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    final ClipboardReadResult<ClipboardImageInfo> read = await clipboard.readImageInfo();

    expect(read.supported, isTrue);
    expect(read.value, const ClipboardImageInfo(width: 640, height: 480, token: 'ours'));
    expect(calls, <String>['isSupported', 'readImageInfo']);
  });

  test('distinguishes a supported empty clipboard', () async {
    messenger.setMockMethodCallHandler(channel, (call) async => call.method == 'isSupported' ? true : null);
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    final ClipboardReadResult<ClipboardImage> read = await clipboard.readImage();

    expect(read.supported, isTrue);
    expect(read.value, isNull);
  });

  test('reads PNG bytes and writes them with an optional token', () async {
    MethodCall? writeCall;
    final Uint8List png = Uint8List.fromList(<int>[137, 80, 78, 71]);
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'isSupported') {
        return true;
      }
      if (call.method == 'readImage') {
        return <String, Object>{'width': 12, 'height': 8, 'bytes': png};
      }
      writeCall = call;
      return null;
    });
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    final ClipboardReadResult<ClipboardImage> read = await clipboard.readImage();
    expect(read.value?.pngBytes, png);
    expect(await clipboard.writeImage(png, token: 'copy-id'), isTrue);

    expect(writeCall?.method, 'writeImage');
    expect((writeCall?.arguments as Map<Object?, Object?>)['bytes'], png);
    expect((writeCall?.arguments as Map<Object?, Object?>)['token'], 'copy-id');
  });

  test('omits the token argument when none is supplied', () async {
    MethodCall? writeCall;
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'isSupported') {
        return true;
      }
      writeCall = call;
      return null;
    });
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    await clipboard.writeImage(Uint8List.fromList(<int>[1]));

    expect((writeCall?.arguments as Map<Object?, Object?>).containsKey('token'), isFalse);
  });

  test('reads absolute local file paths and removes duplicates', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => switch (call.method) {
        'isSupported' => true,
        'readFiles' => <String>['/tmp/first.png', '/tmp/first.png', '/tmp/second.jpg'],
        _ => null,
      },
    );
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    final ClipboardReadResult<List<String>> read = await clipboard.readFiles();

    expect(read.value, <String>['/tmp/first.png', '/tmp/second.jpg']);
  });

  test('accepts absolute Windows drive paths', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'isSupported' ? true : <String>[r'C:\Images\photo.png'],
    );
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    expect((await clipboard.readFiles()).value, <String>[r'C:\Images\photo.png']);
  });

  test('rejects malformed native file paths', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'isSupported' ? true : <String>['relative.png'],
    );
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    await expectLater(clipboard.readFiles(), throwsA(isA<ImClipboardException>()));
  });

  test('rejects malformed native metadata', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'isSupported' ? true : <String, Object>{'width': 0, 'height': 'wrong'},
    );
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    await expectLater(clipboard.readImageInfo(), throwsA(isA<ImClipboardException>()));
  });

  test('rejects invalid write arguments before invoking the channel', () async {
    final MethodChannelImClipboard clipboard = MethodChannelImClipboard(methodChannel: channel);

    await expectLater(clipboard.writeImage(Uint8List(0)), throwsA(isA<ImClipboardException>()));
    await expectLater(clipboard.writeImage(Uint8List.fromList(<int>[1]), token: ''), throwsA(isA<ImClipboardException>()));
  });
}

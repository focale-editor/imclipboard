@JS()
library;

import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:typed_data';

import 'package:flutter_web_plugins/flutter_web_plugins.dart';
import 'package:imclipboard/src/platform_interface.dart';
import 'package:web/web.dart' as web;

/// Browser clipboard object, or `null` when the Async Clipboard API is absent.
@JS('navigator.clipboard')
external JSObject? get _browserClipboard;

/// Browser clipboard-item constructor, or `null` on unsupported browsers.
@JS('ClipboardItem')
external JSFunction? get _clipboardItemConstructor;

/// Web implementation backed by the browser Async Clipboard API.
final class ImClipboardWeb extends ImClipboardPlatform {
  /// MIME type required by the web clipboard specification for bitmap images.
  static const String _pngMimeType = 'image/png';

  /// Fingerprint of the most recent image written by this page.
  _ClipboardFingerprint? _lastWrittenFingerprint;

  /// Token associated with the most recent image written by this page.
  String? _lastWrittenToken;

  /// Creates a browser clipboard implementation.
  ImClipboardWeb();

  /// Registers this implementation with Flutter web.
  static void registerWith(Registrar registrar) {
    ImClipboardPlatform.instance = ImClipboardWeb();
  }

  @override
  Future<bool> isSupported() async {
    try {
      final JSObject? clipboard = _browserClipboard;
      return web.window.isSecureContext && clipboard != null && clipboard.has('read') && clipboard.has('write') && _clipboardItemConstructor != null && web.ClipboardItem.supports(_pngMimeType);
    } on Object {
      return false;
    }
  }

  @override
  Future<ClipboardReadResult<ClipboardImageInfo>> readImageInfo() async {
    final ClipboardReadResult<ClipboardImage> read = await readImage();
    return ClipboardReadResult(supported: read.supported, value: read.value?.info);
  }

  @override
  Future<ClipboardReadResult<ClipboardImage>> readImage() async {
    if (!await isSupported()) {
      return const ClipboardReadResult.unsupported();
    }
    try {
      final web.ClipboardItems items = await web.window.navigator.clipboard.read().toDart;
      for (final web.ClipboardItem item in items.toDart) {
        final List<JSString> types = item.types.toDart;
        if (!types.any((type) => type.toDart == _pngMimeType)) {
          continue;
        }
        final web.Blob blob = await item.getType(_pngMimeType).toDart;
        if (blob.size < 1 || blob.size > ImClipboardPlatform.maxEncodedBytes) {
          throw StateError('The browser clipboard PNG is empty or exceeds ${ImClipboardPlatform.maxEncodedBytes} bytes.');
        }
        final JSArrayBuffer buffer = await blob.arrayBuffer().toDart;
        final Uint8List bytes = JSUint8Array(buffer).toDart;
        final ClipboardImageInfo parsedInfo = _parsePngInfo(bytes, operation: 'readImage');
        final _ClipboardFingerprint fingerprint = _ClipboardFingerprint.fromBytes(bytes);
        final String? token = fingerprint == _lastWrittenFingerprint ? _lastWrittenToken : null;
        final ClipboardImageInfo info = ClipboardImageInfo(width: parsedInfo.width, height: parsedInfo.height, token: token);
        return ClipboardReadResult(
          supported: true,
          value: ClipboardImage(info: info, pngBytes: bytes),
        );
      }
      return const ClipboardReadResult(supported: true);
    } on ImClipboardException {
      rethrow;
    } on Object catch (error, stackTrace) {
      throw ImClipboardException(operation: 'readImage', cause: error, stackTrace: stackTrace);
    }
  }

  @override
  Future<ClipboardReadResult<List<String>>> readImageFiles() async => await isSupported() ? const ClipboardReadResult(supported: true, value: []) : const ClipboardReadResult.unsupported();

  @override
  Future<bool> writeImage(
    Uint8List pngBytes, {
    String? token,
  }) async {
    validateWriteArguments(pngBytes, token);
    _parsePngInfo(pngBytes, operation: 'writeImage');
    if (!await isSupported()) {
      return false;
    }
    try {
      final JSArray<JSAny> parts = <JSAny>[pngBytes.toJS].toJS;
      final web.Blob blob = web.Blob(parts, web.BlobPropertyBag(type: _pngMimeType));
      final JSObject representations = JSObject()..setProperty(_pngMimeType.toJS, blob);
      final web.ClipboardItem item = web.ClipboardItem(representations);
      final web.ClipboardItems items = <web.ClipboardItem>[item].toJS;
      await web.window.navigator.clipboard.write(items).toDart;
      _lastWrittenFingerprint = _ClipboardFingerprint.fromBytes(pngBytes);
      _lastWrittenToken = token;
      return true;
    } on Object catch (error, stackTrace) {
      throw ImClipboardException(operation: 'writeImage', cause: error, stackTrace: stackTrace);
    }
  }

  /// Parses dimensions from the mandatory PNG signature and IHDR chunk.
  static ClipboardImageInfo _parsePngInfo(
    Uint8List bytes, {
    required String operation,
  }) {
    final bool hasHeader =
        bytes.length >= 24 &&
        bytes[0] == 0x89 &&
        bytes[1] == 0x50 &&
        bytes[2] == 0x4E &&
        bytes[3] == 0x47 &&
        bytes[4] == 0x0D &&
        bytes[5] == 0x0A &&
        bytes[6] == 0x1A &&
        bytes[7] == 0x0A &&
        bytes[8] == 0x00 &&
        bytes[9] == 0x00 &&
        bytes[10] == 0x00 &&
        bytes[11] == 0x0D &&
        bytes[12] == 0x49 &&
        bytes[13] == 0x48 &&
        bytes[14] == 0x44 &&
        bytes[15] == 0x52;
    if (!hasHeader) {
      throw ImClipboardException(operation: operation, cause: const FormatException('Expected a PNG image with an IHDR chunk.'));
    }
    final ByteData header = ByteData.sublistView(bytes, 16, 24);
    final int width = header.getUint32(0);
    final int height = header.getUint32(4);
    if (width < 1 || height < 1) {
      throw ImClipboardException(operation: operation, cause: const FormatException('The PNG dimensions must be positive.'));
    }
    return ClipboardImageInfo(width: width, height: height);
  }
}

/// Compact identity for matching a web clipboard read to a recent write.
final class _ClipboardFingerprint {
  /// Encoded byte length.
  final int length;

  /// First independent 32-bit rolling hash.
  final int firstHash;

  /// Second independent 32-bit rolling hash.
  final int secondHash;

  /// Creates a clipboard fingerprint.
  const _ClipboardFingerprint({
    required this.length,
    required this.firstHash,
    required this.secondHash,
  });

  /// Calculates a deterministic fingerprint for [bytes].
  factory _ClipboardFingerprint.fromBytes(Uint8List bytes) {
    int firstHash = 0x811C9DC5;
    int secondHash = 0x9E3779B9;
    final int stride = bytes.length < 65536 ? 1 : bytes.length ~/ 65536;
    for (int index = 0; index < bytes.length; index += stride) {
      final int byte = bytes[index];
      firstHash = ((firstHash ^ byte) * 0x01000193) & 0xFFFFFFFF;
      secondHash = ((secondHash + byte) * 33 ^ secondHash >>> 2) & 0xFFFFFFFF;
    }
    return _ClipboardFingerprint(length: bytes.length, firstHash: firstHash, secondHash: secondHash);
  }

  @override
  bool operator ==(Object other) => other is _ClipboardFingerprint && other.length == length && other.firstHash == firstHash && other.secondHash == secondHash;

  @override
  int get hashCode => Object.hash(length, firstHash, secondHash);
}

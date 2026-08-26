import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:imclipboard/src/platform_interface.dart';

/// Native implementation that communicates through a Flutter method channel.
final class MethodChannelImClipboard extends ImClipboardPlatform {
  /// Channel implemented by Android, iOS, Linux, macOS, and Windows.
  static const String channelName = 'app.focaleeditor.imclipboard/image_clipboard';

  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final MethodChannel methodChannel;

  /// Cached channel availability for the lifetime of this implementation.
  bool? _supported;

  /// Creates a native clipboard implementation.
  MethodChannelImClipboard({
    this.methodChannel = const MethodChannel(channelName),
  });

  @override
  Future<bool> isSupported() async {
    final bool? cached = _supported;
    if (cached != null) {
      return cached;
    }
    try {
      final bool supported = await methodChannel.invokeMethod<bool>('isSupported') ?? false;
      _supported = supported;
      return supported;
    } on MissingPluginException {
      _supported = false;
      return false;
    } on Object catch (error, stackTrace) {
      throw ImClipboardException(operation: 'isSupported', cause: error, stackTrace: stackTrace);
    }
  }

  @override
  Future<ClipboardReadResult<ClipboardImageInfo>> readImageInfo() async {
    if (!await isSupported()) {
      return const ClipboardReadResult.unsupported();
    }
    final Object? rawResult = await _invoke('readImageInfo');
    final Map<Object?, Object?>? result = _mapResult(rawResult, operation: 'readImageInfo');
    if (result == null) {
      return const ClipboardReadResult(supported: true);
    }
    return ClipboardReadResult(supported: true, value: _parseInfo(result, operation: 'readImageInfo'));
  }

  @override
  Future<ClipboardReadResult<ClipboardImage>> readImage() async {
    if (!await isSupported()) {
      return const ClipboardReadResult.unsupported();
    }
    final Object? rawResult = await _invoke('readImage');
    final Map<Object?, Object?>? result = _mapResult(rawResult, operation: 'readImage');
    if (result == null) {
      return const ClipboardReadResult(supported: true);
    }
    final ClipboardImageInfo info = _parseInfo(result, operation: 'readImage');
    final Object? bytes = result['bytes'];
    if (bytes is! Uint8List || bytes.isEmpty || bytes.length > ImClipboardPlatform.maxEncodedBytes) {
      throw ImClipboardException(operation: 'readImage', cause: StateError('The native clipboard returned invalid PNG bytes.'));
    }
    return ClipboardReadResult(
      supported: true,
      value: ClipboardImage(info: info, pngBytes: bytes),
    );
  }

  @override
  Future<ClipboardReadResult<List<String>>> readFiles() async {
    if (!await isSupported()) {
      return const ClipboardReadResult.unsupported();
    }
    final Object? result = await _invoke('readFiles');
    if (result == null) {
      return const ClipboardReadResult(supported: true, value: []);
    }
    if (result is! List<Object?> || result.length > ImClipboardPlatform.maxFileCount) {
      throw ImClipboardException(operation: 'readFiles', cause: StateError('The native clipboard returned an invalid file list.'));
    }

    final List<String> paths = <String>[];
    final Set<String> seen = <String>{};
    for (final Object? value in result) {
      if (value is! String || !_isValidAbsolutePath(value)) {
        throw ImClipboardException(operation: 'readFiles', cause: StateError('The native clipboard returned an invalid local file path.'));
      }
      if (seen.add(value)) {
        paths.add(value);
      }
    }
    return ClipboardReadResult(supported: true, value: List<String>.unmodifiable(paths));
  }

  @override
  Future<bool> writeImage(
    Uint8List pngBytes, {
    String? token,
  }) async {
    validateWriteArguments(pngBytes, token);
    if (!await isSupported()) {
      return false;
    }
    final Map<String, Object> arguments = <String, Object>{'bytes': pngBytes};
    if (token != null) {
      arguments['token'] = token;
    }
    await _invoke('writeImage', arguments);
    return true;
  }

  /// Invokes [method] and translates platform failures into plugin errors.
  Future<Object?> _invoke(String method, [Map<String, Object?>? arguments]) async {
    try {
      return await methodChannel.invokeMethod<Object?>(method, arguments);
    } on MissingPluginException catch (error, stackTrace) {
      _supported = false;
      throw ImClipboardException(operation: method, cause: error, stackTrace: stackTrace);
    } on ImClipboardException {
      rethrow;
    } on Object catch (error, stackTrace) {
      throw ImClipboardException(operation: method, cause: error, stackTrace: stackTrace);
    }
  }

  /// Converts a native response to a metadata map.
  static Map<Object?, Object?>? _mapResult(
    Object? result, {
    required String operation,
  }) {
    if (result == null) {
      return null;
    }
    if (result is! Map<Object?, Object?>) {
      throw ImClipboardException(operation: operation, cause: StateError('The native clipboard returned ${result.runtimeType}, expected a map.'));
    }
    return result;
  }

  /// Validates image metadata returned by a native implementation.
  static ClipboardImageInfo _parseInfo(
    Map<Object?, Object?> result, {
    required String operation,
  }) {
    final Object? width = result['width'];
    final Object? height = result['height'];
    final Object? token = result['token'];
    if (width is! int || width < 1 || height is! int || height < 1 || token != null && token is! String) {
      throw ImClipboardException(operation: operation, cause: StateError('The native clipboard returned invalid image metadata.'));
    }
    return ClipboardImageInfo(width: width, height: height, token: token as String?);
  }

  /// Whether [path] is a bounded absolute POSIX or drive-letter path.
  static bool _isValidAbsolutePath(String path) {
    if (path.isEmpty || path.contains('\u0000') || path.length > ImClipboardPlatform.maxFilePathBytes || utf8.encode(path).length > ImClipboardPlatform.maxFilePathBytes) {
      return false;
    }
    final bool posixPath = path.startsWith('/');
    final bool drivePath = RegExp(r'^[A-Za-z]:[\\/]').hasMatch(path);
    final bool extendedDrivePath = RegExp(r'^\\\\\?\\[A-Za-z]:\\').hasMatch(path);
    return posixPath || drivePath || extendedDrivePath;
  }
}

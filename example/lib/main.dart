import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:imclipboard/imclipboard.dart';

/// Starts the Imclipboard example application.
void main() => runApp(const ClipboardExampleApp());

/// Demonstrates image clipboard reads and writes.
class ClipboardExampleApp extends StatelessWidget {
  /// Creates the example application.
  const ClipboardExampleApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
    home: const ClipboardExamplePage(),
  );
}

/// Interactive page whose buttons satisfy web clipboard user-gesture rules.
class ClipboardExamplePage extends StatefulWidget {
  /// Creates the clipboard example page.
  const ClipboardExamplePage({super.key});

  @override
  State<ClipboardExamplePage> createState() => _ClipboardExamplePageState();
}

/// Mutable state for clipboard operations and their latest result.
class _ClipboardExamplePageState extends State<ClipboardExamplePage> {
  /// Plugin client shared by all actions on this page.
  static const ImClipboard _clipboard = ImClipboard();

  /// Small PNG used by the write demonstration.
  static final Uint8List _samplePng = base64Decode(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=',
  );

  /// Whether the current target exposes the plugin.
  bool? _supported;

  /// Most recently read image.
  ClipboardImage? _image;

  /// Human-readable operation status.
  String _status = 'Checking clipboard support…';

  @override
  void initState() {
    super.initState();
    _checkSupport();
  }

  /// Detects whether the current platform implementation is available.
  Future<void> _checkSupport() async {
    try {
      final bool supported = await _clipboard.isSupported();
      if (!mounted) {
        return;
      }
      setState(() {
        _supported = supported;
        _status = supported
            ? 'Image clipboard ready.'
            : 'Image clipboard unsupported.';
      });
    } on Object catch (error) {
      _showError(error);
    }
  }

  /// Writes the bundled PNG in direct response to a button press.
  Future<void> _writeSample() async {
    try {
      await _clipboard.writeImage(_samplePng, token: 'imclipboard-example');
      if (!mounted) {
        return;
      }
      setState(() => _status = 'A 1 × 1 sample PNG was copied.');
    } on Object catch (error) {
      _showError(error);
    }
  }

  /// Reads and displays the first compatible clipboard image.
  Future<void> _readImage() async {
    try {
      final ClipboardReadResult<ClipboardImage> read = await _clipboard
          .readImage();
      if (!mounted) {
        return;
      }
      setState(() {
        _image = read.value;
        _status = switch (read.value) {
          final ClipboardImage image =>
            'Read ${image.info.width} × ${image.info.height} PNG (${image.pngBytes.length} bytes).',
          null when read.supported && kIsWeb => 'The browser exposed no PNG image. Copy the image pixels rather than its file reference.',
          null when read.supported =>
            'The clipboard contains no compatible image.',
          null => 'Image clipboard unsupported.',
        };
      });
    } on Object catch (error) {
      _showError(error);
    }
  }

  /// Presents [error] without updating a disposed page.
  void _showError(Object error) {
    if (!mounted) {
      return;
    }
    setState(() => _status = 'Clipboard error: $error');
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Imclipboard example')),
    body: Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 560),
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              if (_image case final ClipboardImage image)
                Expanded(
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      border: Border.all(
                        color: Theme.of(context).colorScheme.outlineVariant,
                      ),
                    ),
                    child: Image.memory(
                      image.pngBytes,
                      fit: BoxFit.contain,
                      errorBuilder: (_, _, _) =>
                          const Center(child: Text('Invalid image data')),
                    ),
                  ),
                )
              else
                const Expanded(
                  child: Center(child: Icon(Icons.content_paste, size: 96)),
                ),
              const SizedBox(height: 24),
              Text(_status, textAlign: TextAlign.center),
              const SizedBox(height: 16),
              Wrap(
                alignment: WrapAlignment.center,
                spacing: 12,
                runSpacing: 12,
                children: <Widget>[
                  FilledButton.icon(
                    onPressed: _supported == true ? _writeSample : null,
                    icon: const Icon(Icons.copy),
                    label: const Text('Copy sample PNG'),
                  ),
                  OutlinedButton.icon(
                    onPressed: _supported == true ? _readImage : null,
                    icon: const Icon(Icons.paste),
                    label: const Text('Paste image'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    ),
  );
}

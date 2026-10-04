import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard/imclipboard.dart';
import 'package:imclipboard_example/main.dart' as example;
import 'package:integration_test/integration_test.dart';

/// Exercises the example buttons and the real operating-system clipboard.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('example copies and pastes a PNG through the native clipboard', (
    tester,
  ) async {
    example.main();
    await tester.pumpAndSettle();
    expect(find.text('Image clipboard ready.'), findsOneWidget);
    await tester.tap(find.text('Copy sample PNG'));
    await tester.pumpAndSettle();
    expect(find.text('A 1 × 1 sample PNG was copied.'), findsOneWidget);
    await tester.tap(find.text('Paste image'));
    await tester.pumpAndSettle();
    expect(find.textContaining('Read 1 × 1 PNG'), findsOneWidget);
    expect(find.text('Invalid image data'), findsNothing);
    expect(tester.takeException(), isNull);
    const ImClipboard clipboard = ImClipboard();
    final info = await clipboard.readImageInfo();
    expect(info.supported, isTrue);
    expect(info.value?.width, 1);
    expect(info.value?.height, 1);
    final image = await clipboard.readImage();
    expect(image.value?.info.token, 'imclipboard-example');
    final png = base64Decode(
      'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=',
    );
    expect(
      await clipboard.writeGeneratedPng(png, token: 'audit-generated'),
      isTrue,
    );
    expect((await clipboard.readImage()).value?.info.token, 'audit-generated');
    expect((await clipboard.readFiles()).supported, isTrue);
  });
}

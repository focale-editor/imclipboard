import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard_example/main.dart';

void main() {
  testWidgets('shows both clipboard actions', (WidgetTester tester) async {
    await tester.pumpWidget(const ClipboardExampleApp());

    expect(find.text('Copy sample PNG'), findsOneWidget);
    expect(find.text('Paste image'), findsOneWidget);
  });
}

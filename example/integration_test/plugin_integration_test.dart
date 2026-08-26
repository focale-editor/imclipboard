import 'package:flutter_test/flutter_test.dart';
import 'package:imclipboard/imclipboard.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('loads the host clipboard implementation', (
    WidgetTester tester,
  ) async {
    const ImClipboard clipboard = ImClipboard();

    expect(await clipboard.isSupported(), isTrue);
  });
}

Pod::Spec.new do |s|
  s.name             = 'imclipboard'
  s.version          = '0.1.0'
  s.summary          = 'Cross-platform image clipboard support for Flutter.'
  s.description      = 'Reads and writes PNG images through the iOS and macOS system pasteboards.'
  s.homepage         = 'https://github.com/focale-editor/imclipboard'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Focale Editor' => 'https://github.com/focale-editor' }
  s.source           = { :path => '.' }
  s.source_files     = 'imclipboard/Sources/imclipboard/**/*'

  s.ios.deployment_target = '15.0'
  s.osx.deployment_target = '12.0'
  s.ios.dependency 'Flutter'
  s.osx.dependency 'FlutterMacOS'
  s.ios.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386' }
  s.osx.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end

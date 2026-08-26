#if os(iOS)
import Flutter
import UIKit
import UniformTypeIdentifiers
#elseif os(macOS)
import Cocoa
import FlutterMacOS
#endif

private let channelName = "app.focaleeditor.imclipboard/image_clipboard"
private let tokenTypeName = "app.focaleeditor.imclipboard.token"
private let maximumEncodedBytes = 512 * 1024 * 1024
private let maximumFileCount = 32
private let maximumFilePathBytes = 32 * 1024
private let maximumTokenBytes = 1024
private let pngSignature: [UInt8] = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]

/// Reads and writes images through the Apple platform's general pasteboard.
public class ImclipboardPlugin: NSObject, FlutterPlugin {
  public static func register(with registrar: FlutterPluginRegistrar) {
    #if os(iOS)
      let messenger = registrar.messenger()
    #else
      let messenger = registrar.messenger
    #endif
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    let instance = ImclipboardPlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    do {
      switch call.method {
      case "isSupported":
        result(true)
      case "readFiles":
        result(clipboardFilePaths())
      case "readImageInfo":
        result(try clipboardImage(includePng: false)?.map)
      case "readImage":
        result(try clipboardImage(includePng: true)?.map)
      case "writeImage":
        try writeClipboardImage(arguments: call.arguments)
        result(nil)
      default:
        result(FlutterMethodNotImplemented)
      }
    } catch {
      result(FlutterError(code: "clipboard_failed", message: error.localizedDescription, details: nil))
    }
  }

  private func validatedArguments(_ rawArguments: Any?) throws -> (png: Data, token: String?) {
    guard let arguments = rawArguments as? [String: Any],
          let typedData = arguments["bytes"] as? FlutterStandardTypedData,
          !typedData.data.isEmpty,
          typedData.data.count <= maximumEncodedBytes,
          typedData.data.starts(with: pngSignature) else {
      throw ClipboardError.invalidArguments
    }

    guard let rawToken = arguments["token"] else {
      return (typedData.data, nil)
    }
    guard let token = rawToken as? String, validToken(token) != nil else {
      throw ClipboardError.invalidToken
    }
    return (typedData.data, token)
  }

  private func validToken(_ candidate: String?) -> String? {
    guard let token = candidate,
          !token.isEmpty,
          !token.contains("\0"),
          token.lengthOfBytes(using: .utf8) <= maximumTokenBytes else {
      return nil
    }
    return token
  }

  private func filteredFilePaths(_ urls: [URL]) -> [String] {
    var paths: [String] = []
    var seen: Set<String> = []
    for url in urls where paths.count < maximumFileCount {
      let host = url.host?.lowercased()
      let standardized = url.standardizedFileURL
      let path = standardized.path
      var isDirectory: ObjCBool = false
      guard url.isFileURL,
            host == nil || host == "" || host == "localhost",
            path.hasPrefix("/"),
            path.lengthOfBytes(using: .utf8) <= maximumFilePathBytes,
            FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory),
            !isDirectory.boolValue,
            seen.insert(path).inserted else {
        continue
      }
      paths.append(path)
    }
    return paths
  }

  #if os(iOS)
    private func clipboardImage(includePng: Bool) throws -> ClipboardImage? {
      let pasteboard = UIPasteboard.general
      let token = validToken(pasteboard.value(forPasteboardType: tokenTypeName) as? String)

      if let png = pasteboard.data(forPasteboardType: UTType.png.identifier) {
        guard !png.isEmpty,
              png.count <= maximumEncodedBytes,
              let image = UIImage(data: png),
              let dimensions = pixelDimensions(image) else {
          throw ClipboardError.invalidImage
        }
        return ClipboardImage(width: dimensions.width, height: dimensions.height, token: token, png: includePng ? png : nil)
      }

      guard pasteboard.hasImages,
            let image = pasteboard.image,
            let dimensions = pixelDimensions(image) else {
        return nil
      }
      if !includePng {
        return ClipboardImage(width: dimensions.width, height: dimensions.height, token: token, png: nil)
      }
      guard let png = image.pngData(), !png.isEmpty, png.count <= maximumEncodedBytes else {
        throw ClipboardError.encodingFailed
      }
      return ClipboardImage(width: dimensions.width, height: dimensions.height, token: token, png: png)
    }

    private func writeClipboardImage(arguments rawArguments: Any?) throws {
      let arguments = try validatedArguments(rawArguments)
      guard let image = UIImage(data: arguments.png), pixelDimensions(image) != nil else {
        throw ClipboardError.invalidImage
      }
      var item: [String: Any] = [UTType.png.identifier: arguments.png]
      if let token = arguments.token {
        item[tokenTypeName] = token
      }
      UIPasteboard.general.setItems([item], options: [:])
    }

    private func clipboardFilePaths() -> [String] {
      return filteredFilePaths(UIPasteboard.general.urls ?? [])
    }

    private func pixelDimensions(_ image: UIImage) -> (width: Int, height: Int)? {
      if let cgImage = image.cgImage, cgImage.width > 0, cgImage.height > 0 {
        return (cgImage.width, cgImage.height)
      }
      let width = Int((image.size.width * image.scale).rounded())
      let height = Int((image.size.height * image.scale).rounded())
      return width > 0 && height > 0 ? (width, height) : nil
    }
  #elseif os(macOS)
    private func clipboardImage(includePng: Bool) throws -> ClipboardImage? {
      let pasteboard = NSPasteboard.general
      let tokenType = NSPasteboard.PasteboardType(tokenTypeName)
      let token = validToken(pasteboard.string(forType: tokenType))

      if let png = pasteboard.data(forType: .png) {
        guard !png.isEmpty,
              png.count <= maximumEncodedBytes,
              let representation = NSBitmapImageRep(data: png),
              representation.pixelsWide > 0,
              representation.pixelsHigh > 0 else {
          throw ClipboardError.invalidImage
        }
        return ClipboardImage(width: representation.pixelsWide, height: representation.pixelsHigh, token: token, png: includePng ? png : nil)
      }

      guard let image = NSImage(pasteboard: pasteboard),
            let tiff = image.tiffRepresentation,
            let representation = NSBitmapImageRep(data: tiff),
            representation.pixelsWide > 0,
            representation.pixelsHigh > 0 else {
        return nil
      }
      if !includePng {
        return ClipboardImage(width: representation.pixelsWide, height: representation.pixelsHigh, token: token, png: nil)
      }
      guard let png = representation.representation(using: .png, properties: [:]),
            !png.isEmpty,
            png.count <= maximumEncodedBytes else {
        throw ClipboardError.encodingFailed
      }
      return ClipboardImage(width: representation.pixelsWide, height: representation.pixelsHigh, token: token, png: png)
    }

    private func writeClipboardImage(arguments rawArguments: Any?) throws {
      let arguments = try validatedArguments(rawArguments)
      guard let representation = NSBitmapImageRep(data: arguments.png),
            representation.pixelsWide > 0,
            representation.pixelsHigh > 0 else {
        throw ClipboardError.invalidImage
      }

      let pasteboard = NSPasteboard.general
      pasteboard.clearContents()
      var success = pasteboard.setData(arguments.png, forType: .png)
      if let token = arguments.token {
        success = success && pasteboard.setString(token, forType: NSPasteboard.PasteboardType(tokenTypeName))
      }
      if !success {
        throw ClipboardError.writeFailed
      }
    }

    private func clipboardFilePaths() -> [String] {
      let options: [NSPasteboard.ReadingOptionKey: Any] = [.urlReadingFileURLsOnly: true]
      guard let objects = NSPasteboard.general.readObjects(forClasses: [NSURL.self], options: options) else {
        return []
      }
      let urls = objects.compactMap { object in (object as? NSURL).map { $0 as URL } }
      return filteredFilePaths(urls)
    }
  #endif
}

private struct ClipboardImage {
  let width: Int
  let height: Int
  let token: String?
  let png: Data?

  var map: [String: Any] {
    var value: [String: Any] = ["width": width, "height": height]
    if let token = token {
      value["token"] = token
    }
    if let png = png {
      value["bytes"] = FlutterStandardTypedData(bytes: png)
    }
    return value
  }
}

private enum ClipboardError: LocalizedError {
  case encodingFailed
  case invalidArguments
  case invalidImage
  case invalidToken
  case writeFailed

  var errorDescription: String? {
    switch self {
    case .encodingFailed:
      return "The pasteboard image could not be encoded as PNG"
    case .invalidArguments:
      return "Expected valid PNG bytes and an optional string token"
    case .invalidImage:
      return "The pasteboard image is invalid or too large"
    case .invalidToken:
      return "The clipboard token is invalid"
    case .writeFailed:
      return "The operating system refused the clipboard image"
    }
  }
}

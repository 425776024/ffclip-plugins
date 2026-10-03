import AppKit
import WebKit
import AVFoundation
import VideoToolbox

struct RenderSpec: Decodable {
    let document: String
    let initialize: String
    let tick: String
    let output: String
    let width: Int
    let height: Int
    let frames: Int
    let fpsNumerator: Int
    let fpsDenominator: Int
    let sourceBegin: Int
    let sourceEnd: Int
    let duration: Double
    let bitrate: Int
}

func report(_ value: [String: Any]) {
    if let data = try? JSONSerialization.data(withJSONObject: value) {
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data([10]))
    }
}

@available(macOS 11.0, *)
final class Renderer: NSObject, WKNavigationDelegate {
    let spec: RenderSpec
    let webView: WKWebView
    let window: NSWindow
    var writer: AVAssetWriter!
    var input: AVAssetWriterInput!
    var adaptor: AVAssetWriterInputPixelBufferAdaptor!
    var index = 0
    var lastState: String?
    var lastPixels: CVPixelBuffer?
    var reused = 0
    var captureMs = 0.0
    let started = ProcessInfo.processInfo.systemUptime
    let snapshot = WKSnapshotConfiguration()

    init(_ spec: RenderSpec) {
        self.spec = spec
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        let bounds = NSRect(x: 0, y: 0, width: spec.width, height: spec.height)
        webView = WKWebView(frame: bounds, configuration: config)
        window = NSWindow(contentRect: bounds, styleMask: .borderless, backing: .buffered, defer: false)
        super.init()
        webView.navigationDelegate = self
        window.contentView = webView
        window.setFrameOrigin(NSPoint(x: -Double(spec.width) * 3, y: 0))
        window.isReleasedWhenClosed = false
        window.orderBack(nil)
        snapshot.rect = bounds
        snapshot.snapshotWidth = NSNumber(value: spec.width)
        snapshot.afterScreenUpdates = true
    }

    func start() { webView.loadHTMLString(spec.document, baseURL: nil) }

    func fail(_ error: Error) {
        writer?.cancelWriting()
        report(["error": error.localizedDescription])
        exit(1)
    }

    func error(_ message: String) -> Error { NSError(domain: "ffclip-html", code: 1, userInfo: [NSLocalizedDescriptionKey: message]) }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { fail(error) }
    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) { fail(error) }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        webView.callAsyncJavaScript("return await (" + spec.initialize + ")();", arguments: [:], in: nil, in: .page) { result in
            switch result {
            case .failure(let error): self.fail(error)
            case .success:
                do { try self.prepareWriter(); self.next() } catch { self.fail(error) }
            }
        }
    }

    func prepareWriter() throws {
        writer = try AVAssetWriter(outputURL: URL(fileURLWithPath: spec.output), fileType: .mp4)
        writer.shouldOptimizeForNetworkUse = true
        let settings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: spec.width, AVVideoHeightKey: spec.height,
            AVVideoEncoderSpecificationKey: [
                kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder as String: true,
                kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder as String: true
            ],
            AVVideoCompressionPropertiesKey: [
                AVVideoAverageBitRateKey: spec.bitrate,
                AVVideoExpectedSourceFrameRateKey: Double(spec.fpsNumerator) / Double(spec.fpsDenominator),
                AVVideoMaxKeyFrameIntervalDurationKey: 2,
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel
            ]
        ]
        input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
        input.expectsMediaDataInRealTime = false
        adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: spec.width,
            kCVPixelBufferHeightKey as String: spec.height,
            kCVPixelBufferCGImageCompatibilityKey as String: true,
            kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ])
        guard writer.canAdd(input) else { throw error("无法创建本机 H.264 编码器") }
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? error("本机编码器启动失败") }
        writer.startSession(atSourceTime: .zero)
    }

    func next() {
        if index == spec.frames { finish(); return }
        if writer.status == .failed { fail(writer.error ?? error("视频编码失败")); return }
        if !input.isReadyForMoreMediaData {
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(1)) { self.next() }
            return
        }
        let tick = min(spec.sourceEnd - 1, spec.sourceBegin + Int((Double(index) * 120000 * Double(spec.fpsDenominator) / Double(spec.fpsNumerator)).rounded()))
        let script = "return await " + spec.tick.replacingOccurrences(of: "__VIDEOCUT_TICK__", with: String(tick)) + ";"
        webView.callAsyncJavaScript(script, arguments: [:], in: nil, in: .page) { result in
            switch result {
            case .failure(let error): self.fail(error)
            case .success(let state):
                if let key = state as? String, key == self.lastState, let pixels = self.lastPixels {
                    self.reused += 1
                    self.append(pixels)
                } else {
                    self.lastState = state as? String
                    self.capture(attempt: 0)
                }
            }
        }
    }

    func capture(attempt: Int) {
        let start = ProcessInfo.processInfo.systemUptime
        webView.takeSnapshot(with: snapshot) { image, failure in
            self.captureMs += (ProcessInfo.processInfo.systemUptime - start) * 1000
            if let failure { self.fail(failure); return }
            guard let image, let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                self.fail(self.error("HTML 未返回原始像素")); return
            }
            if cg.width != self.spec.width || cg.height != self.spec.height {
                if attempt >= 2 { self.fail(self.error("HTML 像素尺寸不匹配")); return }
                self.snapshot.snapshotWidth = NSNumber(value: self.snapshot.snapshotWidth!.doubleValue * Double(self.spec.width) / Double(cg.width))
                self.capture(attempt: attempt + 1)
                return
            }
            autoreleasepool {
                guard let pool = self.adaptor.pixelBufferPool else { self.fail(self.error("像素池不可用")); return }
                var optional: CVPixelBuffer?
                guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &optional) == kCVReturnSuccess, let pixels = optional else {
                    self.fail(self.error("无法分配视频像素")); return
                }
                CVPixelBufferLockBaseAddress(pixels, [])
                defer { CVPixelBufferUnlockBaseAddress(pixels, []) }
                let color = CGColorSpace(name: CGColorSpace.sRGB)!
                let flags = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
                guard let context = CGContext(data: CVPixelBufferGetBaseAddress(pixels), width: self.spec.width, height: self.spec.height, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(pixels), space: color, bitmapInfo: flags) else {
                    self.fail(self.error("无法创建原始像素上下文")); return
                }
                context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 1))
                context.fill(CGRect(x: 0, y: 0, width: self.spec.width, height: self.spec.height))
                context.draw(cg, in: CGRect(x: 0, y: 0, width: self.spec.width, height: self.spec.height))
                self.lastPixels = pixels
                self.append(pixels)
            }
        }
    }

    func append(_ pixels: CVPixelBuffer) {
        if !input.isReadyForMoreMediaData {
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(1)) { self.append(pixels) }
            return
        }
        let pts = CMTime(value: Int64(index * spec.fpsDenominator), timescale: Int32(spec.fpsNumerator))
        guard adaptor.append(pixels, withPresentationTime: pts) else { fail(writer.error ?? error("视频帧写入失败")); return }
        index += 1
        if index % 10 == 0 || index == spec.frames { report(["completed": index, "total": spec.frames, "phase": "rendering"]) }
        DispatchQueue.main.async { self.next() }
    }

    func finish() {
        writer.endSession(atSourceTime: CMTime(seconds: spec.duration, preferredTimescale: 120000))
        input.markAsFinished()
        writer.finishWriting {
            if self.writer.status != .completed { self.fail(self.writer.error ?? self.error("视频保存失败")); return }
            report(["complete": true, "frames": self.index, "reused": self.reused, "captureMs": self.captureMs,
                    "wallMs": (ProcessInfo.processInfo.systemUptime - self.started) * 1000, "pngFrames": 0, "frameTransferBytes": 0])
            self.lastPixels = nil
            self.adaptor = nil
            self.input = nil
            self.writer = nil
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(10)) { exit(0) }
        }
    }
}

do {
    guard CommandLine.arguments.count == 2 else { throw NSError(domain: "ffclip-html", code: 1, userInfo: [NSLocalizedDescriptionKey: "Usage: ffclip-html-renderer spec.json"]) }
    let bytes = CommandLine.arguments[1] == "-" ? FileHandle.standardInput.readDataToEndOfFile() : try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
    let spec = try JSONDecoder().decode(RenderSpec.self, from: bytes)
    guard spec.width > 0, spec.height > 0, spec.frames > 0, spec.fpsNumerator > 0, spec.fpsDenominator > 0 else { exit(1) }
    if #available(macOS 11.0, *) {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        let renderer = Renderer(spec)
        renderer.start()
        withExtendedLifetime(renderer) { app.run() }
    } else { exit(1) }
} catch {
    report(["error": error.localizedDescription])
    exit(1)
}

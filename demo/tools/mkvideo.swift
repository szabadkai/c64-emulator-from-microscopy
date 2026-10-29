// mkvideo: turn a PNG frame sequence (+ optional WAV) into an H.264/AAC MP4.
//
// usage: mkvideo <frames_dir> <audio.wav|-> <out.mp4> [--scale N] [--rate NUM/DEN] [--par H:V]
//   --scale  integer nearest-neighbour upscale (default 2)
//   --rate   frame duration as a fraction of a second, i.e. seconds per frame = DEN/NUM
//            (default 985248/19656 = PAL C64: 312 lines * 63 cycles at 985248 Hz)
//   --par    pixel aspect ratio stored in the track (default 936:1000, PAL C64 pixels)

import AVFoundation
import CoreGraphics
import Foundation
import ImageIO

func die(_ msg: String) -> Never {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
var scale = 2
var rateNum: Int64 = 985248, rateDen: Int64 = 19656
var parH = 936, parV = 1000
var positional: [String] = []
while !args.isEmpty {
    let a = args.removeFirst()
    switch a {
    case "--scale": scale = Int(args.removeFirst()) ?? 2
    case "--rate":
        let p = args.removeFirst().split(separator: "/").map { Int64($0)! }
        rateNum = p[0]; rateDen = p[1]
    case "--par":
        let p = args.removeFirst().split(separator: ":").map { Int($0)! }
        parH = p[0]; parV = p[1]
    default: positional.append(a)
    }
}
guard positional.count == 3 else {
    die("usage: mkvideo <frames_dir> <audio.wav|-> <out.mp4> [--scale N] [--rate NUM/DEN] [--par H:V]")
}
let framesDir = URL(fileURLWithPath: positional[0])
let audioPath = positional[1]
let outURL = URL(fileURLWithPath: positional[2])

let frameFiles = (try? FileManager.default.contentsOfDirectory(atPath: framesDir.path))?
    .filter { $0.lowercased().hasSuffix(".png") }.sorted() ?? []
guard !frameFiles.isEmpty else { die("no PNG frames in \(framesDir.path)") }

func loadImage(_ name: String) -> CGImage {
    let url = framesDir.appendingPathComponent(name)
    guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { die("cannot read \(url.path)") }
    return img
}

let first = loadImage(frameFiles[0])
let w = first.width * scale, h = first.height * scale

// Pass 1: video-only file.
let videoURL = outURL.deletingPathExtension().appendingPathExtension("video.mp4")
try? FileManager.default.removeItem(at: videoURL)
let writer = try! AVAssetWriter(outputURL: videoURL, fileType: .mp4)
let vin = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.h264,
    AVVideoWidthKey: w,
    AVVideoHeightKey: h,
    AVVideoPixelAspectRatioKey: [
        AVVideoPixelAspectRatioHorizontalSpacingKey: parH,
        AVVideoPixelAspectRatioVerticalSpacingKey: parV,
    ],
    AVVideoCompressionPropertiesKey: [
        AVVideoAverageBitRateKey: 12_000_000,
        AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
    ],
])
vin.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vin, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
    kCVPixelBufferWidthKey as String: w,
    kCVPixelBufferHeightKey as String: h,
])
writer.add(vin)
writer.startWriting()
writer.startSession(atSourceTime: .zero)

for (i, name) in frameFiles.enumerated() {
    let img = i == 0 ? first : loadImage(name)
    while !vin.isReadyForMoreMediaData { usleep(1000) }
    var pb: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &pb)
    guard let buf = pb else { die("pixel buffer allocation failed") }
    CVPixelBufferLockBaseAddress(buf, [])
    let ctx = CGContext(data: CVPixelBufferGetBaseAddress(buf), width: w, height: h, bitsPerComponent: 8,
                        bytesPerRow: CVPixelBufferGetBytesPerRow(buf), space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    ctx.interpolationQuality = .none
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
    CVPixelBufferUnlockBaseAddress(buf, [])
    // Exact PAL frame timing: frame i starts at i * DEN / NUM seconds.
    adaptor.append(buf, withPresentationTime: CMTime(value: Int64(i) * rateDen, timescale: CMTimeScale(rateNum)))
    if i % 500 == 0 { print("frame \(i)/\(frameFiles.count)") }
}
vin.markAsFinished()
let sem = DispatchSemaphore(value: 0)
writer.endSession(atSourceTime: CMTime(value: Int64(frameFiles.count) * rateDen, timescale: CMTimeScale(rateNum)))
writer.finishWriting { sem.signal() }
sem.wait()
guard writer.status == .completed else { die("video write failed: \(String(describing: writer.error))") }

try? FileManager.default.removeItem(at: outURL)
if audioPath == "-" {
    try! FileManager.default.moveItem(at: videoURL, to: outURL)
    print("wrote \(outURL.path) (no audio), \(frameFiles.count) frames \(w)x\(h)")
    exit(0)
}

// Pass 2: encode WAV to AAC with the system converter, then mux both tracks without re-encoding.
let aacURL = outURL.deletingPathExtension().appendingPathExtension("audio.m4a")
try? FileManager.default.removeItem(at: aacURL)
let conv = Process()
conv.executableURL = URL(fileURLWithPath: "/usr/bin/afconvert")
conv.arguments = ["-f", "m4af", "-d", "aac", "-b", "256000", audioPath, aacURL.path]
try! conv.run()
conv.waitUntilExit()
guard conv.terminationStatus == 0 else { die("afconvert failed") }

let comp = AVMutableComposition()
let vAsset = AVURLAsset(url: videoURL), aAsset = AVURLAsset(url: aacURL)
let vTrack = vAsset.tracks(withMediaType: .video)[0]
let aTrack = aAsset.tracks(withMediaType: .audio)[0]
let dur = vAsset.duration
let cv = comp.addMutableTrack(withMediaType: .video, preferredTrackID: kCMPersistentTrackID_Invalid)!
try! cv.insertTimeRange(CMTimeRange(start: .zero, duration: dur), of: vTrack, at: .zero)
let ca = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid)!
try! ca.insertTimeRange(CMTimeRange(start: .zero, duration: CMTimeMinimum(dur, aAsset.duration)), of: aTrack, at: .zero)

let export = AVAssetExportSession(asset: comp, presetName: AVAssetExportPresetPassthrough)!
export.outputURL = outURL
export.outputFileType = .mp4
export.exportAsynchronously { sem.signal() }
sem.wait()
guard export.status == .completed else { die("mux failed: \(String(describing: export.error))") }
try? FileManager.default.removeItem(at: videoURL)
try? FileManager.default.removeItem(at: aacURL)
print("wrote \(outURL.path), \(frameFiles.count) frames \(w)x\(h), audio from \(audioPath)")

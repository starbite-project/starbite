// encode.swift FRAMES.bgra|- W H FPS OUT.mp4: raw BGRA frames (sbtk-host film; '-' = from a pipe, so no frame ever
// lands on disk) into an H.264 film (AVFoundation, nothing to install on a Mac).
import AVFoundation
import CoreVideo
let a = CommandLine.arguments
let w = Int(a[2])!, h = Int(a[3])!, fps = Int32(a[4])!
let url = URL(fileURLWithPath: a[5])
try? FileManager.default.removeItem(at: url)
let fsize = w * h * 4
let input = a[1] == "-" ? FileHandle.standardInput : FileHandle(forReadingAtPath: a[1])!
func readFrame() -> Data? {
  var d = Data(capacity: fsize)
  while d.count < fsize {
    let c = input.readData(ofLength: fsize - d.count)
    if c.isEmpty { return nil }
    d.append(c)
  }
  return d
}
var n = 0
let wr = try! AVAssetWriter(outputURL: url, fileType: .mp4)
let inp = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: w, AVVideoHeightKey: h,
  AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 12_000_000, AVVideoMaxKeyFrameIntervalKey: 60]])
inp.expectsMediaDataInRealTime = false
let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: inp, sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: w, kCVPixelBufferHeightKey as String: h])
wr.add(inp)
wr.startWriting()
wr.startSession(atSourceTime: .zero)
while let frame = readFrame() {
  let i = n
  n += 1
  frame.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
    while !inp.isReadyForMoreMediaData { usleep(1000) }
    var pb: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, ad.pixelBufferPool!, &pb)
    CVPixelBufferLockBaseAddress(pb!, [])
    let dst = CVPixelBufferGetBaseAddress(pb!)!, stride = CVPixelBufferGetBytesPerRow(pb!)
    for y in 0..<h { memcpy(dst + y * stride, raw.baseAddress! + y * w * 4, w * 4) }
    CVPixelBufferUnlockBaseAddress(pb!, [])
    ad.append(pb!, withPresentationTime: CMTime(value: CMTimeValue(i), timescale: fps))
  }
}
inp.markAsFinished()
let sem = DispatchSemaphore(value: 0)
wr.finishWriting { sem.signal() }
sem.wait()
print("\(a[5]): \(n) frames at \(fps) fps, \(wr.status == .completed ? "ok" : "failed: \(String(describing: wr.error))")")

// spm_ffmpeg/Sources/FFmpegSupport/FFmpegSupport.swift — 앱이 보는 유일한 API (Swift 래퍼)
//
// 스레딩 계약: FFmpegTranscoder.transcode 는 동기(블로킹)이며 호출자가 백그라운드 스레드/큐에서 돌린다.
// 진행/취소 콜백은 트랜스코딩 스레드에서 호출된다. 라이브러리 전역 상태는 av_log 레벨뿐이다.
import Foundation
import CoreGraphics
import FFmpegBridge

// MARK: - 오류

public enum FFmpegError: Error, Equatable, Sendable, CustomStringConvertible {
    case open(String)
    case noVideoTrack
    case decoderUnavailable(String)
    case encoderUnavailable(String)
    case muxer(String)
    case io(String)
    case cancelled
    case decode(String)
    case encode(String)
    case invalidArgument(String)
    case unknown(code: Int32, message: String)

    static func make(code: Int32, message: String) -> FFmpegError {
        switch Int(code) {
        case FFX_ERR_OPEN: return .open(message)
        case FFX_ERR_NO_VIDEO: return .noVideoTrack
        case FFX_ERR_DECODER: return .decoderUnavailable(message)
        case FFX_ERR_ENCODER: return .encoderUnavailable(message)
        case FFX_ERR_MUXER: return .muxer(message)
        case FFX_ERR_IO: return .io(message)
        case FFX_ERR_CANCELLED: return .cancelled
        case FFX_ERR_DECODE: return .decode(message)
        case FFX_ERR_ENCODE: return .encode(message)
        case FFX_ERR_ARG: return .invalidArgument(message)
        default: return .unknown(code: code, message: message)
        }
    }

    public var description: String {
        switch self {
        case .open(let m): return "open: \(m)"
        case .noVideoTrack: return "no video track"
        case .decoderUnavailable(let m): return "decoder: \(m)"
        case .encoderUnavailable(let m): return "encoder: \(m)"
        case .muxer(let m): return "muxer: \(m)"
        case .io(let m): return "io: \(m)"
        case .cancelled: return "cancelled"
        case .decode(let m): return "decode: \(m)"
        case .encode(let m): return "encode: \(m)"
        case .invalidArgument(let m): return "argument: \(m)"
        case .unknown(let c, let m): return "unknown(\(c)): \(m)"
        }
    }
}

// MARK: - 프로브

public struct FFAudioTrackInfo: Sendable, Equatable {
    public let codec: String
    public let channels: Int
    public let sampleRate: Int
    public let bitRate: Int64
    public let isDecodable: Bool
    /// mp4/mov 로 stream copy 가능한 코덱인지(정책 정본은 C 쪽 ffx_audio_codec_mp4_compatible).
    public let isMP4Compatible: Bool
    public let language: String
}

public struct FFMediaInfo: Sendable, Equatable {
    public let container: String
    public let videoCodec: String
    public let videoProfile: String
    public let width: Int
    public let height: Int
    public let rotation: Int
    public let frameRate: Double
    public let duration: Double
    public let videoBitRate: Int64
    public let totalBitRate: Int64
    public let bitDepth: Int
    public let colorPrimaries: String
    public let colorTransfer: String
    public let colorSpace: String
    public let isVideoDecodable: Bool
    public let hardwareDecodeCandidate: Bool
    public let audio: [FFAudioTrackInfo]
    public let otherStreamCount: Int

    /// 표시 기준 크기(회전 반영).
    public var displayWidth: Int { (rotation == 90 || rotation == 270) ? height : width }
    public var displayHeight: Int { (rotation == 90 || rotation == 270) ? width : height }

    /// 비디오 비트레이트 추정(bps): 스트림 보고값 → 컨테이너 총합에서 오디오를 뺀 값 → 0.
    public var estimatedVideoBitRate: Int64 {
        if videoBitRate > 0 { return videoBitRate }
        if totalBitRate > 0 {
            let audioSum = audio.reduce(Int64(0)) { $0 + $1.bitRate }
            return max(0, totalBitRate - audioSum)
        }
        return 0
    }

    /// 이 입력을 ffmpeg 경로로 처리할 수 있는지(비디오 디코더 존재). 오디오는 best-effort.
    public var isTranscodable: Bool { isVideoDecodable }

    init(_ c: ffx_media_info) {
        var c = c
        container = withUnsafePointer(to: &c.container) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        videoCodec = withUnsafePointer(to: &c.video_codec) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        videoProfile = withUnsafePointer(to: &c.video_profile) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        width = Int(c.width)
        height = Int(c.height)
        rotation = Int(c.rotation)
        frameRate = c.frame_rate
        duration = c.duration
        videoBitRate = c.video_bit_rate
        totalBitRate = c.total_bit_rate
        bitDepth = Int(c.bit_depth)
        colorPrimaries = withUnsafePointer(to: &c.color_primaries) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        colorTransfer = withUnsafePointer(to: &c.color_transfer) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        colorSpace = withUnsafePointer(to: &c.color_space) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        isVideoDecodable = c.video_decodable != 0
        hardwareDecodeCandidate = c.hw_decode_supported != 0
        otherStreamCount = Int(c.other_stream_count)
        var tracks: [FFAudioTrackInfo] = []
        let count = Int(c.audio_count)
        withUnsafePointer(to: &c.audio) { ptr in
            let base = UnsafeRawPointer(ptr).assumingMemoryBound(to: ffx_audio_info.self)
            for i in 0..<count {
                var a = base[i]
                let codec = withUnsafePointer(to: &a.codec) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
                let lang = withUnsafePointer(to: &a.language) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
                tracks.append(FFAudioTrackInfo(codec: codec,
                                               channels: Int(a.channels),
                                               sampleRate: Int(a.sample_rate),
                                               bitRate: a.bit_rate,
                                               isDecodable: a.decodable != 0,
                                               isMP4Compatible: a.mp4_compatible != 0,
                                               language: lang))
            }
        }
        audio = tracks
    }
}

public enum FFmpegProber {
    /// 컨테이너/스트림/코덱 정보. 비디오가 없거나 열 수 없으면 throw.
    public static func probe(_ url: URL) throws -> FFMediaInfo {
        var info = ffx_media_info()
        var err = [CChar](repeating: 0, count: 512)
        let rc = url.withUnsafeFileSystemRepresentation { path -> Int32 in
            ffx_probe(path, &info, &err, err.count)
        }
        guard rc == FFX_OK else { throw FFmpegError.make(code: rc, message: String(cString: err)) }
        return FFMediaInfo(info)
    }
}

// MARK: - 트랜스코드

public enum FFAudioPolicy: Sendable, Equatable {
    /// mp4 호환 코덱은 copy, 아니면 AAC 재인코딩(계획 D2 권장안).
    case copyIfCompatible(aacBitRate: Int = 128_000)
    case alwaysAAC(bitRate: Int = 128_000)
    /// 호환 아니면 트랙 제외.
    case copyOrDrop

    var cMode: Int32 {
        switch self {
        case .copyIfCompatible: return Int32(FFX_AUDIO_COPY_OR_AAC.rawValue)
        case .alwaysAAC: return Int32(FFX_AUDIO_ALWAYS_AAC.rawValue)
        case .copyOrDrop: return Int32(FFX_AUDIO_COPY_OR_DROP.rawValue)
        }
    }
    var aacBitRate: Int32 {
        switch self {
        case .copyIfCompatible(let b): return Int32(b)
        case .alwaysAAC(let b): return Int32(b)
        case .copyOrDrop: return 0
        }
    }
}

public enum FFOutputContainer: Sendable, Equatable {
    case mp4, mov
    public var fileExtension: String { self == .mp4 ? "mp4" : "mov" }
}

public struct FFTranscodeOptions: Sendable, Equatable {
    public var videoBitRate: Int
    public var preferHardwareDecode: Bool = true
    public var allowSoftwareEncode: Bool = false
    public var audio: FFAudioPolicy = .copyIfCompatible()
    public var container: FFOutputContainer = .mp4
    public var maxLongEdge: Int? = nil
    public var maxFrameRate: Double? = nil
    public var gopSeconds: Int = 2
    public var prioritizeSpeed: Bool = false
    /// 항상 HEVC 로 인코딩(기본). false 면 H.264 소스는 H.264 유지.
    public var forceHEVC: Bool = true
    /// 출력 컨테이너 메타(mdta keys). 예: ["com.apple.quicktime.encodedby": "encodedByUrsusShock"].
    public var metadata: [String: String] = [:]
    /// av_log 레벨(0 = 기본 error). 진단 시 16(warning)/32(info).
    public var logLevel: Int32 = 0

    public init(videoBitRate: Int) { self.videoBitRate = videoBitRate }
}

public struct FFTranscodeResult: Sendable, Equatable {
    public let framesDecoded: Int64
    public let framesEncoded: Int64
    public let framesDropped: Int64
    public let usedHardwareDecode: Bool
    public let usedHardwareEncode: Bool
    public let audioStreamsIn: Int
    public let audioStreamsOut: Int
    public let audioReencoded: Int
    public let audioDropped: Int
    public let outputWidth: Int
    public let outputHeight: Int
    public let outputBitDepth: Int
    public let videoEncoder: String
    public let elapsedSeconds: Double
    public let outputDuration: Double

    init(_ s: ffx_transcode_stats) {
        var s = s
        framesDecoded = s.frames_decoded
        framesEncoded = s.frames_encoded
        framesDropped = s.frames_dropped
        usedHardwareDecode = s.hw_decode_used != 0
        usedHardwareEncode = s.hw_encode_used != 0
        audioStreamsIn = Int(s.audio_streams_in)
        audioStreamsOut = Int(s.audio_streams_out)
        audioReencoded = Int(s.audio_reencoded)
        audioDropped = Int(s.audio_dropped)
        outputWidth = Int(s.out_width)
        outputHeight = Int(s.out_height)
        outputBitDepth = Int(s.out_bit_depth)
        videoEncoder = withUnsafePointer(to: &s.video_encoder) { String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self)) }
        elapsedSeconds = s.elapsed_seconds
        outputDuration = s.out_duration
    }
}

/// 진행/취소 콜백 상자(C 트램펄린용).
private final class ProgressBox {
    let progress: (Double) -> Void
    let isCancelled: () -> Bool
    init(progress: @escaping (Double) -> Void, isCancelled: @escaping () -> Bool) {
        self.progress = progress
        self.isCancelled = isCancelled
    }
}

private func ffx_progress_trampoline(_ ctx: UnsafeMutableRawPointer?, _ fraction: Double) -> Int32 {
    guard let ctx else { return 0 }
    let box = Unmanaged<ProgressBox>.fromOpaque(ctx).takeUnretainedValue()
    box.progress(fraction)
    return box.isCancelled() ? 1 : 0
}

public final class FFmpegTranscoder: @unchecked Sendable {
    // 상태 없음 — @unchecked 는 클래스 형태(콜백 보관 없이 호출별 상자 사용) 때문이며 공유 가변 상태가 없다.
    public init() {}

    /// 동기 트랜스코드. 백그라운드 스레드에서 호출할 것. 실패/취소 시 출력 파일은 삭제된다.
    public func transcode(input: URL, output: URL,
                          options: FFTranscodeOptions,
                          progress: @escaping (Double) -> Void,
                          isCancelled: @escaping () -> Bool) throws -> FFTranscodeResult {
        var opts = ffx_transcode_options()
        opts.video_bit_rate = Int64(options.videoBitRate)
        opts.prefer_hw_decode = options.preferHardwareDecode ? 1 : 0
        opts.allow_sw_encode = options.allowSoftwareEncode ? 1 : 0
        opts.audio_mode = options.audio.cMode
        opts.aac_bit_rate = options.audio.aacBitRate
        opts.container = Int32(options.container == .mov ? FFX_CONTAINER_MOV.rawValue : FFX_CONTAINER_MP4.rawValue)
        opts.max_long_edge = Int32(options.maxLongEdge ?? 0)
        opts.max_frame_rate = options.maxFrameRate ?? 0
        opts.gop_seconds = Int32(options.gopSeconds)
        opts.prioritize_speed = options.prioritizeSpeed ? 1 : 0
        opts.force_hevc = options.forceHEVC ? 1 : 0
        opts.log_level = options.logLevel

        let keys = options.metadata.keys.sorted()
        let values = keys.map { options.metadata[$0]! }
        let box = ProgressBox(progress: progress, isCancelled: isCancelled)
        let boxPtr = Unmanaged.passRetained(box).toOpaque()
        defer { Unmanaged<ProgressBox>.fromOpaque(boxPtr).release() }

        var stats = ffx_transcode_stats()
        var err = [CChar](repeating: 0, count: 512)
        let rc: Int32 = withCStringArray(keys) { keyPtrs in
            withCStringArray(values) { valPtrs in
                opts.metadata_keys = keyPtrs
                opts.metadata_values = valPtrs
                opts.metadata_count = Int32(keys.count)
                return input.withUnsafeFileSystemRepresentation { inPath in
                    output.withUnsafeFileSystemRepresentation { outPath in
                        ffx_transcode(inPath, outPath, &opts, ffx_progress_trampoline, boxPtr, &stats, &err, err.count)
                    }
                }
            }
        }
        guard rc == FFX_OK else { throw FFmpegError.make(code: rc, message: String(cString: err)) }
        return FFTranscodeResult(stats)
    }
}

/// [String] → NULL 종료 C 문자열 포인터 배열(스코프 한정).
private func withCStringArray<R>(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>) -> R) -> R {
    var cStrings: [UnsafeMutablePointer<CChar>?] = strings.map { strdup($0) }
    defer { cStrings.forEach { free($0) } }
    cStrings.append(nil)
    return cStrings.withUnsafeBufferPointer { buf in
        buf.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) { body($0) }
    }
}

// MARK: - 라이브러리 정보

public enum FFmpegInfo {
    public static var version: String { String(cString: ffx_version()) }
    public static var configuration: String { String(cString: ffx_configuration()) }
    public static var license: String { String(cString: ffx_license()) }
    public static func hasDecoder(_ name: String) -> Bool { ffx_has_decoder(name) != 0 }
    public static func hasEncoder(_ name: String) -> Bool { ffx_has_encoder(name) != 0 }
    public static func isMP4CompatibleAudio(_ codec: String) -> Bool { ffx_audio_codec_mp4_compatible(codec) != 0 }
    public static func setLogLevel(_ level: Int32) { ffx_set_log_level(level) }
}

// MARK: - 썸네일

public struct FFThumbnail: Sendable {
    public let image: CGImage
    /// 표시 회전(도, 0/90/180/270). 픽셀은 회전되지 않은 인코딩 방향이다.
    public let rotation: Int
    public var width: Int { image.width }
    public var height: Int { image.height }
}

public enum FFmpegThumbnailer {
    /// 대표 프레임 1장(RGBA → CGImage). 동기·수십 ms~수백 ms. 백그라운드에서 호출할 것.
    public static func thumbnail(_ url: URL, at seconds: Double = 1.0, maxEdge: Int = 512) throws -> FFThumbnail {
        var rgba: UnsafeMutablePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, rot: Int32 = 0
        var err = [CChar](repeating: 0, count: 256)
        let rc = url.withUnsafeFileSystemRepresentation { path -> Int32 in
            ffx_thumbnail(path, seconds, Int32(maxEdge), &rgba, &w, &h, &rot, &err, err.count)
        }
        guard rc == FFX_OK, let buffer = rgba, w > 0, h > 0 else {
            throw FFmpegError.make(code: rc, message: String(cString: err))
        }
        defer { ffx_free(buffer) }
        let width = Int(w), height = Int(h)
        let byteCount = width * height * 4
        let data = Data(bytes: buffer, count: byteCount)
        guard let provider = CGDataProvider(data: data as CFData) else { throw FFmpegError.decode("CGDataProvider failed") }
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue)
        let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                            bytesPerRow: width * 4, space: colorSpace, bitmapInfo: bitmapInfo,
                            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        guard let image else { throw FFmpegError.decode("CGImage creation failed") }
        return FFThumbnail(image: image, rotation: Int(rot))
    }
}

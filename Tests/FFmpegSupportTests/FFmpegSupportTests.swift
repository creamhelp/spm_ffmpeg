// spm_ffmpeg/Tests/FFmpegSupportTests/FFmpegSupportTests.swift — 프로브/트랜스코드 스모크 (macOS, 픽스처 기반)
//
// 앱 쪽(LeanVidTests)의 정밀 검증(AVFoundation 재검증·라우팅·폴백)과 별개로, 패키지 단독으로
// "모든 픽스처가 열리고, 디코더가 있고, VT 인코더로 완주한다"를 보장한다.
import XCTest
@testable import FFmpegSupport

final class FFmpegSupportTests: XCTestCase {

    // MARK: - 픽스처

    static let transcodable: [(name: String, codec: String, audioCodecs: [String], container: String)] = [
        ("h264_aac.mkv", "h264", ["aac"], "matroska"),
        ("hevc_ac3.mkv", "hevc", ["ac3"], "matroska"),
        ("mpeg4_mp3.avi", "mpeg4", ["mp3"], "avi"),
        ("mpeg2_mp2.ts", "mpeg2video", ["mp2"], "mpegts"),
        ("wmv2_wma.wmv", "wmv2", ["wmav2"], "asf"),
        ("vp9_opus.webm", "vp9", ["opus"], "matroska"),
        ("vp8_vorbis.webm", "vp8", ["vorbis"], "matroska"),
        ("h264_aac.flv", "h264", ["aac"], "flv"),
        ("mpeg4_noaudio.avi", "mpeg4", [], "avi"),
        ("h264_two_audio.mkv", "h264", ["aac", "mp3"], "matroska"),
        ("hevc10_aac.mkv", "hevc", ["aac"], "matroska"),
        ("h264_aac.mp4", "h264", ["aac"], "mov"),
        ("hevc_aac.mov", "hevc", ["aac"], "mov"),
        ("h264_rot90.mp4", "h264", ["aac"], "mov"),
        ("h264_60fps.mkv", "h264", ["aac"], "matroska"),
        ("vp9_aac.mp4", "vp9", ["aac"], "mov"),
    ]

    override func setUp() {
        super.setUp()
        if ProcessInfo.processInfo.environment["FFX_TEST_LOG"] != nil { FFmpegInfo.setLogLevel(32) }
    }

    /// 진단: 실패한 픽스처 1건을 로그와 함께 돌린다(FFX_TEST_LOG=1 FFX_DIAG=<name>).
    func testDiagnoseSingleFixture() throws {
        guard let name = ProcessInfo.processInfo.environment["FFX_DIAG"] else { throw XCTSkip("FFX_DIAG 미지정") }
        let input = try fixture(name)
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        var opts = FFTranscodeOptions(videoBitRate: 200_000)
        opts.logLevel = 32
        do {
            let r = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                     progress: { _ in }, isCancelled: { false })
            print("[diag] \(name): \(r)")
        } catch {
            XCTFail("[diag] \(name): \(error)")
        }
    }

    func fixture(_ name: String) throws -> URL {
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        guard let url = Bundle.module.url(forResource: base, withExtension: ext, subdirectory: "Fixtures") else {
            throw XCTSkip("fixture missing: \(name) — run scripts/make-fixtures.sh")
        }
        return url
    }

    private func tempOutput(_ ext: String = "mp4") -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("ffx-\(UUID().uuidString).\(ext)")
    }

    // MARK: - 라이브러리 정보

    func testLibraryInfo() {
        XCTAssertTrue(FFmpegInfo.version.hasPrefix("8."), FFmpegInfo.version)
        XCTAssertTrue(FFmpegInfo.license.contains("LGPL"), FFmpegInfo.license)
        XCTAssertFalse(FFmpegInfo.configuration.contains("--enable-gpl"))
        XCTAssertTrue(FFmpegInfo.configuration.contains("--disable-gpl"))
        XCTAssertTrue(FFmpegInfo.hasEncoder("hevc_videotoolbox"))
        XCTAssertTrue(FFmpegInfo.hasEncoder("h264_videotoolbox"))
        XCTAssertTrue(FFmpegInfo.hasEncoder("aac"))
        XCTAssertFalse(FFmpegInfo.hasEncoder("libx264"), "GPL 인코더가 들어가면 안 된다")
        for d in ["h264", "hevc", "vp9", "av1", "mpeg2video", "mpeg4", "wmv2", "vc1", "opus", "vorbis", "flac", "dca"] {
            XCTAssertTrue(FFmpegInfo.hasDecoder(d), "decoder missing: \(d)")
        }
        XCTAssertTrue(FFmpegInfo.isMP4CompatibleAudio("aac"))
        XCTAssertFalse(FFmpegInfo.isMP4CompatibleAudio("opus"))
    }

    // MARK: - 프로브

    func testProbeAllFixtures() throws {
        for f in Self.transcodable {
            let url = try fixture(f.name)
            let info = try FFmpegProber.probe(url)
            XCTAssertEqual(info.videoCodec, f.codec, f.name)
            XCTAssertTrue(info.container.contains(f.container), "\(f.name): \(info.container)")
            XCTAssertEqual(info.audio.map(\.codec), f.audioCodecs, f.name)
            XCTAssertTrue(info.isVideoDecodable, f.name)
            XCTAssertEqual(info.width, 320, f.name)
            XCTAssertEqual(info.height, 240, f.name)
            XCTAssertGreaterThan(info.duration, 1.5, f.name)
            XCTAssertLessThan(info.duration, 2.6, f.name)
        }
    }

    func testProbeRotation() throws {
        let info = try FFmpegProber.probe(try fixture("h264_rot90.mp4"))
        XCTAssertEqual(info.rotation, 90)
        XCTAssertEqual(info.displayWidth, 240)
        XCTAssertEqual(info.displayHeight, 320)
    }

    func testProbeBitDepth() throws {
        XCTAssertEqual(try FFmpegProber.probe(try fixture("hevc10_aac.mkv")).bitDepth, 10)
        XCTAssertEqual(try FFmpegProber.probe(try fixture("h264_aac.mkv")).bitDepth, 8)
    }

    func testProbeRejectsGarbage() throws {
        let url = try fixture("not_a_video.bin")
        XCTAssertThrowsError(try FFmpegProber.probe(url)) { error in
            guard case FFmpegError.open = error as! FFmpegError else {
                return XCTFail("expected .open, got \(error)")
            }
        }
    }

    func testProbeAudioOnlyIsNoVideo() throws {
        let url = try fixture("audio_only.m4a")
        XCTAssertThrowsError(try FFmpegProber.probe(url)) { error in
            XCTAssertEqual(error as? FFmpegError, .noVideoTrack)
        }
    }

    // MARK: - 트랜스코드

    func testTranscodeAllFixtures() throws {
        let transcoder = FFmpegTranscoder()
        for f in Self.transcodable {
            let input = try fixture(f.name)
            let output = tempOutput()
            defer { try? FileManager.default.removeItem(at: output) }
            var opts = FFTranscodeOptions(videoBitRate: 200_000)
            opts.metadata = ["com.apple.quicktime.encodedby": "encodedByUrsusShock"]
            var last = 0.0
            let result: FFTranscodeResult
            do {
                result = try transcoder.transcode(input: input, output: output, options: opts,
                                                  progress: { last = $0 }, isCancelled: { false })
            } catch {
                XCTFail("\(f.name): \(error)")
                continue
            }
            XCTAssertEqual(last, 1.0, accuracy: 0.001, "\(f.name): 최종 진행률")
            XCTAssertGreaterThan(result.framesEncoded, 0, f.name)
            XCTAssertEqual(result.videoEncoder, "hevc_videotoolbox", f.name)
            XCTAssertTrue(result.usedHardwareEncode, f.name)
            XCTAssertEqual(result.audioStreamsIn, f.audioCodecs.count, f.name)
            XCTAssertEqual(result.audioStreamsOut, f.audioCodecs.count, "\(f.name): 오디오 트랙 수 보존")
            let expectedReencode = f.audioCodecs.filter { !FFmpegInfo.isMP4CompatibleAudio($0) }.count
            XCTAssertEqual(result.audioReencoded, expectedReencode, "\(f.name): AAC 재인코딩 수")
            let size = (try? FileManager.default.attributesOfItem(atPath: output.path)[.size] as? Int64) ?? 0
            XCTAssertGreaterThan(size, 1_000, f.name)
            // 출력이 다시 프로브되고 hevc / aac(or copy) 인지
            let out = try FFmpegProber.probe(output)
            XCTAssertEqual(out.videoCodec, "hevc", f.name)
            XCTAssertTrue(out.container.contains("mov"), f.name)
            XCTAssertEqual(out.audio.count, f.audioCodecs.count, f.name)
            for a in out.audio { XCTAssertTrue(a.isMP4Compatible, "\(f.name): 출력 오디오 \(a.codec)") }
            XCTAssertEqual(out.duration, (try FFmpegProber.probe(input)).duration, accuracy: 0.25, f.name)
            if f.name == "hevc10_aac.mkv" {
                XCTAssertEqual(result.outputBitDepth, 10, "10-bit 보존")
                XCTAssertEqual(out.bitDepth, 10, "10-bit 보존(프로브)")
            } else {
                XCTAssertEqual(out.bitDepth, 8, f.name)
            }
        }
    }

    func testTranscodeKeepsRotation() throws {
        let input = try fixture("h264_rot90.mp4")
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        _ = try FFmpegTranscoder().transcode(input: input, output: output,
                                             options: FFTranscodeOptions(videoBitRate: 200_000),
                                             progress: { _ in }, isCancelled: { false })
        let out = try FFmpegProber.probe(output)
        XCTAssertEqual(out.rotation, 90, "display matrix 승계")
        XCTAssertEqual(out.width, 320)
    }

    func testTranscodeFrameRateCap() throws {
        let input = try fixture("h264_60fps.mkv")
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        var opts = FFTranscodeOptions(videoBitRate: 200_000)
        opts.maxFrameRate = 30
        let r = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                 progress: { _ in }, isCancelled: { false })
        XCTAssertEqual(r.framesDecoded, 120, accuracy: 2)
        XCTAssertGreaterThan(r.framesDropped, 50)
        XCTAssertEqual(r.framesEncoded, 60, accuracy: 3)
    }

    func testTranscodeDownscale() throws {
        let input = try fixture("h264_aac.mkv")
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        var opts = FFTranscodeOptions(videoBitRate: 200_000)
        opts.maxLongEdge = 160
        let r = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                 progress: { _ in }, isCancelled: { false })
        XCTAssertEqual(r.outputWidth, 160)
        XCTAssertEqual(r.outputHeight, 120)
        let out = try FFmpegProber.probe(output)
        XCTAssertEqual(out.width, 160)
    }

    func testTranscodeMovContainerAndH264Keep() throws {
        let input = try fixture("h264_aac.mkv")
        let output = tempOutput("mov")
        defer { try? FileManager.default.removeItem(at: output) }
        var opts = FFTranscodeOptions(videoBitRate: 200_000)
        opts.container = .mov
        opts.forceHEVC = false
        let r = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                 progress: { _ in }, isCancelled: { false })
        XCTAssertEqual(r.videoEncoder, "h264_videotoolbox")
        XCTAssertEqual(try FFmpegProber.probe(output).videoCodec, "h264")
    }

    func testTranscodeAudioDropPolicy() throws {
        let input = try fixture("vp9_opus.webm")
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        var opts = FFTranscodeOptions(videoBitRate: 200_000)
        opts.audio = .copyOrDrop
        let r = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                 progress: { _ in }, isCancelled: { false })
        XCTAssertEqual(r.audioStreamsOut, 0)
        XCTAssertEqual(r.audioDropped, 1)
        XCTAssertEqual(try FFmpegProber.probe(output).audio.count, 0)
    }

    func testTranscodeSoftwareDecodeForced() throws {
        let input = try fixture("h264_aac.mkv")
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        var opts = FFTranscodeOptions(videoBitRate: 200_000)
        opts.preferHardwareDecode = false
        let r = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                 progress: { _ in }, isCancelled: { false })
        XCTAssertFalse(r.usedHardwareDecode)
        XCTAssertEqual(r.framesEncoded, 60, accuracy: 2)
    }

    func testTranscodeCancellation() throws {
        let input = try fixture("h264_aac.mkv")
        let output = tempOutput()
        defer { try? FileManager.default.removeItem(at: output) }
        var calls = 0
        XCTAssertThrowsError(try FFmpegTranscoder().transcode(
            input: input, output: output, options: FFTranscodeOptions(videoBitRate: 200_000),
            progress: { _ in calls += 1 }, isCancelled: { calls >= 1 })) { error in
            XCTAssertEqual(error as? FFmpegError, .cancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: output.path), "취소 시 출력 삭제")
    }

    func testTranscodeGarbageFails() throws {
        let input = try fixture("not_a_video.bin")
        let output = tempOutput()
        XCTAssertThrowsError(try FFmpegTranscoder().transcode(
            input: input, output: output, options: FFTranscodeOptions(videoBitRate: 200_000),
            progress: { _ in }, isCancelled: { false }))
        XCTAssertFalse(FileManager.default.fileExists(atPath: output.path))
    }

    func testTranscodeInvalidBitrateRejected() throws {
        let input = try fixture("h264_aac.mkv")
        XCTAssertThrowsError(try FFmpegTranscoder().transcode(
            input: input, output: tempOutput(), options: FFTranscodeOptions(videoBitRate: 0),
            progress: { _ in }, isCancelled: { false })) { error in
            guard case FFmpegError.invalidArgument = error as! FFmpegError else { return XCTFail("\(error)") }
        }
    }
}

// MARK: - AVFoundation 상호운용(macOS)
import AVFoundation

final class FFmpegAVFoundationInteropTests: XCTestCase {
    private func fixture(_ name: String) throws -> URL {
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        guard let url = Bundle.module.url(forResource: base, withExtension: ext, subdirectory: "Fixtures") else {
            throw XCTSkip("fixture missing: \(name)")
        }
        return url
    }

    /// mdta 키로 쓴 서명이 AVFoundation 의 quickTimeMetadataEncodedBy 로 읽혀야 한다(앱의 회차 서명 계약).
    func testMetadataSignatureReadableByAVFoundation() async throws {
        let input = try fixture("h264_aac.mkv")
        for container in [FFOutputContainer.mp4, .mov] {
            let output = FileManager.default.temporaryDirectory.appendingPathComponent("ffx-meta-\(UUID().uuidString).\(container.fileExtension)")
            defer { try? FileManager.default.removeItem(at: output) }
            var opts = FFTranscodeOptions(videoBitRate: 200_000)
            opts.container = container
            opts.metadata = ["com.apple.quicktime.encodedby": "encodedByUrsusShock"]
            _ = try FFmpegTranscoder().transcode(input: input, output: output, options: opts,
                                                 progress: { _ in }, isCancelled: { false })
            let asset = AVURLAsset(url: output)
            let items = try await asset.load(.metadata)
            var found: String?
            var seen: [String] = []
            for item in items {
                seen.append("\(item.identifier?.rawValue ?? "nil")=\((try? await item.load(.stringValue)) ?? "")")
                if item.identifier == .quickTimeMetadataEncodedBy { found = try? await item.load(.stringValue) }
            }
            let formats = (try? await asset.load(.availableMetadataFormats).map(\.rawValue)) ?? []
            print("[meta] \(container.fileExtension): formats=\(formats) seen=\(seen)")
            XCTAssertEqual(found, "encodedByUrsusShock", "\(container.fileExtension) seen: \(seen)")
        }
    }

    /// MPEG-TS(시작 1.4s) → 출력 길이가 입력과 같아야 한다(오디오/비디오 오프셋 정합).
    func testTransportStreamStartOffsetNormalized() async throws {
        let input = try fixture("mpeg2_mp2.ts")
        let output = FileManager.default.temporaryDirectory.appendingPathComponent("ffx-ts-\(UUID().uuidString).mp4")
        defer { try? FileManager.default.removeItem(at: output) }
        _ = try FFmpegTranscoder().transcode(input: input, output: output, options: FFTranscodeOptions(videoBitRate: 200_000),
                                             progress: { _ in }, isCancelled: { false })
        let dur = try await AVURLAsset(url: output).load(.duration).seconds
        XCTAssertEqual(dur, try FFmpegProber.probe(input).duration, accuracy: 0.3)
        let audio = try await AVURLAsset(url: output).loadTracks(withMediaType: .audio)
        XCTAssertEqual(audio.count, 1)
    }

    /// mp3 오디오는 AVFoundation 이 mp4 안에서 트랙으로 인식하지 않으므로 AAC 로 재인코딩되어야 한다.
    func testMP3AudioIsReencodedForAVFoundation() async throws {
        let input = try fixture("h264_two_audio.mkv")
        let output = FileManager.default.temporaryDirectory.appendingPathComponent("ffx-mp3-\(UUID().uuidString).mp4")
        defer { try? FileManager.default.removeItem(at: output) }
        let r = try FFmpegTranscoder().transcode(input: input, output: output, options: FFTranscodeOptions(videoBitRate: 200_000),
                                                 progress: { _ in }, isCancelled: { false })
        XCTAssertEqual(r.audioStreamsOut, 2)
        XCTAssertEqual(r.audioReencoded, 1)
        let audio = try await AVURLAsset(url: output).loadTracks(withMediaType: .audio)
        XCTAssertEqual(audio.count, 2, "AVFoundation 이 두 오디오 트랙을 모두 봐야 한다")
    }
}

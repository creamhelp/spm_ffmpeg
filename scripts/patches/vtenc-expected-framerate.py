#!/usr/bin/env python3
"""spm_ffmpeg patch: videotoolbox 인코더가 kVTCompressionPropertyKey_ExpectedFrameRate 를 세션에 알린다.

배경(실측, iPhone 17 Pro Max): FFmpeg 의 vtenc 는 AverageBitRate 만 주고 ExpectedFrameRate 는 설정하지 않으며
프레임 duration 도 kCMTimeInvalid 로 넘긴다. iOS 의 HEVC 하드웨어 인코더는 그 상태에서 프레임레이트를 30fps 로
가정해 레이트 컨트롤 예산을 잡는 것으로 보인다 — 1080p60 VP9 → HEVC 2.71Mbps 목표에서 실측 5.4Mbps(정확히 2배),
결과가 원본(3.0Mbps)보다 커졌다. macOS(시뮬레이터 포함) 인코더는 타임스탬프로 추정하는지 목표를 지켰다.
avctx->framerate 가 있으면(브리지는 항상 준다) ExpectedFrameRate 로 알려 예산을 실제 프레임레이트에 맞춘다.

멱등: 마커로 건너뛴다. 사용: python3 vtenc-expected-framerate.py <ffmpeg-src>/libavcodec/videotoolboxenc.c
"""
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

MARK = "spm_ffmpeg: ExpectedFrameRate"
if MARK in src:
    print("vtenc: already patched")
    sys.exit(0)

old = """    if (status) {
        av_log(avctx, AV_LOG_ERROR, "Error setting bitrate property: %d\\n", status);
        return AVERROR_EXTERNAL;
    }
"""
new = """    if (status) {
        av_log(avctx, AV_LOG_ERROR, "Error setting bitrate property: %d\\n", status);
        return AVERROR_EXTERNAL;
    }

    /* spm_ffmpeg: ExpectedFrameRate — without it the iOS HEVC encoder budgets its average bitrate
     * as if the stream were 30 fps (measured 2x overshoot at 60 fps). Advisory: failure is ignored. */
    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
        double expected_fps = av_q2d(avctx->framerate);
        CFNumberRef fps_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &expected_fps);
        if (fps_num) {
            status = VTSessionSetProperty(vtctx->session,
                                          kVTCompressionPropertyKey_ExpectedFrameRate,
                                          fps_num);
            CFRelease(fps_num);
            if (status) {
                av_log(avctx, AV_LOG_WARNING, "Error setting ExpectedFrameRate property: %d (ignored)\\n", status);
                status = 0;
            } else {
                av_log(avctx, AV_LOG_VERBOSE, "ExpectedFrameRate=%.3f\\n", expected_fps);
            }
        }
    }
"""
assert src.count(old) == 1, "bitrate error block not found exactly once"
src = src.replace(old, new)
open(path, "w", encoding="utf-8").write(src)
print("vtenc: patched (ExpectedFrameRate)")

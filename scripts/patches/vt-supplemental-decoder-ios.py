#!/usr/bin/env python3
"""spm_ffmpeg patch: VideoToolbox 보조 디코더 등록(VTRegisterSupplementalVideoDecoderIfAvailable)을 iOS 26.2+ 에서도 호출한다.

배경: iOS SDK 26.5 의 VTUtilities.h 는 이 API 를 macos(11.0), ios(26.2), tvos(26.2), visionos(26.2) 로 공개한다.
FFmpeg 9.0.1 videotoolbox.c 는 `!TARGET_OS_IPHONE` 가드로 macOS 에서만 호출해, iOS 에서는 VP9 hwaccel 세션 생성이
실패한다(실기기 로그 "Failed setup for format videotoolbox_vld: hwaccel initialisation returned error").
헤더 문구: "Supplemental video decoders registered through this API will not work in applications which have not
performed this opt in" — 즉 앱(프로세스)이 직접 등록해야 한다. 등록 뒤 VTIsHardwareDecodeAvailable 로 HW 여부 확인 가능.

멱등: 마커로 건너뛴다. 사용: python3 vt-supplemental-decoder-ios.py <ffmpeg-src>/libavcodec/videotoolbox.c
"""
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

MARK = "spm_ffmpeg: supplemental decoder on iOS 26.2+"
if MARK in src:
    print("videotoolbox.c: already patched")
    sys.exit(0)

old = """#if defined(MAC_OS_VERSION_11_0) && !TARGET_OS_IPHONE && (MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_11_0) && AV_HAS_BUILTIN(__builtin_available)
    if (__builtin_available(macOS 11.0, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(videotoolbox->cm_codec_type);
    }
#endif
"""
new = """/* spm_ffmpeg: supplemental decoder on iOS 26.2+ — the API is public there since SDK 26.2 (VTUtilities.h);
 * upstream only calls it on macOS, so VP9 hwaccel could never open a session on iOS. */
#if AV_HAS_BUILTIN(__builtin_available) && (TARGET_OS_OSX || TARGET_OS_IPHONE)
    if (__builtin_available(macOS 11.0, iOS 26.2, tvOS 26.2, visionOS 26.2, *)) {
        VTRegisterSupplementalVideoDecoderIfAvailable(videotoolbox->cm_codec_type);
        av_log(avctx, AV_LOG_VERBOSE, "spm_ffmpeg: requested supplemental VideoToolbox decoder for codec type 0x%08x\\n",
               (unsigned)videotoolbox->cm_codec_type);
    }
#endif
"""
assert src.count(old) == 1, "supplemental decoder block not found exactly once"
src = src.replace(old, new)
open(path, "w", encoding="utf-8").write(src)
print("videotoolbox.c: patched (supplemental decoder on iOS 26.2+)")

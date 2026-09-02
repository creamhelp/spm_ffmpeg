# spm_ffmpeg

FFmpeg(LGPL 트림 구성)을 iOS/macOS 앱에서 쓰기 위한 Swift Package. 우르스 쇼크(LeanVid)의 "파일앱 소스 + ffmpeg 폴백 엔진" 계획서
(`UrsusShock/docs/UrsusShock_FilesApp_FFmpeg_Plan.html`) §3 의 구현체다.

## 구조

```
Package.swift
Frameworks/FFmpegCore.xcframework      # umbrella 동적 프레임워크(ios-arm64 / ios-arm64-simulator / macos-arm64), 슬라이스당 ≈8MB
Sources/FFmpegBridge/                  # C 브리지 — libav* 헤더는 vendor/include 에서만 본다. 공개 API 는 include/ffx.h 뿐
  include/ffx.h                        #   ffx_probe / ffx_transcode / 라이브러리 정보
  probe.c · transcode.c · util.c
Sources/FFmpegSupport/                 # Swift 래퍼 — 앱이 import 하는 유일한 제품 (FFmpegProber / FFmpegTranscoder / FFmpegInfo)
Tests/FFmpegSupportTests/              # macOS 스모크(픽스처 17종) + AVFoundation 상호운용
scripts/build-ffmpeg.sh                # FFmpeg 소스 fetch → 패치 → 3플랫폼 크로스컴파일 (configure 플래그 정본)
scripts/make-xcframework.sh            # 정적 .a → 단일 동적 framework → xcframework, 헤더 동기화
scripts/make-fixtures.sh               # 테스트 픽스처 생성(호스트 ffmpeg 필요)
scripts/patches/movenc-mdta-moov.py    # mov muxer 패치: mdta 메타를 QuickTime 레이아웃(moov/meta)으로 — AVFoundation 판독용
```

## 설계 요점

- 인코드는 항상 VideoToolbox(`hevc_videotoolbox` / `h264_videotoolbox`). ffmpeg 는 AVFoundation 이 못 여는 컨테이너의 demux 와
  VT 에 디코더가 없는 코덱의 SW 디코드만 맡는다. HW 디코드(videotoolbox hwaccel)를 먼저 시도하고 실패하면 처음부터 SW 로 재실행한다.
- 오디오: mp4 호환(aac/ac3/eac3/alac)은 stream copy, 그 외(opus/vorbis/flac/dts/wma/mp3/pcm)는 AAC 재인코딩.
  mp3 는 mp4 안에서 AVFoundation 이 트랙을 무시하므로 재인코딩 대상이다(실측).
- 출력 컨테이너 mp4(hvc1) 또는 mov. 회전(display matrix)·HDR 정적 메타(mdcv/clli)·컨테이너 메타를 승계한다.
- 라이선스: `--disable-gpl --disable-nonfree`, 외부 라이브러리 0개. 동적 프레임워크로 임베드(LGPL 재링크 요건). 패치·구성은 이 레포에 공개.

## 빌드

```
scripts/build-ffmpeg.sh          # FFVER=8.0.1 기본. PLATFORMS="ios-arm64" 로 일부만 가능
scripts/make-xcframework.sh
scripts/make-fixtures.sh         # 테스트 픽스처(brew ffmpeg)
swift test                       # macOS
```

앱 연동: `project.yml` → `packages: FFmpegSupport: { path: ../../spm_ffmpeg }`. 시뮬레이터는 arm64 슬라이스만 있으므로
`EXCLUDED_ARCHS[sdk=iphonesimulator*] = x86_64`.

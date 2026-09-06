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
scripts/build-dav1d.sh                 # dav1d(AV1 SW 디코더, BSD-2-Clause) 3플랫폼 크로스컴파일 → build/deps (meson+ninja 필요, 먼저 실행)
scripts/build-ffmpeg.sh                # FFmpeg 소스 fetch → 패치 → 3플랫폼 크로스컴파일 (configure 플래그 정본, --enable-libdav1d)
scripts/make-xcframework.sh            # 정적 .a → 단일 동적 framework → xcframework, 헤더 동기화
scripts/make-fixtures.sh               # 테스트 픽스처 생성(호스트 ffmpeg 필요)
scripts/patches/movenc-mdta-moov.py    # mov muxer 패치: mdta 메타를 QuickTime 레이아웃(moov/meta)으로 — AVFoundation 판독용
scripts/patches/vtenc-expected-framerate.py  # videotoolbox 인코더 패치: ExpectedFrameRate 설정 — iOS 60fps 비트레이트 2배 초과 방지
scripts/patches/vt-supplemental-decoder-ios.py  # videotoolbox 디코더 패치: VP9 보조 디코더 등록을 iOS 26.2+ 에서도(업스트림은 macOS 만)
```

## 설계 요점

- 인코드는 항상 VideoToolbox(`hevc_videotoolbox` / `h264_videotoolbox`). ffmpeg 는 AVFoundation 이 못 여는 컨테이너의 demux 와
  VT 에 디코더가 없는 코덱의 SW 디코드만 맡는다. HW 디코드(videotoolbox hwaccel)를 먼저 시도하고 실패하면 처음부터 SW 로 재실행한다.
- 오디오: mp4 호환(aac/ac3/eac3/alac)은 stream copy, 그 외(opus/vorbis/flac/dts/wma/mp3/pcm)는 AAC 재인코딩.
  mp3 는 mp4 안에서 AVFoundation 이 트랙을 무시하므로 재인코딩 대상이다(실측).
- 출력 컨테이너 mp4(hvc1) 또는 mov. 회전(display matrix)·HDR 정적 메타(mdcv/clli)·컨테이너 메타를 승계한다.
- FFmpeg 9.0.1(2026-09 기준 최신 릴리스). 아이폰 APAC(공간 음향) 트랙은 코덱만 식별되고 디코더가 없어 제외된다(앱이 고지).
- 라이선스: `--disable-gpl --disable-nonfree`, 외부 라이브러리 0개. 동적 프레임워크로 임베드(LGPL 재링크 요건). 패치·구성은 이 레포에 공개.

## License and source offer (English)

This repository is the **corresponding source offer** for the FFmpeg build embedded in the LeanVid iOS app
(bundle `com.cream.world.leanvid`, "FFmpegCore.framework").

- **FFmpeg 9.0.1** — GNU Lesser General Public License v2.1 or later (`LICENSES/FFmpeg-COPYING.LGPLv2.1`).
  Upstream source: https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz. Built with `--disable-gpl --disable-nonfree`
  (no GPL components). The exact configure flags are in `scripts/build-ffmpeg.sh`; the only source modifications are the
  three idempotent patch scripts in `scripts/patches/` (mov muxer QuickTime‑style `mdta` metadata, VideoToolbox encoder
  `ExpectedFrameRate`, VideoToolbox VP9 supplemental decoder registration on iOS 26.2+). Apply them to the upstream
  tarball with `scripts/build-ffmpeg.sh` to reproduce the shipped binary.
- **dav1d 1.5.4** — BSD 2‑Clause (`LICENSES/dav1d-COPYING`), statically linked into the same framework as the AV1
  software decoder. Upstream source: https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/. Built unmodified by
  `scripts/build-dav1d.sh`.
- The framework is embedded as a **dynamic library**, so a user can relink the app against a modified FFmpeg
  (LGPL §6): rebuild with the scripts below and replace `Frameworks/FFmpegCore.xcframework`.
- The bridge sources in `Sources/` (C bridge and Swift wrapper) are part of the app and are published here so the
  LGPL library can be rebuilt and relinked; they are not themselves under the LGPL.

Contact: creamhelp@gmail.com

## 빌드

```
scripts/build-dav1d.sh           # dav1d 3플랫폼(meson·ninja 필요) → build/deps
scripts/build-ffmpeg.sh          # FFVER=9.0.1 기본. PLATFORMS="ios-arm64" 로 일부만 가능
scripts/make-xcframework.sh
scripts/make-fixtures.sh         # 테스트 픽스처(brew ffmpeg)
swift test                       # macOS
```

앱 연동: `project.yml` → `packages: FFmpegSupport: { path: ../../spm_ffmpeg }`. 시뮬레이터는 arm64 슬라이스만 있으므로
`EXCLUDED_ARCHS[sdk=iphonesimulator*] = x86_64`.

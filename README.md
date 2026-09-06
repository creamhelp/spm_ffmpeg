# spm_ffmpeg

A Swift Package that wraps a trimmed, LGPL-only build of FFmpeg for iOS and macOS apps. It is the
"Files-app source + FFmpeg fallback engine" of the LeanVid iOS app (project name UrsusShock), and it is also the
**corresponding source offer** for the FFmpeg binary that ships inside the app (see [License](#license-and-source-offer)).

## Layout

```
Package.swift
Frameworks/FFmpegCore.xcframework      # umbrella dynamic framework (ios-arm64 / ios-arm64-simulator / macos-arm64), ≈8 MB per slice
Sources/FFmpegBridge/                  # C bridge — the only place that sees libav* headers (vendor/include). Public API is include/ffx.h
  include/ffx.h                        #   ffx_probe / ffx_transcode / ffx_remux / ffx_thumbnail / library info
  probe.c · transcode.c · util.c
Sources/FFmpegSupport/                 # Swift wrapper — the only product an app imports (FFmpegProber / FFmpegTranscoder / FFmpegRemuxer / FFmpegInfo)
Tests/FFmpegSupportTests/              # macOS smoke tests (17 fixtures) + AVFoundation interop
scripts/build-dav1d.sh                 # dav1d (AV1 software decoder, BSD-2-Clause) cross-compiled for 3 platforms → build/deps (needs meson + ninja; run first)
scripts/build-ffmpeg.sh                # fetch FFmpeg source → apply patches → cross-compile for 3 platforms (single source of truth for configure flags, --enable-libdav1d)
scripts/make-xcframework.sh            # static .a → one dynamic framework per platform → xcframework, header sync
scripts/make-fixtures.sh               # generate test fixtures (needs a host ffmpeg)
scripts/patches/movenc-mdta-moov.py    # mov muxer: write mdta metadata in the QuickTime layout (moov/meta) so AVFoundation can read it
scripts/patches/vtenc-expected-framerate.py     # videotoolbox encoder: set ExpectedFrameRate — prevents 2× bitrate overshoot on 60 fps sources on iOS
scripts/patches/vt-supplemental-decoder-ios.py  # videotoolbox decoder: register the VP9 supplemental decoder on iOS 26.2+ too (upstream does macOS only)
```

## Design notes

- Encoding always goes through VideoToolbox (`hevc_videotoolbox` / `h264_videotoolbox`). FFmpeg is only used to demux
  containers AVFoundation cannot open and to software-decode codecs VideoToolbox has no decoder for. Hardware decode
  (videotoolbox hwaccel) is tried first; on failure the job is restarted from the beginning in software.
- Audio: MP4-compatible codecs (aac/ac3/eac3/alac) are stream-copied; everything else (opus/vorbis/flac/dts/wma/mp3/pcm)
  is re-encoded to AAC. mp3 is re-encoded because AVFoundation ignores mp3 tracks inside MP4 (measured).
- Output container is MP4 (hvc1) or MOV. Rotation (display matrix), static HDR metadata (mdcv/clli) and container
  metadata are carried over from the source.
- AV1: the native `av1` decoder with VideoToolbox hwaccel is preferred; devices without AV1 hardware fall back to
  `libdav1d` (statically linked).
- FFmpeg 9.0.1 (latest release as of 2026-09). iPhone APAC (spatial audio) tracks are identified but have no decoder
  and are dropped; the app tells the user.
- Licensing: `--disable-gpl --disable-nonfree`, no external libraries other than dav1d. The framework is embedded as a
  dynamic library so it can be relinked (LGPL requirement). Patches and configuration are published in this repository.

## Building

```
scripts/build-dav1d.sh           # dav1d for 3 platforms (needs meson + ninja) → build/deps
scripts/build-ffmpeg.sh          # FFVER=9.0.1 by default. PLATFORMS="ios-arm64" builds a subset
scripts/make-xcframework.sh
scripts/make-fixtures.sh         # test fixtures (brew ffmpeg)
swift test                       # macOS
```

Integrating into an app (XcodeGen): `project.yml` → `packages: FFmpegSupport: { path: ../../spm_ffmpeg }`. The simulator
slice is arm64 only, so set `EXCLUDED_ARCHS[sdk=iphonesimulator*] = x86_64`.

## License and source offer

This repository is the **corresponding source offer** for the FFmpeg build embedded in the LeanVid iOS app
(bundle `com.cream.world.leanvid`, "FFmpegCore.framework").

- **FFmpeg 9.0.1** — GNU Lesser General Public License v2.1 or later (`LICENSES/FFmpeg-COPYING.LGPLv2.1`).
  Upstream source: https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz. Built with `--disable-gpl --disable-nonfree`
  (no GPL components). The exact configure flags are in `scripts/build-ffmpeg.sh`; the only source modifications are the
  three idempotent patch scripts in `scripts/patches/` (mov muxer QuickTime-style `mdta` metadata, VideoToolbox encoder
  `ExpectedFrameRate`, VideoToolbox VP9 supplemental decoder registration on iOS 26.2+). Apply them to the upstream
  tarball with `scripts/build-ffmpeg.sh` to reproduce the shipped binary.
- **dav1d 1.5.4** — BSD 2-Clause (`LICENSES/dav1d-COPYING`), statically linked into the same framework as the AV1
  software decoder. Upstream source: https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/. Built unmodified by
  `scripts/build-dav1d.sh`.
- The framework is embedded as a **dynamic library**, so a user can relink the app against a modified FFmpeg
  (LGPL §6): rebuild with the scripts above and replace `Frameworks/FFmpegCore.xcframework`.
- The bridge sources in `Sources/` (C bridge and Swift wrapper) are part of the app and are published here so the
  LGPL library can be rebuilt and relinked; they are not themselves under the LGPL.

Contact: creamhelp@gmail.com

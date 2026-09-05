#!/bin/bash
# spm_ffmpeg/scripts/build-ffmpeg.sh — FFmpeg LGPL 트림 구성 크로스컴파일 (iOS 실기기 / iOS 시뮬레이터 / macOS)
#
# 산출: build/out/<platform>/{lib,include}. 이후 make-xcframework.sh 가 단일 umbrella 동적 프레임워크로 묶는다.
# 구성 원칙(계획서 §3.6): --disable-gpl --disable-nonfree (LGPL 2.1+), 외부 라이브러리 0개(zlib은 SDK 제공),
# 디코더·디먹서 중심 트림, 인코더는 VideoToolbox(HEVC/H.264) + 내장 aac 만.
#
# 사용: scripts/build-ffmpeg.sh            (기본: ios-arm64 ios-arm64-simulator macos-arm64)
#       PLATFORMS="ios-arm64" scripts/build-ffmpeg.sh
#       FFVER=9.0.1 scripts/build-ffmpeg.sh   (기본값; 다른 버전은 FFVER 로 지정)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FFVER="${FFVER:-9.0.1}"
BUILD="$ROOT/build"
SRC="$BUILD/ffmpeg-$FFVER"
LOGS="$BUILD/logs"
PLATFORMS="${PLATFORMS:-ios-arm64 ios-arm64-simulator macos-arm64}"
MIN_IOS="${MIN_IOS:-18.0}"
MIN_MACOS="${MIN_MACOS:-14.0}"
JOBS="$(sysctl -n hw.ncpu)"

mkdir -p "$BUILD" "$LOGS"

if [ ! -d "$SRC" ]; then
  echo "[build] fetching ffmpeg-$FFVER"
  curl -sSL -o "$BUILD/ffmpeg-$FFVER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz"
  tar xf "$BUILD/ffmpeg-$FFVER.tar.xz" -C "$BUILD"
fi
# 소스 패치(멱등) — LGPL 소스 오퍼에는 이 패치가 포함된다(scripts/patches/).
python3 "$ROOT/scripts/patches/movenc-mdta-moov.py" "$SRC/libavformat/movenc.c"
python3 "$ROOT/scripts/patches/vtenc-expected-framerate.py" "$SRC/libavcodec/videotoolboxenc.c"

# ---- 컴포넌트 목록 (정본) -------------------------------------------------------------
DEMUXERS="mov,matroska,avi,asf,flv,mpegts,mpegps,m4v,mpegvideo,h264,hevc,ivf,mp3,aac,wav,ogg"
VIDEO_DECODERS="h264,hevc,vp8,vp9,av1,mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,h263,h263i,h263p,flv,vc1,wmv1,wmv2,wmv3,mjpeg,prores,rawvideo,theora"
AUDIO_DECODERS="aac,aac_latm,apac,ac3,eac3,mp3,mp2,mp1,vorbis,opus,flac,wmav1,wmav2,wmapro,dca,truehd,alac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_f32le,pcm_f32be,pcm_u8,pcm_alaw,pcm_mulaw,adpcm_ima_wav,adpcm_ms"
ENCODERS="aac,h264_videotoolbox,hevc_videotoolbox,rawvideo"
MUXERS="mp4,mov,ipod,null"
PARSERS="h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,mpegaudio,aac,aac_latm,ac3,vorbis,opus,flac,mjpeg,vc1,h263,dca"
BSFS="h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,aac_adtstoasc,vp9_superframe,vp9_superframe_split,av1_frame_merge,null"
HWACCELS="h264_videotoolbox,hevc_videotoolbox,vp9_videotoolbox,av1_videotoolbox,mpeg2_videotoolbox,mpeg4_videotoolbox,prores_videotoolbox"

COMMON_FLAGS=(
  --disable-gpl --disable-nonfree
  --disable-programs --disable-doc
  --disable-avdevice --disable-avfilter
  --disable-network
  --disable-devices
  --disable-everything
  --disable-protocols --enable-protocol=file
  --enable-demuxer="$DEMUXERS"
  --enable-decoder="$VIDEO_DECODERS,$AUDIO_DECODERS"
  --enable-encoder="$ENCODERS"
  --enable-muxer="$MUXERS"
  --enable-parser="$PARSERS"
  --enable-bsf="$BSFS"
  --enable-hwaccel="$HWACCELS"
  --enable-videotoolbox
  --disable-audiotoolbox
  --enable-swscale --enable-swresample
  --enable-zlib --disable-bzlib --disable-iconv --disable-lzma
  --disable-securetransport --disable-sdl2 --disable-xlib --disable-libxcb
  --disable-coreimage --disable-appkit --disable-metal --disable-vulkan
  --enable-pic
  --disable-debug --disable-stripping
  --enable-static --disable-shared
  --enable-cross-compile
  --target-os=darwin --arch=arm64
)

build_platform() {
  local platform="$1"
  local sdk minflag
  case "$platform" in
    ios-arm64)           sdk=iphoneos;        minflag="-mios-version-min=$MIN_IOS" ;;
    ios-arm64-simulator) sdk=iphonesimulator; minflag="-mios-simulator-version-min=$MIN_IOS" ;;
    macos-arm64)         sdk=macosx;          minflag="-mmacosx-version-min=$MIN_MACOS" ;;
    *) echo "unknown platform $platform"; exit 1 ;;
  esac
  local sysroot; sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  local out="$BUILD/out/$platform"
  local objdir="$BUILD/obj/$platform"
  rm -rf "$out" "$objdir"; mkdir -p "$out" "$objdir"

  echo "[build] === $platform (sdk=$sdk) ==="
  local log="$LOGS/$platform.log"
  (
    cd "$objdir"
    "$SRC/configure" \
      --prefix="$out" \
      --cc="$(xcrun --sdk "$sdk" -f clang)" \
      --ar="$(xcrun --sdk "$sdk" -f ar)" \
      --ranlib="$(xcrun --sdk "$sdk" -f ranlib)" \
      --nm="$(xcrun --sdk "$sdk" -f nm)" \
      --strip="$(xcrun --sdk "$sdk" -f strip)" \
      --sysroot="$sysroot" \
      --extra-cflags="-arch arm64 -isysroot $sysroot $minflag -fno-common -O3" \
      --extra-ldflags="-arch arm64 -isysroot $sysroot $minflag" \
      "${COMMON_FLAGS[@]}"
    make -j"$JOBS"
    make install
  ) > "$log" 2>&1 || { echo "[build] FAILED $platform — see $log"; tail -40 "$log"; exit 1; }
  echo "[build] done $platform → $out"
  ls "$out/lib"
}

for p in $PLATFORMS; do
  build_platform "$p"
done
echo "[build] all done"

#!/bin/bash
# spm_ffmpeg/scripts/build-dav1d.sh — dav1d(AV1 소프트웨어 디코더, BSD-2-Clause) 크로스컴파일 → build/deps/<platform>
#
# 왜: LGPL 트림 FFmpeg 의 내장 av1 디코더는 hwaccel(VideoToolbox) 전용이라 AV1 하드웨어가 없는 기기(A17 Pro 이전)에서는
#     AV1 을 전혀 디코드하지 못한다. dav1d 를 정적으로 붙여 --enable-libdav1d 로 소프트웨어 경로를 연다(D-235).
# 라이선스: BSD-2-Clause — LGPL 동적 프레임워크에 정적 링크해 재배포 가능(저작권·라이선스 고지 필요, 앱 라이선스 화면).
# 도구: meson + ninja (brew install meson ninja). arm64 어셈블리는 clang 이 직접 처리(nasm 불필요).
#
# 사용: scripts/build-dav1d.sh            (기본: ios-arm64 ios-arm64-simulator macos-arm64)
#       PLATFORMS="ios-arm64" scripts/build-dav1d.sh
#       DAV1D_VER=1.5.4 scripts/build-dav1d.sh (기본값)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DAV1D_VER="${DAV1D_VER:-1.5.4}"
BUILD="$ROOT/build"
SRC="$BUILD/dav1d-$DAV1D_VER"
LOGS="$BUILD/logs"
PLATFORMS="${PLATFORMS:-ios-arm64 ios-arm64-simulator macos-arm64}"
MIN_IOS="${MIN_IOS:-18.0}"
MIN_MACOS="${MIN_MACOS:-14.0}"

mkdir -p "$BUILD" "$LOGS"
command -v meson >/dev/null 2>&1 || { echo "[dav1d] meson 필요: brew install meson ninja"; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "[dav1d] ninja 필요: brew install meson ninja"; exit 1; }

if [ ! -d "$SRC" ]; then
  echo "[dav1d] fetching dav1d-$DAV1D_VER"
  curl -sSL -o "$BUILD/dav1d-$DAV1D_VER.tar.xz" "https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D_VER/dav1d-$DAV1D_VER.tar.xz"
  tar xf "$BUILD/dav1d-$DAV1D_VER.tar.xz" -C "$BUILD"
fi

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
  local out="$BUILD/deps/$platform"
  local objdir="$BUILD/obj-dav1d/$platform"
  rm -rf "$out" "$objdir"; mkdir -p "$out" "$objdir"
  local cross="$objdir/cross.txt"
  cat > "$cross" <<EOF
[binaries]
c = '$(xcrun --sdk "$sdk" -f clang)'
ar = '$(xcrun --sdk "$sdk" -f ar)'
strip = '$(xcrun --sdk "$sdk" -f strip)'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-arch', 'arm64', '-isysroot', '$sysroot', '$minflag', '-O3', '-fno-common']
c_link_args = ['-arch', 'arm64', '-isysroot', '$sysroot', '$minflag']

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

  echo "[dav1d] === $platform (sdk=$sdk) ==="
  local log="$LOGS/dav1d-$platform.log"
  (
    meson setup "$objdir" "$SRC" --cross-file "$cross" --prefix "$out" --libdir lib \
      --buildtype release --default-library static \
      -Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_docs=false \
      -Dbitdepths=8,16 -Denable_asm=true -Dlogging=false
    ninja -C "$objdir"
    ninja -C "$objdir" install
  ) > "$log" 2>&1 || { echo "[dav1d] FAILED $platform — see $log"; tail -40 "$log"; exit 1; }
  echo "[dav1d] done $platform → $out"
  ls "$out/lib"
}

for p in $PLATFORMS; do
  build_platform "$p"
done
echo "[dav1d] all done"

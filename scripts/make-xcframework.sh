#!/bin/bash
# spm_ffmpeg/scripts/make-xcframework.sh — build/out/<platform>/lib/*.a → 단일 umbrella 동적 프레임워크 → FFmpegCore.xcframework
#
# 왜 동적(umbrella) 하나로 묶나: binaryTarget 1개로 관리가 단순하고, 동적 임베드가 LGPL 재링크 요건에 유리하다(계획서 §3.5).
# 헤더는 프레임워크에 넣지 않는다 — Sources/FFmpegBridge/vendor/include 로 복사해 C 브리지만 본다(모듈 충돌 방지).
#
# 사용: scripts/make-xcframework.sh   (build-ffmpeg.sh 이후)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
FWNAME="FFmpegCore"
FFVER="${FFVER:-9.0.1}"
PLATFORMS="${PLATFORMS:-ios-arm64 ios-arm64-simulator macos-arm64}"
MIN_IOS="${MIN_IOS:-18.0}"
MIN_MACOS="${MIN_MACOS:-14.0}"
BUNDLE_ID="com.cream.world.ffmpegcore"
LIBS="libavformat libavcodec libswscale libswresample libavutil"
FRAMEWORKS_LINK="-framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -framework Foundation"

write_plist() {
  local path="$1" platform="$2"
  local supported minkey minval
  case "$platform" in
    ios-arm64)           supported="iPhoneOS";        minkey="MinimumOSVersion";      minval="$MIN_IOS" ;;
    ios-arm64-simulator) supported="iPhoneSimulator"; minkey="MinimumOSVersion";      minval="$MIN_IOS" ;;
    macos-arm64)         supported="MacOSX";          minkey="LSMinimumSystemVersion"; minval="$MIN_MACOS" ;;
  esac
  cat > "$path" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$FWNAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>$FWNAME</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$FFVER</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$supported</string></array>
  <key>$minkey</key><string>$minval</string>
</dict>
</plist>
PLIST
}

make_framework() {
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
  local fwdir="$BUILD/frameworks/$platform/$FWNAME.framework"
  rm -rf "$fwdir"; mkdir -p "$fwdir"

  local binary plist install_name
  if [ "$platform" = "macos-arm64" ]; then
    mkdir -p "$fwdir/Versions/A/Resources"
    binary="$fwdir/Versions/A/$FWNAME"
    plist="$fwdir/Versions/A/Resources/Info.plist"
    install_name="@rpath/$FWNAME.framework/Versions/A/$FWNAME"
  else
    binary="$fwdir/$FWNAME"
    plist="$fwdir/Info.plist"
    install_name="@rpath/$FWNAME.framework/$FWNAME"
  fi

  local archives=()
  for l in $LIBS; do archives+=("$out/lib/$l.a"); done

  echo "[xcfw] linking $platform"
  # shellcheck disable=SC2086
  "$(xcrun --sdk "$sdk" -f clang)" -dynamiclib -arch arm64 -isysroot "$sysroot" $minflag \
    -install_name "$install_name" \
    -compatibility_version 1.0.0 -current_version 1.0.0 \
    -Wl,-all_load "${archives[@]}" \
    $FRAMEWORKS_LINK -lz -lm \
    -Wl,-dead_strip_dylibs \
    -o "$binary"
  "$(xcrun --sdk "$sdk" -f strip)" -x "$binary"
  write_plist "$plist" "$platform"

  if [ "$platform" = "macos-arm64" ]; then
    ln -sfn A "$fwdir/Versions/Current"
    ln -sfn "Versions/Current/$FWNAME" "$fwdir/$FWNAME"
    ln -sfn "Versions/Current/Resources" "$fwdir/Resources"
  fi
  codesign --force --sign - "$fwdir" >/dev/null 2>&1 || true
  ls -la "$binary"
}

for p in $PLATFORMS; do make_framework "$p"; done

echo "[xcfw] creating xcframework"
mkdir -p "$ROOT/Frameworks"
rm -rf "$ROOT/Frameworks/$FWNAME.xcframework"
args=()
for p in $PLATFORMS; do args+=(-framework "$BUILD/frameworks/$p/$FWNAME.framework"); done
xcodebuild -create-xcframework "${args[@]}" -output "$ROOT/Frameworks/$FWNAME.xcframework"

echo "[xcfw] syncing headers → Sources/FFmpegBridge/vendor/include"
rm -rf "$ROOT/Sources/FFmpegBridge/vendor/include"
mkdir -p "$ROOT/Sources/FFmpegBridge/vendor"
cp -R "$BUILD/out/ios-arm64/include" "$ROOT/Sources/FFmpegBridge/vendor/include"
du -sh "$ROOT/Frameworks/$FWNAME.xcframework"
echo "[xcfw] done"

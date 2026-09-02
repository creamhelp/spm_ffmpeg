// swift-tools-version:5.9
// spm_ffmpeg — FFmpeg(LGPL 트림 구성)을 iOS/macOS 앱에서 쓰기 위한 SPM 패키지.
//
// 3층 구조(계획서 §3.2):
//   FFmpegCore    : binaryTarget — 단일 umbrella 동적 xcframework(libavformat/avcodec/swscale/swresample/avutil)
//   FFmpegBridge  : C 브리지 — libav* 헤더는 여기(vendor/include)에서만 보고, 최소 C API(ffx.h)만 노출
//   FFmpegSupport : Swift 래퍼 — 앱이 import 하는 유일한 제품
import PackageDescription

let package = Package(
    name: "spm_ffmpeg",
    platforms: [.iOS("18.0"), .macOS("14.0")],
    products: [
        .library(name: "FFmpegSupport", targets: ["FFmpegSupport"]),
    ],
    targets: [
        .binaryTarget(
            name: "FFmpegCore",
            path: "Frameworks/FFmpegCore.xcframework"
        ),
        .target(
            name: "FFmpegBridge",
            dependencies: ["FFmpegCore"],
            path: "Sources/FFmpegBridge",
            exclude: ["vendor"],
            cSettings: [
                .headerSearchPath("vendor/include"),
                .define("FFX_BUILD", to: "1"),
            ],
            linkerSettings: [
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("CoreFoundation"),
                .linkedLibrary("z"),
            ]
        ),
        .target(
            name: "FFmpegSupport",
            dependencies: ["FFmpegBridge"],
            path: "Sources/FFmpegSupport"
        ),
        .testTarget(
            name: "FFmpegSupportTests",
            dependencies: ["FFmpegSupport"],
            path: "Tests/FFmpegSupportTests",
            resources: [.copy("Fixtures")]
        ),
    ]
)

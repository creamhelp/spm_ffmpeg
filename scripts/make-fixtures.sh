#!/bin/bash
# spm_ffmpeg/scripts/make-fixtures.sh — 테스트 픽스처 생성(호스트 ffmpeg 필요, brew ffmpeg 기준)
#
# 2초·소형 클립을 여러 컨테이너/코덱 조합으로 만든다. VideoToolbox/AVFoundation 이 못 여는 조합
# (mkv/avi/ts/asf/webm/flv, VP9/MPEG-2/MPEG-4 ASP/WMV) 이 핵심이고, 대조군으로 mp4/mov 도 포함한다.
# 출력: Tests/FFmpegSupportTests/Fixtures/ (앱 테스트 타깃도 같은 파일을 복사해 쓴다)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/Tests/FFmpegSupportTests/Fixtures}"
mkdir -p "$OUT"
FF="${FFMPEG_BIN:-ffmpeg}"
DUR=2
SIZE=320x240
# 움직임이 있는 합성 영상(testsrc2) + 440Hz 톤. -shortest 로 길이 고정.
VSRC=(-f lavfi -i "testsrc2=size=$SIZE:rate=30:duration=$DUR")
ASRC=(-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=$DUR")
COMMON=(-y -hide_banner -loglevel error)

gen() { echo "[fixture] $1"; }

# 1) 컨테이너만 비호환: H.264 + AAC in Matroska
gen h264_aac.mkv
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 400k -c:a aac -b:a 96k -shortest "$OUT/h264_aac.mkv"
# 2) 컨테이너만 비호환: HEVC + AC3 in Matroska (hvc1 출력 검증)
gen hevc_ac3.mkv
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libx265 -preset veryfast -pix_fmt yuv420p -b:v 300k -x265-params log-level=none -c:a ac3 -b:a 128k -shortest "$OUT/hevc_ac3.mkv"
# 3) 코덱+컨테이너 비호환: MPEG-4 ASP(Xvid 계열) + MP3 in AVI
gen mpeg4_mp3.avi
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v mpeg4 -vtag xvid -q:v 6 -c:a libmp3lame -b:a 96k -shortest "$OUT/mpeg4_mp3.avi"
# 4) MPEG-2 + MP2 in MPEG-TS
gen mpeg2_mp2.ts
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v mpeg2video -b:v 800k -c:a mp2 -b:a 128k -shortest "$OUT/mpeg2_mp2.ts"
# 5) WMV2 + WMAv2 in ASF
gen wmv2_wma.wmv
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v wmv2 -b:v 600k -c:a wmav2 -b:a 96k -shortest "$OUT/wmv2_wma.wmv"
# 6) VP9 + Opus in WebM (SW 디코드 + 오디오 AAC 재인코딩 경로)
gen vp9_opus.webm
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libvpx-vp9 -b:v 300k -deadline realtime -cpu-used 8 -c:a libopus -b:a 64k -shortest "$OUT/vp9_opus.webm"
# 7) VP8 + Vorbis in WebM
gen vp8_vorbis.webm
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libvpx -b:v 300k -deadline realtime -cpu-used 8 -ac 2 -c:a vorbis -strict -2 -b:a 64k -shortest "$OUT/vp8_vorbis.webm"
# 8) H.264 + AAC in FLV
gen h264_aac.flv
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 400k -c:a aac -b:a 96k -shortest -f flv "$OUT/h264_aac.flv"
# 9) 오디오 없음: MPEG-4 ASP in AVI
gen mpeg4_noaudio.avi
"$FF" "${COMMON[@]}" "${VSRC[@]}" -c:v mpeg4 -q:v 6 -an "$OUT/mpeg4_noaudio.avi"
# 10) 오디오 2트랙(aac + mp3) in Matroska — 다중 오디오 보존/재인코딩 혼합
gen h264_two_audio.mkv
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -f lavfi -i "sine=frequency=880:sample_rate=44100:duration=$DUR" \
  -map 0:v -map 1:a -map 2:a -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 400k \
  -c:a:0 aac -b:a:0 96k -c:a:1 libmp3lame -b:a:1 96k -metadata:s:a:0 language=eng -metadata:s:a:1 language=kor -shortest "$OUT/h264_two_audio.mkv"
# 11) HEVC 10-bit(main10) in Matroska — 비트뎁스 보존 경로
gen hevc10_aac.mkv
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libx265 -preset veryfast -pix_fmt yuv420p10le -b:v 300k -x265-params log-level=none -c:a aac -b:a 96k -shortest "$OUT/hevc10_aac.mkv"
# 12) 대조군: H.264 + AAC in MP4 (VT 가능 에셋 — ffmpeg 경로에서도 돌려본다)
gen h264_aac.mp4
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 400k -c:a aac -b:a 96k -shortest -movflags +faststart "$OUT/h264_aac.mp4"
# 13) 대조군: HEVC + AAC in MOV(hvc1)
gen hevc_aac.mov
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libx265 -preset veryfast -pix_fmt yuv420p -b:v 300k -x265-params log-level=none -tag:v hvc1 -c:a aac -b:a 96k -shortest "$OUT/hevc_aac.mov"
# 14) 회전 메타(90도, display matrix) H.264 in MP4 — 컨테이너 매트릭스 승계 검증(mkv 는 매트릭스를 못 담는다)
gen h264_rot90.mp4
"$FF" "${COMMON[@]}" -display_rotation 90 -noautorotate "${VSRC[@]}" "${ASRC[@]}" -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 400k -c:a aac -b:a 96k -shortest "$OUT/h264_rot90.mp4"
rm -f "$OUT/h264_rot90.mkv"
# 15) 60fps 소스(프레임 드롭 상한 검증) H.264 in mkv
gen h264_60fps.mkv
"$FF" "${COMMON[@]}" -f lavfi -i "testsrc2=size=$SIZE:rate=60:duration=$DUR" "${ASRC[@]}" -c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 600k -c:a aac -b:a 96k -shortest "$OUT/h264_60fps.mkv"
# 15b) VP9 in MP4 — 컨테이너는 AVFoundation 이 읽지만 코덱은 VT 디코드 불가(라우터의 isDecodable 판정 → ffmpeg)
gen vp9_aac.mp4
"$FF" "${COMMON[@]}" "${VSRC[@]}" "${ASRC[@]}" -c:v libvpx-vp9 -b:v 300k -deadline realtime -cpu-used 8 -c:a aac -b:a 96k -shortest "$OUT/vp9_aac.mp4"
# 16) 손상/비미디어 파일(스킵 판정 검증)
gen not_a_video.bin
head -c 4096 /dev/urandom > "$OUT/not_a_video.bin"
# 17) 오디오만(비디오 없음) — noVideoTrack 판정
gen audio_only.m4a
"$FF" "${COMMON[@]}" "${ASRC[@]}" -c:a aac -b:a 96k "$OUT/audio_only.m4a"

ls -la "$OUT"
echo "[fixture] done → $OUT"

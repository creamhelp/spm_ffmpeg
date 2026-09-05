// spm_ffmpeg/Sources/FFmpegBridge/include/ffx.h — 앱(Swift)에 노출하는 최소 C API
//
// libav* 헤더는 이 파일에 포함되지 않는다(vendor/include 는 브리지 구현부 전용). Swift 쪽은 여기 정의된
// 값타입/콜백만 본다. 오류는 음수 FFX_ERR_* 코드 + err 버퍼 문자열로 보고한다.
#ifndef FFX_H
#define FFX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 오류 코드 -------------------------------------------------------------------------
enum {
    FFX_OK               = 0,
    FFX_ERR_OPEN         = -1,   // 입력을 열 수 없음(컨테이너 미인식/파일 없음)
    FFX_ERR_NO_VIDEO     = -2,   // 비디오 스트림 없음
    FFX_ERR_DECODER      = -3,   // 디코더 없음/초기화 실패
    FFX_ERR_ENCODER      = -4,   // 인코더 없음/초기화 실패(VideoToolbox)
    FFX_ERR_MUXER        = -5,   // 출력 컨테이너/스트림 구성 실패
    FFX_ERR_IO           = -6,   // 출력 파일 열기/쓰기 실패
    FFX_ERR_CANCELLED    = -7,   // 콜백이 취소를 요청
    FFX_ERR_DECODE       = -8,   // 디코딩 중 오류
    FFX_ERR_ENCODE       = -9,   // 인코딩 중 오류
    FFX_ERR_ARG          = -10,  // 잘못된 인자
    FFX_ERR_HW_FALLBACK  = -11,  // (내부) HW 디코드 실패 — SW 로 재시도해야 함
};

// ---- 프로브 -----------------------------------------------------------------------------
#define FFX_MAX_AUDIO 8

typedef struct ffx_audio_info {
    char    codec[32];        // "aac", "opus", ...
    int     channels;
    int     sample_rate;
    int64_t bit_rate;         // 0 = 미상
    int     decodable;        // 디코더 존재 여부
    int     mp4_compatible;   // mp4/mov 로 stream copy 가능한 코덱인지
    char    language[8];
} ffx_audio_info;

typedef struct ffx_media_info {
    char    container[64];    // avformat 이름("matroska,webm" 등)
    char    video_codec[32];  // "h264","hevc","vp9",... (비디오 없으면 "")
    char    video_profile[48];
    int     width;
    int     height;
    int     rotation;         // 표시 회전(도), 0/90/180/270
    double  frame_rate;       // 평균 fps(0 = 미상)
    double  duration;         // 초
    int64_t video_bit_rate;   // 0 = 미상
    int64_t total_bit_rate;   // 컨테이너 보고값(0 = 미상)
    int     bit_depth;        // 8/10/12 (0 = 미상)
    int     pix_fmt_is_hw_friendly; // nv12/yuv420p/p010 계열이면 1
    char    color_primaries[24];
    char    color_transfer[24];
    char    color_space[24];
    int     has_video;
    int     video_decodable;  // 디코더 존재
    int     audio_count;
    ffx_audio_info audio[FFX_MAX_AUDIO];
    int     other_stream_count;   // 자막/데이터 등 (출력에서 제외됨)
    int     hw_decode_supported;  // videotoolbox hwaccel 후보(코덱 기준, 런타임 성공 보장 아님)
} ffx_media_info;

/// 입력을 열고 스트림 정보를 채운다. 실패 시 음수 코드 + err.
int ffx_probe(const char *path, ffx_media_info *out, char *err, size_t errlen);

// ---- 트랜스코드 --------------------------------------------------------------------------
typedef enum ffx_audio_mode {
    FFX_AUDIO_COPY_OR_AAC = 0,   // mp4 호환 코덱은 copy, 아니면 AAC 재인코딩(계획 D2)
    FFX_AUDIO_ALWAYS_AAC  = 1,
    FFX_AUDIO_COPY_OR_DROP = 2,  // 호환 아니면 트랙 제외
} ffx_audio_mode;

typedef enum ffx_container {
    FFX_CONTAINER_MP4 = 0,
    FFX_CONTAINER_MOV = 1,
} ffx_container;

typedef struct ffx_transcode_options {
    int64_t video_bit_rate;      // 목표 평균 비트레이트(bps) — 필수
    int     prefer_hw_decode;    // 1: videotoolbox hwaccel 우선(실패 시 SW 자동 폴백)
    int     allow_sw_encode;     // 1: HW 인코더 없을 때 SW(VT allow_sw) 허용
    int     audio_mode;          // ffx_audio_mode
    int     aac_bit_rate;        // 재인코딩 시 (0 = 128000)
    int     container;           // ffx_container
    int     max_long_edge;       // 0 = 원본 유지, 아니면 긴 변 상한(다운스케일)
    double  max_frame_rate;      // 0 = 원본 유지, 아니면 프레임 드롭 상한
    int     gop_seconds;         // 키프레임 간격(초, 0 = 2)
    int     prioritize_speed;    // 1: B-프레임 억제
    int     force_hevc;          // 1: 항상 HEVC(기본), 0: 원본이 h264면 h264
    const char *const *metadata_keys;   // 출력 컨테이너 메타(키/값 쌍 배열, NULL 종료 가능)
    const char *const *metadata_values;
    int     metadata_count;
    int     log_level;           // av_log 레벨(-8 quiet ... 32 info), 0 = 기본(error)
} ffx_transcode_options;

typedef struct ffx_transcode_stats {
    int64_t frames_decoded;
    int64_t frames_encoded;
    int64_t frames_dropped;      // max_frame_rate 로 버린 수
    int     hw_decode_used;
    int     hw_encode_used;      // VideoToolbox 인코더 사용(항상 1 — allow_sw 로 SW 로 떨어지면 0)
    int     audio_streams_in;
    int     audio_streams_out;
    int     audio_reencoded;     // 재인코딩된 오디오 스트림 수
    int     audio_dropped;       // 제외된 오디오 스트림 수
    int     out_width;
    int     out_height;
    int     out_bit_depth;
    char    video_encoder[32];
    double  elapsed_seconds;
    double  out_duration;
    int64_t out_bytes;           // 성공 시 출력 파일 크기(바이트) — 실측 비트레이트 = out_bytes*8/out_duration
} ffx_transcode_stats;

/// 진행 콜백. fraction 0..1. 0이 아닌 값을 반환하면 취소.
typedef int (*ffx_progress_cb)(void *ctx, double fraction);

/// 트랜스코드 실행(동기 — 호출자가 백그라운드 스레드에서 돌린다). 성공 0, 실패 음수 코드 + err.
/// 실패/취소 시 출력 파일은 삭제한다.
int ffx_transcode(const char *in_path, const char *out_path,
                  const ffx_transcode_options *opts,
                  ffx_progress_cb progress, void *ctx,
                  ffx_transcode_stats *stats,
                  char *err, size_t errlen);

// ---- 썸네일 ----------------------------------------------------------------------------------
/// 대표 프레임 1장을 RGBA(8bpc, 프리멀티플라이 아님) 로 돌려준다. out_rgba 는 ffx_free 로 해제.
/// seconds: 목표 시각(0 이면 첫 프레임), max_edge: 긴 변 상한(0 = 원본 크기), out_rotation: 표시 회전(도).
int ffx_thumbnail(const char *path, double seconds, int max_edge,
                  uint8_t **out_rgba, int *out_w, int *out_h, int *out_rotation,
                  char *err, size_t errlen);
void ffx_free(void *p);

// ---- 기타 --------------------------------------------------------------------------------
const char *ffx_version(void);              // "8.0.1"
const char *ffx_configuration(void);        // configure 플래그 문자열
const char *ffx_license(void);              // "LGPL version 2.1 or later"
int ffx_has_decoder(const char *name);
int ffx_has_encoder(const char *name);
void ffx_set_log_level(int level);
/// 코덱 이름이 mp4/mov 컨테이너에 stream copy 가능한 오디오인지(정책 정본).
int ffx_audio_codec_mp4_compatible(const char *codec_name);

#ifdef __cplusplus
}
#endif
#endif

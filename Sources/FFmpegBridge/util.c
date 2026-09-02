// spm_ffmpeg/Sources/FFmpegBridge/util.c — 버전/구성/로그/코덱 조회 + 공용 헬퍼
#include "ffx_internal.h"
#include <pthread.h>

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static int g_log_level = AV_LOG_ERROR;

static void init_once(void) {
    av_log_set_level(g_log_level);
}

void ffx_ensure_init(void) {
    pthread_once(&g_init_once, init_once);
}

void ffx_set_log_level(int level) {
    g_log_level = level;
    av_log_set_level(level);
}

void ffx_set_err(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

const char *ffx_averr(int averr, char *buf, size_t buflen) {
    if (av_strerror(averr, buf, buflen) < 0) snprintf(buf, buflen, "averror %d", averr);
    return buf;
}

double ffx_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int ffx_codec_hw_decodable(enum AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_PRORES:
        return 1;
    default:
        return 0;
    }
}

const char *ffx_version(void) { return FFMPEG_VERSION; }
const char *ffx_configuration(void) { return avformat_configuration(); }
const char *ffx_license(void) { return avformat_license(); }

int ffx_has_decoder(const char *name) {
    ffx_ensure_init();
    return name && avcodec_find_decoder_by_name(name) != NULL;
}

int ffx_has_encoder(const char *name) {
    ffx_ensure_init();
    return name && avcodec_find_encoder_by_name(name) != NULL;
}

/// mp4/mov 컨테이너에 stream copy 해도 Apple 재생기가 읽는 오디오 코덱(정책 정본, 계획 D2).
/// opus/vorbis/flac/dts/wma/pcm/mp3 는 AAC 재인코딩 대상(mp3-in-mp4 는 AVFoundation 이 트랙을 무시한다 — 실측).
int ffx_audio_codec_mp4_compatible(const char *codec_name) {
    if (!codec_name) return 0;
    static const char *const ok[] = { "aac", "ac3", "eac3", "alac", NULL };
    for (int i = 0; ok[i]; i++) if (strcmp(ok[i], codec_name) == 0) return 1;
    return 0;
}

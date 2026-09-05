// spm_ffmpeg/Sources/FFmpegBridge/ffx_internal.h — 브리지 구현부 공용(libav* 헤더는 여기서만 포함)
#ifndef FFX_INTERNAL_H
#define FFX_INTERNAL_H

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/ffversion.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#include "ffx.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/// err 버퍼에 printf 형식으로 기록(err == NULL 허용).
void ffx_set_err(char *err, size_t errlen, const char *fmt, ...);
/// AVERROR → 문자열(내부 버퍼).
const char *ffx_averr(int averr, char *buf, size_t buflen);
/// 라이브러리 1회 초기화(로그 레벨 등). 여러 번 호출해도 안전.
void ffx_ensure_init(void);
/// 코덱 id 가 videotoolbox hwaccel 대상인지.
int ffx_codec_hw_decodable(enum AVCodecID id);
/// 벽시계 초.
double ffx_now(void);
/// err 기록 + 코드 반환(단순 경로용).
int fail_simple(char *err, size_t errlen, int code, const char *msg);

#endif

// spm_ffmpeg/Sources/FFmpegBridge/thumbnail.c — 대표 프레임 1장 추출(RGBA) — AVFoundation 이 못 여는 파일의 썸네일용
//
// SW 디코드만 쓴다(썸네일 1장에 HW 세션을 여는 비용이 더 크다). 지정 시각으로 키프레임 역방향 seek 후 첫 프레임을 취한다.
// 회전은 픽셀에 적용하지 않고 rotation 으로 돌려준다(호출자가 이미지 방향으로 처리).
#include "ffx_internal.h"
#include <stdlib.h>

void ffx_free(void *p) { free(p); }

int ffx_thumbnail(const char *path, double seconds, int max_edge,
                  uint8_t **out_rgba, int *out_w, int *out_h, int *out_rotation,
                  char *err, size_t errlen) {
    if (!path || !out_rgba || !out_w || !out_h) { ffx_set_err(err, errlen, "invalid argument"); return FFX_ERR_ARG; }
    ffx_ensure_init();
    *out_rgba = NULL; *out_w = 0; *out_h = 0;
    if (out_rotation) *out_rotation = 0;
    char ebuf[128];
    int rc = FFX_OK;

    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    struct SwsContext *sws = NULL;
    uint8_t *rgba = NULL;
    if (!frame || !pkt) { rc = fail_simple(err, errlen, FFX_ERR_DECODER, "alloc failed"); goto done; }

    int r = avformat_open_input(&fmt, path, NULL, NULL);
    if (r < 0) { rc = FFX_ERR_OPEN; ffx_set_err(err, errlen, "open: %s", ffx_averr(r, ebuf, sizeof ebuf)); goto done; }
    r = avformat_find_stream_info(fmt, NULL);
    if (r < 0) { rc = FFX_ERR_OPEN; ffx_set_err(err, errlen, "stream info: %s", ffx_averr(r, ebuf, sizeof ebuf)); goto done; }
    const AVCodec *codec = NULL;
    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vidx < 0 || !codec) { rc = FFX_ERR_NO_VIDEO; ffx_set_err(err, errlen, "no decodable video stream"); goto done; }
    AVStream *st = fmt->streams[vidx];
    // 썸네일은 SW 전용 — AV1 은 hwaccel 전용 내장 디코더가 아니라 libdav1d 를 써야 한다(D-235).
    const AVCodec *sw_codec = ffx_pick_video_decoder(st->codecpar->codec_id, 0);
    if (sw_codec) codec = sw_codec;

    dec = avcodec_alloc_context3(codec);
    if (!dec) { rc = FFX_ERR_DECODER; ffx_set_err(err, errlen, "decoder alloc"); goto done; }
    r = avcodec_parameters_to_context(dec, st->codecpar);
    if (r < 0) { rc = FFX_ERR_DECODER; ffx_set_err(err, errlen, "parameters: %s", ffx_averr(r, ebuf, sizeof ebuf)); goto done; }
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 2;
    dec->skip_frame = AVDISCARD_NONREF;   // 썸네일엔 참조 프레임만으로 충분 — 빠르게
    r = avcodec_open2(dec, codec, NULL);
    if (r < 0) { rc = FFX_ERR_DECODER; ffx_set_err(err, errlen, "decoder open: %s", ffx_averr(r, ebuf, sizeof ebuf)); goto done; }

    // 지정 시각으로 seek(키프레임 역방향). 실패해도 처음부터 디코드.
    if (seconds > 0) {
        int64_t ts = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
        if (av_seek_frame(fmt, vidx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(fmt, vidx, 0, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(dec);
    }
    int64_t target = (seconds > 0) ? av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base) : AV_NOPTS_VALUE;

    int got = 0;
    int packets = 0;
    while (!got) {
        r = av_read_frame(fmt, pkt);
        if (r < 0) {
            avcodec_send_packet(dec, NULL);   // 플러시
        } else {
            if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
            packets++;
            avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        }
        for (;;) {
            int rr = avcodec_receive_frame(dec, frame);
            if (rr == AVERROR(EAGAIN)) break;
            if (rr == AVERROR_EOF) { got = -1; break; }
            if (rr < 0) { got = -1; break; }
            int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
            // 목표 시각 이전 프레임은 버리되, 너무 오래 걸리지 않도록 300패킷 넘으면 현재 프레임 채택
            if (target != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts < target && packets < 300) {
                av_frame_unref(frame);
                continue;
            }
            got = 1;
            break;
        }
        if (r < 0 && !got) { got = -1; }   // EOF 이고 프레임도 없음
    }
    if (got != 1) { rc = FFX_ERR_DECODE; ffx_set_err(err, errlen, "no frame decoded"); goto done; }

    // 출력 크기(긴 변 상한, 짝수 불필요)
    int w = frame->width, h = frame->height;
    if (max_edge > 0) {
        int longEdge = w > h ? w : h;
        if (longEdge > max_edge) {
            double s = (double)max_edge / (double)longEdge;
            w = (int)(w * s + 0.5); h = (int)(h * s + 0.5);
            if (w < 1) w = 1; if (h < 1) h = 1;
        }
    }
    sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                         w, h, AV_PIX_FMT_RGBA, SWS_AREA, NULL, NULL, NULL);
    if (!sws) { rc = FFX_ERR_DECODE; ffx_set_err(err, errlen, "sws_getContext"); goto done; }
    rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) { rc = FFX_ERR_DECODE; ffx_set_err(err, errlen, "malloc"); goto done; }
    uint8_t *dst[4] = { rgba, NULL, NULL, NULL };
    int dstStride[4] = { w * 4, 0, 0, 0 };
    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst, dstStride);

    *out_rgba = rgba; rgba = NULL;
    *out_w = w; *out_h = h;
    if (out_rotation) {
        const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                             st->codecpar->nb_coded_side_data,
                                                             AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * sizeof(int32_t)) {
            double theta = av_display_rotation_get((const int32_t *)sd->data);
            if (theta == theta) {
                while (theta < 0) theta += 360;
                while (theta >= 360) theta -= 360;
                *out_rotation = ((int)((theta + 45) / 90) * 90) % 360;
            }
        }
    }

done:
    if (rgba) free(rgba);
    if (sws) sws_freeContext(sws);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    return rc;
}

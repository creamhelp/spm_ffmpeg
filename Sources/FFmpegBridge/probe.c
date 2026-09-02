// spm_ffmpeg/Sources/FFmpegBridge/probe.c — 컨테이너/스트림/코덱 프로브 (라우팅 판정 근거)
#include "ffx_internal.h"

static void copy_str(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

static int stream_rotation(const AVStream *st) {
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * sizeof(int32_t)) return 0;
    double theta = av_display_rotation_get((const int32_t *)sd->data);
    if (theta != theta) return 0;   // NaN
    // ffprobe 와 같은 부호 규약(av_display_rotation_get 그대로) — 0..360 정규화 후 90도 단위 반올림
    while (theta < 0) theta += 360;
    while (theta >= 360) theta -= 360;
    int r = (int)((theta + 45) / 90) * 90;
    return r % 360;
}

int ffx_probe(const char *path, ffx_media_info *out, char *err, size_t errlen) {
    if (!path || !out) { ffx_set_err(err, errlen, "invalid argument"); return FFX_ERR_ARG; }
    ffx_ensure_init();
    memset(out, 0, sizeof(*out));
    char ebuf[128];

    AVFormatContext *fmt = NULL;
    int rc = avformat_open_input(&fmt, path, NULL, NULL);
    if (rc < 0) {
        ffx_set_err(err, errlen, "open failed: %s", ffx_averr(rc, ebuf, sizeof ebuf));
        return FFX_ERR_OPEN;
    }
    rc = avformat_find_stream_info(fmt, NULL);
    if (rc < 0) {
        ffx_set_err(err, errlen, "stream info failed: %s", ffx_averr(rc, ebuf, sizeof ebuf));
        avformat_close_input(&fmt);
        return FFX_ERR_OPEN;
    }

    copy_str(out->container, sizeof out->container, fmt->iformat ? fmt->iformat->name : "");
    out->duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    out->total_bit_rate = fmt->bit_rate > 0 ? fmt->bit_rate : 0;

    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx >= 0) {
        AVStream *st = fmt->streams[vidx];
        AVCodecParameters *par = st->codecpar;
        // 커버아트(첨부 그림)는 비디오로 치지 않는다.
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            out->has_video = 1;
            const char *cname = avcodec_get_name(par->codec_id);
            copy_str(out->video_codec, sizeof out->video_codec, cname);
            const char *pname = avcodec_profile_name(par->codec_id, par->profile);
            copy_str(out->video_profile, sizeof out->video_profile, pname ? pname : "");
            out->width = par->width;
            out->height = par->height;
            out->rotation = stream_rotation(st);
            AVRational fr = av_guess_frame_rate(fmt, st, NULL);
            out->frame_rate = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0;
            out->video_bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((enum AVPixelFormat)par->format);
            if (desc) {
                out->bit_depth = desc->comp[0].depth;
                enum AVPixelFormat pf = (enum AVPixelFormat)par->format;
                out->pix_fmt_is_hw_friendly = (pf == AV_PIX_FMT_NV12 || pf == AV_PIX_FMT_YUV420P ||
                                               pf == AV_PIX_FMT_P010LE || pf == AV_PIX_FMT_YUV420P10LE ||
                                               pf == AV_PIX_FMT_YUVJ420P);
            }
            copy_str(out->color_primaries, sizeof out->color_primaries,
                     av_color_primaries_name(par->color_primaries));
            copy_str(out->color_transfer, sizeof out->color_transfer,
                     av_color_transfer_name(par->color_trc));
            copy_str(out->color_space, sizeof out->color_space,
                     av_color_space_name(par->color_space));
            out->video_decodable = avcodec_find_decoder(par->codec_id) != NULL;
            out->hw_decode_supported = ffx_codec_hw_decodable(par->codec_id);
            // 스트림 duration 이 컨테이너보다 신뢰될 때(mkv 등) 보정
            if (out->duration <= 0 && st->duration > 0)
                out->duration = st->duration * av_q2d(st->time_base);
        }
    }

    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        AVCodecParameters *par = st->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (out->audio_count >= FFX_MAX_AUDIO) continue;
            ffx_audio_info *a = &out->audio[out->audio_count++];
            const char *cname = avcodec_get_name(par->codec_id);
            copy_str(a->codec, sizeof a->codec, cname);
            a->channels = par->ch_layout.nb_channels;
            a->sample_rate = par->sample_rate;
            a->bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
            a->decodable = avcodec_find_decoder(par->codec_id) != NULL;
            a->mp4_compatible = ffx_audio_codec_mp4_compatible(cname);
            AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
            copy_str(a->language, sizeof a->language, lang ? lang->value : "");
        } else if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if ((int)i != vidx) out->other_stream_count++;   // 부가 비디오(커버아트 등)
        } else {
            out->other_stream_count++;
        }
    }

    avformat_close_input(&fmt);
    if (!out->has_video) {
        ffx_set_err(err, errlen, "no video stream");
        return FFX_ERR_NO_VIDEO;
    }
    return FFX_OK;
}

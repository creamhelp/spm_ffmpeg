// spm_ffmpeg/Sources/FFmpegBridge/transcode.c — demux → (VT hwaccel | SW) decode → VideoToolbox encode → mp4/mov mux
//
// 설계(계획서 §4.1/§4.3): 인코드는 항상 VideoToolbox(hevc_videotoolbox / h264_videotoolbox). ffmpeg 는
// AVFoundation 이 못 여는 컨테이너의 demux 와 VT 에 디코더가 없는 코덱의 SW 디코드만 맡는다.
// HW 디코드는 '시도 후 실패하면 처음부터 SW 로 재실행'(FFX_ERR_HW_FALLBACK) — 중간 상태를 이어 붙이지 않는다.
// 오디오: mp4 호환 코덱은 stream copy, 아니면 AAC 재인코딩(D2). 자막/데이터 스트림은 제외.
#include "ffx_internal.h"
#include <unistd.h>
#include <sys/stat.h>

typedef struct audio_ctx {
    int in_index;
    AVStream *in_st;
    AVStream *out_st;
    int copy;
    AVBSFContext *bsf;
    AVCodecContext *dec;
    AVCodecContext *enc;
    SwrContext *swr;
    AVAudioFifo *fifo;
    AVFrame *dec_frame;
    AVFrame *enc_frame;
    uint8_t **cvt_data;
    int cvt_linesize;
    int cvt_capacity;
    int64_t next_pts;        // 인코더 pts(샘플 단위)
    int pts_initialized;
    int64_t last_copy_dts;   // stream copy 단조성 보정(출력 타임베이스)
    int have_last_copy_dts;
} audio_ctx;

typedef struct tx {
    const ffx_transcode_options *opts;
    int prefer_hw;
    AVFormatContext *ifmt;
    AVFormatContext *ofmt;
    int vidx;
    AVStream *vin;
    AVStream *vout;
    AVCodecContext *vdec;
    AVCodecContext *venc;
    AVBufferRef *hwdev;
    int hw_active;
    int hw_failed;
    AVFrame *dec_frame;
    AVFrame *sw_frame;
    AVFrame *cvt_frame;
    struct SwsContext *sws;
    enum AVPixelFormat enc_pix_fmt;
    int enc_w, enc_h;
    int bit_depth;
    AVRational enc_tb;
    int64_t dts_safety;
    int64_t last_dts;
    int have_last_dts;
    AVPacket *pkt;
    AVPacket *enc_pkt;
    audio_ctx audio[FFX_MAX_AUDIO];
    int naudio;
    int header_written;
    int64_t frames_decoded, frames_encoded, frames_dropped;
    AVRational frame_rate;
    int64_t frame_duration_tb;   // 프레임 1개 길이(vin time_base)
    int64_t synth_pts;           // pts 결측 시 합성
    double min_frame_gap;        // max_frame_rate 드롭 간격(초)
    double last_kept_sec;
    int have_last_kept;
    double duration_sec;
    int64_t start_pts;
    int start_pts_set;
    int64_t start_offset_us;     // 컨테이너 시작 시각(AV_TIME_BASE) — 모든 스트림에서 공통으로 뺀다(TS 1.4s 등)
    double last_sec;
    ffx_progress_cb cb;
    void *cb_ctx;
    double last_report_time;
    double last_report_fraction;
    int cancelled;
    ffx_transcode_stats *stats;
    char *err;
    size_t errlen;
    char ebuf[128];
} tx;

// ------------------------------------------------------------------------------------------
// 헬퍼
// ------------------------------------------------------------------------------------------
static int fail(tx *t, int code, const char *fmt, ...) {
    if (t->err && t->errlen) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(t->err, t->errlen, fmt, ap);
        va_end(ap);
    }
    return code;
}

static int report_progress(tx *t, double sec) {
    if (!t->cb) return 0;
    double fraction = 0;
    if (t->duration_sec > 0) fraction = sec / t->duration_sec;
    if (fraction < 0) fraction = 0;
    if (fraction > 0.999) fraction = 0.999;
    double now = ffx_now();
    if (fraction - t->last_report_fraction < 0.005 && now - t->last_report_time < 0.25) return 0;
    t->last_report_time = now;
    t->last_report_fraction = fraction;
    if (t->cb(t->cb_ctx, fraction) != 0) { t->cancelled = 1; return 1; }
    return 0;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    tx *t = (tx *)ctx->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
    }
    t->hw_failed = 1;
    return fmts[0];
}

static void even_dims(int *w, int *h) {
    if (*w & 1) (*w)--;
    if (*h & 1) (*h)--;
    if (*w < 2) *w = 2;
    if (*h < 2) *h = 2;
}

static void copy_side_data(AVCodecParameters *dst, const AVCodecParameters *src, enum AVPacketSideDataType type) {
    const AVPacketSideData *sd = av_packet_side_data_get(src->coded_side_data, src->nb_coded_side_data, type);
    if (!sd) return;
    AVPacketSideData *n = av_packet_side_data_new(&dst->coded_side_data, &dst->nb_coded_side_data, type, sd->size, 0);
    if (n) memcpy(n->data, sd->data, sd->size);
}

// ------------------------------------------------------------------------------------------
// 비디오 디코더/인코더 구성
// ------------------------------------------------------------------------------------------
static int open_video_decoder(tx *t) {
    AVCodecParameters *par = t->vin->codecpar;
    // AV1(D-235): HW 선호면 내장 av1(hwaccel 전용) → 실패 시 SW 재실행에서 libdav1d. 그 외 코덱은 기본 디코더.
    const AVCodec *codec = ffx_pick_video_decoder(par->codec_id, t->prefer_hw);
    if (!codec) return fail(t, FFX_ERR_DECODER, "no decoder for %s", avcodec_get_name(par->codec_id));
    t->vdec = avcodec_alloc_context3(codec);
    if (!t->vdec) return fail(t, FFX_ERR_DECODER, "decoder alloc failed");
    int rc = avcodec_parameters_to_context(t->vdec, par);
    if (rc < 0) return fail(t, FFX_ERR_DECODER, "parameters_to_context: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    t->vdec->pkt_timebase = t->vin->time_base;
    t->vdec->opaque = t;
    t->vdec->thread_count = 0;
    t->vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (t->prefer_hw && ffx_codec_hw_decodable(par->codec_id)) {
        int supported = 0;
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if (cfg->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) { supported = 1; break; }
        }
        if (supported && av_hwdevice_ctx_create(&t->hwdev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0) == 0) {
            t->vdec->hw_device_ctx = av_buffer_ref(t->hwdev);
            t->vdec->get_format = get_hw_format;
            t->hw_active = 1;
        }
    }
    rc = avcodec_open2(t->vdec, codec, NULL);
    if (rc < 0) {
        if (t->hw_active) return FFX_ERR_HW_FALLBACK;
        return fail(t, FFX_ERR_DECODER, "decoder open failed: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    }
    return 0;
}

static int open_video_encoder(tx *t) {
    const ffx_transcode_options *o = t->opts;
    AVCodecParameters *par = t->vin->codecpar;

    // 비트뎁스(소스 픽셀 포맷 기준). 미상이면 8.
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(t->vdec->pix_fmt != AV_PIX_FMT_NONE
                                                          ? t->vdec->pix_fmt : (enum AVPixelFormat)par->format);
    t->bit_depth = (desc && desc->comp[0].depth > 8) ? 10 : 8;

    // 출력 크기(다운스케일 옵션, 업스케일 없음). 회전은 컨테이너 매트릭스로 승계하므로 픽셀 크기는 그대로.
    int w = par->width, h = par->height;
    if (w <= 0 || h <= 0) return fail(t, FFX_ERR_ENCODER, "invalid source dimensions %dx%d", w, h);
    if (o->max_long_edge > 0) {
        int longEdge = w > h ? w : h;
        if (longEdge > o->max_long_edge) {
            double s = (double)o->max_long_edge / (double)longEdge;
            w = (int)(w * s + 0.5);
            h = (int)(h * s + 0.5);
        }
    }
    even_dims(&w, &h);
    t->enc_w = w; t->enc_h = h;

    int scaling = (w != par->width || h != par->height);
    if (t->hw_active && !scaling) {
        t->enc_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;     // 제로카피
    } else {
        t->enc_pix_fmt = t->bit_depth > 8 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    }

    int use_hevc = 1;
    if (!o->force_hevc && par->codec_id == AV_CODEC_ID_H264) use_hevc = 0;
    const char *enc_name = use_hevc ? "hevc_videotoolbox" : "h264_videotoolbox";
    const AVCodec *codec = avcodec_find_encoder_by_name(enc_name);
    if (!codec) return fail(t, FFX_ERR_ENCODER, "encoder %s unavailable", enc_name);

    t->venc = avcodec_alloc_context3(codec);
    if (!t->venc) return fail(t, FFX_ERR_ENCODER, "encoder alloc failed");
    AVCodecContext *e = t->venc;
    e->width = w;
    e->height = h;
    e->pix_fmt = t->enc_pix_fmt;
    // 인코더 타임베이스는 90kHz 로 정규화한다(입력이 ms 단위(mkv)면 반올림 격차가 VT 의 dts 산정을 흔든다).
    // 입력 타임베이스가 그보다 촘촘하면 그대로 쓴다.
    e->time_base = (t->vin->time_base.den > 90000 && t->vin->time_base.num == 1)
                   ? t->vin->time_base : (AVRational){1, 90000};
    t->enc_tb = e->time_base;
    e->framerate = t->frame_rate;
    e->bit_rate = o->video_bit_rate;
    // 초당 데이터 상한(kVTCompressionPropertyKey_DataRateLimits, 1초 창) — 평균의 1.5배. VBR 피크는 허용하되
    // 레이트 컨트롤이 목표를 크게 벗어나(실측: iOS 60fps 에서 2배) 결과가 원본보다 커지는 것을 막는 안전망.
    e->rc_max_rate = o->video_bit_rate + o->video_bit_rate / 2;
    double fps = (t->frame_rate.num > 0 && t->frame_rate.den > 0) ? av_q2d(t->frame_rate) : 30.0;
    if (o->max_frame_rate > 0 && fps > o->max_frame_rate) fps = o->max_frame_rate;
    int gop_s = o->gop_seconds > 0 ? o->gop_seconds : 2;
    e->gop_size = (int)(fps * gop_s + 0.5);
    if (e->gop_size < 1) e->gop_size = 1;
    e->max_b_frames = o->prioritize_speed ? 0 : 2;
    e->color_primaries = par->color_primaries;
    e->color_trc = par->color_trc;
    e->colorspace = par->color_space;
    e->color_range = par->color_range;
    e->chroma_sample_location = par->chroma_location;
    e->sample_aspect_ratio = par->sample_aspect_ratio.num > 0 ? par->sample_aspect_ratio : (AVRational){1, 1};
    if (t->ofmt->oformat->flags & AVFMT_GLOBALHEADER) e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (use_hevc) {
        av_opt_set(e->priv_data, "profile", t->bit_depth > 8 ? "main10" : "main", 0);
    } else {
        av_opt_set(e->priv_data, "profile", "high", 0);
    }
    av_opt_set_int(e->priv_data, "allow_sw", o->allow_sw_encode ? 1 : 0, 0);
    av_opt_set_int(e->priv_data, "realtime", 0, 0);

    int rc = avcodec_open2(e, codec, NULL);
    if (rc < 0) return fail(t, FFX_ERR_ENCODER, "encoder %s open failed: %s", enc_name, ffx_averr(rc, t->ebuf, sizeof t->ebuf));

    // dts 안전 오프셋: VideoToolbox 인코더는 B-프레임 재정렬 지연을 '첫 두 프레임 간격'으로 추정하는데,
    // HEVC 는 2프레임 재정렬이 가능하고 입력 타임스탬프 반올림까지 겹치면 pts < dts 패킷이 나온다(mp4 muxer 거부).
    // 모든 비디오 패킷 dts 를 일정 값(재정렬 여유 3프레임)만큼 앞당기면 단조성은 유지되고 pts ≥ dts 가 보장된다.
    // 음수 시작 dts 는 mp4 muxer 의 avoid_negative_ts(auto) 가 edit list 로 정확히 흡수한다.
    if (e->max_b_frames > 0) {
        int64_t frame_dur = av_rescale_q(1, av_inv_q(t->frame_rate), e->time_base);
        t->dts_safety = frame_dur * (e->max_b_frames + 1);
    }

    t->vout = avformat_new_stream(t->ofmt, NULL);
    if (!t->vout) return fail(t, FFX_ERR_MUXER, "new video stream failed");
    rc = avcodec_parameters_from_context(t->vout->codecpar, e);
    if (rc < 0) return fail(t, FFX_ERR_MUXER, "parameters_from_context: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    t->vout->time_base = e->time_base;
    t->vout->avg_frame_rate = t->frame_rate;
    t->vout->r_frame_rate = t->frame_rate;
    if (use_hevc) t->vout->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    // 회전/HDR 정적 메타는 컨테이너로 승계(tkhd matrix, mdcv/clli).
    copy_side_data(t->vout->codecpar, par, AV_PKT_DATA_DISPLAYMATRIX);
    copy_side_data(t->vout->codecpar, par, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    copy_side_data(t->vout->codecpar, par, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    av_dict_copy(&t->vout->metadata, t->vin->metadata, 0);
    av_dict_set(&t->vout->metadata, "handler_name", NULL, 0);

    if (t->stats) {
        strncpy(t->stats->video_encoder, enc_name, sizeof t->stats->video_encoder - 1);
        t->stats->out_width = w;
        t->stats->out_height = h;
        t->stats->out_bit_depth = t->bit_depth;
        t->stats->hw_encode_used = 1;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// 비디오 스트림 복사(리먹스, D-234) — WebM/MKV 안의 VP9/AV1/H.264/HEVC 를 디코드 없이 MP4 로 옮긴다.
// AVFoundation 이 컨테이너를 못 열어 비교재생이 막힌 원본을, 코덱은 그대로 두고 컨테이너만 바꿔 AVPlayer 에 넘기기 위함
// (VP9 는 앱이 VideoToolbox 보조 디코더를 등록한 뒤에만 재생된다). mp4 muxer 가 필요한 bsf(vp9_superframe 등)는
// av_interleaved_write_frame 이 check_bitstream 으로 스스로 끼운다.
// ------------------------------------------------------------------------------------------
static int write_packet(tx *t, AVPacket *pkt);   // 아래(오디오 구성 뒤)에 정의

static int setup_video_copy(tx *t) {
    AVCodecParameters *par = t->vin->codecpar;
    // 컨테이너가 이 코덱을 담을 수 있는지 먼저 확인(VP8·Theora 등은 MP4 규격에 없다) — write_header 의 EINVAL 대신 분명한 사유.
    if (avformat_query_codec(t->ofmt->oformat, par->codec_id, FF_COMPLIANCE_NORMAL) != 1)
        return fail(t, FFX_ERR_MUXER, "codec %s cannot be stored in %s (remux unsupported)",
                    avcodec_get_name(par->codec_id), t->ofmt->oformat->name);
    t->vout = avformat_new_stream(t->ofmt, NULL);
    if (!t->vout) return fail(t, FFX_ERR_MUXER, "new video stream failed");
    int rc = avcodec_parameters_copy(t->vout->codecpar, par);
    if (rc < 0) return fail(t, FFX_ERR_MUXER, "parameters_copy: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    t->vout->codecpar->codec_tag = 0;          // 컨테이너가 바뀐다 — 태그는 muxer 가 고른다(vp09/avc1/hvc1/av01)
    t->vout->time_base = t->vin->time_base;
    t->vout->avg_frame_rate = t->frame_rate;
    t->vout->r_frame_rate = t->frame_rate;
    av_dict_copy(&t->vout->metadata, t->vin->metadata, 0);
    av_dict_set(&t->vout->metadata, "handler_name", NULL, 0);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((enum AVPixelFormat)par->format);
    t->bit_depth = (desc && desc->comp[0].depth > 8) ? 10 : 8;
    if (t->stats) {
        strncpy(t->stats->video_encoder, "copy", sizeof t->stats->video_encoder - 1);
        t->stats->out_width = par->width;
        t->stats->out_height = par->height;
        t->stats->out_bit_depth = t->bit_depth;
        t->stats->hw_encode_used = 0;
    }
    return 0;
}

/// 비디오 패킷 복사: 시작 오프셋 차감(전 스트림 공통) → 출력 tb → dts 단조 보정(copy 오디오와 같은 규칙) → 쓰기.
static int copy_video_packet(tx *t, AVPacket *pkt) {
    if (t->start_offset_us > 0) {
        int64_t off = av_rescale_q(t->start_offset_us, AV_TIME_BASE_Q, t->vin->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= off;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= off;
    }
    int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (pts != AV_NOPTS_VALUE) {
        if (!t->start_pts_set) { t->start_pts = pts; t->start_pts_set = 1; }
        double sec = (double)(pts - t->start_pts) * av_q2d(t->vin->time_base);
        if (sec > t->last_sec) t->last_sec = sec;
    }
    av_packet_rescale_ts(pkt, t->vin->time_base, t->vout->time_base);
    if (pkt->dts != AV_NOPTS_VALUE) {
        if (t->have_last_dts && pkt->dts <= t->last_dts) pkt->dts = t->last_dts + 1;
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) pkt->pts = pkt->dts;
        t->last_dts = pkt->dts;
        t->have_last_dts = 1;
    }
    pkt->stream_index = t->vout->index;
    pkt->pos = -1;
    t->frames_decoded++;
    t->frames_encoded++;
    int w = write_packet(t, pkt);
    if (w) return w;
    return report_progress(t, t->last_sec) ? FFX_ERR_CANCELLED : 0;
}

// ------------------------------------------------------------------------------------------
// 오디오 구성
// ------------------------------------------------------------------------------------------
static int setup_audio_stream(tx *t, AVStream *in_st) {
    const ffx_transcode_options *o = t->opts;
    AVCodecParameters *par = in_st->codecpar;
    const char *cname = avcodec_get_name(par->codec_id);
    int compatible = ffx_audio_codec_mp4_compatible(cname);
    int mode = o->audio_mode;
    int want_copy = (mode == FFX_AUDIO_COPY_OR_AAC || mode == FFX_AUDIO_COPY_OR_DROP) && compatible;

    if (t->stats) t->stats->audio_streams_in++;
    if (!want_copy && mode == FFX_AUDIO_COPY_OR_DROP) {
        if (t->stats) t->stats->audio_dropped++;
        return 0;
    }
    if (t->naudio >= FFX_MAX_AUDIO) {
        if (t->stats) t->stats->audio_dropped++;
        return 0;
    }
    audio_ctx *a = &t->audio[t->naudio];
    memset(a, 0, sizeof *a);
    a->in_index = in_st->index;
    a->in_st = in_st;

    if (want_copy) {
        a->copy = 1;
        a->out_st = avformat_new_stream(t->ofmt, NULL);
        if (!a->out_st) return fail(t, FFX_ERR_MUXER, "new audio stream failed");
        int rc = avcodec_parameters_copy(a->out_st->codecpar, par);
        if (rc < 0) return fail(t, FFX_ERR_MUXER, "audio parameters_copy: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
        a->out_st->codecpar->codec_tag = 0;
        a->out_st->time_base = in_st->time_base;
        av_dict_copy(&a->out_st->metadata, in_st->metadata, 0);
        av_dict_set(&a->out_st->metadata, "handler_name", NULL, 0);
        // ADTS(AAC in TS/raw) → ASC(mp4): extradata 가 없으면 bsf 적용
        if (par->codec_id == AV_CODEC_ID_AAC && par->extradata_size == 0) {
            const AVBitStreamFilter *f = av_bsf_get_by_name("aac_adtstoasc");
            if (f && av_bsf_alloc(f, &a->bsf) == 0) {
                avcodec_parameters_copy(a->bsf->par_in, par);
                a->bsf->time_base_in = in_st->time_base;
                if (av_bsf_init(a->bsf) == 0) {
                    avcodec_parameters_copy(a->out_st->codecpar, a->bsf->par_out);
                    a->out_st->codecpar->codec_tag = 0;
                } else {
                    av_bsf_free(&a->bsf);
                }
            }
        }
        t->naudio++;
        return 0;
    }

    // ---- AAC 재인코딩 ----
    const AVCodec *dcodec = avcodec_find_decoder(par->codec_id);
    if (!dcodec) {
        if (t->stats) t->stats->audio_dropped++;
        return 0;   // 디코더 없음 → 트랙 제외(비디오 압축은 진행)
    }
    a->dec = avcodec_alloc_context3(dcodec);
    if (!a->dec) return fail(t, FFX_ERR_DECODER, "audio decoder alloc failed");
    int rc = avcodec_parameters_to_context(a->dec, par);
    if (rc < 0) return fail(t, FFX_ERR_DECODER, "audio parameters_to_context: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    a->dec->pkt_timebase = in_st->time_base;
    rc = avcodec_open2(a->dec, dcodec, NULL);
    if (rc < 0) {
        avcodec_free_context(&a->dec);
        if (t->stats) t->stats->audio_dropped++;
        return 0;
    }

    const AVCodec *ecodec = avcodec_find_encoder_by_name("aac");
    if (!ecodec) return fail(t, FFX_ERR_ENCODER, "aac encoder unavailable");
    a->enc = avcodec_alloc_context3(ecodec);
    if (!a->enc) return fail(t, FFX_ERR_ENCODER, "aac encoder alloc failed");
    a->enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    int sr = a->dec->sample_rate;
    if (sr <= 0 || sr > 96000) sr = 48000;
    if (sr < 8000) sr = 8000;
    a->enc->sample_rate = sr;
    if (a->dec->ch_layout.nb_channels >= 1 && a->dec->ch_layout.nb_channels <= 8 &&
        a->dec->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_copy(&a->enc->ch_layout, &a->dec->ch_layout);
    } else {
        int nch = a->dec->ch_layout.nb_channels;
        av_channel_layout_default(&a->enc->ch_layout, (nch == 1) ? 1 : 2);
    }
    int nch = a->enc->ch_layout.nb_channels;
    int abr = o->aac_bit_rate > 0 ? o->aac_bit_rate : 128000;
    if (nch == 1) abr = abr / 2 < 64000 ? 64000 : abr / 2;
    if (nch > 2) abr = abr * nch / 2;
    a->enc->bit_rate = abr;
    a->enc->time_base = (AVRational){1, sr};
    if (t->ofmt->oformat->flags & AVFMT_GLOBALHEADER) a->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    rc = avcodec_open2(a->enc, ecodec, NULL);
    if (rc < 0) return fail(t, FFX_ERR_ENCODER, "aac encoder open failed: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));

    a->out_st = avformat_new_stream(t->ofmt, NULL);
    if (!a->out_st) return fail(t, FFX_ERR_MUXER, "new audio stream failed");
    rc = avcodec_parameters_from_context(a->out_st->codecpar, a->enc);
    if (rc < 0) return fail(t, FFX_ERR_MUXER, "audio parameters_from_context: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    a->out_st->time_base = a->enc->time_base;
    av_dict_copy(&a->out_st->metadata, in_st->metadata, 0);
    av_dict_set(&a->out_st->metadata, "handler_name", NULL, 0);

    rc = swr_alloc_set_opts2(&a->swr, &a->enc->ch_layout, a->enc->sample_fmt, a->enc->sample_rate,
                             &a->dec->ch_layout, a->dec->sample_fmt, a->dec->sample_rate, 0, NULL);
    if (rc < 0 || !a->swr || swr_init(a->swr) < 0) return fail(t, FFX_ERR_ENCODER, "swr init failed");
    a->fifo = av_audio_fifo_alloc(a->enc->sample_fmt, nch, a->enc->frame_size * 4);
    a->dec_frame = av_frame_alloc();
    a->enc_frame = av_frame_alloc();
    if (!a->fifo || !a->dec_frame || !a->enc_frame) return fail(t, FFX_ERR_ENCODER, "audio buffers alloc failed");
    if (t->stats) t->stats->audio_reencoded++;
    t->naudio++;
    return 0;
}

/// stream copy 패킷의 dts 단조성 보정(ffmpeg CLI 와 같은 정책): 편집 리스트가 있는 mov 소스는 demux 단계에서
/// 세그먼트 경계마다 dts 가 뒤로 점프할 수 있다 — mp4 muxer 는 이를 거부하므로 직전 dts+1 로 올리고 pts ≥ dts 를 유지한다.
static void sanitize_copy_ts(audio_ctx *a, AVPacket *p) {
    if (p->dts == AV_NOPTS_VALUE) return;
    if (a->have_last_copy_dts && p->dts <= a->last_copy_dts) p->dts = a->last_copy_dts + 1;
    if (p->pts != AV_NOPTS_VALUE && p->pts < p->dts) p->pts = p->dts;
    a->last_copy_dts = p->dts;
    a->have_last_copy_dts = 1;
}

static int write_packet(tx *t, AVPacket *pkt) {
    int rc = av_interleaved_write_frame(t->ofmt, pkt);
    if (rc < 0) return fail(t, FFX_ERR_IO, "write_frame: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    return 0;
}

static int audio_encode_flush_fifo(tx *t, audio_ctx *a, int final) {
    int frame_size = a->enc->frame_size > 0 ? a->enc->frame_size : 1024;
    int nch = a->enc->ch_layout.nb_channels;
    while (av_audio_fifo_size(a->fifo) >= frame_size || (final && av_audio_fifo_size(a->fifo) > 0)) {
        int n = av_audio_fifo_size(a->fifo) >= frame_size ? frame_size : av_audio_fifo_size(a->fifo);
        AVFrame *f = a->enc_frame;
        av_frame_unref(f);
        f->nb_samples = n;
        f->format = a->enc->sample_fmt;
        f->sample_rate = a->enc->sample_rate;
        av_channel_layout_copy(&f->ch_layout, &a->enc->ch_layout);
        int rc = av_frame_get_buffer(f, 0);
        if (rc < 0) return fail(t, FFX_ERR_ENCODE, "audio frame buffer: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
        if (av_audio_fifo_read(a->fifo, (void **)f->data, n) < n)
            return fail(t, FFX_ERR_ENCODE, "audio fifo read short");
        (void)nch;
        f->pts = a->next_pts;
        a->next_pts += n;
        rc = avcodec_send_frame(a->enc, f);
        if (rc < 0) return fail(t, FFX_ERR_ENCODE, "aac send_frame: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
        for (;;) {
            rc = avcodec_receive_packet(a->enc, t->enc_pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return fail(t, FFX_ERR_ENCODE, "aac receive_packet: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
            av_packet_rescale_ts(t->enc_pkt, a->enc->time_base, a->out_st->time_base);
            t->enc_pkt->stream_index = a->out_st->index;
            int w = write_packet(t, t->enc_pkt);
            if (w) return w;
        }
    }
    return 0;
}

static int audio_process_frame(tx *t, audio_ctx *a, AVFrame *frame) {
    if (!a->pts_initialized) {
        a->pts_initialized = 1;
        int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE) {
            if (t->start_offset_us > 0)
                pts -= av_rescale_q(t->start_offset_us, AV_TIME_BASE_Q, a->in_st->time_base);
            int64_t p = av_rescale_q(pts, a->in_st->time_base, a->enc->time_base);
            a->next_pts = p > 0 ? p : 0;
        }
    }
    int out_cap = (int)av_rescale_rnd(swr_get_delay(a->swr, a->dec->sample_rate) + frame->nb_samples,
                                      a->enc->sample_rate, a->dec->sample_rate, AV_ROUND_UP) + 64;
    int nch = a->enc->ch_layout.nb_channels;
    if (out_cap > a->cvt_capacity) {
        if (a->cvt_data) { av_freep(&a->cvt_data[0]); av_freep(&a->cvt_data); }
        int rc = av_samples_alloc_array_and_samples(&a->cvt_data, &a->cvt_linesize, nch, out_cap, a->enc->sample_fmt, 0);
        if (rc < 0) return fail(t, FFX_ERR_ENCODE, "audio cvt alloc: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
        a->cvt_capacity = out_cap;
    }
    int got = swr_convert(a->swr, a->cvt_data, out_cap, (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (got < 0) return fail(t, FFX_ERR_ENCODE, "swr_convert: %s", ffx_averr(got, t->ebuf, sizeof t->ebuf));
    if (got > 0 && av_audio_fifo_write(a->fifo, (void **)a->cvt_data, got) < got)
        return fail(t, FFX_ERR_ENCODE, "audio fifo write short");
    return audio_encode_flush_fifo(t, a, 0);
}

static int audio_handle_packet(tx *t, audio_ctx *a, AVPacket *pkt) {
    if (a->copy) {
        pkt->stream_index = a->out_st->index;
        pkt->pos = -1;
        if (a->bsf) {
            int rc = av_bsf_send_packet(a->bsf, pkt);
            if (rc < 0) return fail(t, FFX_ERR_MUXER, "bsf send: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
            for (;;) {
                rc = av_bsf_receive_packet(a->bsf, t->enc_pkt);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) return fail(t, FFX_ERR_MUXER, "bsf receive: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
                if (t->start_offset_us > 0) {
                    int64_t off = av_rescale_q(t->start_offset_us, AV_TIME_BASE_Q, a->in_st->time_base);
                    if (t->enc_pkt->pts != AV_NOPTS_VALUE) t->enc_pkt->pts -= off;
                    if (t->enc_pkt->dts != AV_NOPTS_VALUE) t->enc_pkt->dts -= off;
                }
                av_packet_rescale_ts(t->enc_pkt, a->in_st->time_base, a->out_st->time_base);
                sanitize_copy_ts(a, t->enc_pkt);
                t->enc_pkt->stream_index = a->out_st->index;
                int w = write_packet(t, t->enc_pkt);
                if (w) return w;
            }
            return 0;
        }
        if (t->start_offset_us > 0) {
            int64_t off = av_rescale_q(t->start_offset_us, AV_TIME_BASE_Q, a->in_st->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= off;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= off;
        }
        av_packet_rescale_ts(pkt, a->in_st->time_base, a->out_st->time_base);
        sanitize_copy_ts(a, pkt);
        return write_packet(t, pkt);
    }
    int rc = avcodec_send_packet(a->dec, pkt);
    av_packet_unref(pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        // 손상 패킷은 건너뛴다(오디오는 best-effort).
        return 0;
    }
    for (;;) {
        rc = avcodec_receive_frame(a->dec, a->dec_frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return 0;
        int p = audio_process_frame(t, a, a->dec_frame);
        av_frame_unref(a->dec_frame);
        if (p) return p;
    }
    return 0;
}

static int audio_flush(tx *t, audio_ctx *a) {
    if (a->copy) {
        if (a->bsf) {
            av_bsf_send_packet(a->bsf, NULL);
            for (;;) {
                int rc = av_bsf_receive_packet(a->bsf, t->enc_pkt);
                if (rc < 0) break;
                av_packet_rescale_ts(t->enc_pkt, a->in_st->time_base, a->out_st->time_base);
                sanitize_copy_ts(a, t->enc_pkt);
                t->enc_pkt->stream_index = a->out_st->index;
                int w = write_packet(t, t->enc_pkt);
                if (w) return w;
            }
        }
        return 0;
    }
    avcodec_send_packet(a->dec, NULL);
    for (;;) {
        int rc = avcodec_receive_frame(a->dec, a->dec_frame);
        if (rc < 0) break;
        int p = audio_process_frame(t, a, a->dec_frame);
        av_frame_unref(a->dec_frame);
        if (p) return p;
    }
    // swr 잔여
    int nch = a->enc->ch_layout.nb_channels;
    (void)nch;
    for (;;) {
        int got = swr_convert(a->swr, a->cvt_data, a->cvt_capacity, NULL, 0);
        if (got <= 0) break;
        av_audio_fifo_write(a->fifo, (void **)a->cvt_data, got);
    }
    int p = audio_encode_flush_fifo(t, a, 1);
    if (p) return p;
    avcodec_send_frame(a->enc, NULL);
    for (;;) {
        int rc = avcodec_receive_packet(a->enc, t->enc_pkt);
        if (rc < 0) break;
        av_packet_rescale_ts(t->enc_pkt, a->enc->time_base, a->out_st->time_base);
        t->enc_pkt->stream_index = a->out_st->index;
        int w = write_packet(t, t->enc_pkt);
        if (w) return w;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// 비디오 프레임 처리
// ------------------------------------------------------------------------------------------
static int drain_video_encoder(tx *t, int final) {
    for (;;) {
        int rc = avcodec_receive_packet(t->venc, t->enc_pkt);
        if (rc == AVERROR(EAGAIN)) return 0;
        if (rc == AVERROR_EOF) return 0;
        if (rc < 0) return fail(t, FFX_ERR_ENCODE, "video receive_packet: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
        AVPacket *p = t->enc_pkt;
        if (p->dts != AV_NOPTS_VALUE) {
            p->dts -= t->dts_safety;
            if (t->have_last_dts && p->dts <= t->last_dts) p->dts = t->last_dts + 1;   // 최후 방어(단조성)
            if (p->pts != AV_NOPTS_VALUE && p->pts < p->dts) p->pts = p->dts;         // 도달 불가 — 형식상 보장
            t->last_dts = p->dts;
            t->have_last_dts = 1;
        }
        av_packet_rescale_ts(p, t->venc->time_base, t->vout->time_base);
        p->stream_index = t->vout->index;
        t->frames_encoded++;
        int w = write_packet(t, t->enc_pkt);
        if (w) return w;
    }
    (void)final;
}

static int encode_video_frame(tx *t, AVFrame *frame) {
    int rc = avcodec_send_frame(t->venc, frame);
    if (rc < 0) return fail(t, FFX_ERR_ENCODE, "video send_frame: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
    return drain_video_encoder(t, 0);
}

static int handle_video_frame(tx *t, AVFrame *frame) {
    t->frames_decoded++;
    // pts 정규화
    int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE && t->start_offset_us > 0)
        pts -= av_rescale_q(t->start_offset_us, AV_TIME_BASE_Q, t->vin->time_base);
    if (pts == AV_NOPTS_VALUE) {
        pts = t->synth_pts;
        t->synth_pts += t->frame_duration_tb > 0 ? t->frame_duration_tb : 1;
    } else {
        t->synth_pts = pts + (t->frame_duration_tb > 0 ? t->frame_duration_tb : 1);
    }
    if (!t->start_pts_set) { t->start_pts = pts; t->start_pts_set = 1; }
    double sec = (double)(pts - t->start_pts) * av_q2d(t->vin->time_base);
    if (sec < 0) sec = 0;
    t->last_sec = sec;

    // 프레임레이트 상한(드롭): 출력 격자(gap 간격) 위의 각 슬롯에 '처음 도달한' 프레임만 남긴다.
    // ms 단위 타임베이스(mkv)의 반올림(16/17ms 교대)에 흔들리지 않도록 입력 프레임 길이의 절반을 허용 오차로 둔다.
    if (t->min_frame_gap > 0) {
        double in_dur = (t->frame_rate.num > 0) ? 1.0 / av_q2d(t->frame_rate) : 0;
        if (!t->have_last_kept) {
            t->have_last_kept = 1;
            t->last_kept_sec = sec + t->min_frame_gap;     // 다음 슬롯 시각
        } else if (sec + in_dur * 0.5 < t->last_kept_sec) {
            t->frames_dropped++;
            return report_progress(t, sec) ? FFX_ERR_CANCELLED : 0;
        } else {
            // 슬롯 통과 — 격자를 유지하되 크게 뒤처졌으면(구간 결측) 현재 시각 기준으로 재동기화.
            t->last_kept_sec += t->min_frame_gap;
            if (t->last_kept_sec < sec) t->last_kept_sec = sec + t->min_frame_gap;
        }
    }

    AVFrame *src = frame;
    // HW 프레임 기대인데 SW 프레임이 왔거나 그 반대 → 처음부터 SW 로 재실행
    if (t->enc_pix_fmt == AV_PIX_FMT_VIDEOTOOLBOX) {
        if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX) return FFX_ERR_HW_FALLBACK;
    } else {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            av_frame_unref(t->sw_frame);
            int rc = av_hwframe_transfer_data(t->sw_frame, frame, 0);
            if (rc < 0) return FFX_ERR_HW_FALLBACK;
            t->sw_frame->pts = frame->pts;
            src = t->sw_frame;
        }
        if (src->format != t->enc_pix_fmt || src->width != t->enc_w || src->height != t->enc_h) {
            if (!t->sws) {
                t->sws = sws_getContext(src->width, src->height, (enum AVPixelFormat)src->format,
                                        t->enc_w, t->enc_h, t->enc_pix_fmt,
                                        SWS_BICUBIC, NULL, NULL, NULL);
                if (!t->sws) return fail(t, FFX_ERR_ENCODE, "sws_getContext failed (%s→%s)",
                                         av_get_pix_fmt_name((enum AVPixelFormat)src->format),
                                         av_get_pix_fmt_name(t->enc_pix_fmt));
                // 컬러 범위 승계(yuvj → full range 등)
                int in_full = (src->color_range == AVCOL_RANGE_JPEG) ||
                              src->format == AV_PIX_FMT_YUVJ420P || src->format == AV_PIX_FMT_YUVJ422P ||
                              src->format == AV_PIX_FMT_YUVJ444P;
                int *inv_table, *table, srcRange, dstRange, brightness, contrast, saturation;
                if (sws_getColorspaceDetails(t->sws, &inv_table, &srcRange, &table, &dstRange,
                                             &brightness, &contrast, &saturation) >= 0) {
                    sws_setColorspaceDetails(t->sws, inv_table, in_full, table, in_full,
                                             brightness, contrast, saturation);
                }
            }
            AVFrame *c = t->cvt_frame;
            av_frame_unref(c);
            c->format = t->enc_pix_fmt;
            c->width = t->enc_w;
            c->height = t->enc_h;
            int rc = av_frame_get_buffer(c, 0);
            if (rc < 0) return fail(t, FFX_ERR_ENCODE, "cvt frame alloc: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
            sws_scale(t->sws, (const uint8_t *const *)src->data, src->linesize, 0, src->height, c->data, c->linesize);
            c->color_primaries = src->color_primaries;
            c->color_trc = src->color_trc;
            c->colorspace = src->colorspace;
            c->color_range = (src->color_range == AVCOL_RANGE_JPEG || src->format == AV_PIX_FMT_YUVJ420P)
                             ? AVCOL_RANGE_JPEG : src->color_range;
            src = c;
        }
    }
    src->pts = av_rescale_q(pts, t->vin->time_base, t->enc_tb);
    src->pict_type = AV_PICTURE_TYPE_NONE;
    int rc = encode_video_frame(t, src);
    if (rc) return rc;
    return report_progress(t, sec) ? FFX_ERR_CANCELLED : 0;
}

static int decode_video_packet(tx *t, AVPacket *pkt) {
    int rc = avcodec_send_packet(t->vdec, pkt);
    if (pkt) av_packet_unref(pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
        if (t->hw_active) return FFX_ERR_HW_FALLBACK;
        // SW: 손상 패킷은 건너뛴다(디코더가 복구).
        return 0;
    }
    for (;;) {
        rc = avcodec_receive_frame(t->vdec, t->dec_frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            if (t->hw_active) return FFX_ERR_HW_FALLBACK;
            return fail(t, FFX_ERR_DECODE, "video receive_frame: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf));
        }
        int h = handle_video_frame(t, t->dec_frame);
        av_frame_unref(t->dec_frame);
        if (h) return h;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------
// 정리
// ------------------------------------------------------------------------------------------
static void cleanup(tx *t) {
    for (int i = 0; i < t->naudio; i++) {
        audio_ctx *a = &t->audio[i];
        if (a->bsf) av_bsf_free(&a->bsf);
        if (a->dec) avcodec_free_context(&a->dec);
        if (a->enc) avcodec_free_context(&a->enc);
        if (a->swr) swr_free(&a->swr);
        if (a->fifo) av_audio_fifo_free(a->fifo);
        if (a->dec_frame) av_frame_free(&a->dec_frame);
        if (a->enc_frame) av_frame_free(&a->enc_frame);
        if (a->cvt_data) { av_freep(&a->cvt_data[0]); av_freep(&a->cvt_data); }
    }
    if (t->sws) sws_freeContext(t->sws);
    if (t->dec_frame) av_frame_free(&t->dec_frame);
    if (t->sw_frame) av_frame_free(&t->sw_frame);
    if (t->cvt_frame) av_frame_free(&t->cvt_frame);
    if (t->pkt) av_packet_free(&t->pkt);
    if (t->enc_pkt) av_packet_free(&t->enc_pkt);
    if (t->venc) avcodec_free_context(&t->venc);
    if (t->vdec) avcodec_free_context(&t->vdec);
    if (t->hwdev) av_buffer_unref(&t->hwdev);
    if (t->ofmt) {
        if (t->ofmt->pb) avio_closep(&t->ofmt->pb);
        avformat_free_context(t->ofmt);
    }
    if (t->ifmt) avformat_close_input(&t->ifmt);
}

// ------------------------------------------------------------------------------------------
// 1회 실행
// ------------------------------------------------------------------------------------------
static int run_once(const char *in_path, const char *out_path, const ffx_transcode_options *opts,
                    int prefer_hw, ffx_progress_cb cb, void *ctx, ffx_transcode_stats *stats,
                    char *err, size_t errlen) {
    tx T;
    memset(&T, 0, sizeof T);
    tx *t = &T;
    t->opts = opts;
    t->prefer_hw = prefer_hw;
    t->cb = cb;
    t->cb_ctx = ctx;
    t->stats = stats;
    t->err = err;
    t->errlen = errlen;
    if (stats) memset(stats, 0, sizeof *stats);
    double t0 = ffx_now();
    int rc;

    // ---- 입력 ----
    rc = avformat_open_input(&t->ifmt, in_path, NULL, NULL);
    if (rc < 0) { rc = fail(t, FFX_ERR_OPEN, "open input: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }
    t->ifmt->flags |= AVFMT_FLAG_GENPTS;
    rc = avformat_find_stream_info(t->ifmt, NULL);
    if (rc < 0) { rc = fail(t, FFX_ERR_OPEN, "stream info: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }

    t->vidx = av_find_best_stream(t->ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (t->vidx < 0 || (t->ifmt->streams[t->vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        rc = fail(t, FFX_ERR_NO_VIDEO, "no video stream"); goto done;
    }
    t->vin = t->ifmt->streams[t->vidx];
    t->frame_rate = av_guess_frame_rate(t->ifmt, t->vin, NULL);
    if (t->frame_rate.num <= 0 || t->frame_rate.den <= 0) t->frame_rate = (AVRational){30, 1};
    t->frame_duration_tb = av_rescale_q(1, av_inv_q(t->frame_rate), t->vin->time_base);
    if (opts->max_frame_rate > 0 && av_q2d(t->frame_rate) > opts->max_frame_rate + 0.01)
        t->min_frame_gap = 1.0 / opts->max_frame_rate;
    t->start_offset_us = (t->ifmt->start_time != AV_NOPTS_VALUE && t->ifmt->start_time > 0) ? t->ifmt->start_time : 0;
    t->duration_sec = t->ifmt->duration > 0 ? (double)t->ifmt->duration / AV_TIME_BASE : 0;
    if (t->duration_sec <= 0 && t->vin->duration > 0) t->duration_sec = t->vin->duration * av_q2d(t->vin->time_base);

    t->pkt = av_packet_alloc();
    t->enc_pkt = av_packet_alloc();
    t->dec_frame = av_frame_alloc();
    t->sw_frame = av_frame_alloc();
    t->cvt_frame = av_frame_alloc();
    if (!t->pkt || !t->enc_pkt || !t->dec_frame || !t->sw_frame || !t->cvt_frame) {
        rc = fail(t, FFX_ERR_ENCODER, "alloc failed"); goto done;
    }

    // ---- 출력 컨테이너 ----
    const char *fmt_name = (opts->container == FFX_CONTAINER_MOV) ? "mov" : "mp4";
    rc = avformat_alloc_output_context2(&t->ofmt, NULL, fmt_name, out_path);
    if (rc < 0 || !t->ofmt) { rc = fail(t, FFX_ERR_MUXER, "alloc output(%s): %s", fmt_name, ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }

    // ---- 디코더/인코더 (또는 비디오 복사) ----
    if (opts->video_copy) {
        rc = setup_video_copy(t);
        if (rc) goto done;
    } else {
        rc = open_video_decoder(t);
        if (rc) goto done;
        rc = open_video_encoder(t);
        if (rc) goto done;
    }
    for (unsigned i = 0; i < t->ifmt->nb_streams; i++) {
        AVStream *st = t->ifmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            rc = setup_audio_stream(t, st);
            if (rc) goto done;
        }
    }
    if (t->stats) t->stats->audio_streams_out = t->naudio;

    // ---- 메타데이터 ----
    av_dict_copy(&t->ofmt->metadata, t->ifmt->metadata, 0);
    for (int i = 0; i < opts->metadata_count; i++) {
        if (opts->metadata_keys && opts->metadata_values && opts->metadata_keys[i] && opts->metadata_values[i])
            av_dict_set(&t->ofmt->metadata, opts->metadata_keys[i], opts->metadata_values[i], 0);
    }

    // ---- 출력 열기 + 헤더 ----
    unlink(out_path);
    rc = avio_open(&t->ofmt->pb, out_path, AVIO_FLAG_WRITE);
    if (rc < 0) { rc = fail(t, FFX_ERR_IO, "open output: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }
    {
        AVDictionary *mux_opts = NULL;
        // use_metadata_tags: 임의 키를 mdta(keys) 로 기록 → AVFoundation 이 quickTimeMetadata 로 읽는다.
        av_dict_set(&mux_opts, "movflags", "use_metadata_tags", 0);
        rc = avformat_write_header(t->ofmt, &mux_opts);
        av_dict_free(&mux_opts);
        if (rc < 0) { rc = fail(t, FFX_ERR_MUXER, "write_header: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }
        t->header_written = 1;
    }

    // ---- 메인 루프 ----
    for (;;) {
        rc = av_read_frame(t->ifmt, t->pkt);
        if (rc == AVERROR_EOF) { rc = 0; break; }
        if (rc < 0) { rc = fail(t, FFX_ERR_DECODE, "read_frame: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }
        int idx = t->pkt->stream_index;
        if (idx == t->vidx) {
            rc = opts->video_copy ? copy_video_packet(t, t->pkt) : decode_video_packet(t, t->pkt);
        } else {
            audio_ctx *a = NULL;
            for (int i = 0; i < t->naudio; i++) if (t->audio[i].in_index == idx) { a = &t->audio[i]; break; }
            if (a) rc = audio_handle_packet(t, a, t->pkt);
            else { av_packet_unref(t->pkt); rc = 0; }
        }
        if (rc) goto done;
        if (t->cancelled) { rc = FFX_ERR_CANCELLED; goto done; }
    }

    // ---- 플러시 ----
    if (!opts->video_copy) {
        rc = decode_video_packet(t, NULL);
        if (rc) goto done;
        rc = avcodec_send_frame(t->venc, NULL);
        if (rc < 0 && rc != AVERROR_EOF) { rc = fail(t, FFX_ERR_ENCODE, "video flush: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }
        rc = drain_video_encoder(t, 1);
        if (rc) goto done;
    }
    for (int i = 0; i < t->naudio; i++) {
        rc = audio_flush(t, &t->audio[i]);
        if (rc) goto done;
    }
    if (t->frames_encoded == 0) { rc = fail(t, FFX_ERR_ENCODE, "no frames encoded"); goto done; }
    rc = av_write_trailer(t->ofmt);
    if (rc < 0) { rc = fail(t, FFX_ERR_IO, "write_trailer: %s", ffx_averr(rc, t->ebuf, sizeof t->ebuf)); goto done; }
    rc = 0;

done:
    if (t->cancelled && rc == 0) rc = FFX_ERR_CANCELLED;
    if (stats) {
        stats->frames_decoded = t->frames_decoded;
        stats->frames_encoded = t->frames_encoded;
        stats->frames_dropped = t->frames_dropped;
        stats->hw_decode_used = (t->hw_active && !t->hw_failed) ? 1 : 0;
        stats->elapsed_seconds = ffx_now() - t0;
        stats->out_duration = t->last_sec;
    }
    if (rc == 0 && t->cb) t->cb(t->cb_ctx, 1.0);
    cleanup(t);
    if (rc == FFX_ERR_HW_FALLBACK && t->cancelled) rc = FFX_ERR_CANCELLED;
    return rc;
}

int ffx_transcode(const char *in_path, const char *out_path,
                  const ffx_transcode_options *opts,
                  ffx_progress_cb progress, void *ctx,
                  ffx_transcode_stats *stats,
                  char *err, size_t errlen) {
    if (!in_path || !out_path || !opts || (opts->video_bit_rate <= 0 && !opts->video_copy)) {
        ffx_set_err(err, errlen, "invalid argument");
        return FFX_ERR_ARG;
    }
    ffx_ensure_init();
    if (opts->log_level != 0) av_log_set_level(opts->log_level);

    int rc = run_once(in_path, out_path, opts, opts->prefer_hw_decode ? 1 : 0, progress, ctx, stats, err, errlen);
    if (rc == FFX_ERR_HW_FALLBACK) {
        av_log(NULL, AV_LOG_INFO, "ffx: hardware decode failed — retrying with software decode\n");
        rc = run_once(in_path, out_path, opts, 0, progress, ctx, stats, err, errlen);
        if (rc == FFX_ERR_HW_FALLBACK) rc = FFX_ERR_DECODE;   // SW 에서도 포맷 불일치(있을 수 없음)
    }
    if (rc != 0) unlink(out_path);
    else if (stats) {
        // 출력 실측 크기 — 호출자가 목표 비트레이트 대비 실제 비트레이트를 로그/검증한다.
        struct stat st;
        stats->out_bytes = (stat(out_path, &st) == 0) ? (int64_t)st.st_size : 0;
    }
    return rc;
}

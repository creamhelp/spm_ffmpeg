#!/usr/bin/env python3
"""spm_ffmpeg patch: mov muxer 의 mdta(use_metadata_tags) 메타를 QuickTime 레이아웃(moov/meta, version/flags 없음)으로 기록한다.

배경(실측): FFmpeg 은 use_metadata_tags 일 때 keys/ilst 를 moov/udta/meta(ISO full box) 아래에 둔다. AVFoundation 은
  · udta/meta 를 iTunes 키스페이스로 해석해 mdta 키 이름을 잃고("itsk/%00%00%00%01"),
  · moov/meta 라도 ISO full box(4바이트 version/flags 선행)면 QuickTime 파서가 자식 atom 을 못 읽어 메타 전체를 버린다.
아이폰 촬영본과 같은 QTFF 형식 — moov/meta { hdlr(mdta), keys, ilst } (version/flags 없음) — 으로 쓰면
AVMetadataIdentifier.quickTimeMetadata* 로 정상 판독된다. 앱의 회차 서명(encodedby) 계약이 이에 의존한다.

멱등: 단계별 마커로 건너뛴다. 사용: python3 movenc-mdta-moov.py <ffmpeg-src>/libavformat/movenc.c
"""
import sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()
changed = False

# ---- step 1: udta 안에서는 mdta meta 를 쓰지 않는다 -------------------------------------------------
MARK1 = "spm_ffmpeg: mdta keys are written at moov/meta instead"
if MARK1 not in src:
    old = """    } else {
        /* iTunes meta data */
        mov_write_meta_tag(pb_buf, mov, s);
        mov_write_loci_tag(s, pb_buf);
    }
"""
    new = """    } else {
        /* iTunes meta data */
        if (!(mov->flags & FF_MOV_FLAG_USE_MDTA))   /* spm_ffmpeg: mdta keys are written at moov/meta instead */
            mov_write_meta_tag(pb_buf, mov, s);
        mov_write_loci_tag(s, pb_buf);
    }
"""
    assert src.count(old) == 1, "udta block not found"
    src = src.replace(old, new)
    changed = True

# ---- step 2: QuickTime 형식 meta 함수 + moov 레벨 호출 ----------------------------------------------
MARK2 = "spm_ffmpeg: QuickTime-style meta"
if MARK2 not in src:
    # 2a) 함수 정의: mov_write_meta_tag 바로 앞에 삽입
    anchor = """/* meta data tags */
static int mov_write_meta_tag(AVIOContext *pb, MOVMuxContext *mov,
                              AVFormatContext *s)
{"""
    assert src.count(anchor) == 1, "meta tag anchor not found"
    qt_func = """/* spm_ffmpeg: QuickTime-style meta (moov/meta, no version/flags) so AVFoundation reads mdta keys */
static int mov_write_qt_meta_tag(AVIOContext *pb, MOVMuxContext *mov,
                                 AVFormatContext *s)
{
    int64_t pos = avio_tell(pb);
    avio_wb32(pb, 0); /* size */
    ffio_wfourcc(pb, "meta");
    mov_write_mdta_hdlr_tag(pb, mov, s);
    mov_write_mdta_keys_tag(pb, mov, s);
    mov_write_mdta_ilst_tag(pb, mov, s);
    return update_size(pb, pos);
}

"""
    src = src.replace(anchor, qt_func + anchor)
    # 2b) moov 레벨 호출: 1차 패치가 넣은 호출이 있으면 교체, 없으면 신규 삽입
    old_call = """    else if (mov->mode != MODE_AVIF) {
        if (mov->flags & FF_MOV_FLAG_USE_MDTA)
            mov_write_meta_tag(pb, mov, s);   /* spm_ffmpeg: mdta — QuickTime layout (moov/meta, hdlr mdta) for AVFoundation */
        mov_write_udta_tag(pb, mov, s);
    }
"""
    new_call = """    else if (mov->mode != MODE_AVIF) {
        if (mov->flags & FF_MOV_FLAG_USE_MDTA)
            mov_write_qt_meta_tag(pb, mov, s);   /* spm_ffmpeg: QuickTime-style meta at moov level */
        mov_write_udta_tag(pb, mov, s);
    }
"""
    if src.count(old_call) == 1:
        src = src.replace(old_call, new_call)
    else:
        old_plain = """    else if (mov->mode != MODE_AVIF)
        mov_write_udta_tag(pb, mov, s);
"""
        assert src.count(old_plain) == 1, "moov udta call not found"
        src = src.replace(old_plain, new_call)
    changed = True

if changed:
    open(path, "w", encoding="utf-8").write(src)
    print("[patch] movenc.c patched (mdta → QuickTime moov/meta)")
else:
    print("[patch] movenc.c already patched")

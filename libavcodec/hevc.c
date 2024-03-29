/*
 * HEVC video Decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Mickael Raulet
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2012 - 2013 Wassim Hamidouche
 * Copyright (C) 2012 - 2013 Seppo Tomperi
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/atomic.h"
#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/internal.h"
#include "libavutil/md5.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"

#include "bswapdsp.h"
#include "bytestream.h"
#include "cabac_functions.h"
#include "golomb.h"
#include "hevc.h"

const uint8_t ff_hevc_pel_weight[65] = { [2] = 0, [4] = 1, [6] = 2, [8] = 3, [12] = 4, [16] = 5, [24] = 6, [32] = 7, [48] = 8, [64] = 9 };

#define POC_DISPLAY_MD5

static void calc_md5(uint8_t *md5, uint8_t* src, int stride, int width, int height, int pixel_shift);
static int compare_md5(uint8_t *md5_in1, uint8_t *md5_in2);
static void display_md5(int poc, uint8_t md5[3][16]);
static void printf_ref_pic_list(HEVCContext *s);



/**
 * NOTE: Each function hls_foo correspond to the function foo in the
 * specification (HLS stands for High Level Syntax).
 */

/**
 * Section 5.7
 */

/* free everything allocated  by pic_arrays_init() */
static void pic_arrays_free(HEVCContext *s)
{
    av_freep(&s->sao);
    av_freep(&s->deblock);

    av_freep(&s->skip_flag);
    av_freep(&s->tab_ct_depth);

    av_freep(&s->tab_ipm);
    av_freep(&s->cbf_luma);
    av_freep(&s->is_pcm);

    av_freep(&s->qp_y_tab);
    av_freep(&s->tab_slice_address);
    av_freep(&s->filter_slice_edges);

    av_freep(&s->horizontal_bs);
    av_freep(&s->vertical_bs);

    av_freep(&s->sh.entry_point_offset);
    av_freep(&s->sh.size);
    av_freep(&s->sh.offset);

    av_buffer_pool_uninit(&s->tab_mvf_pool);
    av_buffer_pool_uninit(&s->rpl_tab_pool);

#ifdef SVC_EXTENSION
#if ACTIVE_BOTH_FRAME_AND_PU
    av_freep(&s->buffer_frame[0]);
    av_freep(&s->buffer_frame[1]);
    av_freep(&s->buffer_frame[2]);
    av_freep(&s->is_upsampled);
#else
#if !ACTIVE_PU_UPSAMPLING
    av_freep(&s->buffer_frame[0]);
    av_freep(&s->buffer_frame[1]);
    av_freep(&s->buffer_frame[2]);
#else
    av_freep(&s->is_upsampled);
#endif
#endif    
#endif

#if PARALLEL_SLICE
    av_freep(&s->decoded_rows);
#endif
}

/* allocate arrays that depend on frame dimensions */
static int pic_arrays_init(HEVCContext *s, const HEVCSPS *sps)
{
    int log2_min_cb_size = sps->log2_min_cb_size;
    int width            = sps->width;
    int height           = sps->height;
    int pic_size         = width * height;
    int pic_size_in_ctb  = ((width  >> log2_min_cb_size) + 1) *
                           ((height >> log2_min_cb_size) + 1);
    int ctb_count        = sps->ctb_width * sps->ctb_height;
    int min_pu_size      = sps->min_pu_width * sps->min_pu_height;

    s->bs_width  = (width  >> 2);
    s->bs_height = (height >> 2);

    s->sao           = av_mallocz_array(ctb_count, sizeof(*s->sao));
    s->deblock       = av_mallocz_array(ctb_count, sizeof(*s->deblock));
    s->dynamic_alloc += sizeof(*s->sao);
    s->dynamic_alloc += sizeof(*s->deblock);
//    s->dynamic_alloc += pic_size;

    if (!s->sao || !s->deblock)
        goto fail;

    s->skip_flag    = av_malloc(sps->min_cb_height * sps->min_cb_width);
    s->tab_ct_depth = av_malloc(sps->min_cb_height * sps->min_cb_width);
    s->dynamic_alloc += (sps->min_cb_height * sps->min_cb_width);
    s->dynamic_alloc += (sps->min_cb_height * sps->min_cb_width);
#if PARALLEL_SLICE
    s->decoded_rows = av_malloc(sps->ctb_height);
#endif
    if (!s->skip_flag || !s->tab_ct_depth)
        goto fail;

    s->cbf_luma = av_malloc(sps->min_tb_width * sps->min_tb_height);
    s->tab_ipm  = av_mallocz(min_pu_size);
    s->is_pcm   = av_mallocz(min_pu_size);

    s->dynamic_alloc += (sps->min_tb_width * sps->min_tb_height);
    s->dynamic_alloc += min_pu_size;
    s->dynamic_alloc += min_pu_size;
    if (!s->tab_ipm || !s->cbf_luma || !s->is_pcm)
        goto fail;

    s->filter_slice_edges = av_malloc(ctb_count);
    s->tab_slice_address  = av_malloc(pic_size_in_ctb *
                                      sizeof(*s->tab_slice_address));
    s->qp_y_tab           = av_malloc(pic_size_in_ctb *
                                      sizeof(*s->qp_y_tab));

    s->dynamic_alloc += ctb_count;
    s->dynamic_alloc += (pic_size_in_ctb *
                         sizeof(*s->tab_slice_address));
    s->dynamic_alloc += (pic_size_in_ctb *
                         sizeof(*s->qp_y_tab));
    if (!s->qp_y_tab || !s->filter_slice_edges || !s->tab_slice_address)
        goto fail;

    
    s->horizontal_bs = av_mallocz((s->bs_width+(4*(1 << sps->hshift[1]))) * s->bs_height);
    s->vertical_bs   = av_mallocz(s->bs_width * (s->bs_height+4*(1 << sps->vshift[1])));
    
    s->dynamic_alloc += (s->bs_width * s->bs_height);
    s->dynamic_alloc += (s->bs_width * s->bs_height);
    if (!s->horizontal_bs || !s->vertical_bs)
        goto fail;

    s->tab_mvf_pool = av_buffer_pool_init(min_pu_size * sizeof(MvField),
                                          av_buffer_allocz);
    s->rpl_tab_pool = av_buffer_pool_init(ctb_count * sizeof(RefPicListTab),
                                          av_buffer_allocz);
    s->dynamic_alloc += (min_pu_size * sizeof(MvField));
    s->dynamic_alloc += (ctb_count * sizeof(RefPicListTab));

    if (!s->tab_mvf_pool || !s->rpl_tab_pool)
        goto fail;
#ifdef SVC_EXTENSION
    if(s->decoder_id)    {

#if ACTIVE_BOTH_FRAME_AND_PU
        s->buffer_frame[0] = av_malloc(pic_size*sizeof(short));
        s->buffer_frame[1] = av_malloc((pic_size>>2)*sizeof(short));
        s->buffer_frame[2] = av_malloc((pic_size>>2)*sizeof(short));
        s->is_upsampled = av_malloc(sps->ctb_width * sps->ctb_height);
        s->dynamic_alloc += (sps->ctb_width * sps->ctb_height);
#else
#if !ACTIVE_PU_UPSAMPLING
        s->buffer_frame[0] = av_malloc(pic_size*sizeof(short));
        s->buffer_frame[1] = av_malloc((pic_size>>2)*sizeof(short));
        s->buffer_frame[2] = av_malloc((pic_size>>2)*sizeof(short));
#else
        s->is_upsampled = av_malloc(sps->ctb_width * sps->ctb_height);
        s->dynamic_alloc += (sps->ctb_width * sps->ctb_height);
#endif
#endif
    }
#endif

#if 0
    printf("dynamic #*# %ld #*#  %d #*# \n", s->dynamic_alloc, s->decoder_id );
#endif
    return 0;
fail:
    pic_arrays_free(s);
    return AVERROR(ENOMEM);
}

static void pred_weight_table(HEVCContext *s, GetBitContext *gb)
{
    int i = 0;
    int j = 0;
    uint8_t luma_weight_l0_flag[16];
    uint8_t chroma_weight_l0_flag[16];
    uint8_t luma_weight_l1_flag[16];
    uint8_t chroma_weight_l1_flag[16];

    s->sh.luma_log2_weight_denom = get_ue_golomb_long(gb);
    s->sh.chroma_log2_weight_denom = 0;
    if (s->sps->chroma_array_type != 0) {
        int delta = get_se_golomb(gb);
        s->sh.chroma_log2_weight_denom = av_clip(s->sh.luma_log2_weight_denom + delta, 0, 7);
    }

    for (i = 0; i < s->sh.nb_refs[L0]; i++) {
        luma_weight_l0_flag[i] = get_bits1(gb);
        if (!luma_weight_l0_flag[i]) {
            s->sh.luma_weight_l0[i] = 1 << s->sh.luma_log2_weight_denom;
            s->sh.luma_offset_l0[i] = 0;
        }
    }
    if (s->sps->chroma_array_type != 0) {
        for (i = 0; i < s->sh.nb_refs[L0]; i++)
            chroma_weight_l0_flag[i] = get_bits1(gb);
    } else {
        for (i = 0; i < s->sh.nb_refs[L0]; i++)
            chroma_weight_l0_flag[i] = 0;
    }
    for (i = 0; i < s->sh.nb_refs[L0]; i++) {
        if (luma_weight_l0_flag[i]) {
            int delta_luma_weight_l0 = get_se_golomb(gb);
            s->sh.luma_weight_l0[i] = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l0;
            s->sh.luma_offset_l0[i] = get_se_golomb(gb);
        }
        if (chroma_weight_l0_flag[i]) {
            for (j = 0; j < 2; j++) {
                int delta_chroma_weight_l0 = get_se_golomb(gb);
                int delta_chroma_offset_l0 = get_se_golomb(gb);
                s->sh.chroma_weight_l0[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l0;
                s->sh.chroma_offset_l0[i][j] = av_clip((delta_chroma_offset_l0 - ((128 * s->sh.chroma_weight_l0[i][j])
                                                                                    >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
            }
        } else {
            s->sh.chroma_weight_l0[i][0] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][0] = 0;
            s->sh.chroma_weight_l0[i][1] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][1] = 0;
        }
    }
    if (s->sh.slice_type == B_SLICE) {
        for (i = 0; i < s->sh.nb_refs[L1]; i++) {
            luma_weight_l1_flag[i] = get_bits1(gb);
            if (!luma_weight_l1_flag[i]) {
                s->sh.luma_weight_l1[i] = 1 << s->sh.luma_log2_weight_denom;
                s->sh.luma_offset_l1[i] = 0;
            }
        }
        if (s->sps->chroma_array_type != 0) {
            for (i = 0; i < s->sh.nb_refs[L1]; i++)
                chroma_weight_l1_flag[i] = get_bits1(gb);
        } else {
            for (i = 0; i < s->sh.nb_refs[L1]; i++)
                chroma_weight_l1_flag[i] = 0;
        }
        for (i = 0; i < s->sh.nb_refs[L1]; i++) {
            if (luma_weight_l1_flag[i]) {
                int delta_luma_weight_l1 = get_se_golomb(gb);
                s->sh.luma_weight_l1[i] = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l1;
                s->sh.luma_offset_l1[i] = get_se_golomb(gb);
            }
            if (chroma_weight_l1_flag[i]) {
                for (j = 0; j < 2; j++) {
                    int delta_chroma_weight_l1 = get_se_golomb(gb);
                    int delta_chroma_offset_l1 = get_se_golomb(gb);
                    s->sh.chroma_weight_l1[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l1;
                    s->sh.chroma_offset_l1[i][j] = av_clip((delta_chroma_offset_l1 - ((128 * s->sh.chroma_weight_l1[i][j])
                                                                                        >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
                }
            } else {
                s->sh.chroma_weight_l1[i][0] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][0] = 0;
                s->sh.chroma_weight_l1[i][1] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][1] = 0;
            }
        }
    }
}

static int decode_lt_rps(HEVCContext *s, LongTermRPS *rps, GetBitContext *gb)
{
    const HEVCSPS *sps = s->sps;
    int max_poc_lsb    = 1 << sps->log2_max_poc_lsb;
    int prev_delta_msb = 0;
    unsigned int nb_sps = 0, nb_sh;
    int i;

    rps->nb_refs = 0;
    if (!sps->long_term_ref_pics_present_flag)
        return 0;

    if (sps->num_long_term_ref_pics_sps > 0) {
        nb_sps = get_ue_golomb_long(gb);
        print_cabac("num_long_term_sps", nb_sps);
    }
    nb_sh = get_ue_golomb_long(gb);
    print_cabac("num_long_term_pics", nb_sh);

    if (nb_sh + nb_sps > FF_ARRAY_ELEMS(rps->poc))
        return AVERROR_INVALIDDATA;

    rps->nb_refs = nb_sh + nb_sps;

    for (i = 0; i < rps->nb_refs; i++) {
        uint8_t delta_poc_msb_present;

        if (i < nb_sps) {
            uint8_t lt_idx_sps = 0;

            if (sps->num_long_term_ref_pics_sps > 1) {
                lt_idx_sps = get_bits(gb, av_ceil_log2(sps->num_long_term_ref_pics_sps));
                print_cabac("lt_idx_sps", lt_idx_sps);
            }

            rps->poc[i]  = sps->lt_ref_pic_poc_lsb_sps[lt_idx_sps];
            rps->used[i] = sps->used_by_curr_pic_lt_sps_flag[lt_idx_sps];
        } else {
            rps->poc[i]  = get_bits(gb, sps->log2_max_poc_lsb);
            rps->used[i] = get_bits1(gb);
            print_cabac("poc_lsb_lt %d \n", rps->poc[i]);
            print_cabac("used_by_curr_pic_lt_flag", rps->used[i]);
        }

        delta_poc_msb_present = get_bits1(gb);
        print_cabac("delta_poc_msb_present_flag", delta_poc_msb_present);
        if (delta_poc_msb_present) {
            int delta = get_ue_golomb_long(gb);
            print_cabac("delta_poc_msb_cycle_lt", delta);

            if (i && i != nb_sps)
                delta += prev_delta_msb;

            rps->poc[i] += s->poc - delta * max_poc_lsb - s->sh.pic_order_cnt_lsb;
            prev_delta_msb = delta;
        }
    }

    return 0;
}

static int get_buffer_sao(HEVCContext *s, AVFrame *frame, const HEVCSPS *sps)
{
    int ret, i;

    frame->width  = s->avctx->width  + 2;
    frame->height = s->avctx->height + 2;
    if ((ret = ff_get_buffer(s->avctx, frame, AV_GET_BUFFER_FLAG_REF)) < 0)
        return ret;
    for (i = 0; frame->data[i]; i++) {
        int offset = frame->linesize[i] + (1 << sps->pixel_shift);
        frame->data[i] += offset;
    }
    frame->width  = s->avctx->width;
    frame->height = s->avctx->height;


    return 0;
}

static int set_sps(HEVCContext *s, const HEVCSPS *sps)
{
    int ret;
    unsigned int num = 0, den = 0;

    pic_arrays_free(s);
    ret = pic_arrays_init(s, sps);
    if (ret < 0)
        goto fail;

    s->avctx->coded_width         = sps->width;
    s->avctx->coded_height        = sps->height;
    s->avctx->width               = sps->output_width;
    s->avctx->height              = sps->output_height;
    s->avctx->pix_fmt             = sps->pix_fmt;
    s->avctx->sample_aspect_ratio = sps->vui.sar;
    s->avctx->has_b_frames        = sps->temporal_layer[sps->max_sub_layers - 1].num_reorder_pics;

    if (sps->vui.video_signal_type_present_flag)
        s->avctx->color_range = sps->vui.video_full_range_flag ? AVCOL_RANGE_JPEG
                                                               : AVCOL_RANGE_MPEG;
    else
        s->avctx->color_range = AVCOL_RANGE_MPEG;

    if (sps->vui.colour_description_present_flag) {
        s->avctx->color_primaries = sps->vui.colour_primaries;
        s->avctx->color_trc       = sps->vui.transfer_characteristic;
        s->avctx->colorspace      = sps->vui.matrix_coeffs;
    } else {
        s->avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
        s->avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
        s->avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;
    }

    ff_hevc_pred_init(&s->hpc,     sps->bit_depth);
    ff_hevc_dsp_init (&s->hevcdsp, sps->bit_depth);
    ff_videodsp_init (&s->vdsp,    sps->bit_depth);

    if (sps->sao_enabled) {
        av_frame_unref(s->tmp_frame);
        //Mvdecoder: create buffer
        ret = get_buffer_sao(s, s->tmp_frame, sps);
        s->sao_frame = s->tmp_frame;
    }

    s->sps = sps;
    s->vps = (HEVCVPS*) s->vps_list[s->sps->vps_id]->data;

    if (s->vps->vps_timing_info_present_flag) {
        num = s->vps->vps_num_units_in_tick;
        den = s->vps->vps_time_scale;
    } else if (sps->vui.vui_timing_info_present_flag) {
        num = sps->vui.vui_num_units_in_tick;
        den = sps->vui.vui_time_scale;
    }

    if (num != 0 && den != 0)
        av_reduce(&s->avctx->time_base.num, &s->avctx->time_base.den,
                  num, den, 1 << 30);

    if (s->decoder_id) {
        int heightBL, widthBL, heightEL, widthEL;
        const int phaseXC = 0;
        const int phaseYC = 1;
        const int phaseAlignFlag = ((HEVCVPS*)s->vps_list[s->sps->vps_id]->data)->m_phaseAlignFlag;
        const int   phaseX = phaseAlignFlag   << 1;
        const int   phaseY = phaseAlignFlag   << 1;
        HEVCSPS *bl_sps = (HEVCSPS*) s->sps_list[s->decoder_id-1]->data;
        HEVCWindow scaled_ref_layer_window;
        if(bl_sps) {
            heightBL = bl_sps->height - bl_sps->output_window.bottom_offset - bl_sps->output_window.top_offset;
            widthBL  = bl_sps->width  - bl_sps->output_window.left_offset - bl_sps->output_window.right_offset;
        } else {
            av_log(s->avctx, AV_LOG_ERROR, "SPS Informations related to the inter layer refrence frame are missing -- \n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        if(!heightBL || !widthBL) {
            av_log(s->avctx, AV_LOG_ERROR, "Informations related to the inter layer refrence frame are missing heightBL: %d widthBL: %d \n", heightBL, widthBL);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        scaled_ref_layer_window = s->sps->scaled_ref_layer_window[((HEVCVPS*)s->vps_list[s->sps->vps_id]->data)->m_refLayerId[s->nuh_layer_id][0]]; // m_phaseAlignFlag;

        heightEL = s->sps->height - scaled_ref_layer_window.bottom_offset   - scaled_ref_layer_window.top_offset;
        widthEL  = s->sps->width  - scaled_ref_layer_window.left_offset     - scaled_ref_layer_window.right_offset;

        s->sh.ScalingFactor[s->nuh_layer_id][0]   = av_clip_c(((widthEL  << 8) + (widthBL  >> 1)) / widthBL,  -4096, 4095 );
        s->sh.ScalingFactor[s->nuh_layer_id][1]   = av_clip_c(((heightEL << 8) + (heightBL >> 1)) / heightBL, -4096, 4095 );
        s->up_filter_inf.scaleXLum = ( ( widthBL << 16 )  + ( widthEL >> 1 ) ) / widthEL ;
        s->up_filter_inf.scaleYLum = ( ( heightBL << 16 ) + ( heightEL >> 1 ) ) / heightEL;

        s->up_filter_inf.addXLum   = (( phaseX * s->up_filter_inf.scaleXLum + 2 ) >> 2 )+ ( 1 << 11 );
        s->up_filter_inf.addYLum   = (( phaseY * s->up_filter_inf.scaleYLum + 2 ) >> 2 )+ ( 1 << 11 );

        widthBL  >>= 1;
        heightBL >>= 1;

        s->up_filter_inf.addXCr   = ( ((phaseXC+phaseAlignFlag) * s->up_filter_inf.scaleXLum + 2) >> 2) + ( 1 << 11 );
        s->up_filter_inf.addYCr   = ( ((phaseYC+phaseAlignFlag) * s->up_filter_inf.scaleYLum + 2) >> 2) + ( 1 << 11 );
        s->up_filter_inf.scaleXCr     = s->up_filter_inf.scaleXLum;
        s->up_filter_inf.scaleYCr     = s->up_filter_inf.scaleYLum;

        if(s->up_filter_inf.scaleXLum == 65536 && s->up_filter_inf.scaleYLum == 65536)
            s->up_filter_inf.idx = SNR;
        else
            if(s->up_filter_inf.scaleXLum == 32768 && s->up_filter_inf.scaleYLum == 32768)
                s->up_filter_inf.idx = X2;
            else
                if(s->up_filter_inf.scaleXLum == 43691 && s->up_filter_inf.scaleYLum == 43691)
                    s->up_filter_inf.idx = X1_5;
                else {
                    s->up_filter_inf.idx = DEFAULT;
                    av_log(s->avctx, AV_LOG_INFO, "DEFAULT mode: SSE optimizations are not implemented for spatial scalability with a ratio different from x2 and x1.5 widthBL %d heightBL %d \n", widthBL<<1, heightBL<<1);
                }
    }
    return 0;
fail:
    pic_arrays_free(s);
    s->sps = NULL;
    return ret;
}

static int is_sps_exist(HEVCContext *s, const HEVCSPS* last_sps)
{
    int i;

    for( i = 0; i < MAX_SPS_COUNT; i++)
        if(s->sps_list[i])
            if (last_sps == (HEVCSPS*)s->sps_list[i]->data)
                return 1;
    return 0;
}

static int hls_slice_header(HEVCContext *s)
{
    GetBitContext *gb   = &s->HEVClc->gb;
    SliceHeader   *sh   = &s->sh;
    HEVCContext   *s1   = (HEVCContext*) s->avctx->priv_data;
    int i, j, ret;

    print_cabac("\n --- Decode slice header --- \n", s->nuh_layer_id);
#if JCTVC_M0458_INTERLAYER_RPS_SIG
    int NumILRRefIdx;
#endif
    int first_slice_in_pic_flag = get_bits1(gb);

#if PARALLEL_SLICE
    if (IS_IRAP(s)) {
        if(!first_slice_in_pic_flag) {
            int self_id, temporal_id, nuh_layer_id, nal_unit_type, job;
            nal_unit_type = s->nal_unit_type;
            self_id       = s->self_id;
            temporal_id   = s->temporal_id;
            nuh_layer_id  = s->nuh_layer_id;
            job           = s->job;
            ff_thread_await_progress_slice(s->avctx);
            memcpy(s, s1, sizeof(HEVCContext));
            s->HEVClc                 = s1->HEVClcList[self_id];
            s->nal_unit_type          = nal_unit_type;
            s->temporal_id            = temporal_id;
            s->nuh_layer_id           = nuh_layer_id;
            s->job                    = job;
        }
    }
#endif
    sh->first_slice_in_pic_flag   = first_slice_in_pic_flag;

	if (s1->force_first_slice_in_pic) {
		av_log(s->avctx, AV_LOG_DEBUG, "First_slice_in_pic_flag forced\n");
		s1->force_first_slice_in_pic = 0;
		sh->first_slice_in_pic_flag = 1;
	}

    print_cabac("first_slice_segment_in_pic_flag", sh->first_slice_in_pic_flag);
    if ((IS_IDR(s) || IS_BLA(s)) && sh->first_slice_in_pic_flag) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
        if (IS_IDR(s))
            ff_hevc_clear_refs(s);
    }
    sh->no_output_of_prior_pics_flag = 0;
    if (IS_IRAP(s)) {
        sh->no_output_of_prior_pics_flag = get_bits1(gb);
        print_cabac("no_output_of_prior_pics_flag", sh->no_output_of_prior_pics_flag);
        if (s->decoder_id)
            av_log(s->avctx, AV_LOG_ERROR, "IRAP %d\n", s->nal_unit_type);
    }

    if (s->nal_unit_type == NAL_CRA_NUT && s->last_eos == 1)
        sh->no_output_of_prior_pics_flag = 1;

    sh->pps_id = get_ue_golomb_long(gb);
    
    print_cabac("slice_pic_parameter_set_id", sh->pps_id);
    if (sh->pps_id >= MAX_PPS_COUNT || !s->pps_list[sh->pps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", sh->pps_id);
        return AVERROR_INVALIDDATA;
    }
    if (!sh->first_slice_in_pic_flag &&
        s->pps != (HEVCPPS*)s->pps_list[sh->pps_id]->data) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS changed between slices.\n");
        return AVERROR_INVALIDDATA;
    }
    s->pps = (HEVCPPS*)s->pps_list[sh->pps_id]->data;

    if (s->sps != (HEVCSPS*)s->sps_list[s->pps->sps_id]->data) {
        const HEVCSPS* last_sps = s->sps;
        s->sps = (HEVCSPS*)s->sps_list[s->pps->sps_id]->data;
        if (last_sps) {
            if (is_sps_exist(s, last_sps)) {
                if (s->sps->width !=  last_sps->width || s->sps->height != last_sps->height ||
                        s->sps->temporal_layer[s->sps->max_sub_layers - 1].max_dec_pic_buffering != last_sps->temporal_layer[last_sps->max_sub_layers - 1].max_dec_pic_buffering)
                    sh->no_output_of_prior_pics_flag = 0;
            }
        }
        ff_hevc_clear_refs(s);
        
        ret = set_sps(s, s->sps);
        if (ret < 0)
            return ret;

        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
    }

    s->avctx->profile = s->sps->ptl.general_ptl.profile_idc;
    s->avctx->level   = s->sps->ptl.general_ptl.level_idc;

    sh->dependent_slice_segment_flag = 0;
    if (!first_slice_in_pic_flag) {
        int slice_address_length;
        if (s->pps->dependent_slice_segments_enabled_flag) {
            sh->dependent_slice_segment_flag = get_bits1(gb);
            print_cabac("dependent_slice_segment_flag", sh->dependent_slice_segment_flag);
        }

        slice_address_length = av_ceil_log2(s->sps->ctb_width *
                                            s->sps->ctb_height);
        sh->slice_segment_addr = get_bits(gb, slice_address_length);
#if PARALLEL_SLICE
        s1->slice_segment_addr[s->job] = sh->slice_segment_addr;
        ff_thread_report_progress_slice2(s->avctx, s->job);
#endif

        print_cabac("slice_segment_address", sh->slice_segment_addr );
        if (sh->slice_segment_addr >= s->sps->ctb_width * s->sps->ctb_height) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Invalid slice segment address: %u.\n",
                   sh->slice_segment_addr);
            return AVERROR_INVALIDDATA;
        }

        if (!sh->dependent_slice_segment_flag) {
            sh->slice_addr = sh->slice_segment_addr;
#if PARALLEL_SLICE
if (0)
    s->slice_idx = s->job;
else
    s->slice_idx++;

#else
            s->slice_idx++;
#endif
        }
    } else {
        sh->slice_segment_addr = sh->slice_addr = 0;
        s->slice_idx           = 0;
        s->slice_initialized   = 0;
    }

    if (!sh->dependent_slice_segment_flag) {
        s->slice_initialized = 0;

#if SVC_EXTENSION
#if POC_RESET_FLAG
        {
            int iBits = 0;
            if(s->pps->num_extra_slice_header_bits > iBits) {
                sh->m_bPocResetFlag = get_bits1(gb);
                print_cabac("poc_reset_flag", sh->m_bPocResetFlag);
                iBits++;
            }
            if(s->pps->num_extra_slice_header_bits > iBits) {
                skip_bits1(gb);
                print_cabac("skip  ", 0);
                iBits++;
            }
#if O0149_CROSS_LAYER_BLA_FLAG
            if(s->pps->num_extra_slice_header_bits > iBits) {
                sh->m_bCrossLayerBLAFlag = get_bits1(gb);
                print_cabac("cross_layer_bla_flag", sh->m_bCrossLayerBLAFlag);
                iBits++;
            }
#endif
           for (; iBits < s->pps->num_extra_slice_header_bits; iBits++) {
                skip_bits1(gb);
                print_cabac("skip ", 0);
            }
#else
            if(s->pps->num_extra_slice_header_bits>0) {
                skip_bits1(gb);
                print_cabac("skip ", 0);
            }
            for ( i = 1; i < s->pps->num_extra_slice_header_bits; i++) {
                skip_bits1(gb);
                print_cabac("skip ", 0);
            }
#endif
        }
#else //SVC_EXTENSION
        for (i = 0; i < s->pps->num_extra_slice_header_bits; i++){
            skip_bits(gb, 1);  // slice_reserved_undetermined_flag[]
            print_cabac("skip ", 0);
        }
#endif //SVC_EXTENSION

        sh->slice_type = get_ue_golomb_long(gb);
        print_cabac("slice_type", sh->slice_type);
        if (!(sh->slice_type == I_SLICE ||
              sh->slice_type == P_SLICE ||
              sh->slice_type == B_SLICE)) {
            av_log(s->avctx, AV_LOG_ERROR, "Unknown slice type: %d %d .\n",
                   sh->slice_type, sh->first_slice_in_pic_flag);
            return AVERROR_INVALIDDATA;
        }
        if (!s->decoder_id && IS_IRAP(s) && sh->slice_type != I_SLICE) {
            av_log(s->avctx, AV_LOG_ERROR, "Inter slices in an IRAP frame.\n");
            return AVERROR_INVALIDDATA;
        }

        sh->pic_output_flag = 1;
        if (s->pps->output_flag_present_flag) {
            sh->pic_output_flag = get_bits1(gb);
            print_cabac("pic_output_flag", sh->pic_output_flag);
        }

        if (s->sps->separate_colour_plane_flag) {
            sh->colour_plane_id = get_bits(gb, 2);
            print_cabac("pic_output_flag", sh->pic_output_flag);
        }
        print_cabac("s->nal_unit_type", s->nal_unit_type);
        if (( s->nuh_layer_id > 0 && !s->vps->m_pocLsbNotPresentFlag[s->vps->m_layerIdInVps[s->nuh_layer_id]] )
            || (!IS_IDR(s))) {
            int poc;

            sh->pic_order_cnt_lsb = get_bits(gb, s->sps->log2_max_poc_lsb);
            print_cabac("pic_order_cnt_lsb", sh->pic_order_cnt_lsb);
            poc = ff_hevc_compute_poc(s, sh->pic_order_cnt_lsb);
            if (!sh->first_slice_in_pic_flag && poc != s->poc) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Ignoring POC change between slices: %d -> %d\n", s->poc, poc);
                if (s->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
                poc = s->poc;
            }
            s->poc = poc;
        }
#if SIM_ERROR_CONCEALMENT
        av_log(s->avctx, AV_LOG_ERROR, "Poc to decode: %d \n", s->poc);
        if((s->poc % 8) == 6 /*|| (s->poc % 8) == 3 || (s->poc % 8)  == 5 || (s->poc % 8) == 7*/){
#if FRAME_CONCEALMENT
            ret = ff_hevc_output_frame(s, s->output_frame, 0);
#endif
            return -10;
        }
#endif
        if(!IS_IDR(s)) {
            int short_term_ref_pic_set_sps_flag = get_bits1(gb);
            print_cabac("short_term_ref_pic_set_sps_fla", short_term_ref_pic_set_sps_flag);
            if (!short_term_ref_pic_set_sps_flag) {
                ret = ff_hevc_decode_short_term_rps(s, &sh->slice_rps, s->sps, 1);
                if (ret < 0)
                    return ret;

                sh->short_term_rps = &sh->slice_rps;
            } else {
                int numbits, rps_idx;

                if (!s->sps->nb_st_rps) {
                    av_log(s->avctx, AV_LOG_ERROR, "No ref lists in the SPS.\n");
                    return AVERROR_INVALIDDATA;
                }

                numbits = av_ceil_log2(s->sps->nb_st_rps);
                rps_idx = numbits > 0 ? get_bits(gb, numbits) : 0;
                print_cabac("short_term_ref_pic_set_idx", rps_idx);
                sh->short_term_rps = &s->sps->st_rps[rps_idx];
            }

            ret = decode_lt_rps(s, &sh->long_term_rps, gb);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING, "Invalid long term RPS.\n");
                if (s->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
            }

            if (s->sps->sps_temporal_mvp_enabled_flag) {
                sh->slice_temporal_mvp_enabled_flag = get_bits1(gb);
                print_cabac("slice_temporal_mvp_enable_flag", sh->slice_temporal_mvp_enabled_flag);
            } else
                sh->slice_temporal_mvp_enabled_flag = 0;
        } else {
            s->sh.short_term_rps = NULL;
            s->poc               = 0;
        }

        /* 8.3.1 */
        if (s->temporal_id == 0 &&
            s->nal_unit_type != NAL_TRAIL_N &&
            s->nal_unit_type != NAL_TSA_N   &&
            s->nal_unit_type != NAL_STSA_N  &&
            s->nal_unit_type != NAL_RADL_N  &&
            s->nal_unit_type != NAL_RADL_R  &&
            s->nal_unit_type != NAL_RASL_N  &&
            s->nal_unit_type != NAL_RASL_R)
            s->pocTid0 = s->poc;
#ifdef REF_IDX_FRAMEWORK
#ifdef JCTVC_M0458_INTERLAYER_RPS_SIG
        s->sh.active_num_ILR_ref_idx = 0;
        NumILRRefIdx = s->vps->m_numDirectRefLayers[s->nuh_layer_id];
        if (s->nuh_layer_id > 0 && NumILRRefIdx>0) {
            s->sh.inter_layer_pred_enabled_flag = get_bits1(gb);
            print_cabac("inter_layer_pred_enabled_flag", s->sh.inter_layer_pred_enabled_flag );
            if (s->sh.inter_layer_pred_enabled_flag) {
                if (NumILRRefIdx>1)  {
                    int numBits = 1;
                    while ((1 << numBits) < NumILRRefIdx) {
                        numBits++;
                    }
                    if (!s->vps->max_one_active_ref_layer_flag) {
                        s->sh.active_num_ILR_ref_idx = get_bits(gb, numBits) + 1;
                        print_cabac("num_inter_layer_ref_pics_minus1", s->sh.active_num_ILR_ref_idx);
                    } else
                        s->sh.active_num_ILR_ref_idx = 1;
                    for (i = 0; i < s->sh.active_num_ILR_ref_idx; i++ ) {
                        s->sh.inter_layer_pred_layer_idc[i] =  get_bits(gb, numBits);
                        print_cabac("inter_layer_pred_layer_idc", s->sh.inter_layer_pred_layer_idc[i]);
                    }
                } else {
                    s->sh.active_num_ILR_ref_idx = 1;
                    s->sh.inter_layer_pred_layer_idc[0] = 0;
                }
            }
        }
#else
        if (s->nuh_layer_id > 0)
            s->sh.active_num_ILR_ref_idx = s->vps->m_numDirectRefLayers[sc->layer_id];
#endif
#endif
        if (s->sps->sao_enabled) {
            sh->slice_sample_adaptive_offset_flag[0] = get_bits1(gb);
            print_cabac("slice_sao_luma_flag", sh->slice_sample_adaptive_offset_flag[0] );
            sh->slice_sample_adaptive_offset_flag[1] =
            sh->slice_sample_adaptive_offset_flag[2] = get_bits1(gb);
            print_cabac("slice_sao_chroma_flag", sh->slice_sample_adaptive_offset_flag[2]);
        } else {
            sh->slice_sample_adaptive_offset_flag[0] = 0;
            sh->slice_sample_adaptive_offset_flag[1] = 0;
            sh->slice_sample_adaptive_offset_flag[2] = 0;
        }

        sh->nb_refs[L0] = sh->nb_refs[L1] = 0;
        if (sh->slice_type == P_SLICE || sh->slice_type == B_SLICE) {
            int nb_refs, num_ref_idx_active_override_flag;

            sh->nb_refs[L0] = s->pps->num_ref_idx_l0_default_active;
            if (sh->slice_type == B_SLICE)
                sh->nb_refs[L1] = s->pps->num_ref_idx_l1_default_active;
            num_ref_idx_active_override_flag = get_bits1(gb);
            if (num_ref_idx_active_override_flag) { // num_ref_idx_active_override_flag
                sh->nb_refs[L0] = get_ue_golomb_long(gb) + 1;
                print_cabac("num_ref_idx_l0_active_minus1", sh->nb_refs[L0] -1);
                if (sh->slice_type == B_SLICE) {
                    sh->nb_refs[L1] = get_ue_golomb_long(gb) + 1;
                    print_cabac("num_ref_idx_l1_active_minus1", sh->nb_refs[L1]-1);
                }
            }
            if (sh->nb_refs[L0] > MAX_REFS || sh->nb_refs[L1] > MAX_REFS) {
                av_log(s->avctx, AV_LOG_ERROR, "Too many refs: %d/%d.\n",
                       sh->nb_refs[L0], sh->nb_refs[L1]);
                return AVERROR_INVALIDDATA;
            }

            sh->rpl_modification_flag[0] = 0;
            sh->rpl_modification_flag[1] = 0;
            nb_refs = ff_hevc_frame_nb_refs(s);
            if (!nb_refs) {
                av_log(s->avctx, AV_LOG_ERROR, "Zero refs for a frame with P or B slices.\n");
                return AVERROR_INVALIDDATA;
            }

            if (s->pps->lists_modification_present_flag && nb_refs > 1) {
                sh->rpl_modification_flag[0] = get_bits1(gb);
                print_cabac("ref_pic_list_modification_flag_l0", sh->rpl_modification_flag[0]);
                if (sh->rpl_modification_flag[0]) {
                    for (i = 0; i < sh->nb_refs[L0]; i++) {
                        sh->list_entry_lx[0][i] = get_bits(gb, av_ceil_log2(nb_refs));
                        print_cabac("list_entry_l0", sh->list_entry_lx[0][i]);
                    }
                }

                if (sh->slice_type == B_SLICE) {
                    sh->rpl_modification_flag[1] = get_bits1(gb);
                    print_cabac("ref_pic_list_modification_flag_l1", sh->rpl_modification_flag[1]);
                    if (sh->rpl_modification_flag[1] == 1)
                        for (i = 0; i < sh->nb_refs[L1]; i++) {
                            sh->list_entry_lx[1][i] = get_bits(gb, av_ceil_log2(nb_refs));
                            print_cabac("list_entry_l1", sh->list_entry_lx[1][i]);
                        }
                }
            }

            if (sh->slice_type == B_SLICE) {
                sh->mvd_l1_zero_flag = get_bits1(gb);
                print_cabac("mvd_l1_zero_flag", sh->mvd_l1_zero_flag);
            }

            if (s->pps->cabac_init_present_flag) {
                sh->cabac_init_flag = get_bits1(gb);
                print_cabac("cabac_init_flag", sh->cabac_init_flag);
            } else
                sh->cabac_init_flag = 0;

            sh->collocated_ref_idx = 0;
            if (sh->slice_temporal_mvp_enabled_flag) {
                sh->collocated_list = L0;
                if (sh->slice_type == B_SLICE) {
                    sh->collocated_list = !get_bits1(gb);
                    print_cabac("collocated_from_l0_flag", sh->collocated_list);
                }

                if (sh->nb_refs[sh->collocated_list] > 1) {
                    sh->collocated_ref_idx = get_ue_golomb_long(gb);
                    print_cabac("collocated_ref_idx", sh->collocated_ref_idx);
                    if (sh->collocated_ref_idx >= sh->nb_refs[sh->collocated_list]) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "Invalid collocated_ref_idx: %d.\n",
                               sh->collocated_ref_idx);
                        return AVERROR_INVALIDDATA;
                    }
                }
            }

            if ((s->pps->weighted_pred_flag   && sh->slice_type == P_SLICE) ||
                (s->pps->weighted_bipred_flag && sh->slice_type == B_SLICE)) {
                pred_weight_table(s, gb);
            }

            sh->max_num_merge_cand = 5 - get_ue_golomb_long(gb);
            print_cabac("five_minus_max_num_merge_cand", 5-sh->max_num_merge_cand);
            if (sh->max_num_merge_cand < 1 || sh->max_num_merge_cand > 5) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "Invalid number of merging MVP candidates: %d.\n",
                       sh->max_num_merge_cand);
                return AVERROR_INVALIDDATA;
            }
        }

        sh->slice_qp_delta = get_se_golomb(gb);
        print_cabac("slice_qp_delta", sh->slice_qp_delta);
        if (s->pps->pic_slice_level_chroma_qp_offsets_present_flag) {
            sh->slice_cb_qp_offset = get_se_golomb(gb);
            print_cabac("slice_qp_delta_cb", sh->slice_cb_qp_offset);
            sh->slice_cr_qp_offset = get_se_golomb(gb);
            print_cabac("slice_qp_delta_cr", sh->slice_cr_qp_offset);
        } else {
            sh->slice_cb_qp_offset = 0;
            sh->slice_cr_qp_offset = 0;
        }

        if (s->pps->chroma_qp_offset_list_enabled_flag)
            sh->cu_chroma_qp_offset_enabled_flag = get_bits1(gb);
        else
            sh->cu_chroma_qp_offset_enabled_flag = 0;

        if (s->pps->deblocking_filter_control_present_flag) {
            int deblocking_filter_override_flag = 0;

            if (s->pps->deblocking_filter_override_enabled_flag) {
                deblocking_filter_override_flag = get_bits1(gb);
                print_cabac("deblocking_filter_override_flag", deblocking_filter_override_flag);
            }

            if (deblocking_filter_override_flag) {
                sh->disable_deblocking_filter_flag = get_bits1(gb);
                print_cabac("slice_disable_deblocking_filter_flag", sh->disable_deblocking_filter_flag);
                if (!sh->disable_deblocking_filter_flag) {
                    sh->beta_offset = get_se_golomb(gb) * 2;
                    print_cabac("slice_beta_offset_div2", sh->beta_offset);
                    sh->tc_offset   = get_se_golomb(gb) * 2;
                    print_cabac("slice_tc_offset_div2", sh->tc_offset);
                }
            } else {
                sh->disable_deblocking_filter_flag = s->pps->disable_dbf;
                sh->beta_offset                    = s->pps->beta_offset;
                sh->tc_offset                      = s->pps->tc_offset;
            }
        } else {
            sh->disable_deblocking_filter_flag = 0;
            sh->beta_offset                    = 0;
            sh->tc_offset                      = 0;
        }

        if (s->pps->seq_loop_filter_across_slices_enabled_flag &&
            (sh->slice_sample_adaptive_offset_flag[0] ||
             sh->slice_sample_adaptive_offset_flag[1] ||
             !sh->disable_deblocking_filter_flag)) {
            sh->slice_loop_filter_across_slices_enabled_flag = get_bits1(gb);
            print_cabac("slice_loop_filter_across_slices_enabled_flag", sh->slice_loop_filter_across_slices_enabled_flag);
        } else {
            sh->slice_loop_filter_across_slices_enabled_flag = s->pps->seq_loop_filter_across_slices_enabled_flag;
        } 
    } else if (!s->slice_initialized) {
        av_log(s->avctx, AV_LOG_ERROR, "Independent slice segment missing.\n");
        return AVERROR_INVALIDDATA;
    }

    sh->num_entry_point_offsets = 0;
    if (s->pps->tiles_enabled_flag || s->pps->entropy_coding_sync_enabled_flag) {
        sh->num_entry_point_offsets = get_ue_golomb_long(gb);
        print_cabac("num_entry_point_offsets", sh->num_entry_point_offsets);
        if(s->pps->entropy_coding_sync_enabled_flag) {
            if(sh->num_entry_point_offsets < 0) {
                av_log(s->avctx, AV_LOG_ERROR,
                   "The number of entries %d is higher than the number of CTB rows %d \n",
                   sh->num_entry_point_offsets,
                   s->sps->ctb_height);
                return AVERROR_INVALIDDATA;
            }
        } else {
            if(sh->num_entry_point_offsets > s->sps->ctb_height*s->sps->ctb_width || sh->num_entry_point_offsets < 0) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "The number of entries %d is higher than the number of CTBs %d \n",
                       sh->num_entry_point_offsets,
                       s->sps->ctb_height*s->sps->ctb_width);
                return AVERROR_INVALIDDATA;
            }
        }
        if (sh->num_entry_point_offsets > 0) {
            int offset_len = get_ue_golomb_long(gb) + 1;
            print_cabac("offset_len_minus1", offset_len-1);
            int segments = offset_len >> 4;
            int rest = (offset_len & 15);
            av_freep(&sh->entry_point_offset);
            av_freep(&sh->offset);
            av_freep(&sh->size);
            sh->entry_point_offset = av_malloc(sh->num_entry_point_offsets * sizeof(int));
            sh->offset = av_malloc(sh->num_entry_point_offsets * sizeof(int));
            sh->size = av_malloc(sh->num_entry_point_offsets * sizeof(int));
            if (!sh->entry_point_offset || !sh->offset || !sh->size) {
                sh->num_entry_point_offsets = 0;
                av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory\n");
                return AVERROR(ENOMEM);
            }
            for (i = 0; i < sh->num_entry_point_offsets; i++) {
                int val = 0;
                for (j = 0; j < segments; j++) {
                    val <<= 16;
                    val += get_bits(gb, 16);
                }
                if (rest) {
                    val <<= rest;
                    val += get_bits(gb, rest);
                }
                sh->entry_point_offset[i] = val + 1; // +1; // +1 to get the size
                print_cabac("entry_point_offset_minus1", sh->entry_point_offset[i] - 1);
            }
        }
    }

    if (s->pps->slice_header_extension_present_flag) {
        unsigned int length = get_ue_golomb_long(gb);
        print_cabac("slice_header_extension_length", length);
        av_log(s->avctx, AV_LOG_ERROR,
               "========= SLICE HEADER extension not supported yet\n");
        for (i = 0; i < length; i++)
            skip_bits(gb, 8);  // slice_header_extension_data_byte
    }

    // Inferred parameters
    sh->slice_qp = 26U + s->pps->pic_init_qp_minus26 + sh->slice_qp_delta;
    if (sh->slice_qp > 51 ||
        sh->slice_qp < -s->sps->qp_bd_offset) {
        av_log(s->avctx, AV_LOG_ERROR,
               "The slice_qp %d is outside the valid range "
               "[%d, 51].\n",
               sh->slice_qp,
               -s->sps->qp_bd_offset);
        return AVERROR_INVALIDDATA;
    }

    sh->slice_ctb_addr_rs = sh->slice_segment_addr;

    if (!s->sh.slice_ctb_addr_rs && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible slice segment.\n");
        return AVERROR_INVALIDDATA;
    }

    s->HEVClc->first_qp_group = !s->sh.dependent_slice_segment_flag;

    if (!s->pps->cu_qp_delta_enabled_flag)
        s->HEVClc->qp_y = s->sh.slice_qp;

    s->slice_initialized = 1;
    s->HEVClc->tu.cu_qp_offset_cb = 0;
    s->HEVClc->tu.cu_qp_offset_cr = 0;


    return 0;
}

#define CTB(tab, x, y) ((tab)[(y) * s->sps->ctb_width + (x)])

#define SET_SAO(elem, value)                            \
do {                                                    \
    if (!sao_merge_up_flag && !sao_merge_left_flag)     \
        sao->elem = value;                              \
    else if (sao_merge_left_flag)                       \
        sao->elem = CTB(s->sao, rx-1, ry).elem;         \
    else if (sao_merge_up_flag)                         \
        sao->elem = CTB(s->sao, rx, ry-1).elem;         \
    else                                                \
        sao->elem = 0;                                  \
} while (0)

static void hls_sao_param(HEVCContext *s, int rx, int ry)
{
    HEVCLocalContext *lc    = s->HEVClc;
    int sao_merge_left_flag = 0;
    int sao_merge_up_flag   = 0;
    SAOParams *sao          = &CTB(s->sao, rx, ry);
    int c_idx, i;

    if (s->sh.slice_sample_adaptive_offset_flag[0] ||
        s->sh.slice_sample_adaptive_offset_flag[1]) {
        if (rx > 0) {
            if (lc->ctb_left_flag)
                sao_merge_left_flag = ff_hevc_sao_merge_flag_decode(s);
        }
        if (ry > 0 && !sao_merge_left_flag) {
            if (lc->ctb_up_flag)
                sao_merge_up_flag = ff_hevc_sao_merge_flag_decode(s);
        }
    }

    for (c_idx = 0; c_idx < 3; c_idx++) {
        int log2_sao_offset_scale = c_idx == 0 ? s->pps->log2_sao_offset_scale_luma :
                                                 s->pps->log2_sao_offset_scale_chroma;

        if (!s->sh.slice_sample_adaptive_offset_flag[c_idx]) {
            sao->type_idx[c_idx] = SAO_NOT_APPLIED;
            continue;
        }

        if (c_idx == 2) {
            sao->type_idx[2] = sao->type_idx[1];
            sao->eo_class[2] = sao->eo_class[1];
        } else {
            SET_SAO(type_idx[c_idx], ff_hevc_sao_type_idx_decode(s));
        }

        if (sao->type_idx[c_idx] == SAO_NOT_APPLIED)
            continue;

        for (i = 0; i < 4; i++)
            SET_SAO(offset_abs[c_idx][i], ff_hevc_sao_offset_abs_decode(s));

        if (sao->type_idx[c_idx] == SAO_BAND) {
            for (i = 0; i < 4; i++) {
                if (sao->offset_abs[c_idx][i]) {
                    SET_SAO(offset_sign[c_idx][i],
                            ff_hevc_sao_offset_sign_decode(s));
                } else {
                    sao->offset_sign[c_idx][i] = 0;
                }
            }
            SET_SAO(band_position[c_idx], ff_hevc_sao_band_position_decode(s));
        } else if (c_idx != 2) {
            SET_SAO(eo_class[c_idx], ff_hevc_sao_eo_class_decode(s));
        }

        // Inferred parameters
        sao->offset_val[c_idx][0] = 0;
        for (i = 0; i < 4; i++) {
            sao->offset_val[c_idx][i + 1] = sao->offset_abs[c_idx][i];
            if (sao->type_idx[c_idx] == SAO_EDGE) {
                if (i > 1)
                    sao->offset_val[c_idx][i + 1] = -sao->offset_val[c_idx][i + 1];
            } else if (sao->offset_sign[c_idx][i]) {
                sao->offset_val[c_idx][i + 1] = -sao->offset_val[c_idx][i + 1];
            }
            sao->offset_val[c_idx][i + 1] <<= log2_sao_offset_scale;
        }
    }
}

#undef SET_SAO
#undef CTB

static int hls_cross_component_pred(HEVCContext *s, int idx) {
    HEVCLocalContext *lc    = s->HEVClc;
    int log2_res_scale_abs_plus1 = ff_hevc_log2_res_scale_abs(s, idx);

    if (log2_res_scale_abs_plus1 !=  0) {
        int res_scale_sign_flag = ff_hevc_res_scale_sign_flag(s, idx);
        lc->tu.res_scale_val = (1 << (log2_res_scale_abs_plus1 - 1)) *
                               (1 - 2 * res_scale_sign_flag);
    } else {
        lc->tu.res_scale_val = 0;
    }


    return 0;
}


//处理TU-帧内预测、DCT反变换
static int hls_transform_unit(HEVCContext *s, int x0, int y0,
                              int xBase, int yBase, int cb_xBase, int cb_yBase,
                              int log2_cb_size, int log2_trafo_size,
                              int trafo_depth, int blk_idx,
                              int cbf_luma, int *cbf_cb, int *cbf_cr)
{
    HEVCLocalContext *lc = s->HEVClc;
    const int log2_trafo_size_c = log2_trafo_size - s->sps->hshift[1];
    int i;

    if (lc->cu.pred_mode == MODE_INTRA) {
        int trafo_size = 1 << log2_trafo_size;
        ff_hevc_set_neighbour_available(s, x0, y0, trafo_size, trafo_size);


        //注意：帧内预测也是在这里完成
        //帧内预测
        //log2_trafo_size为当前TU大小取log2()之后的值
        //hls_transform_unit()会调用HEVCPredContext的intra_pred[]()汇编函数进行帧内预测；然后不论帧内预测还是帧间CU都会调用ff_hevc_hls_residual_coding()解码残差数据，并叠加在预测数据上。
        s->hpc.intra_pred[log2_trafo_size - 2](s, x0, y0, 0); //this function call Assembly function to do intra prediction and fill out PDB buffer.
        /*hpc->intra_pred[0]   = intra_pred_2_8;
        hpc->intra_pred[1]   = intra_pred_3_8;
        hpc->intra_pred[2]   = intra_pred_4_8;
        hpc->intra_pred[3]   = intra_pred_5_8;
        hpc->pred_planar[0]  = pred_planar_0_8;
        hpc->pred_planar[1]  = pred_planar_1_8;
        hpc->pred_planar[2]  = pred_planar_2_8;
        hpc->pred_planar[3]  = pred_planar_3_8;
        hpc->pred_dc         = pred_dc_8;
        hpc->pred_angular[0] = pred_angular_0_8;
        hpc->pred_angular[1] = pred_angular_1_8;
        hpc->pred_angular[2] = pred_angular_2_8;
        hpc->pred_angular[3] = pred_angular_3_8;*/
    }


    if (cbf_luma || cbf_cb[0] || cbf_cr[0] ||
        (s->sps->chroma_format_idc == 2 && (cbf_cb[1] || cbf_cr[1]))) {
        int scan_idx   = SCAN_DIAG;
        int scan_idx_c = SCAN_DIAG;
        int cbf_chroma = cbf_cb[0] || cbf_cr[0] ||
                         (s->sps->chroma_format_idc == 2 &&
                         (cbf_cb[1] || cbf_cr[1]));

        if (s->pps->cu_qp_delta_enabled_flag && !lc->tu.is_cu_qp_delta_coded) {
            lc->tu.cu_qp_delta = ff_hevc_cu_qp_delta_abs(s);
            if (lc->tu.cu_qp_delta != 0)
                if (ff_hevc_cu_qp_delta_sign_flag(s) == 1)
                    lc->tu.cu_qp_delta = -lc->tu.cu_qp_delta;
            lc->tu.is_cu_qp_delta_coded = 1;

            if (lc->tu.cu_qp_delta < -(26 + s->sps->qp_bd_offset / 2) ||
                lc->tu.cu_qp_delta >  (25 + s->sps->qp_bd_offset / 2)) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "The cu_qp_delta %d is outside the valid range "
                       "[%d, %d].\n",
                       lc->tu.cu_qp_delta,
                       -(26 + s->sps->qp_bd_offset / 2),
                        (25 + s->sps->qp_bd_offset / 2));
                return AVERROR_INVALIDDATA;
            }

            ff_hevc_set_qPy(s, cb_xBase, cb_yBase, log2_cb_size);
        }

        if (s->sh.cu_chroma_qp_offset_enabled_flag && cbf_chroma &&
            !lc->cu.cu_transquant_bypass_flag  &&  !lc->tu.is_cu_chroma_qp_offset_coded) {
            int cu_chroma_qp_offset_flag = ff_hevc_cu_chroma_qp_offset_flag(s);
            if (cu_chroma_qp_offset_flag) {
                int cu_chroma_qp_offset_idx  = 0;
                if (s->pps->chroma_qp_offset_list_len_minus1 > 0) {
                    cu_chroma_qp_offset_idx = ff_hevc_cu_chroma_qp_offset_idx(s);
                    av_log(s->avctx, AV_LOG_ERROR,
                        "cu_chroma_qp_offset_idx not yet tested.\n");
                }
                lc->tu.cu_qp_offset_cb = s->pps->cb_qp_offset_list[cu_chroma_qp_offset_idx];
                lc->tu.cu_qp_offset_cr = s->pps->cr_qp_offset_list[cu_chroma_qp_offset_idx];
            } else {
                lc->tu.cu_qp_offset_cb = 0;
                lc->tu.cu_qp_offset_cr = 0;
            }
            lc->tu.is_cu_chroma_qp_offset_coded = 1;
        }

        if (lc->cu.pred_mode == MODE_INTRA && log2_trafo_size < 4) {
            if (lc->tu.intra_pred_mode >= 6 &&
                lc->tu.intra_pred_mode <= 14) {
                scan_idx = SCAN_VERT;
            } else if (lc->tu.intra_pred_mode >= 22 &&
                       lc->tu.intra_pred_mode <= 30) {
                scan_idx = SCAN_HORIZ;
            }

            if (lc->tu.intra_pred_mode_c >=  6 &&
                lc->tu.intra_pred_mode_c <= 14) {
                scan_idx_c = SCAN_VERT;
            } else if (lc->tu.intra_pred_mode_c >= 22 &&
                       lc->tu.intra_pred_mode_c <= 30) {
                scan_idx_c = SCAN_HORIZ;
            }
        }

        lc->tu.cross_pf = 0;


        //读取残差数据，进行反量化，DCT反变换

        //亮度Y

        if (cbf_luma)
            //this function call Assembly function to do residual prediction and fill out PDB buffer.
            ff_hevc_hls_residual_coding(s, x0, y0, log2_trafo_size, scan_idx, 0
#if COM16_C806_EMT
            		, log2_cb_size
#endif
            );

        if (log2_trafo_size > 2 || s->sps->chroma_array_type == 3) {
            int trafo_size_h = 1 << (log2_trafo_size_c + s->sps->hshift[1]);
            int trafo_size_v = 1 << (log2_trafo_size_c + s->sps->vshift[1]);
            lc->tu.cross_pf  = (s->pps->cross_component_prediction_enabled_flag && cbf_luma &&
                                (lc->cu.pred_mode == MODE_INTER ||
                                 (lc->tu.chroma_mode_c ==  4)));

            if (lc->tu.cross_pf) {
                hls_cross_component_pred(s, 0);
            }
            //色度U
            for (i = 0; i < (s->sps->chroma_array_type  ==  2 ? 2 : 1 ); i++ ) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, x0, y0 + (i << log2_trafo_size_c), trafo_size_h, trafo_size_v);
                    s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (i << log2_trafo_size_c), 1);
                    //do intra in U and fill the DPB
                }
                if (cbf_cb[i])
                    ff_hevc_hls_residual_coding(s, x0, y0 + (i << log2_trafo_size_c),
                                                log2_trafo_size_c, scan_idx_c, 1
#if COM16_C806_EMT
            		, log2_cb_size
#endif
                    );
                else
                    if (lc->tu.cross_pf) {
                        ptrdiff_t stride = s->frame->linesize[1];
                        int hshift = s->sps->hshift[1];
                        int vshift = s->sps->vshift[1];
                        int16_t *coeffs_y = lc->tu.coeffs[0];
                        int16_t *coeffs =   lc->tu.coeffs[1];
                        int size = 1 << log2_trafo_size_c;

                        uint8_t *dst = &s->frame->data[1][(y0 >> vshift) * stride +
                                                              ((x0 >> hshift) << s->sps->pixel_shift)];
                        for (i = 0; i < (size * size); i++) {
                            coeffs[i] = ((lc->tu.res_scale_val * coeffs_y[i]) >> 3);
                        }
                        //叠加残差数据
                        s->hevcdsp.transform_add[log2_trafo_size-2](dst, coeffs, stride);
                    }
            }

            if (lc->tu.cross_pf) {
                hls_cross_component_pred(s, 1);
            }
            //色度V
            for (i = 0; i < (s->sps->chroma_array_type  ==  2 ? 2 : 1 ); i++ ) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, x0, y0 + (i << log2_trafo_size_c), trafo_size_h, trafo_size_v);
                    s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (i << log2_trafo_size_c), 2);
                }
                //色度Cr
                if (cbf_cr[i])
                    ff_hevc_hls_residual_coding(s, x0, y0 + (i << log2_trafo_size_c),
                                                log2_trafo_size_c, scan_idx_c, 2
#if COM16_C806_EMT
            		, log2_cb_size
#endif
                    );
                else
                    if (lc->tu.cross_pf) {
                        ptrdiff_t stride = s->frame->linesize[2];
                        int hshift = s->sps->hshift[2];
                        int vshift = s->sps->vshift[2];
                        int16_t *coeffs_y = lc->tu.coeffs[0];
                        int16_t *coeffs =   lc->tu.coeffs[1];
                        int size = 1 << log2_trafo_size_c;

                        uint8_t *dst = &s->frame->data[2][(y0 >> vshift) * stride +
                                                          ((x0 >> hshift) << s->sps->pixel_shift)];
                        for (i = 0; i < (size * size); i++) {
                            coeffs[i] = ((lc->tu.res_scale_val * coeffs_y[i]) >> 3);
                        }
                        s->hevcdsp.transform_add[log2_trafo_size-2](dst, coeffs, stride);
                    }
            }
        } else if (blk_idx == 3) {
            int trafo_size_h = 1 << (log2_trafo_size + 1);
            int trafo_size_v = 1 << (log2_trafo_size + s->sps->vshift[1]);
            for (i = 0; i < (s->sps->chroma_array_type  ==  2 ? 2 : 1 ); i++ ) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, xBase, yBase + (i << log2_trafo_size),
                                                    trafo_size_h, trafo_size_v);
                    s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (i << log2_trafo_size), 1);
                }
                if (cbf_cb[i])
                    ff_hevc_hls_residual_coding(s, xBase, yBase + (i << log2_trafo_size),
                                                log2_trafo_size, scan_idx_c, 1
#if COM16_C806_EMT
            		, log2_cb_size
#endif
            		);
            }
            for (i = 0; i < (s->sps->chroma_array_type  ==  2 ? 2 : 1 ); i++ ) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, xBase, yBase + (i << log2_trafo_size),
                                                trafo_size_h, trafo_size_v);
                    s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (i << log2_trafo_size), 2);
                }
                if (cbf_cr[i])
                    ff_hevc_hls_residual_coding(s, xBase, yBase + (i << log2_trafo_size),
                                                log2_trafo_size, scan_idx_c, 2
#if COM16_C806_EMT
            		, log2_cb_size
#endif
                    );
            }
        }
    }
    else if (lc->cu.pred_mode == MODE_INTRA) {
        if (log2_trafo_size > 2 || s->sps->chroma_array_type == 3) {
            int trafo_size_h = 1 << (log2_trafo_size_c + s->sps->hshift[1]);
            int trafo_size_v = 1 << (log2_trafo_size_c + s->sps->vshift[1]);
            ff_hevc_set_neighbour_available(s, x0, y0, trafo_size_h, trafo_size_v);
            s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0, 1);
            s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0, 2);
            if (s->sps->chroma_array_type == 2) {
                ff_hevc_set_neighbour_available(s, x0, y0 + (1 << log2_trafo_size_c),
                                                trafo_size_h, trafo_size_v);
                s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (1 << log2_trafo_size_c), 1);
                s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (1 << log2_trafo_size_c), 2);
            }
        } else if (blk_idx == 3) {
            int trafo_size_h = 1 << (log2_trafo_size + 1);
            int trafo_size_v = 1 << (log2_trafo_size + s->sps->vshift[1]);
            ff_hevc_set_neighbour_available(s, xBase, yBase,
                                            trafo_size_h, trafo_size_v);
            s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase, 1);
            s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase, 2);
            if (s->sps->chroma_array_type == 2) {
                ff_hevc_set_neighbour_available(s, xBase, yBase + (1 << (log2_trafo_size)),
                                                trafo_size_h, trafo_size_v);
                s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (1 << (log2_trafo_size)), 1);
                s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (1 << (log2_trafo_size)), 2);
            }
        }
    }



    return 0;
}
/*从源代码可以看出，如果是帧内CU的话，hls_transform_unit()会调用HEVCPredContext的intra_pred[]()汇编函数进行帧内预测
 * 然后不论帧内预测还是帧间CU都会调用ff_hevc_hls_residual_coding()解码残差数据，并叠加在预测数据上。*/


static void set_deblocking_bypass(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int cb_size          = 1 << log2_cb_size;
    int log2_min_pu_size = s->sps->log2_min_pu_size;

    int min_pu_width     = s->sps->min_pu_width;
    int x_end = FFMIN(x0 + cb_size, s->sps->width);
    int y_end = FFMIN(y0 + cb_size, s->sps->height);
    int i, j;

    for (j = (y0 >> log2_min_pu_size); j < (y_end >> log2_min_pu_size); j++)
        for (i = (x0 >> log2_min_pu_size); i < (x_end >> log2_min_pu_size); i++)
            s->is_pcm[i + j * min_pu_width] = 2;
}
//处理TU四叉树
static int hls_transform_tree(HEVCContext *s, int x0, int y0,
                              int xBase, int yBase, int cb_xBase, int cb_yBase,
                              int log2_cb_size, int log2_trafo_size,
                              int trafo_depth, int blk_idx,
                              const int *base_cbf_cb, const int *base_cbf_cr)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t split_transform_flag;
    int cbf_cb[2];
    int cbf_cr[2];
    int ret;

    cbf_cb[0] = base_cbf_cb[0];
    cbf_cb[1] = base_cbf_cb[1];
    cbf_cr[0] = base_cbf_cr[0];
    cbf_cr[1] = base_cbf_cr[1];

    if (lc->cu.intra_split_flag) {
        if (trafo_depth == 1) {
            lc->tu.intra_pred_mode   = lc->pu.intra_pred_mode[blk_idx];
            if (s->sps->chroma_array_type == 3) {
                lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[blk_idx];
                lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[blk_idx];
            } else {
                lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[0];
                lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[0];
            }
        }
    } else {
        lc->tu.intra_pred_mode   = lc->pu.intra_pred_mode[0];
        lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[0];
        lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[0];
    }

    if (log2_trafo_size <= s->sps->log2_max_trafo_size &&
        log2_trafo_size >  s->sps->log2_min_tb_size    &&
        trafo_depth     < lc->cu.max_trafo_depth       &&
        !(lc->cu.intra_split_flag && trafo_depth == 0)) {

        //split_transform_flag标记当前TU是否要进行四叉树划分
        //为1则需要划分为4个大小相等的，为0则不再划分
        split_transform_flag = ff_hevc_split_transform_flag_decode(s, log2_trafo_size);
    } else {
        int inter_split = s->sps->max_transform_hierarchy_depth_inter == 0 &&
                          lc->cu.pred_mode == MODE_INTER &&
                          lc->cu.part_mode != PART_2Nx2N &&
                          trafo_depth == 0;

        //split_transform_flag标记当前TU是否要进行四叉树划分
        //为1则需要划分为4个大小相等的，为0则不再划分
        split_transform_flag = log2_trafo_size > s->sps->log2_max_trafo_size ||
                               (lc->cu.intra_split_flag && trafo_depth == 0) ||
                               inter_split;
    }

    if (log2_trafo_size > 2 || s->sps->chroma_array_type == 3) {
        if (trafo_depth == 0 || cbf_cb[0]) {
            cbf_cb[0] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            if (s->sps->chroma_format_idc == 2 && (!split_transform_flag || log2_trafo_size == 3)) {
                cbf_cb[1] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            }
        } else if (trafo_depth == 0) {
            cbf_cb[0] =
            cbf_cb[1] = 0;
        }

        if (trafo_depth == 0 || cbf_cr[0]) {
            cbf_cr[0] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            if (s->sps->chroma_format_idc == 2 && (!split_transform_flag || log2_trafo_size == 3)) {
                cbf_cr[1] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            }
        } else if (trafo_depth == 0) {
            cbf_cr[0] =
            cbf_cr[1] = 0;
        }
    }

    //如果当前TU要进行四叉树划分
    if (split_transform_flag) {
        const int trafo_size_split = 1 << (log2_trafo_size - 1);
        const int x1 = x0 + trafo_size_split;
        const int y1 = y0 + trafo_size_split;

#if COM16_C806_EMT
        if (0==trafo_depth)
        {
        	s->HEVClc->cu.emt_cu_flag = ff_hevc_emt_cu_flag_decode(s, log2_cb_size, 1);
        }
#endif

#define SUBDIVIDE(x, y, idx)                                                    \
do {                                                                            \
    ret = hls_transform_tree(s, x, y, x0, y0, cb_xBase, cb_yBase, log2_cb_size, \
                             log2_trafo_size - 1, trafo_depth + 1, idx,         \
                             cbf_cb, cbf_cr);                                   \
    if (ret < 0)                                                                \
        return ret;                                                             \
} while (0)
        //递归调用
        SUBDIVIDE(x0, y0, 0);
        SUBDIVIDE(x1, y0, 1);
        SUBDIVIDE(x0, y1, 2);
        SUBDIVIDE(x1, y1, 3);

#undef SUBDIVIDE
    } else {
        int min_tu_size      = 1 << s->sps->log2_min_tb_size;
        int log2_min_tu_size = s->sps->log2_min_tb_size;
        int min_tu_width     = s->sps->min_tb_width;
        int cbf_luma         = 1;

        if (lc->cu.pred_mode == MODE_INTRA || trafo_depth != 0 ||
            cbf_cb[0] || cbf_cr[0] ||
            (s->sps->chroma_format_idc == 2 && (cbf_cb[1] || cbf_cr[1]))) {
            cbf_luma = ff_hevc_cbf_luma_decode(s, trafo_depth);
        }

#if COM16_C806_EMT
        if (0 == trafo_depth)
        {
        	s->HEVClc->cu.emt_cu_flag = ff_hevc_emt_cu_flag_decode(s, log2_cb_size, cbf_luma);
        }
#endif

        //处理TU-帧内预测、DCT反变换
        ret = hls_transform_unit(s, x0, y0, xBase, yBase, cb_xBase, cb_yBase,
                                 log2_cb_size, log2_trafo_size, trafo_depth,
                                 blk_idx, cbf_luma, cbf_cb, cbf_cr);
        if (ret < 0)
            return ret;
        // TODO: store cbf_luma somewhere else
        if (cbf_luma) {
            int i, j;
            for (i = 0; i < (1 << log2_trafo_size); i += min_tu_size)
                for (j = 0; j < (1 << log2_trafo_size); j += min_tu_size) {
                    int x_tu = (x0 + j) >> log2_min_tu_size;
                    int y_tu = (y0 + i) >> log2_min_tu_size;
                    s->cbf_luma[y_tu * min_tu_width + x_tu] = 1;
                }
        }
        if (!s->sh.disable_deblocking_filter_flag) {
            ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_trafo_size);
            if (s->pps->transquant_bypass_enable_flag &&
                lc->cu.cu_transquant_bypass_flag)
                set_deblocking_bypass(s, x0, y0, log2_trafo_size);
        }
    }
    return 0;
}
/* 从源代码可以看出，hls_transform_tree()首先调用ff_hevc_split_transform_flag_decode()判断当前TU是否还需要划分。
 * 如果需要划分的话，就会递归调用4次hls_transform_tree()分别对4个子块继续进行四叉树解析；
 * 如果不需要划分，就会调用hls_transform_unit()对TU进行解码。
 * 总而言之，hls_transform_tree()会解析出来一个TU树中的所有TU，并且对每一个TU逐一调用hls_transform_unit()进行解码。
 * */

static int hls_pcm_sample(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    //TODO: non-4:2:0 support
    GetBitContext gb;
    int cb_size   = 1 << log2_cb_size;
    int stride0   = s->frame->linesize[0];
    uint8_t *dst0 = &s->frame->data[0][y0 * stride0 + (x0 << s->sps->pixel_shift)];
    int   stride1 = s->frame->linesize[1];
    uint8_t *dst1 = &s->frame->data[1][(y0 >> s->sps->vshift[1]) * stride1 + ((x0 >> s->sps->hshift[1]) << s->sps->pixel_shift)];
    int   stride2 = s->frame->linesize[2];
    uint8_t *dst2 = &s->frame->data[2][(y0 >> s->sps->vshift[2]) * stride2 + ((x0 >> s->sps->hshift[2]) << s->sps->pixel_shift)];

    int length         = cb_size * cb_size * s->sps->pcm.bit_depth +
                         (((cb_size >> s->sps->hshift[1]) * (cb_size >> s->sps->vshift[1])) +
                          ((cb_size >> s->sps->hshift[2]) * (cb_size >> s->sps->vshift[2]))) *
                          s->sps->pcm.bit_depth_chroma;
    const uint8_t *pcm = skip_bytes(&s->HEVClc->cc, (length + 7) >> 3);
    int ret;

    if (!s->sh.disable_deblocking_filter_flag)
        ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);

    ret = init_get_bits(&gb, pcm, length);
    if (ret < 0)
        return ret;

    s->hevcdsp.put_pcm(dst0, stride0, cb_size, cb_size,     &gb, s->sps->pcm.bit_depth);
    s->hevcdsp.put_pcm(dst1, stride1,
                       cb_size >> s->sps->hshift[1],
                       cb_size >> s->sps->vshift[1],
                       &gb, s->sps->pcm.bit_depth_chroma);
    s->hevcdsp.put_pcm(dst2, stride2,
                       cb_size >> s->sps->hshift[2],
                       cb_size >> s->sps->vshift[2],
                       &gb, s->sps->pcm.bit_depth_chroma);
    return 0;
}

/**
 * 8.5.3.2.2.1 Luma sample unidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param luma_weight weighting factor applied to the luma prediction
 * @param luma_offset additive offset applied to the luma prediction value
 */

static void luma_mc_uni(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                        AVFrame *ref, const Mv *mv, int x_off, int y_off,
                        int block_w, int block_h, int luma_weight, int luma_offset)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t *src         = ref->data[0];
    ptrdiff_t srcstride  = ref->linesize[0];
    int pic_width        = s->sps->width;
    int pic_height       = s->sps->height;
    //亚像素的运动矢量
    //mv0,mv1单位是1/4像素，与00000011相与之后保留后两位
    int mx               = mv->x & 3;
    int my               = mv->y & 3;
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->pps->weighted_bipred_flag);
    int idx              = ff_hevc_pel_weight[block_w];

    //整像素的偏移值
    //mv0,mv1单位是1/4像素，在这里除以4之后单位变成整像素
    x_off += mv->x >> 2;
    y_off += mv->y >> 2;
    src   += y_off * srcstride + (x_off << s->sps->pixel_shift);
    //边界处处理
    if (x_off < QPEL_EXTRA_BEFORE || y_off < QPEL_EXTRA_AFTER ||
        x_off >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * srcstride       + (QPEL_EXTRA_BEFORE << s->sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src - offset,
                                 edge_emu_stride, srcstride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off - QPEL_EXTRA_BEFORE, y_off - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src = lc->edge_emu_buffer + buf_offset;
        srcstride = edge_emu_stride;
    }
    //运动补偿
    if (!weight_flag)
        //普通的
        s->hevcdsp.put_hevc_qpel_uni[idx][!!my][!!mx](dst, dststride, src, srcstride,
                                                      block_h, mx, my, block_w);
    else
        //加权的
        s->hevcdsp.put_hevc_qpel_uni_w[idx][!!my][!!mx](dst, dststride, src, srcstride,
                                                        block_h, s->sh.luma_log2_weight_denom,
                                                        luma_weight, luma_offset, mx, my, block_w);
    /*
     * 从源代码可以看出，luma_mc_uni()调用了HEVCDSPContext的put_hevc_qpel_uni()汇编函数完成了运动补偿。
     */
}

/**
 * 8.5.3.2.2.1 Luma sample bidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref0 reference picture0 buffer at origin (0, 0)
 * @param mv0 motion vector0 (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param ref1 reference picture1 buffer at origin (0, 0)
 * @param mv1 motion vector1 (relative to block position) to get pixel data from
 * @param current_mv current motion vector structure
 */
 static void luma_mc_bi(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                       AVFrame *ref0, const Mv *mv0, int x_off, int y_off,
                       int block_w, int block_h, AVFrame *ref1, const Mv *mv1, struct MvField *current_mv)
{
    HEVCLocalContext *lc = s->HEVClc;
    DECLARE_ALIGNED(16, int16_t,  tmp[MAX_PB_SIZE * MAX_PB_SIZE]);
    ptrdiff_t src0stride  = ref0->linesize[0];
    ptrdiff_t src1stride  = ref1->linesize[0];
    int pic_width        = s->sps->width;
    int pic_height       = s->sps->height;

    //亚像素的运动矢量
    //mv0,mv1单位是1/4像素，与00000011相与之后保留后两位
    int mx0              = mv0->x & 3;
    int my0              = mv0->y & 3;
    int mx1              = mv1->x & 3;
    int my1              = mv1->y & 3;
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->pps->weighted_bipred_flag);


    //整像素的偏移值
    //mv0,mv1单位是1/4像素，在这里除以4之后单位变成整像素
    int x_off0           = x_off + (mv0->x >> 2);
    int y_off0           = y_off + (mv0->y >> 2);
    int x_off1           = x_off + (mv1->x >> 2);
    int y_off1           = y_off + (mv1->y >> 2);
    int idx              = ff_hevc_pel_weight[block_w];

    //匹配块数据（整像素精度，没有进行差值）
    //list0
    uint8_t *src0  = ref0->data[0] + y_off0 * src0stride + (int)((unsigned)x_off0 << s->sps->pixel_shift);
    //list1
    uint8_t *src1  = ref1->data[0] + y_off1 * src1stride + (int)((unsigned)x_off1 << s->sps->pixel_shift);
    //边界位置的处理
    if (x_off0 < QPEL_EXTRA_BEFORE || y_off0 < QPEL_EXTRA_AFTER ||
        x_off0 >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off0 >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * src0stride       + (QPEL_EXTRA_BEFORE << s->sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src0 - offset,
                                 edge_emu_stride, src0stride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off0 - QPEL_EXTRA_BEFORE, y_off0 - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src0 = lc->edge_emu_buffer + buf_offset;
        src0stride = edge_emu_stride;
    }

    if (x_off1 < QPEL_EXTRA_BEFORE || y_off1 < QPEL_EXTRA_AFTER ||
        x_off1 >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off1 >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * src1stride       + (QPEL_EXTRA_BEFORE << s->sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer2, src1 - offset,
                                 edge_emu_stride, src1stride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off1 - QPEL_EXTRA_BEFORE, y_off1 - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src1 = lc->edge_emu_buffer2 + buf_offset;
        src1stride = edge_emu_stride;
    }

    //双向预测分成2步：
    //  (1)使用list0中的匹配块进行单向预测
    //  (2)使用list1中的匹配块再次进行单向预测，然后与第1次预测的结果求平均

    //第1步
    s->hevcdsp.put_hevc_qpel[idx][!!my0][!!mx0](tmp, MAX_PB_SIZE, src0, src0stride,
                                                block_h, mx0, my0, block_w);
    //第2步
    if (!weight_flag)
        s->hevcdsp.put_hevc_qpel_bi[idx][!!my1][!!mx1](dst, dststride, src1, src1stride, tmp, MAX_PB_SIZE,
                                                       block_h, mx1, my1, block_w);
    else
        s->hevcdsp.put_hevc_qpel_bi_w[idx][!!my1][!!mx1](dst, dststride, src1, src1stride, tmp, MAX_PB_SIZE,
                                                         block_h, s->sh.luma_log2_weight_denom,
                                                         s->sh.luma_weight_l0[current_mv->ref_idx[0]],
                                                         s->sh.luma_weight_l1[current_mv->ref_idx[1]],
                                                         s->sh.luma_offset_l0[current_mv->ref_idx[0]],
                                                         s->sh.luma_offset_l1[current_mv->ref_idx[1]],
                                                         mx1, my1, block_w);

    /*从源代码可以看出，luma_mc_bi()调用了HEVCDSPContext的put_hevc_qpel()和put_hevc_qpel_bi()汇编函数完成了运动补偿。后文将会对这些运动补偿汇编函数进行分析。*/
}


/**
 * MvDecoder_write_mv_buffer
 *
 * @param s HEVC decoding context
 * @param block_w width of block
 * @param block_h height of block
 */

static void MvDecoder_write_mv_buffer(HEVCContext *s, uint8_t *dst3_base, int x0, int y0,
                        struct MvField *current_mv, int block_w, int block_h)
{
    int pu_resolution = (s->frame->coded_height>>2)*(s->frame->linesize[0]>>2);
    int pu_linesize = s->frame->linesize[0]>>2;
    int pu_y0 = y0 >> 2;
    int pu_x0 = x0 >> 2;
    int pu_block_w = block_w >> 2;
    int pu_block_h = block_h >> 2;
    int16_t *dst_l0_mx = (int16_t*)(dst3_base)+pu_y0 * pu_linesize + pu_x0;
    int16_t *dst_l0_my = (int16_t*)(dst3_base + pu_resolution*2) + pu_y0 * pu_linesize + pu_x0;
    int16_t *dst_l1_mx = (int16_t*)(dst3_base + pu_resolution*4) + pu_y0 * pu_linesize + pu_x0;
    int16_t *dst_l1_my = (int16_t*)(dst3_base + pu_resolution*6) + pu_y0 * pu_linesize + pu_x0;
    uint8_t *dst_l0_ref = dst3_base + pu_resolution*8 + pu_y0 * pu_linesize + pu_x0;
    uint8_t *dst_l1_ref = dst3_base + pu_resolution*9 + pu_y0 * pu_linesize + pu_x0;

    //亚像素的运动矢量
    //mv0,mv1单位是1/4像素 for UV, 1/8 for Y.
    //reverse motion vector sign if refer backwards
    int x, y;
    if (current_mv->pred_flag == PF_L0) {
        for (y = 0; y < pu_block_h; y++) {
            for (x = 0; x < pu_block_w; x++) {
                dst_l0_mx[x] = current_mv->mv[0].x;
                dst_l0_my[x] = current_mv->mv[0].y;
                dst_l0_ref[x] = s->poc - current_mv->poc[0];
            }
            dst_l0_mx += pu_linesize;
            dst_l0_my += pu_linesize;
            dst_l0_ref += pu_linesize;
        }
    }

    else if(current_mv->pred_flag == PF_L1) {
        for (y = 0; y < pu_block_h; y++) {
            for (x = 0; x < pu_block_w; x++) {
                dst_l1_mx[x] = current_mv->mv[1].x;
                dst_l1_my[x] = current_mv->mv[1].y;
                dst_l1_ref[x] = current_mv->poc[1] - s->poc;
            }
            dst_l1_mx += pu_linesize;
            dst_l1_my += pu_linesize;
            dst_l1_ref += pu_linesize;
        }
    }

    else if(current_mv->pred_flag == PF_BI) {
        for (y = 0; y < pu_block_h; y++) {
            for (x = 0; x < pu_block_w; x++) {
                dst_l0_mx[x] = current_mv->mv[0].x;
                dst_l0_my[x] = current_mv->mv[0].y;
                dst_l0_ref[x] = s->poc - current_mv->poc[0];
                dst_l1_mx[x] = current_mv->mv[1].x;
                dst_l1_my[x] = current_mv->mv[1].y;
                dst_l1_ref[x] = current_mv->poc[1] - s->poc;
            }
            dst_l0_mx += pu_linesize;
            dst_l0_my += pu_linesize;
            dst_l0_ref += pu_linesize;
            dst_l1_mx += pu_linesize;
            dst_l1_my += pu_linesize;
            dst_l1_ref += pu_linesize;
        }
    }

}



/**
 * MvDecoder_write_size_buffer
 *
 * @param s HEVC decoding context
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param log2_cb_size log2 of block size
 * @param cu_byte_size number of bytes used in the bytestream of this CU block.
 */
static void MvDecoder_write_size_buffer(HEVCContext *s, int x0, int y0, int log2_cb_size, int cu_byte_size)
{

    int pu_resolution = (s->frame->coded_height>>2)*(s->frame->linesize[0]>>2);
    int cu_linesize = s->frame->linesize[0]>>3;
    int cu_y0 = y0 >> 3;
    int cu_x0 = x0 >> 3;
    int cb_size = (1 << log2_cb_size) >> 3;
    uint8_t *dst_size = &s->frame->data[3][pu_resolution*10 + cu_y0 * cu_linesize + cu_x0];

    int x, y;
    int bit_density = cu_byte_size * 8 / (cb_size * cb_size);
    //处理x*y个像素
    for (y = 0; y < cb_size; y++) {
        for (x = 0; x < cb_size; x++) {
            dst_size[x] = bit_density;
        }
        dst_size += cu_linesize;
    }
}


static void MvDecoder_write_residual_initialization(uint8_t* dst, int block_w, int block_h, int linesize)
{
    int x, y;
    //处理x*y个像素
    for (y = 0; y < block_h; y++) {
        for (x = 0; x < block_w; x++) {
            dst[x] = 128;
        }
        dst += linesize;
    }
}

/**
 * 8.5.3.2.2.2 Chroma sample uniprediction interpolation process
 *
 * @param s HEVC decoding context
 * @param dst1 target buffer for block data at block position (U plane)
 * @param dst2 target buffer for block data at block position (V plane)
 * @param dststride stride of the dst1 and dst2 buffers
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param chroma_weight weighting factor applied to the chroma prediction
 * @param chroma_offset additive offset applied to the chroma prediction value
 */

static void chroma_mc_uni(HEVCContext *s, uint8_t *dst0,
                          ptrdiff_t dststride, uint8_t *src0, ptrdiff_t srcstride, int reflist,
                          int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int chroma_weight, int chroma_offset)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pic_width        = s->sps->width >> s->sps->hshift[1];
    int pic_height       = s->sps->height >> s->sps->vshift[1];
    const Mv *mv         = &current_mv->mv[reflist];
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->pps->weighted_bipred_flag);
    int idx              = ff_hevc_pel_weight[block_w];
    int hshift           = s->sps->hshift[1];
    int vshift           = s->sps->vshift[1];
    intptr_t mx          = mv->x & ((1 << (2 + hshift)) - 1);
    intptr_t my          = mv->y & ((1 << (2 + vshift)) - 1);
    intptr_t _mx         = mx << (1 - hshift);
    intptr_t _my         = my << (1 - vshift);

    x_off += mv->x >> (2 + hshift);
    y_off += mv->y >> (2 + vshift);
    src0  += y_off * srcstride + (x_off << s->sps->pixel_shift);

    if (x_off < EPEL_EXTRA_BEFORE || y_off < EPEL_EXTRA_AFTER ||
        x_off >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->sps->pixel_shift;
        int offset0 = EPEL_EXTRA_BEFORE * (srcstride + (1 << s->sps->pixel_shift));
        int buf_offset0 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->sps->pixel_shift));
        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src0 - offset0,
                                 edge_emu_stride, srcstride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off - EPEL_EXTRA_BEFORE,
                                 y_off - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src0 = lc->edge_emu_buffer + buf_offset0;
        srcstride = edge_emu_stride;
    }
    if (!weight_flag)
        s->hevcdsp.put_hevc_epel_uni[idx][!!my][!!mx](dst0, dststride, src0, srcstride,
                                                  block_h, _mx, _my, block_w);
    else
        s->hevcdsp.put_hevc_epel_uni_w[idx][!!my][!!mx](dst0, dststride, src0, srcstride,
                                                        block_h, s->sh.chroma_log2_weight_denom,
                                                        chroma_weight, chroma_offset, _mx, _my, block_w);
}

/**
 * 8.5.3.2.2.2 Chroma sample bidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref0 reference picture0 buffer at origin (0, 0)
 * @param mv0 motion vector0 (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param ref1 reference picture1 buffer at origin (0, 0)
 * @param mv1 motion vector1 (relative to block position) to get pixel data from
 * @param current_mv current motion vector structure
 * @param cidx chroma component(cb, cr)
 */
static void chroma_mc_bi(HEVCContext *s, uint8_t *dst0, ptrdiff_t dststride, AVFrame *ref0, AVFrame *ref1,
                         int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int cidx)
{
    DECLARE_ALIGNED(16, int16_t, tmp [MAX_PB_SIZE * MAX_PB_SIZE]);
    int tmpstride = MAX_PB_SIZE;
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t *src1        = ref0->data[cidx+1];
    uint8_t *src2        = ref1->data[cidx+1];
    ptrdiff_t src1stride = ref0->linesize[cidx+1];
    ptrdiff_t src2stride = ref1->linesize[cidx+1];
    int weight_flag      = (s->sh.slice_type == P_SLICE && s->pps->weighted_pred_flag) ||
                           (s->sh.slice_type == B_SLICE && s->pps->weighted_bipred_flag);
    int pic_width        = s->sps->width >> s->sps->hshift[1];
    int pic_height       = s->sps->height >> s->sps->vshift[1];
    Mv *mv0              = &current_mv->mv[0];
    Mv *mv1              = &current_mv->mv[1];
    int hshift = s->sps->hshift[1];
    int vshift = s->sps->vshift[1];

    intptr_t mx0 = mv0->x & ((1 << (2 + hshift)) - 1);
    intptr_t my0 = mv0->y & ((1 << (2 + vshift)) - 1);
    intptr_t mx1 = mv1->x & ((1 << (2 + hshift)) - 1);
    intptr_t my1 = mv1->y & ((1 << (2 + vshift)) - 1);
    intptr_t _mx0 = mx0 << (1 - hshift);
    intptr_t _my0 = my0 << (1 - vshift);
    intptr_t _mx1 = mx1 << (1 - hshift);
    intptr_t _my1 = my1 << (1 - vshift);

    int x_off0 = x_off + (mv0->x >> (2 + hshift));
    int y_off0 = y_off + (mv0->y >> (2 + vshift));
    int x_off1 = x_off + (mv1->x >> (2 + hshift));
    int y_off1 = y_off + (mv1->y >> (2 + vshift));
    int idx = ff_hevc_pel_weight[block_w];
    src1  += y_off0 * src1stride + (int)((unsigned)x_off0 << s->sps->pixel_shift);
    src2  += y_off1 * src2stride + (int)((unsigned)x_off1 << s->sps->pixel_shift);

    if (x_off0 < EPEL_EXTRA_BEFORE || y_off0 < EPEL_EXTRA_AFTER ||
        x_off0 >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off0 >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->sps->pixel_shift;
        int offset1 = EPEL_EXTRA_BEFORE * (src1stride + (1 << s->sps->pixel_shift));
        int buf_offset1 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->sps->pixel_shift));

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src1 - offset1,
                                 edge_emu_stride, src1stride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off0 - EPEL_EXTRA_BEFORE,
                                 y_off0 - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src1 = lc->edge_emu_buffer + buf_offset1;
        src1stride = edge_emu_stride;
    }

    if (x_off1 < EPEL_EXTRA_BEFORE || y_off1 < EPEL_EXTRA_AFTER ||
        x_off1 >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off1 >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->sps->pixel_shift;
        int offset1 = EPEL_EXTRA_BEFORE * (src2stride + (1 << s->sps->pixel_shift));
        int buf_offset1 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->sps->pixel_shift));

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer2, src2 - offset1,
                                 edge_emu_stride, src2stride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off1 - EPEL_EXTRA_BEFORE,
                                 y_off1 - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src2 = lc->edge_emu_buffer2 + buf_offset1;
        src2stride = edge_emu_stride;
    }

    s->hevcdsp.put_hevc_epel[idx][!!my0][!!mx0](tmp, tmpstride, src1, src1stride,
                                                block_h, _mx0, _my0, block_w);
    if (!weight_flag)
        s->hevcdsp.put_hevc_epel_bi[idx][!!my1][!!mx1](dst0, s->frame->linesize[cidx+1],
                                                       src2, src2stride, tmp, tmpstride,
                                                       block_h, _mx1, _my1, block_w);
    else
        s->hevcdsp.put_hevc_epel_bi_w[idx][!!my1][!!mx1](dst0, s->frame->linesize[cidx+1],
                                                         src2, src2stride, tmp, tmpstride,
                                                         block_h,
                                                         s->sh.chroma_log2_weight_denom,
                                                         s->sh.chroma_weight_l0[current_mv->ref_idx[0]][cidx],
                                                         s->sh.chroma_weight_l1[current_mv->ref_idx[1]][cidx],
                                                         s->sh.chroma_offset_l0[current_mv->ref_idx[0]][cidx],
                                                         s->sh.chroma_offset_l1[current_mv->ref_idx[1]][cidx],
                                                         _mx1, _my1, block_w);
}

static void hevc_await_progress(HEVCContext *s, HEVCFrame *ref,
                                const Mv *mv, int y0, int height)
{
    int y = (mv->y >> 2) + y0 + height + 9;

    if (s->threads_type & FF_THREAD_FRAME )
        ff_thread_await_progress(&ref->tf, y, 0);
}
static void hevc_await_progress_bl(HEVCContext *s, HEVCFrame *ref,
                                const Mv *mv, int y0)
{
    int y = (mv->y >> 2) + y0 + (1<<s->sps->log2_ctb_size)*2 + 9;
    int bl_y = (( (y  - s->sps->pic_conf_win.top_offset) * s->up_filter_inf.scaleYLum + s->up_filter_inf.addYLum) >> 12) >> 4;
    if (s->threads_type & FF_THREAD_FRAME )
        ff_thread_await_progress(&s->BL_frame->tf, bl_y, 0);
}

static void hls_prediction_unit(HEVCContext *s, int x0, int y0,
                                int nPbW, int nPbH,
                                int log2_cb_size, int partIdx, int idx)
{
#define POS(c_idx, x, y)                                                              \
    &s->frame->data[c_idx][((y) >> s->sps->vshift[c_idx]) * s->frame->linesize[c_idx] + \
                           (((x) >> s->sps->hshift[c_idx]) << s->sps->pixel_shift)]
    HEVCLocalContext *lc = s->HEVClc;
    int merge_idx = 0;
    struct MvField current_mv;
    int min_pu_width = s->sps->min_pu_width;
    MvField *tab_mvf = s->ref->tab_mvf;
    RefPicList  *refPicList = s->ref->refPicList[s->slice_idx];
    //参考帧
    HEVCFrame *ref0 = NULL, *ref1 = NULL;
    //分别指向Y，U，V分量
    uint8_t *dst0 = POS(0, x0, y0);
    uint8_t *dst1 = POS(1, x0, y0);
    uint8_t *dst2 = POS(2, x0, y0);

    // MvDecoder: get buffer dst pointer.
    uint8_t *MvDecoder_dst3_base = &s->frame->data[3][0];

    int log2_min_cb_size = s->sps->log2_min_cb_size;
    int min_cb_width     = s->sps->min_cb_width;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int mvp_flag;
    int x_pu, y_pu;
    int i, j;

    if (SAMPLE_CTB(s->skip_flag, x_cb, y_cb)) {


        if (s->sh.max_num_merge_cand > 1)
            //Merge模式
            merge_idx = ff_hevc_merge_idx_decode(s);
        else
            merge_idx = 0;


        ff_hevc_luma_mv_merge_mode(s, x0, y0,
                                   1 << log2_cb_size,
                                   1 << log2_cb_size,
                                   log2_cb_size, partIdx,
                                   merge_idx, &current_mv);
    } else { /* MODE_INTER */
        lc->pu.merge_flag = ff_hevc_merge_flag_decode(s);
        if (lc->pu.merge_flag) {
            if (s->sh.max_num_merge_cand > 1)
                merge_idx = ff_hevc_merge_idx_decode(s);
            else
                merge_idx = 0;

            ff_hevc_luma_mv_merge_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                       partIdx, merge_idx, &current_mv);
        } else {
            enum InterPredIdc inter_pred_idc = PRED_L0;
            ff_hevc_set_neighbour_available(s, x0, y0, nPbW, nPbH);
            current_mv.pred_flag = 0;
            if (s->sh.slice_type == B_SLICE)
                inter_pred_idc = ff_hevc_inter_pred_idc_decode(s, nPbW, nPbH);

            if (inter_pred_idc != PRED_L1) {
                if (s->sh.nb_refs[L0]) {
                    current_mv.ref_idx[0] = ff_hevc_ref_idx_lx_decode(s, s->sh.nb_refs[L0]);
#ifdef TEST_MV_POC
                    current_mv.poc[0] = refPicList[0].list[current_mv.ref_idx[0]];
#endif
                }
                current_mv.pred_flag = PF_L0;
                ff_hevc_hls_mvd_coding(s, x0, y0, 0);
                mvp_flag = ff_hevc_mvp_lx_flag_decode(s);
                ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                         partIdx, merge_idx, &current_mv,
                                         mvp_flag, 0);
                current_mv.mv[0].x += lc->pu.mvd.x;
                current_mv.mv[0].y += lc->pu.mvd.y;
            }

            if (inter_pred_idc != PRED_L0) {
                if (s->sh.nb_refs[L1]) {
                    current_mv.ref_idx[1] = ff_hevc_ref_idx_lx_decode(s, s->sh.nb_refs[L1]);
#ifdef TEST_MV_POC
                    current_mv.poc[1] = refPicList[1].list[current_mv.ref_idx[1]];
#endif
                }

                if (s->sh.mvd_l1_zero_flag == 1 && inter_pred_idc == PRED_BI) {
                    lc->pu.mvd.x = 0;
                    lc->pu.mvd.y = 0;
                } else {
                    ff_hevc_hls_mvd_coding(s, x0, y0, 1);
                }

                current_mv.pred_flag += PF_L1;
                mvp_flag = ff_hevc_mvp_lx_flag_decode(s);
                ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                         partIdx, merge_idx, &current_mv,
                                         mvp_flag, 1);
                current_mv.mv[1].x += lc->pu.mvd.x;
                current_mv.mv[1].y += lc->pu.mvd.y;
            }
        }
    }


    x_pu = x0 >> s->sps->log2_min_pu_size;
    y_pu = y0 >> s->sps->log2_min_pu_size;
    for (j = 0; j < nPbH >> s->sps->log2_min_pu_size; j++)
        for (i = 0; i < nPbW >> s->sps->log2_min_pu_size; i++)
            tab_mvf[(y_pu + j) * min_pu_width + x_pu + i] = current_mv;

    //参考了List0
    if (current_mv.pred_flag & PF_L0) {
        ref0 = refPicList[0].ref[current_mv.ref_idx[0]];
        if (!ref0)
            return;
#if ACTIVE_PU_UPSAMPLING
        if(ref0 == s->inter_layer_ref) {
            int y = (current_mv.mv[0].y >> 2) + y0;
            int x = (current_mv.mv[0].x >> 2) + x0;
            hevc_await_progress_bl(s, ref0, &current_mv.mv[0], y0);

            ff_upsample_block(s, ref0, x, y, nPbW, nPbH);
        }
#endif
        hevc_await_progress(s, ref0, &current_mv.mv[0], y0, nPbH);
    }
    //参考了List1
    if (current_mv.pred_flag & PF_L1) {
        ref1 = refPicList[1].ref[current_mv.ref_idx[1]];
        if (!ref1)
            return;
#if ACTIVE_PU_UPSAMPLING
        if(ref1 == s->inter_layer_ref ) {
            int y = (current_mv.mv[1].y >> 2) + y0;
            int x = (current_mv.mv[1].x >> 2) + x0;
            hevc_await_progress_bl(s, ref1, &current_mv.mv[1], y0);

            ff_upsample_block(s, ref1, x, y, nPbW, nPbH);
        }
#endif
        hevc_await_progress(s, ref1, &current_mv.mv[1], y0, nPbH);
    }

    //current_mv
    //current_mv_flag
    if (current_mv.pred_flag == PF_L0) {
        int x0_c = x0 >> s->sps->hshift[1];
        int y0_c = y0 >> s->sps->vshift[1];
        int nPbW_c = nPbW >> s->sps->hshift[1];
        int nPbH_c = nPbH >> s->sps->vshift[1];
        // MvDecoder: write motion vector value into buffer.
        MvDecoder_write_mv_buffer(s, MvDecoder_dst3_base, x0, y0, &current_mv, nPbW, nPbH);
        //亮度运动补偿-单向
        luma_mc_uni(s, dst0, s->frame->linesize[0], ref0->frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH,
                    s->sh.luma_weight_l0[current_mv.ref_idx[0]],
                    s->sh.luma_offset_l0[current_mv.ref_idx[0]]);
        chroma_mc_uni(s, dst1, s->frame->linesize[1], ref0->frame->data[1], ref0->frame->linesize[1],
                      0, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                      s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0]);
        chroma_mc_uni(s, dst2, s->frame->linesize[2], ref0->frame->data[2], ref0->frame->linesize[2],
                      0, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                      s->sh.chroma_weight_l0[current_mv.ref_idx[0]][1], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][1]);
    } else if (current_mv.pred_flag == PF_L1) {
        int x0_c = x0 >> s->sps->hshift[1];
        int y0_c = y0 >> s->sps->vshift[1];
        int nPbW_c = nPbW >> s->sps->hshift[1];
        int nPbH_c = nPbH >> s->sps->vshift[1];
        // MvDecoder: write motion vector value into buffer.
        MvDecoder_write_mv_buffer(s, MvDecoder_dst3_base, x0, y0, &current_mv, nPbW, nPbH);
        //亮度运动补偿-单向
        luma_mc_uni(s, dst0, s->frame->linesize[0], ref1->frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH,
                    s->sh.luma_weight_l1[current_mv.ref_idx[1]],
                    s->sh.luma_offset_l1[current_mv.ref_idx[1]]);
        //printf("PF_L1 x0:%d, y0:%d, mv1_x:%d, mv1_y:%d \n", x0,y0,current_mv.mv[1].x, current_mv.mv[1].y);
        //色度运动补偿
        chroma_mc_uni(s, dst1, s->frame->linesize[1], ref1->frame->data[1], ref1->frame->linesize[1],
                      1, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                      s->sh.chroma_weight_l1[current_mv.ref_idx[1]][0], s->sh.chroma_offset_l1[current_mv.ref_idx[1]][0]);

        chroma_mc_uni(s, dst2, s->frame->linesize[2], ref1->frame->data[2], ref1->frame->linesize[2],
                      1, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                      s->sh.chroma_weight_l1[current_mv.ref_idx[1]][1], s->sh.chroma_offset_l1[current_mv.ref_idx[1]][1]);
    } else if (current_mv.pred_flag == PF_BI) {
        int x0_c = x0 >> s->sps->hshift[1];
        int y0_c = y0 >> s->sps->vshift[1];
        int nPbW_c = nPbW >> s->sps->hshift[1];
        int nPbH_c = nPbH >> s->sps->vshift[1];
        // MvDecoder: write motion vector value into buffer.
        MvDecoder_write_mv_buffer(s, MvDecoder_dst3_base, x0, y0, &current_mv, nPbW, nPbH);

        //亮度运动补偿-双向
        luma_mc_bi(s, dst0, s->frame->linesize[0], ref0->frame,
                   &current_mv.mv[0], x0, y0, nPbW, nPbH,
                   ref1->frame, &current_mv.mv[1], &current_mv);
        //printf("PF_BI x0:%d, y0:%d, mv0_x:%d, mv0_y:%d, mv1_x:%d, mv1_y:%d \n", x0,y0,current_mv.mv[0].x, current_mv.mv[0].y,current_mv.mv[1].x, current_mv.mv[1].y);

        chroma_mc_bi(s, dst1, s->frame->linesize[1], ref0->frame, ref1->frame,
                     x0_c, y0_c, nPbW_c, nPbH_c, &current_mv, 0);

        chroma_mc_bi(s, dst2, s->frame->linesize[2], ref0->frame, ref1->frame,
                     x0_c, y0_c, nPbW_c, nPbH_c, &current_mv, 1);
    }
}

/* Conclusion
 * （1）解析码流得到运动矢量。HEVC中包含了Merge和AMVP两种运动矢量预测技术。
 *          对于使用Merge的码流，调用ff_hevc_luma_mv_merge_mode()；
 *          对于使用AMVP的码流，调用hevc_luma_mv_mpv_mode()。
 *
 * （2）根据运动矢量进行运动补偿。
 *          对于单向预测亮度运动补偿，调用luma_mc_uni()，
 *          对于单向预测色度运动补偿，调用chroma_mc_uni()；
 *
 *          对于双向预测亮度运动补偿，调用luma_mc_bi()，
 *          对于单向预测色度运动补偿，调用chroma_mc_bi()。
 * */


/**
 * 8.4.1
 */
static int luma_intra_pred_mode(HEVCContext *s, int x0, int y0, int pu_size,
                                int prev_intra_luma_pred_flag)
{
    HEVCLocalContext *lc = s->HEVClc;
    int x_pu             = x0 >> s->sps->log2_min_pu_size;
    int y_pu             = y0 >> s->sps->log2_min_pu_size;
    int min_pu_width     = s->sps->min_pu_width;
    int size_in_pus      = pu_size >> s->sps->log2_min_pu_size;
    int x0b              = x0 & ((1 << s->sps->log2_ctb_size) - 1);
    int y0b              = y0 & ((1 << s->sps->log2_ctb_size) - 1);

    int cand_up   = (lc->ctb_up_flag || y0b) ?
                    s->tab_ipm[(y_pu - 1) * min_pu_width + x_pu] : INTRA_DC;
    int cand_left = (lc->ctb_left_flag || x0b) ?
                    s->tab_ipm[y_pu * min_pu_width + x_pu - 1]   : INTRA_DC;

    int y_ctb = (y0 >> (s->sps->log2_ctb_size)) << (s->sps->log2_ctb_size);

    MvField *tab_mvf = s->ref->tab_mvf;
    int intra_pred_mode;
    int candidate[3];
    int i, j;

    // intra_pred_mode prediction does not cross vertical CTB boundaries
    if ((y0 - 1) < y_ctb)
        cand_up = INTRA_DC;

    if (cand_left == cand_up) {
        if (cand_left < 2) {
            candidate[0] = INTRA_PLANAR;
            candidate[1] = INTRA_DC;
            candidate[2] = INTRA_ANGULAR_26;
        } else {
            candidate[0] = cand_left;
            candidate[1] = 2 + ((cand_left - 2 - 1 + 32) & 31);
            candidate[2] = 2 + ((cand_left - 2 + 1) & 31);
        }
    } else {
        candidate[0] = cand_left;
        candidate[1] = cand_up;
        if (candidate[0] != INTRA_PLANAR && candidate[1] != INTRA_PLANAR) {
            candidate[2] = INTRA_PLANAR;
        } else if (candidate[0] != INTRA_DC && candidate[1] != INTRA_DC) {
            candidate[2] = INTRA_DC;
        } else {
            candidate[2] = INTRA_ANGULAR_26;
        }
    }

    if (prev_intra_luma_pred_flag) {
        intra_pred_mode = candidate[lc->pu.mpm_idx];
    } else {
        if (candidate[0] > candidate[1])
            FFSWAP(uint8_t, candidate[0], candidate[1]);
        if (candidate[0] > candidate[2])
            FFSWAP(uint8_t, candidate[0], candidate[2]);
        if (candidate[1] > candidate[2])
            FFSWAP(uint8_t, candidate[1], candidate[2]);

        intra_pred_mode = lc->pu.rem_intra_luma_pred_mode;
        for (i = 0; i < 3; i++)
            if (intra_pred_mode >= candidate[i])
                intra_pred_mode++;
    }

    /* write the intra prediction units into the mv array */
    if (!size_in_pus)
        size_in_pus = 1;
    for (i = 0; i < size_in_pus; i++) {
        memset(&s->tab_ipm[(y_pu + i) * min_pu_width + x_pu],
               intra_pred_mode, size_in_pus);

        for (j = 0; j < size_in_pus; j++) {
            tab_mvf[(y_pu + j) * min_pu_width + x_pu + i].pred_flag = PF_INTRA;
        }
    }

    return intra_pred_mode;
}

static av_always_inline void set_ct_depth(HEVCContext *s, int x0, int y0,
                                          int log2_cb_size, int ct_depth)
{
    int length = (1 << log2_cb_size) >> s->sps->log2_min_cb_size;
    int x_cb   = x0 >> s->sps->log2_min_cb_size;
    int y_cb   = y0 >> s->sps->log2_min_cb_size;
    int y;

    for (y = 0; y < length; y++)
        memset(&s->tab_ct_depth[(y_cb + y) * s->sps->min_cb_width + x_cb],
               ct_depth, length);
}

static const uint8_t tab_mode_idx[] = {
     0,  1,  2,  2,  2,  2,  3,  5,  7,  8, 10, 12, 13, 15, 17, 18, 19, 20,
    21, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 28, 29, 29, 30, 31};

static void intra_prediction_unit(HEVCContext *s, int x0, int y0,
                                  int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    static const uint8_t intra_chroma_table[4] = { 0, 26, 10, 1 };
    uint8_t prev_intra_luma_pred_flag[4];
    int split   = lc->cu.part_mode == PART_NxN;
    int pb_size = (1 << log2_cb_size) >> split;
    int side    = split + 1;
    int chroma_mode;
    int i, j;

    for (i = 0; i < side; i++)
        for (j = 0; j < side; j++)
            prev_intra_luma_pred_flag[2 * i + j] = ff_hevc_prev_intra_luma_pred_flag_decode(s);

    for (i = 0; i < side; i++) {
        for (j = 0; j < side; j++) {
            if (prev_intra_luma_pred_flag[2 * i + j])
                lc->pu.mpm_idx = ff_hevc_mpm_idx_decode(s);
            else
                lc->pu.rem_intra_luma_pred_mode = ff_hevc_rem_intra_luma_pred_mode_decode(s);

            lc->pu.intra_pred_mode[2 * i + j] =
                luma_intra_pred_mode(s, x0 + pb_size * j, y0 + pb_size * i, pb_size,
                                     prev_intra_luma_pred_flag[2 * i + j]);
        }
    }

    if (s->sps->chroma_array_type  ==  3) {
        for (i = 0; i < side; i++) {
            for (j = 0; j < side; j++) {
                lc->pu.chroma_mode_c[2 * i + j] = chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
                if (chroma_mode != 4) {
                    if (lc->pu.intra_pred_mode[2 * i + j] == intra_chroma_table[chroma_mode])
                        lc->pu.intra_pred_mode_c[2 * i + j] = 34;
                    else
                        lc->pu.intra_pred_mode_c[2 * i + j] = intra_chroma_table[chroma_mode];
                } else {
                    lc->pu.intra_pred_mode_c[2 * i + j] = lc->pu.intra_pred_mode[2 * i + j];
                }
            }
        }
    } else if (s->sps->chroma_array_type == 2) {
        int mode_idx;
        lc->pu.chroma_mode_c[0] = chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
        if (chroma_mode != 4) {
            if (lc->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode])
                mode_idx = 34;
            else
                mode_idx = intra_chroma_table[chroma_mode];
        } else {
            mode_idx = lc->pu.intra_pred_mode[0];
        }
        lc->pu.intra_pred_mode_c[0] = tab_mode_idx[mode_idx];
    } else if (s->sps->chroma_array_type  !=  0) {
        chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
        if (chroma_mode != 4) {
            if (lc->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode])
                lc->pu.intra_pred_mode_c[0] = 34;
            else
                lc->pu.intra_pred_mode_c[0] = intra_chroma_table[chroma_mode];
        } else {
            lc->pu.intra_pred_mode_c[0] = lc->pu.intra_pred_mode[0];
        }
    }
}

static void intra_prediction_unit_default_value(HEVCContext *s,
                                                int x0, int y0,
                                                int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pb_size          = 1 << log2_cb_size;
    int size_in_pus      = pb_size >> s->sps->log2_min_pu_size;
    int min_pu_width     = s->sps->min_pu_width;
    MvField *tab_mvf     = s->ref->tab_mvf;
    int x_pu             = x0 >> s->sps->log2_min_pu_size;
    int y_pu             = y0 >> s->sps->log2_min_pu_size;
    int j, k;

    if (size_in_pus == 0)
        size_in_pus = 1;
    for (j = 0; j < size_in_pus; j++)
        memset(&s->tab_ipm[(y_pu + j) * min_pu_width + x_pu], INTRA_DC, size_in_pus);
    if (lc->cu.pred_mode == MODE_INTRA)
        for (j = 0; j < size_in_pus; j++)
            for (k = 0; k < size_in_pus; k++)
                tab_mvf[(y_pu + j) * min_pu_width + x_pu + k].pred_flag = PF_INTRA;
}


//处理CU单元-真正的解码
static int hls_coding_unit(HEVCContext *s, int x0, int y0, int log2_cb_size, u_int8_t *MvDecoder_ctu_quadtree, int MvDecoder_quadtree_bit_idx)
{
    /* （1）调用hls_prediction_unit()处理PU。
     * （2）调用hls_transform_tree()处理TU树
    */
     //CB大小
    int cb_size          = 1 << log2_cb_size;
    HEVCLocalContext *lc = s->HEVClc;

    // MvDevoder: bytestream checkpoint of the start of cu
    uint8_t* bytestream_last = lc->cc.bytestream;

    int log2_min_cb_size = s->sps->log2_min_cb_size;
    int length           = cb_size >> log2_min_cb_size;
    int min_cb_width     = s->sps->min_cb_width;

    //以最小的CB为单位（例如4x4）的时候，当前CB的位置——x坐标和y坐标
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int idx              = log2_cb_size - 2;
    int qp_block_mask    = (1<<(s->sps->log2_ctb_size - s->pps->diff_cu_qp_delta_depth)) - 1;
    int x, y, ret;

    //设置CU的属性值
    lc->cu.x                = x0;
    lc->cu.y                = y0;
    lc->cu.rqt_root_cbf     = 1;
    lc->cu.pred_mode        = MODE_INTRA;
    lc->cu.part_mode        = PART_2Nx2N;
    lc->cu.intra_split_flag = 0;
    lc->cu.pcm_flag         = 0;



    //Mvdecoder: initailize residual yuv base as 128 as residual offset might be negative
    uint8_t *dst4 = &s->frame->data[4][((y0) >> s->sps->vshift[0]) * s->frame->linesize[0] + \
                           (((x0) >> s->sps->hshift[0]) << s->sps->pixel_shift)];
    uint8_t *dst5 = &s->frame->data[5][((y0) >> s->sps->vshift[1]) * s->frame->linesize[1] + \
                           (((x0) >> s->sps->hshift[1]) << s->sps->pixel_shift)];
    uint8_t *dst6 = &s->frame->data[6][((y0) >> s->sps->vshift[2]) * s->frame->linesize[2] + \
                           (((x0) >> s->sps->hshift[2]) << s->sps->pixel_shift)];

    MvDecoder_write_residual_initialization(dst4, cb_size, cb_size, s->frame->linesize[0]);
    MvDecoder_write_residual_initialization(dst5, cb_size >> s->sps->hshift[1], cb_size >> s->sps->vshift[1], s->frame->linesize[1]);
    MvDecoder_write_residual_initialization(dst6, cb_size >> s->sps->hshift[1], cb_size >> s->sps->vshift[1], s->frame->linesize[2]);



    //int bytestream_pu;
    //int bytestream_tu;
    SAMPLE_CTB(s->skip_flag, x_cb, y_cb) = 0;
    for (x = 0; x < 4; x++)
        lc->pu.intra_pred_mode[x] = 1;
    if (s->pps->transquant_bypass_enable_flag) {
        lc->cu.cu_transquant_bypass_flag = ff_hevc_cu_transquant_bypass_flag_decode(s);
        if (lc->cu.cu_transquant_bypass_flag)
            set_deblocking_bypass(s, x0, y0, log2_cb_size);
    } else
        lc->cu.cu_transquant_bypass_flag = 0;

    if (s->sh.slice_type != I_SLICE) {
        //Skip类型
        uint8_t skip_flag = ff_hevc_skip_flag_decode(s, x0, y0, x_cb, y_cb);
        //设置到skip_flag缓存中
        x = y_cb * min_cb_width + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->skip_flag[x], skip_flag, length);
            x += min_cb_width;
        }
        lc->cu.pred_mode = skip_flag ? MODE_SKIP : MODE_INTER;
    } else {
        x = y_cb * min_cb_width + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->skip_flag[x], 0, length);
            x += min_cb_width;
        }
    }

    if (SAMPLE_CTB(s->skip_flag, x_cb, y_cb)) {
        hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, idx);
        intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);

        if (!s->sh.disable_deblocking_filter_flag)
            ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
    } else {
        //读取预测模式（非 I Slice）
        if (s->sh.slice_type != I_SLICE)
            lc->cu.pred_mode = ff_hevc_pred_mode_decode(s);
        //不是帧内预测模式的时候
        //或者已经是最小CB的时候
        if (lc->cu.pred_mode != MODE_INTRA ||
            log2_cb_size == s->sps->log2_min_cb_size) {
            lc->cu.part_mode        = ff_hevc_part_mode_decode(s, log2_cb_size);
            lc->cu.intra_split_flag = lc->cu.part_mode == PART_NxN &&
                                      lc->cu.pred_mode == MODE_INTRA;
        }

        if (lc->cu.pred_mode == MODE_INTRA) {
            //帧内预测模式

            //PCM方式编码，不常见
            if (lc->cu.part_mode == PART_2Nx2N && s->sps->pcm_enabled_flag &&
                log2_cb_size >= s->sps->pcm.log2_min_pcm_cb_size &&
                log2_cb_size <= s->sps->pcm.log2_max_pcm_cb_size) {
                lc->cu.pcm_flag = ff_hevc_pcm_flag_decode(s);
            }
            if (lc->cu.pcm_flag) {
                intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
                ret = hls_pcm_sample(s, x0, y0, log2_cb_size);
                if (s->sps->pcm.loop_filter_disable_flag)
                    set_deblocking_bypass(s, x0, y0, log2_cb_size);

                if (ret < 0)
                    return ret;
            } else {
                //获取帧内预测模式
                intra_prediction_unit(s, x0, y0, log2_cb_size);
            }

        } else {
            //帧间预测模式
            intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);

            //帧间模式一共有8种划分模式
            //uint8_t* bytestream_before_pu = lc->cc.bytestream;
            switch (lc->cu.part_mode) {
            case PART_2Nx2N:
                /*
				 * PART_2Nx2N:
				 * +--------+--------+
				 * |                 |
				 * |                 |
				 * |                 |
				 * +        +        +
				 * |                 |
				 * |                 |
				 * |                 |
				 * +--------+--------+
            	 */
                //处理PU单元-运动补偿
                hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, idx);
                break;
            case PART_2NxN:

                /*
    			 * PART_2NxN:
    			 * +--------+--------+
    			 * |                 |
    			 * |                 |
    			 * |                 |
    			 * +--------+--------+
    			 * |                 |
    			 * |                 |
    			 * |                 |
    			 * +--------+--------+
    			 *
                 */
                /*
                 * hls_prediction_unit()参数：
                 * x0 : PU左上角x坐标
                 * y0 : PU左上角y坐标
                 * nPbW : PU宽度
                 * nPbH : PU高度
                 * log2_cb_size : CB大小取log2()的值
                 * partIdx : PU的索引号-分成4个块的时候取0-3，分成两个块的时候取0和1
                 */

                //上
                hls_prediction_unit(s, x0, y0,               cb_size, cb_size / 2, log2_cb_size, 0, idx);
                //下
                hls_prediction_unit(s, x0, y0 + cb_size / 2, cb_size, cb_size / 2, log2_cb_size, 1, idx);

                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));
                break;
            case PART_Nx2N:
                /*
    			 * PART_Nx2N:
    			 * +--------+--------+
    			 * |        |        |
    			 * |        |        |
    			 * |        |        |
    			 * +        +        +
    			 * |        |        |
    			 * |        |        |
    			 * |        |        |
    			 * +--------+--------+
    			 *
                 */
                //左
                hls_prediction_unit(s, x0,               y0, cb_size / 2, cb_size, log2_cb_size, 0, idx - 1);
                //右
                hls_prediction_unit(s, x0 + cb_size / 2, y0, cb_size / 2, cb_size, log2_cb_size, 1, idx - 1);

                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));
                break;
            case PART_2NxnU:

                /*
    			 * PART_2NxnU (Upper) :
    			 * +--------+--------+
    			 * |                 |
    			 * +--------+--------+
    			 * |                 |
    			 * +        +        +
    			 * |                 |
    			 * |                 |
    			 * |                 |
    			 * +--------+--------+
    			 *
                 */

                //上
                hls_prediction_unit(s, x0, y0,               cb_size, cb_size     / 4, log2_cb_size, 0, idx);
                //下
                hls_prediction_unit(s, x0, y0 + cb_size / 4, cb_size, cb_size * 3 / 4, log2_cb_size, 1, idx);


                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));
                break;
            case PART_2NxnD:
                /*
    			 * PART_2NxnD (Down) :
    			 * +--------+--------+
    			 * |                 |
    			 * |                 |
    			 * |                 |
    			 * +        +        +
    			 * |                 |
    			 * +--------+--------+
    			 * |                 |
    			 * +--------+--------+
    			 *
                 */

                //上
                hls_prediction_unit(s, x0, y0,                   cb_size, cb_size * 3 / 4, log2_cb_size, 0, idx);
                //下
                hls_prediction_unit(s, x0, y0 + cb_size * 3 / 4, cb_size, cb_size     / 4, log2_cb_size, 1, idx);


                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));
                break;
            case PART_nLx2N:
                /*
                 * PART_nLx2N (Left):
                 * +----+---+--------+
                 * |    |            |
                 * |    |            |
                 * |    |            |
                 * +    +   +        +
                 * |    |            |
                 * |    |            |
                 * |    |            |
                 * +----+---+--------+
                 *
                 */

                //左
                hls_prediction_unit(s, x0,               y0, cb_size     / 4, cb_size, log2_cb_size, 0, idx - 2);
                //右
                hls_prediction_unit(s, x0 + cb_size / 4, y0, cb_size * 3 / 4, cb_size, log2_cb_size, 1, idx - 2);

                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));
                break;
            case PART_nRx2N:

                /*
    			 * PART_nRx2N (Right):
    			 * +--------+---+----+
    			 * |            |    |
    			 * |            |    |
    			 * |            |    |
    			 * +        +   +    +
    			 * |            |    |
    			 * |            |    |
    			 * |            |    |
    			 * +--------+---+----+
    			 *
                 */

                //左
                hls_prediction_unit(s, x0,                   y0, cb_size * 3 / 4, cb_size, log2_cb_size, 0, idx - 2);
                //右
                hls_prediction_unit(s, x0 + cb_size * 3 / 4, y0, cb_size     / 4, cb_size, log2_cb_size, 1, idx - 2);

                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));

                break;
            case PART_NxN:

                /*
    			 * PART_NxN:
    			 * +--------+--------+
    			 * |        |        |
    			 * |        |        |
    			 * |        |        |
    			 * +--------+--------+
    			 * |        |        |
    			 * |        |        |
    			 * |        |        |
    			 * +--------+--------+
    			 *
                 */
                //MvDecoder set split bit of the tree

                hls_prediction_unit(s, x0,               y0,               cb_size / 2, cb_size / 2, log2_cb_size, 0, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0,               cb_size / 2, cb_size / 2, log2_cb_size, 1, idx - 1);
                hls_prediction_unit(s, x0,               y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 2, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 3, idx - 1);
                //MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));

                break;
            }
            //bytestream_pu = lc->cc.bytestream-bytestream_before_pu;
        }

        if (!lc->cu.pcm_flag) {
            if (lc->cu.pred_mode != MODE_INTRA &&
                !(lc->cu.part_mode == PART_2Nx2N && lc->pu.merge_flag)) {
                lc->cu.rqt_root_cbf = ff_hevc_no_residual_syntax_flag_decode(s);
            }
            if (lc->cu.rqt_root_cbf) {
                const static int cbf[2] = { 0 };
                lc->cu.max_trafo_depth = lc->cu.pred_mode == MODE_INTRA ?
                                         s->sps->max_transform_hierarchy_depth_intra + lc->cu.intra_split_flag :
                                         s->sps->max_transform_hierarchy_depth_inter;
                //处理TU四叉树
                //uint8_t* bytestream_before_tu = lc->cc.bytestream;
                ret = hls_transform_tree(s, x0, y0, x0, y0, x0, y0,
                                         log2_cb_size,
                                         log2_cb_size, 0, 0, cbf, cbf);
                //bytestream_tu = lc->cc.bytestream-bytestream_before_tu;
                if (ret < 0)
                    return ret;
            } else {
                if (!s->sh.disable_deblocking_filter_flag)
                    ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
            }
        }
    }

    if (s->pps->cu_qp_delta_enabled_flag && lc->tu.is_cu_qp_delta_coded == 0)
        ff_hevc_set_qPy(s, x0, y0, log2_cb_size);

    x = y_cb * min_cb_width + x_cb;
    for (y = 0; y < length; y++) {
        memset(&s->qp_y_tab[x], lc->qp_y, length);
        x += min_cb_width;
    }

    if(((x0 + (1<<log2_cb_size)) & qp_block_mask) == 0 &&
       ((y0 + (1<<log2_cb_size)) & qp_block_mask) == 0) {
        lc->qPy_pred = lc->qp_y;
    }

    set_ct_depth(s, x0, y0, log2_cb_size, lc->ct.depth);

    // MvDevoder: bytestream checkpoint of the start of cu
    int bytes_size_cu = lc->cc.bytestream - bytestream_last;
    //int bytes_pu_tu = bytestream_pu + bytestream_tu;
    // MvDeocder: fill totalByteSize of this CU.
    MvDecoder_write_size_buffer(s, x0, y0, log2_cb_size, bytes_size_cu);

    return 0;
}


/*
 * 解析四叉树结构，并且解码
 * 注意该函数是递归调用
 * 注释和处理：雷霄骅
 *
 *
 * s：HEVCContext上下文结构体
 * x_ctb：CB位置的x坐标
 * y_ctb：CB位置的y坐标
 * log2_cb_size：CB大小取log2之后的值
 * cb_depth：深度
 */
static int hls_coding_quadtree(HEVCContext *s, int x0, int y0,
                               int log2_cb_size, int cb_depth,
                               u_int8_t *MvDecoder_ctu_quadtree,
                               int MvDecoder_quadtree_bit_idx)
{
    /*
     * hls_coding_quadtree()完成了CTU解码工作
     * 函数是一个递归调用的函数，可以按照四叉树的句法格式解析CTU并获得其中的CU。对于每个CU会调用hls_coding_unit()进行解码。
     * hls_coding_unit()会调用hls_prediction_unit()对CU中的PU进行处理。
     *      hls_prediction_unit()调用luma_mc_uni()对亮度单向预测块进行运动补偿处理，
     *                           调用chroma_mc_uni()对色度单向预测块进行运动补偿处理，
     *                           调用luma_mc_bi()对亮度单向预测块进行运动补偿处理。
     *
     * hls_coding_unit()会调用hls_transform_tree()对CU中的TU进行处理。
     * hls_transform_tree()是一个递归调用的函数，可以按照四叉树的句法格式解析并获得其中的TU。
     * 对于每一个TU会调用hls_transform_unit()进行解码。hls_transform_unit()会进行帧内预测，并且调用ff_hevc_hls_residual_coding()解码DCT残差数据。
     */
    HEVCLocalContext *lc = s->HEVClc;

    //CB的大小,split flag=0
    //log2_cb_size为CB大小取log之后的结果
    const int cb_size    = 1 << log2_cb_size;
    int ret;
    int qp_block_mask = (1<<(s->sps->log2_ctb_size - s->pps->diff_cu_qp_delta_depth)) - 1;
    int split_cu_flag;

    //确定CU是否还会划分？
    lc->ct.depth = cb_depth;
    if (x0 + cb_size <= s->sps->width  &&
        y0 + cb_size <= s->sps->height &&
        log2_cb_size > s->sps->log2_min_cb_size) {
        split_cu_flag = ff_hevc_split_coding_unit_flag_decode(s, cb_depth, x0, y0);
    } else {
        split_cu_flag = (log2_cb_size > s->sps->log2_min_cb_size);
    }
    if (s->pps->cu_qp_delta_enabled_flag &&
        log2_cb_size >= s->sps->log2_ctb_size - s->pps->diff_cu_qp_delta_depth) {
        lc->tu.is_cu_qp_delta_coded = 0;
        lc->tu.cu_qp_delta          = 0;
    }

	if (s->sh.cu_chroma_qp_offset_enabled_flag &&
        log2_cb_size >= s->sps->log2_ctb_size - s->pps->diff_cu_chroma_qp_offset_depth) {
        lc->tu.is_cu_chroma_qp_offset_coded = 0;
	}
    if (split_cu_flag) {
        //MvDecoder set split bit of the tree
        MvDecoder_ctu_quadtree[MvDecoder_quadtree_bit_idx / 8] |= (1 << (MvDecoder_quadtree_bit_idx % 8));

        //如果CU还可以继续划分，则继续解析划分后的CU
        //注意这里是递归调用


        //CB的大小,split flag=1
        const int cb_size_split = cb_size >> 1;
        const int x1 = x0 + cb_size_split;
        const int y1 = y0 + cb_size_split;
        /*
         * (x0, y0)  (x1, y0)
		 *     +--------+--------+
		 *     |                 |
		 *     |        |        |
		 *     |                 |
		 *     +  --  --+ --  -- +
		 * (x0, y1)  (x1, y1)    |
		 *     |        |        |
		 *     |                 |
		 *     +--------+--------+
		 *
         */


        int more_data = 0;

        //注意：
        //CU大小减半，log2_cb_size-1
        //深度d加1，cb_depth+1
        //MvDecoder: child bit idx = 4 * MvDecoder_quadtree_grid_idx + 1
        more_data = hls_coding_quadtree(s, x0, y0, log2_cb_size - 1, cb_depth + 1, MvDecoder_ctu_quadtree, 4 * MvDecoder_quadtree_bit_idx + 1);
        if (more_data < 0)
            return more_data;

        if (more_data && x1 < s->sps->width) {
            more_data = hls_coding_quadtree(s, x1, y0, log2_cb_size - 1, cb_depth + 1, MvDecoder_ctu_quadtree, 4 * MvDecoder_quadtree_bit_idx + 2);
            if (more_data < 0)
                return more_data;
        }
        if (more_data && y1 < s->sps->height) {
            more_data = hls_coding_quadtree(s, x0, y1, log2_cb_size - 1, cb_depth + 1, MvDecoder_ctu_quadtree, 4 * MvDecoder_quadtree_bit_idx + 3);
            if (more_data < 0)
                return more_data;
        }
        if (more_data && x1 < s->sps->width &&
            y1 < s->sps->height) {
            more_data = hls_coding_quadtree(s, x1, y1, log2_cb_size - 1, cb_depth + 1, MvDecoder_ctu_quadtree, 4 * MvDecoder_quadtree_bit_idx + 4);
            if (more_data < 0)
                return more_data;
        }

        if(((x0 + (1<<log2_cb_size)) & qp_block_mask) == 0 &&
            ((y0 + (1<<log2_cb_size)) & qp_block_mask) == 0)
            lc->qPy_pred = lc->qp_y;

        if (more_data)
            return ((x1 + cb_size_split) < s->sps->width ||
                    (y1 + cb_size_split) < s->sps->height);
        else
            return 0;
    } else {

        /*
         * (x0, y0)
		 *     +--------+--------+
		 *     |                 |
		 *     |                 |
		 *     |                 |
		 *     +                 +
		 *     |                 |
		 *     |                 |
		 *     |                 |
		 *     +--------+--------+
         *
         */
        //注意处理的是不可划分的CU单元
        //处理CU单元-真正的解码
        ret = hls_coding_unit(s, x0, y0, log2_cb_size, MvDecoder_ctu_quadtree, MvDecoder_quadtree_bit_idx);
        if (ret < 0)
            return ret;
        if ((!((x0 + cb_size) &
               ((1 << (s->sps->log2_ctb_size))) - 1) ||
             (x0 + cb_size >= s->sps->width)) &&
            (!((y0 + cb_size) &
               ((1 << (s->sps->log2_ctb_size))) - 1) ||
             (y0 + cb_size >= s->sps->height))) {
            int end_of_slice_flag = ff_hevc_end_of_slice_flag_decode(s);
            return !end_of_slice_flag;
        } else {
            return 1;
        }
    }

    return 0;
}
/* 从源代码可以看出，hls_coding_quadtree()首先调用ff_hevc_split_coding_unit_flag_decode()判断当前CU是否还需要划分。
 * 如果需要划分的话，就会递归调用4次hls_coding_quadtree()分别对4个子块继续进行四叉树解析；
 * 如果不需要划分，就会调用hls_coding_unit()对CU进行解码。
 * 总而言之，hls_coding_quadtree()会解析出来一个CTU中的所有CU，并且对每一个CU逐一调用hls_coding_unit()进行解码。
 * 一个CTU中CU的解码顺序如下图所示。图中a, b, c …即代表了的先后顺序。
 * https://img-blog.csdn.net/20150613173036048
*/
static void hls_decode_neighbour(HEVCContext *s, int x_ctb, int y_ctb,
                                 int ctb_addr_ts)
{
    HEVCLocalContext *lc  = s->HEVClc;
    int ctb_size          = 1 << s->sps->log2_ctb_size;
    int ctb_addr_rs       = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];
    int ctb_addr_in_slice = ctb_addr_rs - s->sh.slice_addr;

    int tile_left_boundary, tile_up_boundary;
    int slice_left_boundary, slice_up_boundary;

    s->tab_slice_address[ctb_addr_rs] = s->sh.slice_addr;

    if (s->pps->entropy_coding_sync_enabled_flag) {
        if (x_ctb == 0 && (y_ctb & (ctb_size - 1)) == 0)
            lc->first_qp_group = 1;
        lc->end_of_tiles_x = s->sps->width;
    } else if (s->pps->tiles_enabled_flag) {
        if (ctb_addr_ts && s->pps->tile_id[ctb_addr_ts] != s->pps->tile_id[ctb_addr_ts - 1]) {
            int idxX = s->pps->col_idxX[x_ctb >> s->sps->log2_ctb_size];
            lc->end_of_tiles_x   = x_ctb + (s->pps->column_width[idxX] << s->sps->log2_ctb_size);
            lc->first_qp_group   = 1;
        }
    } else {
        lc->end_of_tiles_x = s->sps->width;
    }

    lc->end_of_tiles_y = FFMIN(y_ctb + ctb_size, s->sps->height);

    if (s->pps->tiles_enabled_flag) {
        tile_left_boundary = x_ctb > 0 &&
                             s->pps->tile_id[ctb_addr_ts] != s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[ctb_addr_rs-1]];
        slice_left_boundary = x_ctb > 0 &&
                              s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - 1];
        tile_up_boundary  = y_ctb > 0 &&
                            s->pps->tile_id[ctb_addr_ts] != s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[ctb_addr_rs - s->sps->ctb_width]];
        slice_up_boundary = y_ctb > 0 &&
                            s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - s->sps->ctb_width];
    } else {
        tile_left_boundary =
        tile_up_boundary   = 0;
        slice_left_boundary = ctb_addr_in_slice <= 0;
        slice_up_boundary   = ctb_addr_in_slice < s->sps->ctb_width;
    }
    lc->slice_or_tiles_left_boundary = slice_left_boundary + (tile_left_boundary << 1);
    lc->slice_or_tiles_up_boundary   = slice_up_boundary   + (tile_up_boundary   << 1);
    lc->ctb_left_flag = ((x_ctb > 0) && (ctb_addr_in_slice > 0)                  && !tile_left_boundary);
    lc->ctb_up_flag   = ((y_ctb > 0) && (ctb_addr_in_slice >= s->sps->ctb_width) && !tile_up_boundary);
    lc->ctb_up_right_flag = ((y_ctb > 0)                 && (ctb_addr_in_slice+1 >= s->sps->ctb_width) && (s->pps->tile_id[ctb_addr_ts] == s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[ctb_addr_rs+1 - s->sps->ctb_width]]));
    lc->ctb_up_left_flag  = ((x_ctb > 0) && (y_ctb > 0)  && (ctb_addr_in_slice-1 >= s->sps->ctb_width) && (s->pps->tile_id[ctb_addr_ts] == s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[ctb_addr_rs-1 - s->sps->ctb_width]]));
}

static int hls_decode_entry(AVCodecContext *avctxt, void *isFilterThread)
{
    /*
    * Decode entry
    * 注释：雷霄骅
    * leixiaohua1020@126.com
    * http://blog.csdn.net/leixiaohua1020
    */
    /*
     * hls_decode_entry()以CTB为单位处理输入的视频流。每个CTB的压缩数据经过下面两个基本步骤进行处理：
     * （1）调用hls_coding_quadtree()对CTB解码。其中包括了CU、PU、TU的解码.
     * （2）调用ff_hevc_hls_filters()进行滤波。其中包括去块效应滤波和SAO滤波.
     *
     *  hls_decode_entry()函数完成了Slice解码工作
     */
    HEVCContext *s  = avctxt->priv_data;
    //CTB Size
    int ctb_size    = 1 << s->sps->log2_ctb_size;
    int more_data   = 1;
    int x_ctb       = 0;
    int y_ctb       = 0;
    int ctb_addr_ts = s->pps->ctb_addr_rs_to_ts[s->sh.slice_ctb_addr_rs];

    if (!ctb_addr_ts && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible initial tile.\n");
        return AVERROR_INVALIDDATA;
    }

    if (s->sh.dependent_slice_segment_flag) {
        int prev_rs = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts - 1];
        if (s->tab_slice_address[prev_rs] != s->sh.slice_addr) {
            av_log(s->avctx, AV_LOG_ERROR, "Previous slice segment missing\n");
            return AVERROR_INVALIDDATA;
        }
    }

    while (more_data && ctb_addr_ts < s->sps->ctb_size) {
        int ctb_addr_rs = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];

        // CTB position x and y.
        x_ctb = FFUMOD(ctb_addr_rs, s->sps->ctb_width) << s->sps->log2_ctb_size;
        y_ctb = FFUDIV(ctb_addr_rs, s->sps->ctb_width) << s->sps->log2_ctb_size;
        //MvDecoder: as index of coding block
        // init neighbour parameters
        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);
        // init CABAC
        /*Context-adaptive binary arithmetic coding (CABAC) is a form of entropy encoding used in the H.264/MPEG-4 AVC[1][2] and High Efficiency Video Coding (HEVC) standards
        */
        ff_hevc_cabac_init(s, ctb_addr_ts);
        // sample adaptive offset
        hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;

        //MvDecoder: get grid index for the CUT quadtree:
        //For each quadtree, need 1+4+16+64=85 bits to save(including PU partition).
        //85 bits = 11 Bytes < 12 Bytes. Use 12 Bytes/u_int_8/u_char to hold one grid.
        //quadtree data start at 1024 bytes onwards
        uint8_t *MvDecoder_ctu_quadtree = s->frame->data[3] + ((s->frame->linesize[0]>>1)*(s->frame->height>>1))*3 + 1024 + ctb_addr_rs*12;
        /*
         * CU
         *
		 * 64x64 block
		 * depth=0
		 * when split_flag=1, split to 4 32x32 blocks
		 *
		 * +--------+--------+--------+--------+--------+--------+--------+--------+
		 * |                                                                       |
		 * |                                   |                                   |
		 * |                                                                       |
		 * +                                   |                                   +
		 * |                                                                       |
		 * |                                   |                                   |
		 * |                                                                       |
		 * +                                   |                                   +
		 * |                                                                       |
		 * |                                   |                                   |
		 * |                                                                       |
		 * +                                   |                                   +
		 * |                                                                       |
		 * |                                   |                                   |
		 * |                                                                       |
		 * + --  --  --  --  --  --  --  --  --+ --  --  --  --  --  --  --  --  --+
		 * |                                   |                                   |
		 * |                                                                       |
		 * |                                   |                                   |
		 * +                                                                       +
		 * |                                   |                                   |
		 * |                                                                       |
		 * |                                   |                                   |
		 * +                                                                       +
		 * |                                   |                                   |
		 * |                                                                       |
		 * |                                   |                                   |
		 * +                                                                       +
		 * |                                   |                                   |
		 * |                                                                       |
		 * |                                   |                                   |
		 * +--------+--------+--------+--------+--------+--------+--------+--------+
         *
         *
         * 32x32 block
		 * depth=1
		 * when split_flag=1, split to 4 16x16 blocks
		 *
		 * +--------+--------+--------+--------+
		 * |                                   |
		 * |                 |                 |
		 * |                                   |
		 * +                 |                 +
		 * |                                   |
		 * |                 |                 |
		 * |                                   |
		 * + --  --  --  --  + --  --  --  --  +
		 * |                                   |
		 * |                 |                 |
		 * |                                   |
		 * +                 |                 +
		 * |                                   |
		 * |                 |                 |
		 * |                                   |
		 * +--------+--------+--------+--------+
         *
         *
         *
         * 16x16 block
		 * depth=2
		 * when split_flag=1, split to 4 8x8 blocks
		 *
		 * +--------+--------+
		 * |                 |
		 * |        |        |
		 * |                 |
		 * +  --  --+ --  -- +
		 * |                 |
		 * |        |        |
		 * |                 |
		 * +--------+--------+
         *
         *
         * 8x8 block
		 * depth=3
		 * when split_flag=1, split to 4 4x4 blocks
         *
		 * +----+----+
		 * |    |    |
		 * + -- + -- +
		 * |    |    |
		 * +----+----+
         *
         */
        /*
         * decode quadtree structure
         *
         * in hls_coding_quadtree(HEVCContext *s, int x0, int y0, int log2_cb_size, int cb_depth)：
         * s：HEVCContext
         * x_ctb：CB position x
         * y_ctb：CB position y
         * log2_cb_size：CB size after take log2
         * cb_depth：depth
         */

        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0, MvDecoder_ctu_quadtree, 0);
        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            return more_data;
        }

        ctb_addr_ts++;
        s->HEVClc->ctb_tile_rs++;

        // save decode states for further usage
        ff_hevc_save_states(s, ctb_addr_ts);
        // de-block filter
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);
    }

    if (x_ctb + ctb_size >= s->sps->width &&
        y_ctb + ctb_size >= s->sps->height)
        ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);

    return ctb_addr_ts;
}

#if PARALLEL_SLICE
static int hls_decode_entry_slice(HEVCContext *s)
{
    int ctb_size    = 1 << s->sps->log2_ctb_size;
    int more_data   = 1;
    int x_ctb       = 0;
    int y_ctb       = 0;
    int ctb_addr_ts = s->pps->ctb_addr_rs_to_ts[s->sh.slice_ctb_addr_rs];

    if (!ctb_addr_ts && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible initial tile.\n");
        return AVERROR_INVALIDDATA;
    }

    if (s->sh.dependent_slice_segment_flag) {
        int prev_rs = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts - 1];
        if (s->tab_slice_address[prev_rs] != s->sh.slice_addr) {
            av_log(s->avctx, AV_LOG_ERROR, "Previous slice segment missing\n");
            return AVERROR_INVALIDDATA;
        }
    }

    while (more_data) {
        int ctb_addr_rs = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];

        x_ctb = FFUMOD(ctb_addr_rs, s->sps->ctb_width) << s->sps->log2_ctb_size;
        y_ctb = FFUDIV(ctb_addr_rs, s->sps->ctb_width) << s->sps->log2_ctb_size;
        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);

        ff_hevc_cabac_init(s, ctb_addr_ts);

        hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0);

        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            return more_data;
        }
        ctb_addr_ts++;
        ff_hevc_save_states(s, ctb_addr_ts);
#if PARALLEL_FILTERS
        ff_hevc_hls_filters_slice(s, x_ctb, y_ctb, ctb_size);
#endif
    }
    return ctb_addr_ts;
}
#endif
static int hls_decode_entry_wpp(AVCodecContext *avctxt, void *input_ctb_row, int job, int self_id)
{
    HEVCContext *s1  = avctxt->priv_data, *s;
    HEVCLocalContext *lc;
    int ctb_size    = 1<< s1->sps->log2_ctb_size;
    int more_data   = 1;
    int *ctb_row_p    = input_ctb_row;
    int ctb_row = ctb_row_p[job];
    int ctb_addr_rs = s1->sh.slice_ctb_addr_rs + s1->pps->ctb_row_to_rs[ctb_row];
    int ctb_addr_ts = s1->pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    int thread = ctb_row % s1->threads_number;
    int ret;

    s = s1->sList[self_id];
    s->HEVClc->ctb_tile_rs = ctb_addr_rs;
    lc = s->HEVClc;

    if(ctb_row) {
        ret = init_get_bits8(&lc->gb, s->data + s->sh.offset[ctb_row - 1], s->sh.size[ctb_row - 1]);

        if (ret < 0)
            return ret;
        ff_init_cabac_decoder(&lc->cc, s->data + s->sh.offset[(ctb_row)-1], s->sh.size[ctb_row - 1]);
    }

    while(more_data && ctb_addr_ts < s->sps->ctb_size) {
        int x_ctb = (ctb_addr_rs % s->sps->ctb_width) << s->sps->log2_ctb_size;
        int y_ctb = (ctb_addr_rs / s->sps->ctb_width) << s->sps->log2_ctb_size;



        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);

        ff_thread_await_progress2(s->avctx, ctb_row, thread, SHIFT_CTB_WPP);

        if (avpriv_atomic_int_get(&s1->wpp_err)){
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }

        ff_hevc_cabac_init(s, ctb_addr_ts);
        hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;


        //For each quadtree, need 1+4+16+64=85 bits to save(including PU partition).
        //85 bits = 11 Bytes < 12 Bytes. Use 12 Bytes/u_int_8/u_char to hold one grid.
        //quadtree data start at 1024 bytes onwards
        uint8_t *MvDecoder_ctu_quadtree = s->frame->data[3] + ((s->frame->linesize[0]>>1)*(s->frame->coded_height>>1))*3 + 1024 + ctb_addr_rs*12;
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0, MvDecoder_ctu_quadtree, 0);
        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            avpriv_atomic_int_set(&s1->wpp_err,  1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return more_data;
        }

        ctb_addr_ts++;
        s->HEVClc->ctb_tile_rs++;

        ff_hevc_save_states(s, ctb_addr_ts);
        ff_thread_report_progress2(s->avctx, ctb_row, thread, 1);
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);

        if (!more_data && (x_ctb+ctb_size) < s->sps->width && ctb_row != s->sh.num_entry_point_offsets) {
            avpriv_atomic_int_set(&s1->wpp_err,  1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return 0;
        }

        if ((x_ctb+ctb_size) >= s->sps->width && (y_ctb+ctb_size) >= s->sps->height ) {
            ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }
        ctb_addr_rs       = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        x_ctb+=ctb_size;

        if(x_ctb >= s->sps->width) {
            break;
        }
    }
    ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);

    return ctb_addr_ts;
}

// wpp parallel version
static int hls_decode_entry_wpp_in_tiles(AVCodecContext *avctxt, int *input_ctb_row, int job, int self_id)
{
    HEVCContext *s1  = avctxt->priv_data, *s;
    HEVCLocalContext *lc;
    int ctb_size    = 1<< s1->sps->log2_ctb_size;
    int more_data   = 1;
    int *ctb_row_p    = input_ctb_row;
    int ctb_row = ctb_row_p[job];
    int ctb_addr_rs = s1->sh.slice_ctb_addr_rs + s1->pps->ctb_row_to_rs[ctb_row];
    int ctb_addr_ts = s1->pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    int thread = ctb_row % s1->threads_number;
    int ret;

    s = s1->sList[self_id];
    s->HEVClc->ctb_tile_rs = ctb_addr_rs;
    lc = s->HEVClc;

    if(ctb_row) {
        ret = init_get_bits8(&lc->gb, s->data + s->sh.offset[ctb_row - 1], s->sh.size[ctb_row - 1]);

        if (ret < 0)
            return ret;
        ff_init_cabac_decoder(&lc->cc, s->data + s->sh.offset[(ctb_row)-1], s->sh.size[ctb_row - 1]);
    }

    while(more_data && ctb_addr_ts < s->sps->ctb_size) {
        int x_ctb = (ctb_addr_rs % s->sps->ctb_width) << s->sps->log2_ctb_size;
        int y_ctb = (ctb_addr_rs / s->sps->ctb_width) << s->sps->log2_ctb_size;

        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);

        ff_thread_await_progress2(s->avctx, ctb_row, thread, SHIFT_CTB_WPP);

        if (avpriv_atomic_int_get(&s1->wpp_err)){
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }

        ff_hevc_cabac_init(s, ctb_addr_ts);
        hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;

        //MvDecoder: get grid index for the CUT quadtree:
        //For each quadtree, need 1+4+16+64=85 bits to save(including PU partition).
        //85 bits = 11 Bytes < 12 Bytes. Use 12 Bytes/u_int_8/u_char to hold one grid.
        //quadtree data start at 1024 bytes onwards
        uint8_t *MvDecoder_ctu_quadtree = s->frame->data[3] + ((s->frame->linesize[0]>>1)*(s->frame->coded_height>>1))*3 + 1024 + ctb_addr_rs*12;
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0, MvDecoder_ctu_quadtree, 0);
        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            avpriv_atomic_int_set(&s1->wpp_err,  1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return more_data;
        }

        ctb_addr_ts++;
        s->HEVClc->ctb_tile_rs++;

        ff_hevc_save_states(s, ctb_addr_ts);
        ff_thread_report_progress2(s->avctx, ctb_row, thread, 1);
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);

        if (!more_data && (x_ctb+ctb_size) < s->sps->width && ctb_row != s->sh.num_entry_point_offsets) {
            avpriv_atomic_int_set(&s1->wpp_err,  1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return 0;
        }

        if ((x_ctb+ctb_size) >= s->sps->width && (y_ctb+ctb_size) >= s->sps->height ) {
            ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }
        ctb_addr_rs       = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        x_ctb+=ctb_size;

        if(x_ctb >= s->sps->width) {
            break;
        }
    }
    ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);

    return ctb_addr_ts;
}

static int hls_decode_entry_tiles(AVCodecContext *avctxt, int *input_ctb_row, int job, int self_id)
{
    HEVCContext *s = avctxt->priv_data;
    HEVCLocalContext *lc;
    int ctb_size    = 1 << s->sps->log2_ctb_size;
    int x_ctb = 0, y_ctb = 0;
    int more_data  = 1;
    int *ctb_row_p  = input_ctb_row;
    int ctb_row     = ctb_row_p[job];
    int tile_id     = s->pps->tile_id[s->pps->ctb_addr_rs_to_ts[s->sh.slice_ctb_addr_rs]]+ctb_row;
    int ctb_addr_rs = ctb_row == 0 ? s->sh.slice_ctb_addr_rs : s->pps->tile_pos_rs[tile_id];
    int ctb_addr_ts = s->pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    int ret;

    s = s->sList[self_id];
    lc = s->HEVClc;

    if(ctb_row) {
        ret = init_get_bits8(&lc->gb, s->data + s->sh.offset[ctb_row - 1], s->sh.size[ctb_row - 1]);
        if (ret < 0)
            return ret;
    }
    while (more_data) {

        ctb_addr_rs = s->pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        x_ctb = (ctb_addr_rs % s->sps->ctb_width) << s->sps->log2_ctb_size;
        y_ctb = (ctb_addr_rs / s->sps->ctb_width) << s->sps->log2_ctb_size;

        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);
        ff_hevc_cabac_init(s, ctb_addr_ts);
        hls_sao_param(s, x_ctb >> s->sps->log2_ctb_size, y_ctb >> s->sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;

        //MvDecoder: get grid index for the CUT quadtree:
        //For each quadtree, need 1+4+16+64=85 bits to save(including PU partition).
        //85 bits = 11 Bytes < 12 Bytes. Use 12 Bytes/u_int_8/u_char to hold one grid.
        //quadtree data start at 1024 bytes onwards
        uint8_t *MvDecoder_ctu_quadtree = s->frame->data[3] + ((s->frame->linesize[0]>>1)*(s->frame->coded_height>>1))*3 + 1024 + ctb_addr_rs*12;

        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->sps->log2_ctb_size, 0, MvDecoder_ctu_quadtree, 0);
        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            return more_data;
        }
        ctb_addr_ts++;
        s->HEVClc->ctb_tile_rs++;
        if (x_ctb + ctb_size < s->sps->width || y_ctb + ctb_size < s->sps->height)
            if (s->pps->tile_id[ctb_addr_ts] != s->pps->tile_id[ctb_addr_ts-1])
                break;
    }
    return ctb_addr_ts;
}

static void tiles_filters(HEVCContext *s)
{
    uint16_t ctb_size        = 1 << s->sps->log2_ctb_size;
    int min_size            = 1 << s->sps->log2_min_tb_size;
    int ctb_addr_rs;
    int x0, y0, i;

    // Deblocking and SAO filters
    if(s->pps->loop_filter_across_tiles_enabled_flag) {
        for (i = 1; i < s->pps->num_tile_columns; i++) {
            int slice_left_boundary;
            ctb_addr_rs = s->pps->tile_pos_rs[i];
            x0 = (ctb_addr_rs % s->sps->ctb_width) << s->sps->log2_ctb_size;
            for (y0 = 0; y0 < s->sps->height; y0+=min_size) {
                ctb_addr_rs = (x0 >> s->sps->log2_ctb_size) + ((y0 >> s->sps->log2_ctb_size) * s->sps->ctb_width);
                slice_left_boundary = ((x0 > 0) &&
                                        (s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - 1]));
                ff_hevc_deblocking_boundary_strengths_v(s, x0, y0, !s->filter_slice_edges[ctb_addr_rs] && slice_left_boundary);
            }
        }
        for (i = 1; i < s->pps->num_tile_rows; i++) {
            int slice_up_boundary;
            ctb_addr_rs = s->pps->tile_pos_rs[i * s->pps->num_tile_columns];
            y0 = (ctb_addr_rs / s->sps->ctb_width) << s->sps->log2_ctb_size;
            for (x0 = 0; x0 < s->sps->width; x0+=min_size) {
                ctb_addr_rs = (x0 >> s->sps->log2_ctb_size) + ((y0 >> s->sps->log2_ctb_size) * s->sps->ctb_width);
                slice_up_boundary = ((y0 > 0) &&
                                        (s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - s->sps->ctb_width]));
                ff_hevc_deblocking_boundary_strengths_h(s, x0, y0, !s->filter_slice_edges[ctb_addr_rs] && slice_up_boundary);
            }
        }
    }

    for (y0 = 0; y0 < s->sps->height; y0 += ctb_size)
        for (x0 = 0; x0 < s->sps->width; x0 += ctb_size)
            ff_hevc_hls_filter(s, x0, y0, ctb_size);
}
#if !PARALLEL_FILTERS
static void slices_filters(HEVCContext *s)
{
    uint16_t ctb_size        = 1 << s->sps->log2_ctb_size;
    int x0, y0;
    // Deblocking and SAO filters
    for (y0 = 0; y0 < s->sps->height; y0 += ctb_size)
        for (x0 = 0; x0 < s->sps->width; x0 += ctb_size)
            ff_hevc_hls_filter(s, x0, y0, ctb_size);
}
#endif


static int hls_slice_data(HEVCContext *s, const uint8_t *nal, int length)
{
    HEVCLocalContext *lc = s->HEVClc;
    int *ret = av_malloc((s->sh.num_entry_point_offsets + 1) * sizeof(int));
    int *arg = av_malloc((s->sh.num_entry_point_offsets + 1) * sizeof(int));
    int offset;
    int startheader, cmpt = 0;
    int i, j, res = 0;

    ff_alloc_entries(s->avctx, s->sh.num_entry_point_offsets + 1);

    if (s->sh.num_entry_point_offsets > 0) {
        offset = (lc->gb.index >> 3);
        for (j = 0, cmpt = 0, startheader = offset + s->sh.entry_point_offset[0]; j < s->skipped_bytes; j++) {
            if (s->skipped_bytes_pos[j] >= offset && s->skipped_bytes_pos[j] < startheader) {
                startheader--;
                cmpt++;
            }
        }

        for (i = 1; i < s->sh.num_entry_point_offsets; i++) {
            offset += (s->sh.entry_point_offset[i - 1] - cmpt);
            for (j = 0, cmpt = 0, startheader = offset
                    + s->sh.entry_point_offset[i]; j < s->skipped_bytes; j++) {
                if (s->skipped_bytes_pos[j] >= offset && s->skipped_bytes_pos[j] < startheader) {
                    startheader--;
                    cmpt++;
                }
            }
            s->sh.size[i - 1] = s->sh.entry_point_offset[i] - cmpt;
            s->sh.offset[i - 1] = offset;
        }
        offset += s->sh.entry_point_offset[s->sh.num_entry_point_offsets - 1] - cmpt;
        s->sh.size[s->sh.num_entry_point_offsets - 1] = length - offset;
        s->sh.offset[s->sh.num_entry_point_offsets - 1] = offset;

        if(s->sh.offset[i - 1]+s->sh.size[i - 1] > length) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "hls_slice_data:  packet length < image size : %d < %d\n",
                   length, s->sh.offset[i - 1]+s->sh.size[i - 1]);
            return AVERROR_INVALIDDATA;
        }

        avpriv_atomic_int_set(&s->wpp_err, 0);
        ff_reset_entries(s->avctx);
    }
    s->data = nal;
    if (s->sh.first_slice_in_pic_flag){
        s->HEVClc->ctb_tile_rs = 0;
    }
    for (i = 1; i < s->threads_number; i++) {
        if (s->sh.first_slice_in_pic_flag){
            s->sList[i]->HEVClc->ctb_tile_rs = 0;
        }
        s->sList[i]->HEVClc->first_qp_group = 1;
        s->sList[i]->HEVClc->qp_y = s->sList[0]->HEVClc->qp_y;
        memcpy(s->sList[i], s, sizeof(HEVCContext));
        s->sList[i]->HEVClc = s->HEVClcList[i];
    }

    for (i = 0; i <= s->sh.num_entry_point_offsets; i++) {
        arg[i] = i;
        ret[i] = 0;
    }

    if (s->pps->entropy_coding_sync_enabled_flag && s->pps->tiles_enabled_flag && s->threads_number!=1)
        s->avctx->execute2(s->avctx, (void *) hls_decode_entry_wpp_in_tiles, arg, ret, s->sh.num_entry_point_offsets + 1);
    else if (s->pps->entropy_coding_sync_enabled_flag && s->threads_number!=1)
        s->avctx->execute2(s->avctx, (void *) hls_decode_entry_wpp  , arg, ret, s->sh.num_entry_point_offsets + 1);
    else if (s->pps->tiles_enabled_flag        && s->threads_number!=1)
        s->avctx->execute2(s->avctx, (void *) hls_decode_entry_tiles, arg, ret, s->sh.num_entry_point_offsets + 1);
    else
        s->avctx->execute(s->avctx, hls_decode_entry, arg, ret , 1, sizeof(int));

    res = ret[s->threads_number==1 ? 0:s->sh.num_entry_point_offsets];

    av_free(ret);
    av_free(arg);
    return res;
}




    

/**
 * @return AVERROR_INVALIDDATA if the packet is not a valid NAL unit,
 * 0 if the unit should be skipped, 1 otherwise
 */
static int hls_nal_unit(HEVCContext *s)
{
    int ret;
    GetBitContext *gb = &s->HEVClc->gb;

    if (get_bits1(gb) != 0)
        return AVERROR_INVALIDDATA;

    s->nal_unit_type = get_bits(gb, 6);
    ret              = get_bits(gb, 6);

    s->temporal_id = get_bits(gb, 3) - 1;
    if (s->temporal_id < 0)
        return AVERROR_INVALIDDATA;
    av_log(s->avctx, AV_LOG_DEBUG,
           "nal_unit_type: %d, nuh_layer_id: %d temporal_id: %d decoder id %d\n",
           s->nal_unit_type, ret, s->temporal_id , s->decoder_id);
    return ret;
}

static int set_side_data(HEVCContext *s)
{
    AVFrame *out = s->ref->frame;

    if (s->sei_frame_packing_present &&
        s->frame_packing_arrangement_type >= 3 &&
        s->frame_packing_arrangement_type <= 5 &&
        s->content_interpretation_type > 0 &&
        s->content_interpretation_type < 3) {
        AVStereo3D *stereo = av_stereo3d_create_side_data(out);
        if (!stereo)
            return AVERROR(ENOMEM);

        switch (s->frame_packing_arrangement_type) {
        case 3:
            if (s->quincunx_subsampling)
                stereo->type = AV_STEREO3D_SIDEBYSIDE_QUINCUNX;
            else
                stereo->type = AV_STEREO3D_SIDEBYSIDE;
            break;
        case 4:
            stereo->type = AV_STEREO3D_TOPBOTTOM;
            break;
        case 5:
            stereo->type = AV_STEREO3D_FRAMESEQUENCE;
            break;
        }

        if (s->content_interpretation_type == 2)
            stereo->flags = AV_STEREO3D_FLAG_INVERT;
    }

    return 0;
}

static int hevc_ref_frame(HEVCContext *s, HEVCFrame *dst, HEVCFrame *src)
{
    int ret;

    ret = ff_thread_ref_frame(&dst->tf, &src->tf);
    if (ret < 0)
        return ret;

    dst->tab_mvf_buf = av_buffer_ref(src->tab_mvf_buf);
    if (!dst->tab_mvf_buf)
        goto fail;
    dst->tab_mvf = src->tab_mvf;

    dst->rpl_tab_buf = av_buffer_ref(src->rpl_tab_buf);
    if (!dst->rpl_tab_buf)
        goto fail;
    dst->rpl_tab = src->rpl_tab;

    dst->rpl_buf = av_buffer_ref(src->rpl_buf);
    if (!dst->rpl_buf)
        goto fail;

    dst->poc        = src->poc;
    dst->ctb_count  = src->ctb_count;
    dst->window     = src->window;
    dst->flags      = src->flags;
    dst->sequence   = src->sequence;

    return 0;
fail:
    ff_hevc_unref_frame(s, dst, ~0);
    return AVERROR(ENOMEM);
}


static int hevc_frame_start(HEVCContext *s)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pic_size_in_ctb  = ((s->sps->width  >> s->sps->log2_min_cb_size) + 1) *
                           ((s->sps->height >> s->sps->log2_min_cb_size) + 1);
    int ret = 0;
    AVFrame *cur_frame;
    av_log(s->avctx, AV_LOG_DEBUG, "frame start %d\n", s->decoder_id);


    memset(s->horizontal_bs, 0, s->bs_width * s->bs_height);
    memset(s->vertical_bs,   0, s->bs_width * s->bs_height);
    memset(s->cbf_luma,      0, s->sps->min_tb_width * s->sps->min_tb_height);
    memset(s->tab_slice_address, -1, pic_size_in_ctb * sizeof(*s->tab_slice_address));
#if PARALLEL_SLICE
    memset(s->decoded_rows, 0,s->sps->ctb_height);
#endif
    s->is_decoded        = 0;
    s->first_nal_type    = s->nal_unit_type;

    if (s->pps->tiles_enabled_flag)
        lc->end_of_tiles_x = s->pps->column_width[0] << s->sps->log2_ctb_size;
#ifdef SVC_EXTENSION
    if (s->nuh_layer_id) {
#if ACTIVE_PU_UPSAMPLING
        memset (s->is_upsampled, 0, s->sps->ctb_width * s->sps->ctb_height);
#endif
        if (s->el_decoder_el_exist ){
            ff_thread_await_il_progress(s->avctx, s->poc_id, &s->avctx->BL_frame);
        } else
            if(s->threads_type&FF_THREAD_FRAME)
                s->avctx->BL_frame = NULL; // Base Layer does not exist

        if(s->avctx->BL_frame)
             s->BL_frame = (HEVCFrame*)s->avctx->BL_frame;
        else {
            av_log(s->avctx, AV_LOG_ERROR, "Error BL reference frame does not exist. decoder_id %d \n", s->decoder_id);
            goto fail;  // FIXME: add error concealment solution when the base layer frame is missing
        }
        s->poc = s->BL_frame->poc;
        ret = ff_hevc_set_new_iter_layer_ref(s, &s->EL_frame, s->poc);
        if (ret < 0)
            goto fail;
#if !ACTIVE_PU_UPSAMPLING || ACTIVE_BOTH_FRAME_AND_PU
        s->hevcdsp.upsample_base_layer_frame(s->EL_frame, s->BL_frame->frame, s->buffer_frame, &s->sps->scaled_ref_layer_window[s->vps->m_refLayerId[s->nuh_layer_id][0]], &s->up_filter_inf, 1);
#endif
    }
#endif
    ret = ff_hevc_set_new_ref(s, &s->frame, s->poc);

    if (ret < 0)
        goto fail;
    s->avctx->BL_frame = s->ref;
    ret = ff_hevc_frame_rps(s);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Error constructing the frame RPS. decoder_id %d \n", s->decoder_id);
        goto fail;
    }

    ret = set_side_data(s);
    if (ret < 0)
        goto fail;

    cur_frame = s->sps->sao_enabled ? s->sao_frame : s->frame;
    cur_frame->pict_type = 3 - s->sh.slice_type;

    uint8_t *MvDecoder_metaBuffer = s->frame->data[3] + ((s->frame->linesize[0]>>1)*(s->frame->coded_height>>1))*3;
    //MvDecoder: Add magic number at the front of the buffer
    MvDecoder_metaBuffer[0] = 4;
    MvDecoder_metaBuffer[1] = 2;
    //MvDecoder: save frame type to buffer

    if(cur_frame->pict_type==AV_PICTURE_TYPE_I) {
        MvDecoder_metaBuffer[2] = 0;
    }
    else if(cur_frame->pict_type==AV_PICTURE_TYPE_P) {
        MvDecoder_metaBuffer[2] = 1;
    }
    else if(cur_frame->pict_type==AV_PICTURE_TYPE_B) {
        MvDecoder_metaBuffer[2] = 2;
    }

    if (!IS_IRAP(s))
        ff_hevc_bump_frame(s);

    av_frame_unref(s->output_frame);
    ret = ff_hevc_output_frame(s, s->output_frame, 0);
    if (ret < 0)
        goto fail;

    ff_thread_finish_setup(s->avctx);

    return 0;

fail:
    if (s->ref && (s->threads_type & FF_THREAD_FRAME))
        ff_thread_report_progress(&s->ref->tf, INT_MAX, 0);
    if (s->decoder_id) {
        if(s->el_decoder_el_exist)
            ff_thread_report_il_status(s->avctx, s->poc_id, 2);
        if (s->inter_layer_ref)
            ff_hevc_unref_frame(s, s->inter_layer_ref, ~0);
    }
    s->ref = NULL;
    return ret;
}

static int decode_nal_unit(HEVCContext *s, const uint8_t *nal, int length)
{
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext *gb    = &lc->gb;
    int ctb_addr_ts, ret;

    ret = init_get_bits8(gb, nal, length);
    if (ret < 0)
        return ret;

    ret = hls_nal_unit(s);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Invalid NAL unit %d, skipping.\n",
               s->nal_unit_type);
        goto fail;
    } else if (ret != (s->decoder_id) && (s->nal_unit_type != NAL_VPS && (s->nal_unit_type != NAL_SPS) /*&& s->nal_unit_type != NAL_PPS*/))
        return 0;

    if ((s->temporal_id > s->temporal_layer_id) || (ret > s->quality_layer_id))
        return 0;
    s->nuh_layer_id = ret;
    
    s->nuh_layer_id = ret;

    switch (s->nal_unit_type) {
    case NAL_VPS:
        ret = ff_hevc_decode_nal_vps(s);
        if (ret < 0)
            goto fail;
        break;
    case NAL_SPS:
        ret = ff_hevc_decode_nal_sps(s);
        if (ret < 0)
            goto fail;
        break;
    case NAL_PPS:
        ret = ff_hevc_decode_nal_pps(s);
        if (ret < 0)
            goto fail;
        break;
    case NAL_SEI_PREFIX:
    case NAL_SEI_SUFFIX:
        ret = ff_hevc_decode_nal_sei(s);
        if (ret < 0)
            goto fail;
        break;
    case NAL_TRAIL_R:
    case NAL_TRAIL_N:
    case NAL_TSA_N:
    case NAL_TSA_R:
    case NAL_STSA_N:
    case NAL_STSA_R:
    case NAL_BLA_W_LP:
    case NAL_BLA_W_RADL:
    case NAL_BLA_N_LP:
    case NAL_IDR_W_RADL:
    case NAL_IDR_N_LP:
    case NAL_CRA_NUT:
    case NAL_RADL_N:
    case NAL_RADL_R:
    case NAL_RASL_N:
    case NAL_RASL_R:
#if 0
        {
            int loss_rate = 10;
            int var = (rand()%100);
            if( var < loss_rate && (s->nal_unit_type != NAL_VPS) && (s->nal_unit_type != NAL_SPS) && (s->nal_unit_type != NAL_PPS))
                get_bits(gb, 3);
            //  Packet loss
            //return 0;
        }
#endif
        ret = hls_slice_header(s);

#if 0
        if (ret == -10)
            return 0;
#endif

        if (ret < 0)
            return ret;
        if(s->au_poc !=-1 && s->au_poc != s->poc) {
            av_log(s->avctx, AV_LOG_ERROR, "Receive different poc in one AU. \n");
            s->max_ra == INT_MAX;
            goto fail;
        }
        s->au_poc = s->poc;
        if (s->max_ra == INT_MAX) {
            if (s->nal_unit_type == NAL_CRA_NUT || IS_BLA(s)) {
                s->max_ra = s->poc;
                av_log(s->avctx, AV_LOG_WARNING,
                       "max_ra equal to s->max_ra %d \n", s->max_ra);
            } else {
                if (IS_IDR(s))
                    s->max_ra = INT_MIN;
                else if( s->decoder_id ) {
                    av_log(s->avctx, AV_LOG_WARNING,
                           "Nal type %d s->max_ra %d \n", s->nal_unit_type,  s->max_ra);
                    break;
                }
            }
        }

        if ((s->nal_unit_type == NAL_RASL_R || s->nal_unit_type == NAL_RASL_N) &&
            s->poc <= s->max_ra) {
            s->is_decoded = 0;
                return 0;
        } else {
            if (s->nal_unit_type == NAL_RASL_R && s->poc > s->max_ra)
                s->max_ra = INT_MIN;
        }

        if (s->sh.first_slice_in_pic_flag) {
            ret = hevc_frame_start(s);
            if (ret < 0)
                return ret;
        } else if (!s->ref) {
            av_log(s->avctx, AV_LOG_ERROR, "First slice in a frame missing.\n");
            goto fail;
        }

        if (s->nal_unit_type != s->first_nal_type) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Non-matching NAL types of the VCL NALUs: %d %d\n",
                   s->first_nal_type, s->nal_unit_type);
            goto fail;
        }

        if (!s->sh.dependent_slice_segment_flag &&
            s->sh.slice_type != I_SLICE) {
            ret = ff_hevc_slice_rpl(s);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Error constructing the reference lists for the current slice.\n");
                goto fail;
            }
        }

#if ACTIVE_PU_UPSAMPLING
            if (s->bl_decoder_el_exist) {
                int i;
                s->bl_decoder_el_exist = 0;
                for (i = 0; i < FF_ARRAY_ELEMS(s->Add_ref); i++) {
                    HEVCFrame *frame = &s->Add_ref[i];
                    if (frame->frame->buf[0])
                        continue;
                    ret = hevc_ref_frame(s, &s->Add_ref[i], s->ref);
                    if (ret < 0)
                        return ret;
                    ff_thread_report_il_progress(s->avctx, s->poc_id, &s->Add_ref[i], s->ref);
                    break;
                }
                if(i==FF_ARRAY_ELEMS(s->Add_ref))
                    av_log(s->avctx, AV_LOG_ERROR, "Error allocating frame, Addditional DPB full, decoder_%d.\n", s->decoder_id);
            }
#endif
        ctb_addr_ts = hls_slice_data(s, nal, length);

        if (ctb_addr_ts >= (s->sps->ctb_width * s->sps->ctb_height)) {
            s->is_decoded = 1;
            if (s->pps->tiles_enabled_flag && s->threads_number!=1)
                tiles_filters(s);
#ifdef SVC_EXTENSION
#if !ACTIVE_PU_UPSAMPLING
            if (s->bl_decoder_el_exist) {
                int i;
                s->bl_decoder_el_exist = 0;
                for (i = 0; i < FF_ARRAY_ELEMS(s->Add_ref); i++) {
                    HEVCFrame *frame = &s->Add_ref[i];
                    if (frame->frame->buf[0])
                        continue;
                    ret = hevc_ref_frame(s, &s->Add_ref[i], s->ref);
                    if (ret < 0)
                        return ret;
                    ff_thread_report_il_progress(s->avctx, s->poc_id, &s->Add_ref[i], s->ref);
                    break;
                }
                if(i==FF_ARRAY_ELEMS(s->Add_ref))
                    av_log(s->avctx, AV_LOG_ERROR, "Error allocating frame, Addditional DPB full, decoder_%d.\n", s->decoder_id);
            }
#endif
#endif

#ifdef SVC_EXTENSION
            if(s->decoder_id > 0)
                ff_hevc_unref_frame(s, s->inter_layer_ref, ~0);
#endif
        }

        if (ctb_addr_ts < 0) {
            ret = ctb_addr_ts;
            goto fail;
        }
        break;
    case NAL_EOS_NUT:
    case NAL_EOB_NUT:
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
        break;
    case NAL_AUD:
    case NAL_FD_NUT:
        break;
    default:
        av_log(s->avctx, AV_LOG_INFO,
               "Skipping NAL unit %d\n", s->nal_unit_type);
    }

    return 0;
fail:
    if (s->avctx->err_recognition & AV_EF_EXPLODE)
        return ret;
    return 0;
}

#if PARALLEL_SLICE
static int decode_nal_unit_slice(AVCodecContext *avctxt, void *input_ctb_row, int job, int self_id) {
    HEVCContext *s1      = avctxt->priv_data;
    HEVCContext *s       = s1->sList[self_id];
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext *gb    = &lc->gb;
    s->avctx = avctxt;
    int i; 
    int *nal_id          = input_ctb_row;

    const uint8_t *nal   = s1->nals[nal_id[job]].data;
    int length           = s1->nals[nal_id[job]].size;
    int ctb_addr_ts, ret;
    

    ret = init_get_bits8(gb, nal, length);
    if (ret < 0)
        return ret;

    ret = hls_nal_unit(s);

    av_log(s->avctx, AV_LOG_DEBUG,
           "decode IRAP in parallel #%d.\n", s->nal_unit_type);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Invalid NAL unit %d, skipping.\n",
               s->nal_unit_type);
        goto fail;
    } else if (ret != (s->decoder_id) && (s->nal_unit_type != NAL_VPS && (s->nal_unit_type != NAL_SPS) /*&& s->nal_unit_type != NAL_PPS*/))
        return 0;

    if ((s->temporal_id > s->temporal_layer_id) || (ret > s->quality_layer_id))
        return 0;
    s->nuh_layer_id          = ret;
    s->avctx->layers_size   += length;
    s->self_id               = self_id;
    s->job                   = job;


    switch (s->nal_unit_type) {
        case NAL_VPS:
            ret = ff_hevc_decode_nal_vps(s);
            if (ret < 0)
                goto fail;
            break;
        case NAL_SPS:
            ret = ff_hevc_decode_nal_sps(s);
            if (ret < 0)
                goto fail;
            break;
        case NAL_PPS:
            ret = ff_hevc_decode_nal_pps(s);
            if (ret < 0)
                goto fail;
            break;
        case NAL_SEI_PREFIX:
        case NAL_SEI_SUFFIX:
            ret = ff_hevc_decode_nal_sei(s);
            if (ret < 0)
                goto fail;
            break;
        case NAL_TRAIL_R:
        case NAL_TRAIL_N:
        case NAL_TSA_N:
        case NAL_TSA_R:
        case NAL_STSA_N:
        case NAL_STSA_R:
        case NAL_BLA_W_LP:
        case NAL_BLA_W_RADL:
        case NAL_BLA_N_LP:
        case NAL_IDR_W_RADL:
        case NAL_IDR_N_LP:
        case NAL_CRA_NUT:
        case NAL_RADL_N:
        case NAL_RADL_R:
        case NAL_RASL_N:
        case NAL_RASL_R:
            ret = hls_slice_header(s);
            
            if (ret < 0)
                return ret;

            if (s->max_ra == INT_MAX) {
                if (s->nal_unit_type == NAL_CRA_NUT || IS_BLA(s)) {
                    s->max_ra = s->poc;
                    av_log(s->avctx, AV_LOG_WARNING,
                           "max_ra equal to s->max_ra %d \n", s->max_ra);
                } else {
                    if (IS_IDR(s))
                        s->max_ra = INT_MIN;
                    else if( s->decoder_id ) {
                        av_log(s->avctx, AV_LOG_WARNING,
                               "Nal type %d s->max_ra %d \n", s->nal_unit_type,  s->max_ra);
                        break;
                    }
                }
            }

            if ((s->nal_unit_type == NAL_RASL_R || s->nal_unit_type == NAL_RASL_N) &&
                s->poc <= s->max_ra) {
                s->is_decoded = 0;
                return 0;
            } else {
                if (s->nal_unit_type == NAL_RASL_R && s->poc > s->max_ra)
                    s->max_ra = INT_MIN;
            }
            
            if (s->sh.first_slice_in_pic_flag) {
                ret = hevc_frame_start(s);
                ff_thread_report_progress_slice(s->avctx);
                if (ret < 0)
                    return ret;

            } else if (!s->ref) {
                av_log(s->avctx, AV_LOG_ERROR, "First slice in a frame missing.\n");
                goto fail;
            }

            if (s->nal_unit_type != s->first_nal_type) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "Non-matching NAL types of the VCL NALUs: %d %d\n",
                       s->first_nal_type, s->nal_unit_type);
                goto fail;
            }
            
            if (!s->sh.dependent_slice_segment_flag &&
                s->sh.slice_type != I_SLICE) {
                
                ret = ff_hevc_slice_rpl(s);
                if (ret < 0) {
                    av_log(s->avctx, AV_LOG_WARNING,
                           "Error constructing the reference lists for the current slice.\n");
                    goto fail;
                }
            } 
              
#if ACTIVE_PU_UPSAMPLING
            if (s->bl_decoder_el_exist) {
                int i;
                s->bl_decoder_el_exist = 0;
                for (i = 0; i < FF_ARRAY_ELEMS(s->Add_ref); i++) {
                    HEVCFrame *frame = &s->Add_ref[i];
                    if (frame->frame->buf[0])
                        continue;
                    ret = hevc_ref_frame(s, &s->Add_ref[i], s->ref);
                    if (ret < 0)
                        return ret;
                    ff_thread_report_il_progress(s->avctx, s->poc_id, &s->Add_ref[i], s->ref);
                    break;
                }
                if(i==FF_ARRAY_ELEMS(s->Add_ref))
                    av_log(s->avctx, AV_LOG_ERROR, "Error allocating frame, Addditional DPB full, decoder_%d.\n", s->decoder_id);
            }
#endif
            if(s->job != s1->max_slices) {
                ff_thread_await_progress_slice2(s->avctx, s->job);
                for(i=s1->slice_segment_addr[s->job]; i < s1->slice_segment_addr[s->job+1]; i++)
                    s->tab_slice_address[i] = s1->slice_segment_addr[s->job];
            } else {
                for(i = s1->slice_segment_addr[s->job]; i < s->sps->ctb_height*s->sps->ctb_width; i++)
                    s->tab_slice_address[i] = s1->slice_segment_addr[s->job];
            }
            ctb_addr_ts = hls_decode_entry_slice(s);


            if (ctb_addr_ts >= (s->sps->ctb_width * s->sps->ctb_height)) {
                for(int i= 0; i < s->threads_number ; i++)
                    s->sList[i]->is_decoded = 1;
                if (s->pps->tiles_enabled_flag && s->threads_number!=1)
                    tiles_filters(s);
#ifdef SVC_EXTENSION
#if !ACTIVE_PU_UPSAMPLING
                if (s->active_el_frame) {
                    int i;
                    s->active_el_frame = 0;
                    for (i = 0; i < FF_ARRAY_ELEMS(s->Add_ref); i++) {
                        HEVCFrame *frame = &s->Add_ref[i];
                        if (frame->frame->buf[0])
                            continue;
                        ret = hevc_ref_frame(s, &s->Add_ref[i], s->ref);
                        if (ret < 0)
                            return ret;
                        ff_thread_report_il_progress(s->avctx, s->poc_id, &s->Add_ref[i], s->ref);
                        break;
                    }
                    if(i==FF_ARRAY_ELEMS(s->Add_ref))
                        av_log(s->avctx, AV_LOG_ERROR, "Error allocating frame, Addditional DPB full, decoder_%d.\n", s->decoder_id);
                }
#endif
#endif

#ifdef SVC_EXTENSION
                if(s->decoder_id > 0)
                    ff_hevc_unref_frame(s, s->inter_layer_ref, ~0);
#endif
            }
            
            if (ctb_addr_ts < 0) {
                ret = ctb_addr_ts;
                goto fail;
            }
            break;
        case NAL_EOS_NUT:
        case NAL_EOB_NUT:
            s->seq_decode = (s->seq_decode + 1) & 0xff;
            s->max_ra     = INT_MAX;
            break;
        case NAL_AUD:
        case NAL_FD_NUT:
            break;
        default:
            av_log(s->avctx, AV_LOG_INFO,
                   "Skipping NAL unit %d\n", s->nal_unit_type);
    }
    return 0;
fail:
    if (s->avctx->err_recognition & AV_EF_EXPLODE)
        return ret;
    return 0;
}
#endif
/* FIXME: This is adapted from ff_h264_decode_nal, avoiding duplication
 * between these functions would be nice. */
int ff_hevc_extract_rbsp(HEVCContext *s, const uint8_t *src, int length,
                         HEVCNAL *nal)
{
    int i, si, di;
    uint8_t *dst;

    s->skipped_bytes = 0;
#define STARTCODE_TEST                                                  \
        if (i + 2 < length && src[i + 1] == 0 && src[i + 2] <= 3) {     \
            if (src[i + 2] != 3) {                                      \
                /* startcode, so we must be past the end */             \
                length = i;                                             \
            }                                                           \
            break;                                                      \
        }
#if HAVE_FAST_UNALIGNED
#define FIND_FIRST_ZERO                                                 \
        if (i > 0 && !src[i])                                           \
            i--;                                                        \
        while (src[i])                                                  \
            i++
#if HAVE_FAST_64BIT
    for (i = 0; i + 1 < length; i += 9) {
        if (!((~AV_RN64A(src + i) &
               (AV_RN64A(src + i) - 0x0100010001000101ULL)) &
              0x8000800080008080ULL))
            continue;
        FIND_FIRST_ZERO;
        STARTCODE_TEST;
        i -= 7;
    }
#else
    for (i = 0; i + 1 < length; i += 5) {
        if (!((~AV_RN32A(src + i) &
               (AV_RN32A(src + i) - 0x01000101U)) &
              0x80008080U))
            continue;
        FIND_FIRST_ZERO;
        STARTCODE_TEST;
        i -= 3;
    }
#endif /* HAVE_FAST_64BIT */
#else
    for (i = 0; i + 1 < length; i += 2) {
        if (src[i])
            continue;
        if (i > 0 && src[i - 1] == 0)
            i--;
        STARTCODE_TEST;
    }
#endif /* HAVE_FAST_UNALIGNED */

    if (i >= length - 1) { // no escaped 0
        nal->data = src;
        nal->size = length;
        return length;
    }

    av_fast_malloc(&nal->rbsp_buffer, &nal->rbsp_buffer_size,
                   length + FF_INPUT_BUFFER_PADDING_SIZE);
    if (!nal->rbsp_buffer)
        return AVERROR(ENOMEM);

    dst = nal->rbsp_buffer;

    memcpy(dst, src, i);
    si = di = i;
    while (si + 2 < length) {
        // remove escapes (very rare 1:2^22)
        if (src[si + 2] > 3) {
            dst[di++] = src[si++];
            dst[di++] = src[si++];
        } else if (src[si] == 0 && src[si + 1] == 0) {
            if (src[si + 2] == 3) { // escape
                dst[di++] = 0;
                dst[di++] = 0;
                si       += 3;

                s->skipped_bytes++;
                if (s->skipped_bytes_pos_size < s->skipped_bytes) {
                    s->skipped_bytes_pos_size *= 2;
                    av_reallocp_array(&s->skipped_bytes_pos,
                            s->skipped_bytes_pos_size,
                            sizeof(*s->skipped_bytes_pos));
                    if (!s->skipped_bytes_pos)
                        return AVERROR(ENOMEM);
                }
                if (s->skipped_bytes_pos)
                    s->skipped_bytes_pos[s->skipped_bytes-1] = di - 1;
                continue;
            } else // next start code
                goto nsc;
        }

        dst[di++] = src[si++];
    }
    while (si < length)
        dst[di++] = src[si++];

nsc:
    memset(dst + di, 0, FF_INPUT_BUFFER_PADDING_SIZE);

    nal->data = dst;
    nal->size = di;
    return si;
}

static int decode_nal_units(HEVCContext *s, const uint8_t *buf, int length)
{
    int i,  consumed, ret = 0;
    
#if PARALLEL_SLICE
    int cum_nal_pos = 0, k, nal_type, prv_nal_type=-1;
    int arg[128];
    int is_irap = 1;
#endif
    s->ref = NULL;
    s->au_poc = -1;
    s->last_eos = s->eos;
    s->eos = 0;
    s->bl_decoder_el_exist  = 0;
    s->el_decoder_el_exist  = 0;
    s->el_decoder_bl_exist  = 0;
#if PARALLEL_SLICE
    s->NbListElement        = 0;
    for(i = 0; i < 16; i++)
        s->NALListOrder[i]  = 0;
#endif
    /* split the input packet into NAL units, so we know the upper bound on the
     * number of slices in the frame */
    s->nb_nals = 0;
    while (length >= 4) {
        HEVCNAL *nal;
        int extract_length = 0;

        if (s->is_nalff) {
            int i;
            for (i = 0; i < s->nal_length_size; i++)
                extract_length = (extract_length << 8) | buf[i];
            buf    += s->nal_length_size;
            length -= s->nal_length_size;

            if (extract_length > length) {
                av_log(s->avctx, AV_LOG_ERROR, "Invalid NAL unit size.\n");
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
        } else {
            /* search start code */
            if (buf[2] == 0) {
                length--;
                buf++;
                continue;
            }
            if (buf[0] != 0 || buf[1] != 0 || buf[2] != 1) {
                av_log(s->avctx, AV_LOG_ERROR, "No start code is found.\n");
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }

            buf           += 3;
            length        -= 3;
        }

        if (!s->is_nalff)
            extract_length = length;

        if (s->nals_allocated < s->nb_nals + 1) {
            int new_size = s->nals_allocated + 1;
            HEVCNAL *tmp = av_realloc_array(s->nals, new_size, sizeof(*tmp));
            if (!tmp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            s->nals = tmp;
            memset(s->nals + s->nals_allocated, 0,
                   (new_size - s->nals_allocated) * sizeof(*tmp));
            av_reallocp_array(&s->skipped_bytes_nal, new_size, sizeof(*s->skipped_bytes_nal));
            av_reallocp_array(&s->skipped_bytes_pos_size_nal, new_size, sizeof(*s->skipped_bytes_pos_size_nal));
            av_reallocp_array(&s->skipped_bytes_pos_nal, new_size, sizeof(*s->skipped_bytes_pos_nal));
            s->skipped_bytes_pos_size_nal[s->nals_allocated] = 1024; // initial buffer size
            s->skipped_bytes_pos_nal[s->nals_allocated] = av_malloc_array(s->skipped_bytes_pos_size_nal[s->nals_allocated], sizeof(*s->skipped_bytes_pos));
            s->nals_allocated = new_size;
        }
        s->skipped_bytes_pos_size = s->skipped_bytes_pos_size_nal[s->nb_nals];
        s->skipped_bytes_pos = s->skipped_bytes_pos_nal[s->nb_nals];
        nal = &s->nals[s->nb_nals];
        consumed = ff_hevc_extract_rbsp(s, buf, extract_length, nal);

        s->skipped_bytes_nal[s->nb_nals] = s->skipped_bytes;
        s->skipped_bytes_pos_size_nal[s->nb_nals] = s->skipped_bytes_pos_size;
        s->skipped_bytes_pos_nal[s->nb_nals++] = s->skipped_bytes_pos;


        if (consumed < 0) {
            ret = consumed;
            goto fail;
        }

        ret = init_get_bits8(&s->HEVClc->gb, nal->data, nal->size);
        if (ret < 0)
            goto fail;
        ret = hls_nal_unit(s);

#if PARALLEL_SLICE
        /*   Find out the set of slices to run in parallel   */
        nal_type = s->nal_unit_type > NAL_CRA_NUT;
        if (!nal_type)
            is_irap &= IS_IRAP(s);

        if(prv_nal_type==-1)
            s->NALListOrder[0]++;
        else {
            if(nal_type || (!nal_type && nal_type != prv_nal_type))
                s->NbListElement++;
            s->NALListOrder[s->NbListElement]++;
        }

        prv_nal_type = nal_type;
#endif

        if(!s->bl_decoder_el_exist && ret == s->decoder_id+1 && s->avctx->quality_id >= ret && s->nal_unit_type <= NAL_CRA_NUT && (s->threads_type&FF_THREAD_FRAME)) {
            s->bl_decoder_el_exist = 1;
            s->poc_id++;
            s->poc_id &= (MAX_POC-1);
        }
        if(!s->el_decoder_bl_exist && s->decoder_id && ret == s->decoder_id-1 && s->nal_unit_type <= NAL_CRA_NUT && (s->threads_type&FF_THREAD_FRAME)) {
            s->el_decoder_bl_exist=1;
        }
        if(!s->el_decoder_el_exist && s->decoder_id && ret == s->decoder_id && s->nal_unit_type <= NAL_CRA_NUT && (s->threads_type&FF_THREAD_FRAME)) {
            s->poc_id++;
            s->poc_id &= (MAX_POC-1);
            s->el_decoder_el_exist = 1;
        }
        if (s->nal_unit_type == NAL_EOB_NUT ||
            s->nal_unit_type == NAL_EOS_NUT)
            s->eos = 1;

        buf    += consumed;
        length -= consumed;
    }

    /* parse the NAL units */
    if(!s->el_decoder_bl_exist) {
        s->el_decoder_el_exist = 0;
    }
#if PARALLEL_SLICE
    if (is_irap) {
        for(i = 0; i <= s->NbListElement; i++) {
            //      s->skipped_bytes = s->skipped_bytes_nal[i];
            //      s->skipped_bytes_pos = s->skipped_bytes_pos_nal[i];
            s->max_slices = s->NALListOrder[i]-1;
            for(k=0; k < s->NALListOrder[i]; k++)
                arg[k] = cum_nal_pos+k;
            s->avctx->execute2(s->avctx, (void *) decode_nal_unit_slice, arg, ret, s->NALListOrder[i]);
            cum_nal_pos += s->NALListOrder[i];

            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Error parsing NAL unit #%d.\n", i);
                goto fail;
            }
        }
    } else {
        for (i = 0; i < s->nb_nals; i++) {
            int ret;
            s->skipped_bytes = s->skipped_bytes_nal[i];
            s->skipped_bytes_pos = s->skipped_bytes_pos_nal[i];

            ret = decode_nal_unit(s, s->nals[i].data, s->nals[i].size);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Error parsing NAL unit #%d.\n", i);
                goto fail;
            }
        }

    }
#if !PARALLEL_FILTERS
    if(s->is_decoded)
        slices_filters(s);
#endif

#else
    for (i = 0; i < s->nb_nals; i++) {
        int ret;
        s->skipped_bytes = s->skipped_bytes_nal[i];
        s->skipped_bytes_pos = s->skipped_bytes_pos_nal[i];

        ret = decode_nal_unit(s, s->nals[i].data, s->nals[i].size);
        if (ret < 0) {
            av_log(s->avctx, AV_LOG_WARNING,
                   "Error parsing NAL unit #%d.\n", i);
            goto fail;
        }
    }
#endif
fail:
#if PARALLEL_SLICE
    ff_thread_report_progress_slice(s->avctx);
    ff_thread_report_progress_slice2(s->avctx, s->job);
#endif
    if (s->ref && (s->threads_type & FF_THREAD_FRAME))
        ff_thread_report_progress(&s->ref->tf, INT_MAX, 0);
    if (s->decoder_id) {
        if(s->el_decoder_el_exist)
            ff_thread_report_il_status(s->avctx, s->poc_id, 2);
    }
    if (s->bl_decoder_el_exist)
        ff_thread_report_il_progress(s->avctx, s->poc_id, NULL, NULL);

    return ret;
}

static void print_md5(void *log_ctx, int level, uint8_t md5[16])
{
    int i;
    for (i = 0; i < 16; i++)
        av_log(log_ctx, level, "%02"PRIx8, md5[i]);
}

static int verify_md5(HEVCContext *s, AVFrame *frame)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int pixel_shift;
    int i, j;

    if (!desc)
        return AVERROR(EINVAL);

    pixel_shift = desc->comp[0].depth_minus1 > 7;

    av_log(s->avctx, AV_LOG_DEBUG, "Verifying checksum for frame with POC %d: ",
           s->poc);

    /* the checksums are LE, so we have to byteswap for >8bpp formats
     * on BE arches */
#if HAVE_BIGENDIAN
    if (pixel_shift && !s->checksum_buf) {
        av_fast_malloc(&s->checksum_buf, &s->checksum_buf_size,
                       FFMAX3(frame->linesize[0], frame->linesize[1],
                              frame->linesize[2]));
        if (!s->checksum_buf)
            return AVERROR(ENOMEM);
    }
#endif

    for (i = 0; frame->data[i]; i++) {
        int width  = s->avctx->coded_width;
        int height = s->avctx->coded_height;
        int w = (i == 1 || i == 2) ? (width  >> desc->log2_chroma_w) : width;
        int h = (i == 1 || i == 2) ? (height >> desc->log2_chroma_h) : height;
        uint8_t md5[16];

        av_md5_init(s->md5_ctx);
        for (j = 0; j < h; j++) {
            const uint8_t *src = frame->data[i] + j * frame->linesize[i];
#if HAVE_BIGENDIAN
            if (pixel_shift) {
                s->dsp.bswap16_buf((uint16_t*)s->checksum_buf,
                                   (const uint16_t*)src, w);
                src = s->checksum_buf;
            }
#endif
            av_md5_update(s->md5_ctx, src, w << pixel_shift);
        }
        av_md5_final(s->md5_ctx, md5);

        if (!memcmp(md5, s->md5[i], 16)) {
            av_log   (s->avctx, AV_LOG_DEBUG, "plane %d - correct ", i);
            print_md5(s->avctx, AV_LOG_DEBUG, md5);
            av_log   (s->avctx, AV_LOG_DEBUG, "; ");
        } else {
            av_log   (s->avctx, AV_LOG_ERROR, "mismatching checksum of plane %d - ", i);
            print_md5(s->avctx, AV_LOG_ERROR, md5);
            av_log   (s->avctx, AV_LOG_ERROR, " != ");
            print_md5(s->avctx, AV_LOG_ERROR, s->md5[i]);
            av_log   (s->avctx, AV_LOG_ERROR, "\n");
            return AVERROR_INVALIDDATA;
        }
    }

    av_log(s->avctx, AV_LOG_DEBUG, "\n");

    return 0;
}

static int hevc_decode_frame(AVCodecContext *avctx, void *data, int *got_output,
                             AVPacket *avpkt)
{
    int ret;
    HEVCContext *s = avctx->priv_data;

    if (!avpkt->size) {
        ret = ff_hevc_output_frame(s, data, 1);
        if (ret < 0)
            return ret;
        if (s->decoder_id) {
            // av_log(s->avctx, AV_LOG_ERROR, "flush poc %d\n", s->poc);
            s->max_ra = INT_MAX;
        }
        *got_output = ret;
        return 0;
    }
    s->ref = NULL;
#if PARALLEL_SLICE
    ff_thread_set_slice_flag(avctx, 0);
    ff_init_flags(avctx);
#endif

	if (avpkt->pts != AV_NOPTS_VALUE) {
		if (! s->last_frame_pts || (s->last_frame_pts!=avpkt->pts)) {
			s->force_first_slice_in_pic = 1;
		}
		s->last_frame_pts = avpkt->pts;
	}
	
	ret    = decode_nal_units(s, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;

    /* verify the SEI checksum */
    if (s->decode_checksum_sei && s->is_decoded) {
        AVFrame *frame = s->ref->frame;
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
        int cIdx;
        uint8_t md5[3][16];

        calc_md5(md5[0], frame->data[0], frame->linesize[0], s->sps->width  , s->sps->height  , s->sps->pixel_shift);
        calc_md5(md5[1], frame->data[1], frame->linesize[1], s->sps->width >> desc->log2_chroma_w, s->sps->height >> desc->log2_chroma_h, s->sps->pixel_shift);
        calc_md5(md5[2], frame->data[2], frame->linesize[2], s->sps->width >> desc->log2_chroma_w, s->sps->height >> desc->log2_chroma_h, s->sps->pixel_shift);
        if (s->is_md5) {
            for( cIdx = 0; cIdx < ((s->sps->chroma_array_type == 0) ? 1 : 3); cIdx++ ) {
                if (!compare_md5(md5[cIdx], s->md5[cIdx])) {
                     av_log(s->avctx, AV_LOG_ERROR, "Incorrect MD5 (poc: %d, plane: %d)\n", s->poc, cIdx);
                 } else {
                     av_log(s->avctx, AV_LOG_INFO, "Correct MD5 (poc: %d, plane: %d)\n", s->poc, cIdx);
                 }
            }
            s->is_md5 = 0;
        }
/*
#ifdef POC_DISPLAY_MD5
        printf_ref_pic_list(s);
        display_md5(s->poc, md5);
#endif
*/
    }
    s->is_md5 = 0;

    if (s->is_decoded) {
        s->ref->frame->key_frame = IS_IRAP(s);
        av_log(avctx, AV_LOG_DEBUG, "Decoded frame with POC %d.\n", s->poc);
        s->is_decoded = 0;
    }

    if (s->output_frame->buf[0]) {
        av_frame_move_ref(data, s->output_frame);
        *got_output = 1;
    }
    av_log(s->avctx, AV_LOG_DEBUG, "frame end %d\n", s->decoder_id);

    return avpkt->size;
}

static av_cold int hevc_decode_free(AVCodecContext *avctx)
{
    HEVCContext       *s = avctx->priv_data;
    HEVCLocalContext *lc = s->HEVClc;
    int i;

    pic_arrays_free(s);
    DeleteCryptoC(s->HEVClc->dbs_g);
    av_freep(&s->md5_ctx);

    for(i=0; i < s->nals_allocated; i++) {
        av_freep(&s->skipped_bytes_pos_nal[i]);
    }
    av_freep(&s->skipped_bytes_pos_size_nal);
    av_freep(&s->skipped_bytes_nal);
    av_freep(&s->skipped_bytes_pos_nal);

    av_freep(&s->cabac_state);

    av_frame_free(&s->tmp_frame);
    av_frame_free(&s->output_frame);

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        ff_hevc_unref_frame(s, &s->DPB[i], ~0);
        av_frame_free(&s->DPB[i].frame);
    }
    for (i = 0; i < FF_ARRAY_ELEMS(s->Add_ref); i++) {
        ff_hevc_unref_frame(s, &s->Add_ref[i], ~0);
        av_frame_free(&s->Add_ref[i].frame);
    }



    for (i = 0; i < FF_ARRAY_ELEMS(s->vps_list); i++)
        av_buffer_unref(&s->vps_list[i]);
    for (i = 0; i < FF_ARRAY_ELEMS(s->sps_list); i++)
        av_buffer_unref(&s->sps_list[i]);
    for (i = 0; i < FF_ARRAY_ELEMS(s->pps_list); i++)
        av_buffer_unref(&s->pps_list[i]);

    av_freep(&s->sh.entry_point_offset); // TODO Free for each slice
    av_freep(&s->sh.offset);
    av_freep(&s->sh.size);

    for (i = 1; i < s->threads_number; i++) {
        lc = s->HEVClcList[i];
        if (lc) {
            av_freep(&s->HEVClcList[i]);
            av_freep(&s->sList[i]);
        }
    }
    if (s->HEVClc == s->HEVClcList[0])
        s->HEVClc = NULL;
    av_freep(&s->HEVClcList[0]);

    for (i = 0; i < s->nals_allocated; i++)
        av_freep(&s->nals[i].rbsp_buffer);
    av_freep(&s->nals);
    s->nals_allocated = 0;

    return 0;
}

static av_cold int hevc_init_context(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int i;
    s->dynamic_alloc = 0;
    s->avctx = avctx;
    s->HEVClc = av_mallocz(sizeof(HEVCLocalContext));
    if (!s->HEVClc)
        goto fail;

#if 0
    printf("static ## %ld ## \n", sizeof(HEVCLocalContext) );
#endif
    s->HEVClcList[0] = s->HEVClc;
    s->sList[0] = s;

    s->cabac_state    = av_malloc(HEVC_CONTEXTS);
    s->dynamic_alloc += HEVC_CONTEXTS;
    if (!s->cabac_state)
        goto fail;
     s->HEVClc->dbs_g = InitC();
    s->tmp_frame = av_frame_alloc();
    s->dynamic_alloc += sizeof(AVFrame); 
    if (!s->tmp_frame)
        goto fail;

    s->output_frame = av_frame_alloc();
    s->dynamic_alloc += sizeof(AVFrame); 
    if (!s->output_frame)
        goto fail;

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        s->DPB[i].frame = av_frame_alloc();
        s->dynamic_alloc += sizeof(AVFrame); 
        if (!s->DPB[i].frame)
            goto fail;
        s->DPB[i].tf.f = s->DPB[i].frame;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(s->Add_ref); i++) {
        s->Add_ref[i].frame = av_frame_alloc();
        if (!s->Add_ref[i].frame)
            goto fail;
        s->Add_ref[i].tf.f = s->Add_ref[i].frame;
    }

    s->max_ra = INT_MAX;

    s->md5_ctx = av_md5_alloc();
    if (!s->md5_ctx)
        goto fail;

    ff_bswapdsp_init(&s->bdsp);
#if FRAME_CONCEALMENT
    s->prev_display_poc = -1;
    s->no_display_pic   =  0;
#endif

    s->temporal_layer_id   = 8;
    s->quality_layer_id    = 8;

    s->context_initialized = 1;
    s->threads_type        = avctx->active_thread_type;
    if(avctx->active_thread_type & FF_THREAD_SLICE)
        s->threads_number  = avctx->thread_count;
    else
        s->threads_number  = 1;
    s->eos = 0;

    for (i = 1; i < s->threads_number ; i++) {
        s->sList[i] = av_mallocz(sizeof(HEVCContext));
        memcpy(s->sList[i], s, sizeof(HEVCContext));
        s->HEVClcList[i] = av_mallocz(sizeof(HEVCLocalContext));
        s->sList[i]->HEVClc = s->HEVClcList[i];
    }

#if 0
    printf("### %ld ### \n", s->dynamic_alloc );
#endif
    s->eos = 0;
    return 0;

fail:
    hevc_decode_free(avctx);
    return AVERROR(ENOMEM);
}

static int hevc_update_thread_context(AVCodecContext *dst,
                                      const AVCodecContext *src)
{
    HEVCContext *s  = dst->priv_data;
    HEVCContext *s0 = src->priv_data;
    int i, ret;

    if (!s->context_initialized) {
        ret = hevc_init_context(dst);
        if (ret < 0)
            return ret;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        ff_hevc_unref_frame(s, &s->DPB[i], ~0);
        if (s0->DPB[i].frame->buf[0] && &s0->DPB[i] != s0->inter_layer_ref ) {
            ret = hevc_ref_frame(s, &s->DPB[i], &s0->DPB[i]);
            if (ret < 0)
                return ret;
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->vps_list); i++) {
        av_buffer_unref(&s->vps_list[i]);
        if (s0->vps_list[i]) {
            s->vps_list[i] = av_buffer_ref(s0->vps_list[i]);
            if (!s->vps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->sps_list); i++) {
        av_buffer_unref(&s->sps_list[i]);
        if (s0->sps_list[i]) {
            s->sps_list[i] = av_buffer_ref(s0->sps_list[i]);
            if (!s->sps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->pps_list); i++) {
        av_buffer_unref(&s->pps_list[i]);
        if (s0->pps_list[i]) {
            s->pps_list[i] = av_buffer_ref(s0->pps_list[i]);
            if (!s->pps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    s->seq_decode           = s0->seq_decode;
    s->seq_output           = s0->seq_output;
    s->pocTid0              = s0->pocTid0;
    s->max_ra               = s0->max_ra;
    s->eos        = s0->eos;
    s->is_nalff             = s0->is_nalff;
    s->nal_length_size      = s0->nal_length_size;
    s->threads_number       = s0->threads_number;
    s->threads_type         = s0->threads_type;
    s->nuh_layer_id         = s0->nuh_layer_id;
    s->decoder_id           = s0->decoder_id;
    s->temporal_layer_id    = s0->temporal_layer_id;
    s->quality_layer_id     = s0->quality_layer_id;
    s->decode_checksum_sei  = s0->decode_checksum_sei;
    s->poc_id               = s0->poc_id;

    if (s->sps != s0->sps)
        ret = set_sps(s, s0->sps);

    if (s0->eos) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra = INT_MAX;
    }

    return 0;
}

static int hevc_decode_extradata(HEVCContext *s)
{
    AVCodecContext *avctx = s->avctx;
    GetByteContext gb;
    int ret;

    bytestream2_init(&gb, avctx->extradata, avctx->extradata_size);

    if (avctx->extradata_size > 3 &&
        (avctx->extradata[0] || avctx->extradata[1] ||
         avctx->extradata[2] > 1)) {
        /* It seems the extradata is encoded as hvcC format.
         * Temporarily, we support configurationVersion==0 until 14496-15 3rd
         * is finalized. When finalized, configurationVersion will be 1 and we
         * can recognize hvcC by checking if avctx->extradata[0]==1 or not. */
        int i, j, num_arrays, nal_len_size;

        s->is_nalff = 1;

        bytestream2_skip(&gb, 21);
        nal_len_size = (bytestream2_get_byte(&gb) & 3) + 1;
        num_arrays   = bytestream2_get_byte(&gb);

        /* nal units in the hvcC always have length coded with 2 bytes,
         * so put a fake nal_length_size = 2 while parsing them */
        s->nal_length_size = 2;

        /* Decode nal units from hvcC. */
        for (i = 0; i < num_arrays; i++) {
            int type = bytestream2_get_byte(&gb) & 0x3f;
            int cnt  = bytestream2_get_be16(&gb);

            for (j = 0; j < cnt; j++) {
                // +2 for the nal size field
                int nalsize = bytestream2_peek_be16(&gb) + 2;
                if (bytestream2_get_bytes_left(&gb) < nalsize) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "Invalid NAL unit size in extradata.\n");
                    return AVERROR_INVALIDDATA;
                }

                ret = decode_nal_units(s, gb.buffer, nalsize);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "Decoding nal unit %d %d from hvcC failed\n",
                           type, i);
                    return ret;
                }
                bytestream2_skip(&gb, nalsize);
            }
        }
        /* Now store right nal length size, that will be used to parse
         * all other nals */
        s->nal_length_size = nal_len_size;
    } else {
        s->is_nalff = 0;
        ret = decode_nal_units(s, avctx->extradata, avctx->extradata_size);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static av_cold int hevc_decode_init(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int ret;

    ff_init_cabac_states();

    avctx->internal->allocate_progress = 1;

    ret = hevc_init_context(avctx);
    if (ret < 0)
        return ret;

    s->picture_struct = 0;
    s->prev_pos = 0;
    s->encrypt_params = 0; //HEVC_CRYPTO_MV_SIGNS | HEVC_CRYPTO_MVs | HEVC_CRYPTO_TRANSF_COEFF_SIGNS | HEVC_CRYPTO_TRANSF_COEFFS;

    if (avctx->extradata_size > 0 && avctx->extradata) {
        ret = hevc_decode_extradata(s);
        if (ret < 0) {
            hevc_decode_free(avctx);
            return ret;
        }
    }
    return 0;
}

static av_cold int hevc_init_thread_copy(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int ret;

    memset(s, 0, sizeof(*s));

    ret = hevc_init_context(avctx);
    if (ret < 0)
        return ret;

    return 0;
}

static void hevc_decode_flush(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    ff_hevc_flush_dpb(s);
    s->max_ra = INT_MAX;
}

#define OFFSET(x) offsetof(HEVCContext, x)
#define PAR (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVProfile profiles[] = {
    { FF_PROFILE_HEVC_MAIN,                 "Main"                },
    { FF_PROFILE_HEVC_MAIN_10,              "Main 10"             },
    { FF_PROFILE_HEVC_MAIN_STILL_PICTURE,   "Main Still Picture"  },
    { FF_PROFILE_HEVC_REXT,                 "Rext"  },
    { FF_PROFILE_UNKNOWN },
};

static const AVOption options[] = {
    { "decode-checksum", "decode picture checksum SEI message", OFFSET(decode_checksum_sei),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, PAR },
    { "strict-displaywin", "stricly apply default display window size", OFFSET(apply_defdispwin),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, PAR },
    { "decoder-id", "set the decoder id", OFFSET(decoder_id),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 10, PAR },
    { "temporal-layer-id", "set the max temporal id", OFFSET(temporal_layer_id),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 10, PAR },
    { "quality_layer_id", "set the max quality id", OFFSET(quality_layer_id),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 10, PAR },
    { NULL },
};

static const AVClass hevc_decoder_class = {
    .class_name = "HEVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_decoder = {
    .name                  = "hevc",
    .long_name             = NULL_IF_CONFIG_SMALL("HEVC (High Efficiency Video Coding)"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_HEVC,
    .priv_data_size        = sizeof(HEVCContext),
    .priv_class            = &hevc_decoder_class,
    .init                  = hevc_decode_init,
    .close                 = hevc_decode_free,
    .decode                = hevc_decode_frame,
    .flush                 = hevc_decode_flush,
    .update_thread_context = hevc_update_thread_context,
    .init_thread_copy      = hevc_init_thread_copy,
    .capabilities          = CODEC_CAP_DR1 | CODEC_CAP_DELAY |
                             CODEC_CAP_SLICE_THREADS | CODEC_CAP_FRAME_THREADS,
    .profiles              = NULL_IF_CONFIG_SMALL(profiles),
};


#ifdef POC_DISPLAY_MD5


static void printf_ref_pic_list(HEVCContext *s)
{
    RefPicList  *refPicList = s->ref->refPicList[s->slice_idx];

    int i, list_idx;
    if (s->sh.slice_type == I_SLICE)
        printf("\nPOC %4d TId: %1d QId: %1d ( I-SLICE, QP%3d ) ", s->poc, s->temporal_id, s->nuh_layer_id, s->sh.slice_qp);
    else if (s->sh.slice_type == B_SLICE)
        printf("\nPOC %4d TId: %1d QId: %1d ( B-SLICE, QP%3d ) ", s->poc, s->temporal_id, s->nuh_layer_id, s->sh.slice_qp);
    else
        printf("\nPOC %4d TId: %1d QId: %1d ( P-SLICE, QP%3d ) ", s->poc, s->temporal_id, s->nuh_layer_id,  s->sh.slice_qp);

    for ( list_idx = 0; list_idx < 2; list_idx++) {
        printf("[L%d ",list_idx);
        if (refPicList)
            for(i = 0; i < refPicList[list_idx].nb_refs; i++)
                printf("%d ",refPicList[list_idx].list[i]);
        else
            printf("O");
        printf("] ");
    }
}

static void display_md5(int poc, uint8_t md5[3][16])
{
    int i, j;
    printf("\n[MD5:");
    for (j = 0; j < 3; j++) {
        printf("\n");
        for (i = 0; i < 16; i++)
            printf("%02x", md5[j][i]);
    }
    printf("\n]");

}
#endif

static int compare_md5(uint8_t *md5_in1, uint8_t *md5_in2)
{
    int i;
    for (i = 0; i < 16; i++)
        if (md5_in1[i] != md5_in2[i])
            return 0;
    return 1;
}

static void calc_md5(uint8_t *md5, uint8_t* src, int stride, int width, int height, int pixel_shift)
{
    uint8_t *buf;
    int y, x;
    int stride_buf = width << pixel_shift;
    buf = av_malloc(stride_buf * height);

    for (y = 0; y < height; y++) {
        for (x = 0; x < stride_buf; x++)
            buf[y * stride_buf + x] = src[x];

        src += stride;
    }
    av_md5_sum(md5, buf, stride_buf * height);
    av_free(buf);
}


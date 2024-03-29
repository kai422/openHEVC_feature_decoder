/*
 * Copyright (c) 2004 Roman Shaposhnik
 * Copyright (c) 2008 Alexander Strange (astrange@ithinksw.com)
 *
 * Many thanks to Steven M. Schultz for providing clever ideas and
 * to Michael Niedermayer <michaelni@gmx.at> for writing initial
 * implementation.
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

/**
 * @file
 * Multithreading support functions
 * @see doc/multithreading.txt
 */

#include "avcodec.h"
#include "internal.h"
#include "pthread_internal.h"
#include "thread.h"

/**
 * Set the threading algorithms used.
 *
 * Threading requires more than one thread.
 * Frame threading requires entire frames to be passed to the codec,
 * and introduces extra decoding delay, so is incompatible with low_delay.
 *
 * @param avctx The context.
 */
static void validate_thread_parameters(AVCodecContext *avctx)
{
    int frame_threading_supported      = (avctx->codec->capabilities & CODEC_CAP_FRAME_THREADS)
                                        && !(avctx->flags  & CODEC_FLAG_TRUNCATED)
                                        && !(avctx->flags  & CODEC_FLAG_LOW_DELAY)
                                        && !(avctx->flags2 & CODEC_FLAG2_CHUNKS  );
    int slice_threading_supported      = (avctx->codec->capabilities & CODEC_CAP_SLICE_THREADS);
    int frameslice_threading_supported = slice_threading_supported && frame_threading_supported;

    if (avctx->thread_count == 1) {
        avctx->active_thread_type = 0;
        avctx->thread_count_frame = 1;
    } else if (frame_threading_supported     && (avctx->thread_type & FF_THREAD_FRAME)) {
        avctx->active_thread_type = FF_THREAD_FRAME;
        avctx->thread_count_frame = avctx->thread_count;
        avctx->thread_count       = 1;
    } else if (slice_threading_supported     && (avctx->thread_type & FF_THREAD_SLICE)) {
        avctx->active_thread_type = FF_THREAD_SLICE;
        avctx->thread_count_frame = 1;
    } else if(frameslice_threading_supported && (avctx->thread_type & FF_THREAD_FRAME_SLICE)) {
        avctx->thread_count        = avctx->thread_count ? avctx->thread_count : av_cpu_count()>>1;
        avctx->thread_count_frame  = FFMIN((av_cpu_count() / avctx->thread_count) + 1, MAX_AUTO_THREADS);
        if (avctx->thread_count_frame > 1)
            avctx->active_thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        else
            avctx->active_thread_type  = FF_THREAD_SLICE;
    } else if (!(avctx->codec->capabilities & CODEC_CAP_AUTO_THREADS)) {
        avctx->thread_count       = 1;
        avctx->thread_count_frame = 1;
        avctx->active_thread_type = 0;
    }

    if (avctx->thread_count > MAX_AUTO_THREADS)
        av_log(avctx, AV_LOG_WARNING,
               "Application has requested %d threads. Using a thread count greater than %d is not recommended.\n",
               avctx->thread_count, MAX_AUTO_THREADS);
}

int ff_thread_init(AVCodecContext *avctx)
{
    int ret = 0;

    validate_thread_parameters(avctx);

    if (avctx->active_thread_type&FF_THREAD_FRAME)
        ret = ff_frame_thread_init(avctx);
    else if (avctx->active_thread_type&FF_THREAD_SLICE)
        ret = ff_slice_thread_init(avctx);
    /*
    av_log(avctx, AV_LOG_INFO, "nb threads_frame = %d, nb threads_slice %d, thread_type = %s%s \n",
           avctx->thread_count_frame, avctx->thread_count,
           (avctx->active_thread_type == 0 ? "null" : (avctx->active_thread_type & FF_THREAD_FRAME ? "frame" : "")),
           (avctx->active_thread_type & FF_THREAD_SLICE ? "slice" : ""));
    */
    return ret;
}

void ff_thread_free(AVCodecContext *avctx)
{
    if (avctx->active_thread_type&FF_THREAD_FRAME)
        ff_frame_thread_free(avctx, avctx->thread_count_frame);
    else
        ff_slice_thread_free(avctx);
}

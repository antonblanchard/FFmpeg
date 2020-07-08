/*
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

#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include <sys/sysinfo.h>

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avdevice.h"

typedef struct CPUContext {
    AVClass *class;
    AVFormatContext *ctx;

    unsigned int nprocs;
    uint8_t *cpus;
    pthread_t *tids;
    int window_width, window_height;
} CPUContext;

#define TIMESLICE_NS (10UL*1000*1000)

static void busy_loop(unsigned long ns)
{
    struct timespec tv_start, tv_now;
    unsigned long elapsed_ns;

    clock_gettime(CLOCK_MONOTONIC, &tv_start);

    do {
        clock_gettime(CLOCK_MONOTONIC, &tv_now);
        elapsed_ns = (tv_now.tv_nsec-tv_start.tv_nsec) + (tv_now.tv_sec-tv_start.tv_sec)*1000000000UL;
    } while (elapsed_ns < ns);
}

static void idle_loop(unsigned long ns)
{
    struct timespec tv;

    tv.tv_sec = 0;
    tv.tv_nsec = ns;

    nanosleep(&tv, NULL);
}

static void *soaker_thread(void *p)
{
    uint8_t *c = p;

    while (1) {
        uint8_t x = *(volatile uint8_t *)c;
        unsigned long busy_ns, idle_ns;

        idle_ns = TIMESLICE_NS * x / 255.0;
        busy_ns = TIMESLICE_NS-idle_ns;

        busy_loop(busy_ns);
        idle_loop(idle_ns);
    }

    return NULL;
}

static int cpu_write_trailer(AVFormatContext *s)
{
    CPUContext *c = s->priv_data;

    for (unsigned long i = 0; i < c->nprocs; i++)
        pthread_cancel(c->tids[i]);

    for (unsigned long i = 0; i < c->nprocs; i++)
        pthread_join(c->tids[i], NULL);

    free(c->tids);
    free(c->cpus);

    return 0;
}

static int cpu_write_header(AVFormatContext *s)
{
    CPUContext *c = s->priv_data;
    AVStream *st = s->streams[0];
    AVCodecParameters *encctx = st->codecpar;
    int bpp;
    cpu_set_t *cpumask;
    size_t size;

    c->ctx = s;

    if (   s->nb_streams > 1
        || encctx->codec_type != AVMEDIA_TYPE_VIDEO
        || encctx->codec_id   != AV_CODEC_ID_RAWVIDEO) {
        av_log(s, AV_LOG_ERROR, "Only supports one rawvideo stream\n");
        return AVERROR(EINVAL);
    }

    if (encctx->format != AV_PIX_FMT_GRAY8) {
        av_log(s, AV_LOG_ERROR,
               "Unsupported pixel format '%s', choose gray8\n",
               av_get_pix_fmt_name(encctx->format));
        return AVERROR(EINVAL);
    }

    bpp = av_get_bits_per_pixel(av_pix_fmt_desc_get(encctx->format));
    if (bpp != 8) {
        av_log(s, AV_LOG_ERROR,
               "Unsupported pixel format '%s', choose gray8\n",
               av_get_pix_fmt_name(encctx->format));
        return AVERROR(EINVAL);
    }

    c->nprocs = get_nprocs();

    if ((encctx->width*encctx->height) != c->nprocs) {
        av_log(s, AV_LOG_ERROR,
               "Resolution must match CPUs");
        return AVERROR(EINVAL);
    }

    c->window_width = encctx->width;
    c->window_height = encctx->height;

    c->cpus = malloc(c->nprocs);
    assert(c->cpus);
    memset(c->cpus, 1, c->nprocs);

    c->tids = malloc(c->nprocs*sizeof(pthread_t));
    assert(c->tids);

    cpumask = CPU_ALLOC(c->nprocs);
    if (cpumask == NULL) {
        perror("CPU_ALLOC");
        return AVERROR(EINVAL);
    }

    size = CPU_ALLOC_SIZE(c->nprocs);

    for (unsigned long cpu = 0; cpu < c->nprocs; cpu++) {
        pthread_attr_t attr;

        CPU_ZERO_S(size, cpumask);
        CPU_SET_S(cpu, size, cpumask);

        pthread_attr_init(&attr);

        if (pthread_attr_setaffinity_np(&attr, size, cpumask)) {
            perror("pthread_attr_setaffinity_np");
            exit(1);
        }

        if (pthread_create(&c->tids[cpu], &attr, soaker_thread, (void *)&c->cpus[cpu])) {
            perror("pthread_create");
            exit(1);
        }

        pthread_attr_destroy(&attr);
    }

    CPU_FREE(cpumask);

    return 0;
}

static int cpu_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    CPUContext *c = s->priv_data;

    memcpy(c->cpus, pkt->data, c->window_width*c->window_height);

    return 0;
}

static const AVClass cpu_class = {
    .class_name = "CPU outdev",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

AVOutputFormat ff_cpu_muxer = {
    .name           = "cpu",
    .long_name      = NULL_IF_CONFIG_SMALL("CPU (CPUs are pixels, and consumption) output device"),
    .priv_data_size = sizeof(CPUContext),
    .audio_codec    = AV_CODEC_ID_NONE,
    .video_codec    = AV_CODEC_ID_RAWVIDEO,
    .write_header   = cpu_write_header,
    .write_packet   = cpu_write_packet,
    .write_trailer  = cpu_write_trailer,
    .flags          = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .priv_class     = &cpu_class,
};

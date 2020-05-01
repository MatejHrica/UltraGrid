/**
 * @file   video_capture/screen_osx.c
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2012-2020 CESNET, z.s.p.o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif /* HAVE_CONFIG_H */

#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "video.h"
#include "video_capture.h"

#include "tv.h"

#include "audio/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <pthread.h>

#include <Carbon/Carbon.h>

#define MOD_NAME "[screen cap mac] "

/* prototypes of functions defined in this module */
static void show_help(void);
static void vidcap_screen_osx_done(void *state);

static void show_help()
{
        printf("Screen capture\n");
        printf("Usage\n");
        printf("\t-t screen[:fps=<fps>][:codec=<c>]\n");
        printf("\t\t<fps> - preferred grabbing fps (otherwise unlimited)\n");
        printf("\t\t <c>  - requested codec to capture (RGB /default/ or RGBA)\n");
}

struct vidcap_screen_osx_state {
        struct video_desc desc;
        int frames;
        struct       timeval t, t0;
        CGDirectDisplayID display;
        decoder_t       decode; ///< decoder, must accept BGRA (different shift)

        struct timeval prev_time;

        bool initialized;
};

static void initialize(struct vidcap_screen_osx_state *s) {
        s->display = CGMainDisplayID();
        CGImageRef image = CGDisplayCreateImage(s->display);

        s->desc.width = CGImageGetWidth(image);
        s->desc.height = CGImageGetHeight(image);
        CFRelease(image);
}

static struct vidcap_type * vidcap_screen_osx_probe(bool verbose, void (**deleter)(void *))
{
        struct vidcap_type*		vt;
        *deleter = free;

        vt = (struct vidcap_type *) calloc(1, sizeof(struct vidcap_type));
        if (vt == NULL) {
                return NULL;
        }
        vt->name        = "screen";
        vt->description = "Grabbing screen";

        if (!verbose) {
                return vt;
        }

        vt->card_count = 1;
        vt->cards = calloc(vt->card_count, sizeof(struct device_info));
        // vt->cards[0].id can be "" since screen cap. doesn't require parameters
        snprintf(vt->cards[0].name, sizeof vt->cards[0].name, "Screen capture");

        int framerates[] = {24, 30, 60};

        snprintf(vt->cards[0].modes[0].name, sizeof vt->cards[0].name,
                        "Unlimited fps");
        snprintf(vt->cards[0].modes[0].id, sizeof vt->cards[0].id,
                        "{\"fps\":\"\"}");

        for(unsigned i = 0; i < sizeof(framerates) / sizeof(framerates[0]); i++){
                snprintf(vt->cards[0].modes[i + 1].name, sizeof vt->cards[0].name,
                                "%d fps", framerates[i]);
                snprintf(vt->cards[0].modes[i + 1].id, sizeof vt->cards[0].id,
                                "{\"fps\":\"%d\"}", framerates[i]);
        }

        return vt;
}

static int vidcap_screen_osx_init(struct vidcap_params *params, void **state)
{
        struct vidcap_screen_osx_state *s;

        printf("vidcap_screen_init\n");

        if (vidcap_params_get_flags(params) & VIDCAP_FLAG_AUDIO_ANY) {
                return VIDCAP_INIT_AUDIO_NOT_SUPPOTED;
        }

        s = (struct vidcap_screen_osx_state *) calloc(1, sizeof(struct vidcap_screen_osx_state));
        if(s == NULL) {
                printf("Unable to allocate screen capture state\n");
                return VIDCAP_INIT_FAIL;
        }

        s->initialized = false;

        gettimeofday(&s->t0, NULL);

        s->desc.tile_count = 1;
        s->desc.color_spec = RGB;
        s->desc.fps = 30;
        s->desc.interlacing = PROGRESSIVE;

        if(vidcap_params_get_fmt(params)) {
                if (strcmp(vidcap_params_get_fmt(params), "help") == 0) {
                        show_help();
                        return VIDCAP_INIT_NOERR;
                } else if (strncasecmp(vidcap_params_get_fmt(params), "fps=", strlen("fps=")) == 0) {
                        s->desc.fps = atof(vidcap_params_get_fmt(params) + strlen("fps="));
                } else if (strncasecmp(vidcap_params_get_fmt(params), "codec=", strlen("codec=")) == 0) {
                        s->desc.color_spec = get_codec_from_name(vidcap_params_get_fmt(params) + strlen("codec="));
                }
        }

        switch (s->desc.color_spec) {
        case RGB:
                s->decode = vc_copylineRGBAtoRGBwithShift;
                break;
        case RGBA:
                s->decode = vc_copylineRGBA;
                break;
        default:
                log_msg(LOG_LEVEL_ERROR, MOD_NAME "Only RGB and RGBA are currently supported!\n");
                vidcap_screen_osx_done(s);
                return VIDCAP_INIT_FAIL;
        }

        *state = s;
        return VIDCAP_INIT_OK;
}

static void vidcap_screen_osx_done(void *state)
{
        struct vidcap_screen_osx_state *s = (struct vidcap_screen_osx_state *) state;

        assert(s != NULL);

        free(s);
}

static struct video_frame * vidcap_screen_osx_grab(void *state, struct audio_frame **audio)
{
        struct vidcap_screen_osx_state *s = (struct vidcap_screen_osx_state *) state;

        if (!s->initialized) {
                initialize(s);
                s->initialized = true;
        }

        struct video_frame *frame = vf_alloc_desc_data(s->desc);
        struct tile *tile = vf_get_tile(frame, 0);
        frame->callbacks.dispose = vf_free;

        *audio = NULL;

        CGImageRef image = CGDisplayCreateImage(s->display);
        CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
        const unsigned char *pixels = CFDataGetBytePtr(data);

        int src_linesize = tile->width * 4;
        int dst_linesize = vc_get_linesize(tile->width, frame->color_spec);
        unsigned char *dst = (unsigned char *) tile->data;
        const unsigned char *src = (const unsigned char *) pixels;
        for (unsigned int y = 0; y < tile->height; ++y) {
                s->decode(dst, src, dst_linesize, 16, 8, 0);
                src += src_linesize;
                dst += dst_linesize;
        }

        CFRelease(data);
        CFRelease(image);

        struct timeval cur_time;
        gettimeofday(&cur_time, NULL);
        while(tv_diff_usec(cur_time, s->prev_time) < 1000000.0 / frame->fps) {
                gettimeofday(&cur_time, NULL);
        }
        s->prev_time = cur_time;

        gettimeofday(&s->t, NULL);
        double seconds = tv_diff(s->t, s->t0);        
        if (seconds >= 5) {
                float fps  = s->frames / seconds;
                log_msg(LOG_LEVEL_INFO, "[screen capture] %d frames in %g seconds = %g FPS\n", s->frames, seconds, fps);
                s->t0 = s->t;
                s->frames = 0;
        }

        s->frames++;

        return frame;
}

static const struct video_capture_info vidcap_screen_osx_info = {
        vidcap_screen_osx_probe,
        vidcap_screen_osx_init,
        vidcap_screen_osx_done,
        vidcap_screen_osx_grab,
        false
};

REGISTER_MODULE(screen, &vidcap_screen_osx_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);


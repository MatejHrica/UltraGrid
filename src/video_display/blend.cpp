/**
 * @file   video_display/blend.cpp
 * @author Martin Pulec     <pulec@cesnet.cz>
 */
/*
 * Copyright (c) 2014-2015 CESNET, z. s. p. o.
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
/**
 * @file
 * @todo
 * Rewrite the code to have more clear state machine!
 */

#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#include "debug.h"
#include "lib_common.h"
#include "video.h"
#include "video_display.h"

#include <cinttypes>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

using namespace std;

static constexpr int TRANSITION_COUNT = 10;
static constexpr int BUFFER_LEN = 5;
static constexpr chrono::milliseconds SOURCE_TIMEOUT(500);
static constexpr unsigned int IN_QUEUE_MAX_BUFFER_LEN = 5;
static constexpr int SKIP_FIRST_N_FRAMES_IN_STREAM = 5;

struct state_blend_common {
        ~state_blend_common() {
                display_done(real_display);

                for (auto && ssrc_map : frames) {
                        for (auto && frame : ssrc_map.second) {
                                vf_free(frame);
                        }
                }
        }
        struct display *real_display;
        struct video_desc display_desc;

        uint32_t current_ssrc;
        uint32_t old_ssrc;

        int transition;

        queue<struct video_frame *> incoming_queue;
        condition_variable in_queue_decremented_cv;
        map<uint32_t, list<struct video_frame *> > frames;
        unordered_map<uint32_t, chrono::system_clock::time_point> disabled_ssrc;

        mutex lock;
        condition_variable cv;

        struct module *parent;
};

struct state_blend {
        shared_ptr<struct state_blend_common> common;
        struct video_desc desc;
};

static struct display *display_blend_fork(void *state)
{
        shared_ptr<struct state_blend_common> s = ((struct state_blend *)state)->common;
        struct display *out;
        char fmt[2 + sizeof(void *) * 2 + 1] = "";
        snprintf(fmt, sizeof fmt, "%p", state);

        int rc = initialize_video_display(s->parent,
                        "blend", fmt, 0, NULL, &out);
        if (rc == 0) return out; else return NULL;

        return out;
}

static void *display_blend_init(struct module *parent, const char *fmt, unsigned int flags)
{
        char *fmt_copy = NULL;
        const char *requested_display = "";
        const char *cfg = "";
        int ret;

        if (fmt == nullptr || strlen(fmt) == 0 || "help"s == fmt) {
                cout << "blend is a helper display to combine (blend) multiple incoming streams.\n"
                                "Please do not use directly, intended for internal purposes!\n";
                return nullptr;
        }

        auto *s = new state_blend();

        if (isdigit(fmt[0]) != 0) { // fork
                struct state_blend *orig = nullptr;
                sscanf(fmt, "%p", &orig);
                s->common = orig->common;
                return s;
        }
        fmt_copy = strdup(fmt);
        requested_display = fmt_copy;
        char *delim = strchr(fmt_copy, ':');
        if (delim != nullptr) {
                *delim = '\0';
                cfg = delim + 1;
        }
        s->common = shared_ptr<state_blend_common>(new state_blend_common());
        ret = initialize_video_display(parent, requested_display, cfg, flags, NULL, &s->common->real_display);
        assert(ret == 0 && "Unable to initialize real display for blend");
        free(fmt_copy);

        display_run_new_thread(s->common->real_display);

        s->common->parent = parent;

        return s;
}

static void check_reconf(struct state_blend_common *s, struct video_desc desc)
{
        if (!video_desc_eq(desc, s->display_desc)) {
                s->display_desc = desc;
                fprintf(stderr, "RECONFIGURED\n");
                display_reconfigure(s->real_display, s->display_desc, VIDEO_NORMAL);
        }
}

static void display_blend_run(void *state)
{
        shared_ptr<struct state_blend_common> s = ((struct state_blend *)state)->common;
        bool prefill = false;
        int skipped = 0;

        while (1) {
                struct video_frame *frame;
                {
                        unique_lock<mutex> lg(s->lock);
                        s->cv.wait(lg, [s]{return s->incoming_queue.size() > 0;});
                        frame = s->incoming_queue.front();
                        s->incoming_queue.pop();
                        s->in_queue_decremented_cv.notify_one();
                }

                if (!frame) {
                        display_put_frame(s->real_display, NULL, PUTF_BLOCKING);
                        break;
                }

                chrono::system_clock::time_point now = chrono::system_clock::now();
                auto it = s->disabled_ssrc.find(frame->ssrc);
                if (it != s->disabled_ssrc.end()) {
                        it->second = now;
                        vf_free(frame);
                        continue;
                }

                it = s->disabled_ssrc.begin();
                while (it != s->disabled_ssrc.end()) {
                        if (chrono::duration_cast<chrono::milliseconds>(now - it->second) > SOURCE_TIMEOUT) {
                                verbose_msg("Source 0x%08" PRIx32 " timeout. Deleting from blend display.\n", it->first);
                                s->disabled_ssrc.erase(it++);
                        } else {
                                ++it;
                        }
                }

                if (frame->ssrc != s->current_ssrc && frame->ssrc != s->old_ssrc) {
                        if (skipped >= SKIP_FIRST_N_FRAMES_IN_STREAM) {
                                s->old_ssrc = s->current_ssrc; // if != 0, we will be in transition state
                                s->current_ssrc = frame->ssrc;
                                prefill = true;
                                skipped = 0;
                        } else {
                                skipped++;
                                continue;
                        }
                }

                s->frames[frame->ssrc].push_back(frame);

                if (s->frames[s->current_ssrc].size() >= BUFFER_LEN) {
                        prefill = false;
                }

                // we may receive two streams concurrently, therefore we use the timing according to
                // the later one
                if (frame->ssrc != s->current_ssrc) {
                        continue;
                }

                if (s->old_ssrc != 0) {
                        if (prefill) {
                                auto & ssrc_list = s->frames[s->old_ssrc];
                                if (ssrc_list.empty()) {
                                        fprintf(stderr, "SMOLIK1!\n");
                                } else {
                                        frame = ssrc_list.front();
                                        ssrc_list.pop_front();

                                        check_reconf(s.get(), video_desc_from_frame(frame));

                                        struct video_frame *real_display_frame = display_get_frame(s->real_display);
                                        memcpy(real_display_frame->tiles[0].data, frame->tiles[0].data, frame->tiles[0].data_len);
                                        vf_free(frame);
                                        real_display_frame->ssrc = s->current_ssrc;
                                        display_put_frame(s->real_display, real_display_frame, PUTF_BLOCKING);
                                }
                        } else {
                                auto & old_list = s->frames[s->old_ssrc];
                                auto & new_list = s->frames[s->current_ssrc];

                                s->transition += 1;

                                if (old_list.empty() || new_list.empty()) {
                                        fprintf(stderr, "SMOLIK2!\n");
                                        // ok here, we do not have nothing to mix, therefore cancel smooth transition
                                        s->transition = TRANSITION_COUNT;
                                } else {
                                        struct video_frame *old_frame, *new_frame;

                                        old_frame = old_list.front();
                                        old_list.pop_front();
                                        new_frame = new_list.front();
                                        new_list.pop_front();

                                        struct video_desc old_desc = video_desc_from_frame(old_frame),
                                                          new_desc = video_desc_from_frame(new_frame);

                                        check_reconf(s.get(), new_desc);

                                        struct video_frame *real_display_frame = display_get_frame(s->real_display);

                                        if (video_desc_eq(old_desc, new_desc)) {
                                                for (unsigned int i = 0; i < new_frame->tiles[0].data_len; ++i) {
                                                        int old_val = ((unsigned char *) old_frame->tiles[0].data)[i];
                                                        int new_val = ((unsigned char *) new_frame->tiles[0].data)[i];
                                                        ((unsigned char *) real_display_frame->tiles[0].data)[i] =
                                                                ((new_val * s->transition) + (old_val * (TRANSITION_COUNT - s->transition))) / TRANSITION_COUNT;
                                                }
                                        } else {
                                                // new desc is different than old desc!
                                                fprintf(stderr, "SMOLIK4!\n");
                                                memcpy(real_display_frame->tiles[0].data, new_frame->tiles[0].data, new_frame->tiles[0].data_len);
                                        }
                                        vf_free(old_frame);
                                        vf_free(new_frame);
                                        real_display_frame->ssrc = s->current_ssrc;
                                        display_put_frame(s->real_display, real_display_frame, PUTF_BLOCKING);
                                }
                        }
                } else {
                        if (!prefill) {
                                if (s->frames[s->current_ssrc].empty()) {
                                        // this should not happen, heh ?
                                        fprintf(stderr, "SMOLIK3!\n");
                                } else {
                                        frame = s->frames[s->current_ssrc].front();
                                        s->frames[s->current_ssrc].pop_front();

                                        check_reconf(s.get(), video_desc_from_frame(frame));

                                        struct video_frame *real_display_frame = display_get_frame(s->real_display);
                                        memcpy(real_display_frame->tiles[0].data, frame->tiles[0].data, frame->tiles[0].data_len);
                                        vf_free(frame);
                                        real_display_frame->ssrc = s->current_ssrc;
                                        display_put_frame(s->real_display, real_display_frame, PUTF_BLOCKING);
                                }
                        }
                }

                if (s->old_ssrc != 0 && s->transition >= TRANSITION_COUNT) {
                        for (auto && frame : s->frames[s->old_ssrc]) {
                                vf_free(frame);
                        }

                        s->frames.erase(s->old_ssrc);

                        s->disabled_ssrc[s->old_ssrc] = chrono::system_clock::now();
                        s->old_ssrc = 0u;
                        s->transition = 0;
                }
        }

        display_join(s->real_display);
}

static void display_blend_done(void *state)
{
        struct state_blend *s = (struct state_blend *)state;
        delete s;
}

static struct video_frame *display_blend_getf(void *state)
{
        struct state_blend *s = (struct state_blend *)state;

        return vf_alloc_desc_data(s->desc);
}

static int display_blend_putf(void *state, struct video_frame *frame, int flags)
{
        shared_ptr<struct state_blend_common> s = ((struct state_blend *)state)->common;

        if (flags == PUTF_DISCARD) {
                vf_free(frame);
        } else {
                unique_lock<mutex> lg(s->lock);
                if (s->incoming_queue.size() >= IN_QUEUE_MAX_BUFFER_LEN) {
                        fprintf(stderr, "blend: queue full!\n");
                }
                if (flags == PUTF_NONBLOCK && s->incoming_queue.size() >= IN_QUEUE_MAX_BUFFER_LEN) {
                        vf_free(frame);
                        return 1;
                }
                s->in_queue_decremented_cv.wait(lg, [s]{return s->incoming_queue.size() < IN_QUEUE_MAX_BUFFER_LEN;});
                s->incoming_queue.push(frame);
                lg.unlock();
                s->cv.notify_one();
        }

        return 0;
}

static int display_blend_get_property(void *state, int property, void *val, size_t *len)
{
        shared_ptr<struct state_blend_common> s = ((struct state_blend *)state)->common;
        if (property == DISPLAY_PROPERTY_SUPPORTS_MULTI_SOURCES) {
                ((struct multi_sources_supp_info *) val)->val = true;
                ((struct multi_sources_supp_info *) val)->fork_display = display_blend_fork;
                ((struct multi_sources_supp_info *) val)->state = state;
                *len = sizeof(struct multi_sources_supp_info);
                return TRUE;

        } else {
                return display_ctl_property(s->real_display, property, val, len);
        }
}

static int display_blend_reconfigure(void *state, struct video_desc desc)
{
        struct state_blend *s = (struct state_blend *) state;

        s->desc = desc;

        return 1;
}

static void display_blend_put_audio_frame(void *state, const struct audio_frame *frame)
{
        UNUSED(state);
        UNUSED(frame);
}

static int display_blend_reconfigure_audio(void *state, int quant_samples, int channels,
                int sample_rate)
{
        UNUSED(state);
        UNUSED(quant_samples);
        UNUSED(channels);
        UNUSED(sample_rate);

        return FALSE;
}

static void display_blend_probe(struct device_info **available_cards, int *count, void (**deleter)(void *)) {
        UNUSED(deleter);
        *available_cards = nullptr;
        *count = 0;
}

static const struct video_display_info display_blend_info = {
        display_blend_probe,
        display_blend_init,
        display_blend_run,
        display_blend_done,
        display_blend_getf,
        display_blend_putf,
        display_blend_reconfigure,
        display_blend_get_property,
        display_blend_put_audio_frame,
        display_blend_reconfigure_audio,
        DISPLAY_DOESNT_NEED_MAINLOOP,
        false,
};

REGISTER_HIDDEN_MODULE(blend, &display_blend_info, LIBRARY_CLASS_VIDEO_DISPLAY, VIDEO_DISPLAY_ABI_VERSION);


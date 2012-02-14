/*
 * FILE:    audio/audio.c
 * AUTHORS: Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2010 CESNET z.s.p.o.
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 * 
 *      This product includes software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of CESNET nor the names of its contributors may be used 
 *    to endorse or promote products derived from this software without specific
 *    prior written permission.
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
 *
 *
 */

#include "audio/audio.h" 
#include "audio/audio_capture.h" 
#include "audio/jack.h" 
#include "audio/playback/portaudio.h" 
#include "audio/playback/alsa.h" 
#include "audio/playback/coreaudio.h" 
#include "audio/playback/none.h" 
#include "audio/playback/jack.h" 
#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif
#include "debug.h"
#include "host.h"
#include "perf.h"
#include "rtp/audio_decoders.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/pbuf.h"
#include "tv.h"
#include "transmit.h"
#include "pdb.h"
#include "lib_common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define EXIT_FAIL_USAGE		1
#define EXIT_FAIL_NETWORK	5

struct audio_device_t {
        int index;
        void *state;
};

enum audio_transport_device {
        NET_NATIVE,
        NET_JACK
};

struct state_audio {
        struct state_audio_capture *audio_capture_device;
        struct audio_device_t audio_playback_device;
        
        struct rtp *audio_network_device;
        struct pdb *audio_participants;
        void *jack_connection;
        enum audio_transport_device sender;
        enum audio_transport_device receiver;
        
        struct timeval start_time;

        struct tx *tx_session;
        
        pthread_t audio_sender_thread_id,
                  audio_receiver_thread_id;
};

struct state_sdi_playback {
        struct audio_frame * (*get_callback)(void *);
        void (*put_callback)(void *, struct audio_frame *);
        int (*reconfigure_callback)(void *state, int quant_samples, int channels,
                int sample_rate);
        void *get_udata;
        void *put_udata;
        void *reconfigure_udata;
};

/** 
 * Copies one input channel into n output (interlaced).
 * 
 * Input and output data may overlap. 
 */
void copy_channel(char *out, const char *in, int bps, int in_len /* bytes */, int out_channel_count); 


typedef void (*audio_device_help_t)(void);
/**
 * @return state
 */

/* playback */
typedef void * (*audio_init_t)(char *cfg);
typedef void (*audio_finish_t)(void *state);
typedef void (*audio_done_t)(void *state);
typedef struct audio_frame* (*audio_get_frame_t)(void *state);
typedef void (*audio_put_frame_t)(void *state, struct audio_frame *frame);
/*
 * Returns TRUE if succeeded, FALSE otherwise
 */
typedef int (*audio_reconfigure_t)(void *state, int quant_samples, int channels,
                int sample_rate);
typedef void (*audio_playback_done_t)(void *s);

struct audio_playback_t {
        const char              *name;

        const char              *library_name;
        audio_device_help_t      audio_help;
        const char              *audio_help_str;
        audio_init_t             audio_init;
        const char              *audio_init_str;
        audio_get_frame_t        audio_get_frame;
        const char              *audio_get_frame_str;
        audio_put_frame_t        audio_put_frame;
        const char              *audio_put_frame_str;
        audio_playback_done_t    audio_playback_done;
        const char              *audio_playback_done_str;
        audio_reconfigure_t      audio_reconfigure;
        const char              *audio_reconfigure_str;

        void *handle;
};

void sdi_playback_help(void);
void * sdi_playback_init(char *cfg);
void sdi_playback_done(void *s);
int sdi_reconfigure(void *state, int quant_samples, int channels,
                int sample_rate);

static void *audio_sender_thread(void *arg);
static void *audio_receiver_thread(void *arg);
static struct rtp *initialize_audio_network(char *addr, int port, struct pdb *participants);
void initialize_audio_playback(void);
void print_audio_playback_devices(void);

static struct audio_playback_t audio_playback_table[] = {
        { "embedded",
               NULL,
               MK_STATIC(sdi_playback_help),
               MK_STATIC(sdi_playback_init),
               MK_STATIC(sdi_get_frame),
               MK_STATIC(sdi_put_frame),
               MK_STATIC(sdi_playback_done),
               MK_STATIC(sdi_reconfigure),
               NULL
        },
#if defined HAVE_ALSA || defined BUILD_LIBRARIES
        { "alsa",
                "alsa",
                MK_NAME(audio_play_alsa_help),
                MK_NAME(audio_play_alsa_init),
                MK_NAME(audio_play_alsa_get_frame),
                MK_NAME(audio_play_alsa_put_frame),
                MK_NAME(audio_play_alsa_done),
                MK_NAME(audio_play_alsa_reconfigure),
                NULL
        },
#endif
#if defined HAVE_COREAUDIO
        { "coreaudio",
                NULL,
                MK_STATIC(audio_play_ca_help),
                MK_STATIC(audio_play_ca_init),
                MK_STATIC(audio_play_ca_get_frame),
                MK_STATIC(audio_play_ca_put_frame),
                MK_STATIC(audio_play_ca_done),
                MK_STATIC(audio_play_reconfigure),
                NULL
        },
#endif
#if defined HAVE_JACK || defined BUILD_LIBRARIES
        { "jack",
                "jack",
                MK_NAME(audio_play_jack_help),
                MK_NAME(audio_play_jack_init),
                MK_NAME(audio_play_jack_get_frame),
                MK_NAME(audio_play_jack_put_frame),
                MK_NAME(audio_play_jack_done),
                MK_NAME(audio_play_jack_reconfigure),
                NULL
        },
#endif
#if defined HAVE_PORTAUDIO || defined BUILD_LIBRARIES
        { "portaudio",
                "portaudio",
                MK_NAME(portaudio_playback_help),
                MK_NAME(portaudio_playback_init),
                MK_NAME(portaudio_get_frame),
                MK_NAME(portaudio_put_frame),
                MK_NAME(portaudio_close_playback),
                MK_NAME(portaudio_reconfigure),
                NULL
        },
#endif
        { "none",
                NULL,
                MK_STATIC(audio_play_none_help),
                MK_STATIC(audio_play_none_init),
                MK_STATIC(audio_play_none_get_frame),
                MK_STATIC(audio_play_none_put_frame),
                MK_STATIC(audio_play_none_done),
                MK_STATIC(audio_play_none_reconfigure),
                NULL
        }
};

#define MAX_AUDIO_PLAY (sizeof(audio_playback_table) / sizeof(struct audio_playback_t))

struct audio_playback_t *available_audio_playback[MAX_AUDIO_PLAY];
int available_audio_playback_count = 0;

#ifdef BUILD_LIBRARIES
static void *audio_playback_open_library(const char *playback_name)
{
        char name[128];
        snprintf(name, sizeof(name), "aplay_%s.so.%d", playback_name, AUDIO_PLAYBACK_ABI_VERSION);

        return open_library(name);
}

static int audio_playback_fill_symbols(struct audio_playback_t *device)
{
        void *handle = device->handle;

        device->audio_help = (audio_device_help_t)
                dlsym(handle, device->audio_help_str);
        device->audio_init = (audio_init_t)
                dlsym(handle, device->audio_init_str);
        device->audio_get_frame = (audio_get_frame_t)
                dlsym(handle, device->audio_get_frame_str);
        device->audio_put_frame = (audio_put_frame_t)
                dlsym(handle, device->audio_put_frame_str);
        device->audio_playback_done = (audio_done_t)
                dlsym(handle, device->audio_playback_done_str);
        device->audio_reconfigure = (audio_reconfigure_t)
                dlsym(handle, device->audio_reconfigure_str);

        if(!device->audio_help || !device->audio_init || !device->audio_get_frame ||
                        !device->audio_put_frame || !device->audio_playback_done || !device->audio_reconfigure) {
                fprintf(stderr, "Library %s opening error: %s \n", device->library_name, dlerror());
                return FALSE;
        }

        return TRUE;
}
#endif

void initialize_audio_playback()
{
        unsigned int i;

        for(i = 0; i < MAX_AUDIO_PLAY; ++i) {
#ifdef BUILD_LIBRARIES
                if(audio_playback_table[i].library_name) {
                        int ret;
                        audio_playback_table[i].handle =
                                audio_playback_open_library(audio_playback_table[i].library_name);
                        if(!audio_playback_table[i].handle) {
                                continue;
                        }
                        ret = audio_playback_fill_symbols(&audio_playback_table[i]);
                        if(!ret) {
                                continue;
                        }
                }
#endif
                available_audio_playback[available_audio_playback_count] = &audio_playback_table[i];
                available_audio_playback_count++;
        }
}

void sdi_playback_help(void)
{
        printf("\tembedded : SDI audio (if available)\n");
}

void print_audio_playback_devices()
{
        int i;
        printf("Available audio playback devices:\n");
        for (i = 0; i < available_audio_playback_count; ++i) {
                available_audio_playback[i]->audio_help();
                printf("\n");
        }
}

/**
 * take care that addrs can also be comma-separated list of addresses !
 */
struct state_audio * audio_cfg_init(char *addrs, int port, char *send_cfg, char *recv_cfg, char *jack_cfg)
{
        struct state_audio *s = NULL;
        char *tmp, *unused = NULL;
        char *addr;
        int i;
        
        audio_capture_init_devices();
        initialize_audio_playback();

        if (send_cfg != NULL &&
                        !strcmp("help", send_cfg)) {
                audio_capture_print_help();
                exit_uv(0);
                return NULL;
        }
        
        if (recv_cfg != NULL &&
                        !strcmp("help", recv_cfg)) {
                print_audio_playback_devices();
                exit_uv(0);
                return NULL;
        }
        
        s = calloc(1, sizeof(struct state_audio));
        s->audio_participants = NULL;
        
        s->tx_session = tx_init(1500, NULL);
        gettimeofday(&s->start_time, NULL);        
        
        tmp = strdup(addrs);
        s->audio_participants = pdb_init();
        addr = strtok_r(tmp, ",", &unused);
        if ((s->audio_network_device =
             initialize_audio_network(addr, port,
                                      s->audio_participants)) ==
            NULL) {
                printf("Unable to open audio network\n");
                goto error;
        }
        free(tmp);

        if (send_cfg != NULL) {
                char *device = strtok(send_cfg, ":");
                char *cfg = strtok(NULL, ":");

                s->audio_capture_device = audio_capture_init(device, cfg);
                
                if(!s->audio_capture_device) {
                        fprintf(stderr, "Error initializing audio capture.\n");
                        goto error;
                }
        } else {
                s->audio_capture_device = audio_capture_init_null_device();
        }
        
        if (recv_cfg != NULL) {
                char *tmp = strtok(recv_cfg, ":");
                for (i = 0; i < available_audio_playback_count; ++i) {
                        if(strcmp(tmp, available_audio_playback[i]->name) == 0) {
                                s->audio_playback_device.index = i;
                                break;
                        }
                }
                if(i == available_audio_playback_count) {
                        fprintf(stderr, "Unknown audio driver: %s\n", tmp);
                        goto error;
                }
                
                tmp = strtok(NULL, ":");
                s->audio_playback_device.state =
                        available_audio_playback[s->audio_playback_device.index]->audio_init(tmp);
                if(!s->audio_playback_device.state) {
                        fprintf(stderr, "Error initializing audio playback.\n");
                        goto error;
                }
        } else {
                for (i = 0; i < available_audio_playback_count; ++i) {
                        if(strcmp("none", available_audio_playback[i]->name) == 0) {
                                s->audio_playback_device.index = i;
                        }
                }
        }

        if (send_cfg != NULL) {
                if (pthread_create
                    (&s->audio_sender_thread_id, NULL, audio_sender_thread, (void *)s) != 0) {
                        fprintf(stderr,
                                "Error creating audio thread. Quitting\n");
                        goto error;
                }
        }

        if (recv_cfg != NULL) {
                if (pthread_create
                    (&s->audio_receiver_thread_id, NULL, audio_receiver_thread, (void *)s) != 0) {
                        fprintf(stderr,
                                "Error creating audio thread. Quitting\n");
                        goto error;
                }
        }
        
        s->sender = NET_NATIVE;
        s->receiver = NET_NATIVE;
        
#ifdef HAVE_JACK_TRANS
        s->jack_connection = jack_start(jack_cfg);
        if(s->jack_connection) {
                if(is_jack_sender(s->jack_connection))
                        s->sender = NET_JACK;
                if(is_jack_receiver(s->jack_connection))
                        s->receiver = NET_JACK;
        }
#else
        if(jack_cfg) {
                fprintf(stderr, "[Audio] JACK configuration string entered ('-j'), "
                                "but JACK support isn't compiled.\n");
                goto error;
        }
#endif


        return s;

error:
        if(s->tx_session)
                tx_done(s->tx_session);
        if(s->audio_participants)
                pdb_destroy(&s->audio_participants);
        free(s);
        exit_uv(1);
        return NULL;
}

void audio_join(struct state_audio *s) {
        if(s) {
                if(s->audio_receiver_thread_id)
                        pthread_join(s->audio_receiver_thread_id, NULL);
                if(s->audio_sender_thread_id)
                        pthread_join(s->audio_sender_thread_id, NULL);
        }
}
        
void audio_finish(struct state_audio *s)
{
        if(s) {
                audio_capture_finish(s->audio_capture_device);
        }
}

void audio_done(struct state_audio *s)
{
        if(s) {
                available_audio_playback[s->audio_playback_device.index]->audio_playback_done(s->audio_playback_device.state);
                audio_capture_done(s->audio_capture_device);
                tx_done(s->tx_session);
                if(s->audio_network_device)
                        rtp_done(s->audio_network_device);
                if(s->audio_participants)
                        pdb_destroy(&s->audio_participants);
                free(s);
        }
}

void * sdi_playback_init(char *cfg)
{
        struct state_sdi_playback *s = calloc(1, sizeof(struct state_sdi_playback));
        UNUSED(cfg);
        s->get_callback = NULL;
        s->put_callback = NULL;
        return s;
}

static struct rtp *initialize_audio_network(char *addr, int port, struct pdb *participants)       // GiX
{
        struct rtp *r;
        double rtcp_bw = 1024 * 512;    // FIXME:  something about 5% for rtcp is said in rfc

        r = rtp_init(addr, port, port, 255, rtcp_bw, FALSE, rtp_recv_callback,
                     (void *)participants);
        if (r != NULL) {
                pdb_add(participants, rtp_my_ssrc(r));
                rtp_set_option(r, RTP_OPT_WEAK_VALIDATION, TRUE);
                rtp_set_sdes(r, rtp_my_ssrc(r), RTCP_SDES_TOOL,
                             PACKAGE_STRING, strlen(PACKAGE_VERSION));
        }

        return r;
}

static void *audio_receiver_thread(void *arg)
{
        struct state_audio *s = arg;
        // rtp variables
        struct timeval timeout, curr_time;
        uint32_t ts;
        struct pdb_e *cp;
        struct pbuf_audio_data pbuf_data;

        pbuf_data.buffer = available_audio_playback[s->audio_playback_device.index]->audio_get_frame(
                        s->audio_playback_device.state);
        pbuf_data.audio_state = s;
        pbuf_data.saved_channels = pbuf_data.saved_bps = pbuf_data.saved_sample_rate = 0;
                
        printf("Audio receiving started.\n");
        while (!should_exit) {
                if(s->receiver == NET_NATIVE) {
                        gettimeofday(&curr_time, NULL);
                        ts = tv_diff(curr_time, s->start_time) * 90000;
                        rtp_update(s->audio_network_device, curr_time);
                        rtp_send_ctrl(s->audio_network_device, ts, 0, curr_time);
                        timeout.tv_sec = 0;
                        timeout.tv_usec = 999999 / 59.94; /* audio goes almost always at the same rate
                                                             as video frames */
                        rtp_recv_r(s->audio_network_device, &timeout, ts);
                        cp = pdb_iter_init(s->audio_participants);
                
                        while (cp != NULL) {
                                if(pbuf_data.buffer != NULL) {
                                        if (pbuf_decode(cp->playout_buffer, curr_time, decode_audio_frame, &pbuf_data, FALSE)) {
                                                available_audio_playback[s->audio_playback_device.index]->audio_put_frame(
                                                        s->audio_playback_device.state, pbuf_data.buffer);
                                                pbuf_data.buffer= available_audio_playback[s->audio_playback_device.index]->audio_get_frame(
                                                        s->audio_playback_device.state);
                                        }
                                } else {
                                        pbuf_data.buffer = available_audio_playback[s->audio_playback_device.index]->audio_get_frame(
                                                s->audio_playback_device.state);
                                }
                                pbuf_remove(cp->playout_buffer, curr_time);
                                cp = pdb_iter_next(s->audio_participants);
                        }
                        pdb_iter_done(s->audio_participants);
                } else { /* NET_JACK */
#ifdef HAVE_JACK_TRANS
                        jack_receive(s->jack_connection, frame);
                        available_audio_playback[s->audio_playback_device.index]->audio_put_frame(
                                                s->audio_playback_device.state, frame);
                        frame = available_audio_playback[s->audio_playback_device.index]->audio_get_frame(
                                s->audio_playback_device.state);
#endif
                }
        }

        return NULL;
}

static void *audio_sender_thread(void *arg)
{
        struct state_audio *s = (struct state_audio *) arg;
        struct audio_frame *buffer = NULL;
        
        printf("Audio sending started.\n");
        while (!should_exit) {
                buffer = audio_capture_read(s->audio_capture_device);
                if(buffer) {
                        if(s->sender == NET_NATIVE)
                                audio_tx_send(s->tx_session, s->audio_network_device, buffer);
#ifdef HAVE_JACK_TRANS
                        else
                                jack_send(s->jack_connection, buffer);
#endif
                }
        }

        return NULL;
}

void audio_sdi_send(struct state_audio *s, struct audio_frame *frame) {
        void *sdi_capture;
        if(!audio_capture_does_send_sdi(s->audio_capture_device))
                return;
        
        sdi_capture = audio_capture_get_state_pointer(s->audio_capture_device);
        sdi_capture_new_incoming_frame(sdi_capture, frame);
}

int audio_does_send_sdi(struct state_audio *s)
{
        return audio_capture_does_send_sdi(s->audio_capture_device);
}

void audio_register_get_callback(struct state_audio *s, struct audio_frame * (*callback)(void *),
                void *udata)
{
        struct state_sdi_playback *sdi;
        //assert(strcmp(audio_capture[s->audio_capture_device.index].name, "embedded") == 0);
        
        sdi = (struct state_sdi_playback *) s->audio_playback_device.state;
        sdi->get_callback = callback;
        sdi->get_udata = udata;
}

void audio_register_put_callback(struct state_audio *s, void (*callback)(void *, struct audio_frame *),
                void *udata)
{
        struct state_sdi_playback *sdi;
        //assert(strcmp(audio_capture[s->audio_capture_device.index].name, "embedded") == 0);
        
        sdi = (struct state_sdi_playback *) s->audio_playback_device.state;
        sdi->put_callback = callback;
        sdi->put_udata = udata;
}

void audio_register_reconfigure_callback(struct state_audio *s, int (*callback)(void *, int, int,
                        int),
                void *udata)
{
        struct state_sdi_playback *sdi;
        //assert(strcmp(audio_capture[s->audio_capture_device.index].name, "embedded") == 0);
        
        sdi = (struct state_sdi_playback *) s->audio_playback_device.state;
        sdi->reconfigure_callback = callback;
        sdi->reconfigure_udata = udata;
}

int audio_does_receive_sdi(struct state_audio *s)
{
        if(!s) 
                return FALSE;
        return strcmp(available_audio_playback[s->audio_playback_device.index]->name, "embedded") == 0;
}

void sdi_put_frame(void *state, struct audio_frame *frame)
{
        struct state_sdi_playback *s;
        s = (struct state_sdi_playback *) state;

        if(s->put_callback)
                s->put_callback(s->put_udata, frame);
}

struct audio_frame * sdi_get_frame(void *state)
{
        struct state_sdi_playback *s;
        s = (struct state_sdi_playback *) state;
        
        if(s->get_callback)
                return s->get_callback(s->get_udata);
        else
                return NULL;
}

void sdi_playback_done(void *s)
{
        UNUSED(s);
}

int sdi_reconfigure(void *state, int quant_samples, int channels,
                int sample_rate)
{
        struct state_sdi_playback *s;
        s = (struct state_sdi_playback *) state;

        if(s->reconfigure_callback)
                return s->reconfigure_callback(s->put_udata, quant_samples, channels, sample_rate);
        else
                return FALSE;
}

struct audio_frame * audio_get_frame(struct state_audio *s)
{
        return available_audio_playback[s->audio_playback_device.index]->audio_get_frame(
                                                        s->audio_playback_device.state);
}

int audio_reconfigure(struct state_audio *s, int quant_samples, int channels,
                int sample_rate)
{
        return available_audio_playback[s->audio_playback_device.index]->audio_reconfigure(
                                                        s->audio_playback_device.state,
                                                        quant_samples, channels, sample_rate);
}


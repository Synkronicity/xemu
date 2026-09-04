/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"
#include <math.h>
#include <AL/al.h>
#include <AL/alc.h>

#ifndef AL_FORMAT_51CHN16
#define AL_FORMAT_51CHN16 0x120B
#endif

/* Globally static OpenAL components that persist across QEMU machine resets */
static bool al_globally_initialized = false;
static ALCdevice *al_dev = NULL;
static ALCcontext *al_ctx = NULL;
static ALuint al_source;
static ALuint al_buffers[4];
static ALuint al_free_buffers[4];
static int al_free_count = 0;

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
    };

    d->monitor.stream = NULL;

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    d->monitor.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (d->monitor.stream == NULL) {
        error_setg(errp, "SDL_OpenAudioDeviceStream failed: %s",
                   SDL_GetError());
        return;
    }

    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(d->monitor.stream);

    SDL_AudioSpec dev_spec;
    int dev_buf_frames = 0;
    int dev_drain_bytes = 0;
    if (SDL_GetAudioDeviceFormat(dev, &dev_spec, &dev_buf_frames)) {
        dev_drain_bytes = dev_buf_frames * spec.channels *
                          SDL_AUDIO_BYTESIZE(spec.format) *
                          spec.freq / dev_spec.freq;
    }
    int frame_bytes = sizeof(d->monitor.frame_buf);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
        d->monitor.stream = NULL;
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* 1. Global OpenAL initialization (runs once, persists across QEMU machine resets) */
    if (!al_globally_initialized) {
        al_dev = alcOpenDevice(NULL);
        if (al_dev) {
            al_ctx = alcCreateContext(al_dev, NULL);
            if (al_ctx && alcMakeContextCurrent(al_ctx)) {
                alGenSources(1, &al_source);
                if (alGetError() == AL_NO_ERROR) {
                    alGenBuffers(4, al_buffers);
                    if (alGetError() == AL_NO_ERROR) {
                        for (int i = 0; i < 4; i++) {
                            al_free_buffers[i] = al_buffers[i];
                        }
                        al_free_count = 4;
                        al_globally_initialized = true;
                    } else {
                        alDeleteSources(1, &al_source);
                        alcMakeContextCurrent(NULL);
                        alcDestroyContext(al_ctx);
                        al_ctx = NULL;
                        alcCloseDevice(al_dev);
                        al_dev = NULL;
                    }
                } else {
                    alcMakeContextCurrent(NULL);
                    alcDestroyContext(al_ctx);
                    al_ctx = NULL;
                    alcCloseDevice(al_dev);
                    al_dev = NULL;
                }
            } else {
                if (al_ctx) {
                    alcDestroyContext(al_ctx);
                    al_ctx = NULL;
                }
                alcCloseDevice(al_dev);
                al_dev = NULL;
            }
        }
    }

    /* 2. Mode A: 5.1 Surround Sound LPCM Scraping */
    if (d->is_5_1_active && al_globally_initialized) {
        /* Unqueue processed buffers */
        ALint processed = 0;
        alGetSourcei(al_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed > 0) {
            ALuint unqueued = 0;
            alSourceUnqueueBuffers(al_source, 1, &unqueued);
            if (alGetError() == AL_NO_ERROR && unqueued != 0) {
                if (al_free_count < 4) {
                    al_free_buffers[al_free_count++] = unqueued;
                }
            }
            processed--;
        }

        /* If source was stopped with queued buffers, reclaim them */
        if (al_free_count == 0) {
            ALint state = 0;
            alGetSourcei(al_source, AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED) {
                ALint queued = 0;
                alGetSourcei(al_source, AL_BUFFERS_QUEUED, &queued);
                while (queued > 0) {
                    ALuint unqueued = 0;
                    alSourceUnqueueBuffers(al_source, 1, &unqueued);
                    if (unqueued != 0 && al_free_count < 4) {
                        al_free_buffers[al_free_count++] = unqueued;
                    }
                    queued--;
                }
            }
        }

        /* Load new 5.1 PCM data into a free buffer and queue it */
        if (al_free_count > 0) {
            ALuint target_buf = al_free_buffers[--al_free_count];
            alBufferData(target_buf, AL_FORMAT_51CHN16, d->monitor.surround_buf,
                         sizeof(d->monitor.surround_buf), 48000);
            alSourceQueueBuffers(al_source, 1, &target_buf);
        }

        /* Set volume / gain */
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        alSourcef(al_source, AL_GAIN, vu);

        /* Handle buffer starvation / underrun: restart playback if stopped */
        ALint state = 0;
        alGetSourcei(al_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) {
            ALint queued = 0;
            alGetSourcei(al_source, AL_BUFFERS_QUEUED, &queued);
            if (queued > 0) {
                alSourcePlay(al_source);
            }
        }
    } else {
        /* Stop 5.1 OpenAL source if it was playing */
        if (al_globally_initialized) {
            ALint state = 0;
            alGetSourcei(al_source, AL_SOURCE_STATE, &state);
            if (state == AL_PLAYING) {
                alSourceStop(al_source);
            }
        }

        /* Stereo fallback through standard SDL audio stream */
        if (d->monitor.stream) {
            float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
            SDL_SetAudioStreamGain(d->monitor.stream, vu);
            SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                                   sizeof(d->monitor.frame_buf));
        }
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    memset(d->monitor.surround_buf, 0, sizeof(d->monitor.surround_buf));
}

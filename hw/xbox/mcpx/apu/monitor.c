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
#include <AL/al.h>
#include <AL/alc.h>
#ifdef __has_include
#if __has_include(<AL/alext.h>)
#include <AL/alext.h>
#endif
#endif
#ifndef AL_FORMAT_51CHN16
#define AL_FORMAT_51CHN16 0x120B
#endif

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

    /* Initialize OpenAL 5.1 backend */
    d->monitor.al.initialized = false;
    ALCdevice *al_dev = alcOpenDevice(NULL);
    if (al_dev) {
        ALCcontext *al_ctx = alcCreateContext(al_dev, NULL);
        if (al_ctx && alcMakeContextCurrent(al_ctx)) {
            ALuint al_src = 0;
            alGenSources(1, &al_src);
            if (alGetError() == AL_NO_ERROR) {
                ALuint al_bufs[4] = { 0 };
                alGenBuffers(4, al_bufs);
                if (alGetError() == AL_NO_ERROR) {
                    d->monitor.al.device = al_dev;
                    d->monitor.al.context = al_ctx;
                    d->monitor.al.source = al_src;
                    for (int i = 0; i < 4; i++) {
                        d->monitor.al.buffers[i] = al_bufs[i];
                        d->monitor.al.free_buffers[i] = al_bufs[i];
                    }
                    d->monitor.al.free_count = 4;
                    d->monitor.al.initialized = true;
                } else {
                    alDeleteSources(1, &al_src);
                    alcMakeContextCurrent(NULL);
                    alcDestroyContext(al_ctx);
                    alcCloseDevice(al_dev);
                }
            } else {
                alcMakeContextCurrent(NULL);
                alcDestroyContext(al_ctx);
                alcCloseDevice(al_dev);
            }
        } else {
            if (al_ctx) {
                alcDestroyContext(al_ctx);
            }
            alcCloseDevice(al_dev);
        }
    }
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
        d->monitor.stream = NULL;
    }

    if (d->monitor.al.initialized) {
        ALCcontext *ctx = (ALCcontext *)d->monitor.al.context;
        ALCdevice *dev = (ALCdevice *)d->monitor.al.device;
        if (ctx) {
            alcMakeContextCurrent(ctx);
            alSourceStop(d->monitor.al.source);
            alSourcei(d->monitor.al.source, AL_BUFFER, 0);

            ALuint bufs[4];
            for (int i = 0; i < 4; i++) {
                bufs[i] = d->monitor.al.buffers[i];
            }
            alDeleteBuffers(4, bufs);
            alDeleteSources(1, &d->monitor.al.source);

            alcMakeContextCurrent(NULL);
            alcDestroyContext(ctx);
        }
        if (dev) {
            alcCloseDevice(dev);
        }
        d->monitor.al.initialized = false;
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (d->is_5_1_active && d->monitor.al.initialized) {
        ALCcontext *ctx = (ALCcontext *)d->monitor.al.context;
        if (alcGetCurrentContext() != ctx) {
            alcMakeContextCurrent(ctx);
        }

        ALuint src = d->monitor.al.source;

        /* Unqueue processed OpenAL buffers */
        ALint processed = 0;
        alGetSourcei(src, AL_BUFFERS_PROCESSED, &processed);
        while (processed > 0) {
            ALuint unqueued = 0;
            alSourceUnqueueBuffers(src, 1, &unqueued);
            if (alGetError() == AL_NO_ERROR && unqueued != 0) {
                if (d->monitor.al.free_count < 4) {
                    d->monitor.al.free_buffers[d->monitor.al.free_count++] = unqueued;
                }
            }
            processed--;
        }

        /* Load new 5.1 PCM data into a free buffer and queue it */
        if (d->monitor.al.free_count > 0) {
            ALuint target_buf = d->monitor.al.free_buffers[--d->monitor.al.free_count];
            alBufferData(target_buf, AL_FORMAT_51CHN16, d->monitor.surround_buf,
                         sizeof(d->monitor.surround_buf), 48000);
            alSourceQueueBuffers(src, 1, &target_buf);
        }

        /* Set volume / gain */
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        alSourcef(src, AL_GAIN, vu);

        /* Handle buffer starvation / underrun: restart playback if stopped */
        ALint state = 0;
        alGetSourcei(src, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) {
            ALint queued = 0;
            alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
            if (queued > 0) {
                alSourcePlay(src);
            }
        }
    } else {
        if (d->monitor.al.initialized) {
            ALint state = 0;
            alGetSourcei(d->monitor.al.source, AL_SOURCE_STATE, &state);
            if (state == AL_PLAYING) {
                alSourceStop(d->monitor.al.source);
            }
        }

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

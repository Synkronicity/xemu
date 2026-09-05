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

static SDL_AudioStream *surround_stream = NULL;
float ext_surround_buf[6][256];
int ext_surround_idx = 0;

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
    int frame_bytes = 256 * 6 * sizeof(int16_t);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);

    SDL_AudioSpec surr_spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 6,
    };
    surround_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &surr_spec, NULL, NULL);
    if (surround_stream) {
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(surround_stream));
    }
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (surround_stream) {
        SDL_DestroyAudioStream(surround_stream);
        surround_stream = NULL;
    }
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (d->is_5_1_active && surround_stream) {
        int16_t interleaved[6 * 256];
        for (int i = 0; i < 256; i++) {
            float fl  = ext_surround_buf[0][i] * 0.25f;
            float fr  = ext_surround_buf[1][i] * 0.25f;
            float fc  = ext_surround_buf[2][i] * 0.25f;
            float lfe = ext_surround_buf[3][i] * 0.25f;
            float rl  = ext_surround_buf[4][i] * 0.25f;
            float rr  = ext_surround_buf[5][i] * 0.25f;

            interleaved[i * 6 + 0] = (int16_t)MAX(-32768.0f, MIN(32767.0f, fl * 32767.0f));
            interleaved[i * 6 + 1] = (int16_t)MAX(-32768.0f, MIN(32767.0f, fr * 32767.0f));
            interleaved[i * 6 + 2] = (int16_t)MAX(-32768.0f, MIN(32767.0f, fc * 32767.0f));
            interleaved[i * 6 + 3] = (int16_t)MAX(-32768.0f, MIN(32767.0f, lfe * 32767.0f));
            interleaved[i * 6 + 4] = (int16_t)MAX(-32768.0f, MIN(32767.0f, rl * 32767.0f));
            interleaved[i * 6 + 5] = (int16_t)MAX(-32768.0f, MIN(32767.0f, rr * 32767.0f));
        }

        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        SDL_SetAudioStreamGain(surround_stream, vu);
        SDL_PutAudioStreamData(surround_stream, interleaved, sizeof(interleaved));
        ext_surround_idx = 0;

        /* Mute the primary throttle stream */
        if (d->monitor.stream) {
            SDL_SetAudioStreamGain(d->monitor.stream, 0.0f);
            SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                                   sizeof(d->monitor.frame_buf));
        }
    } else {
        if (surround_stream) {
            SDL_SetAudioStreamGain(surround_stream, 0.0f);
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

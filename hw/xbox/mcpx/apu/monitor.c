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

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = 6,
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
    int frame_bytes = NUM_SAMPLES_PER_FRAME * 6 * sizeof(int16_t);
    int drain = MAX(dev_drain_bytes, frame_bytes);
    d->monitor.queued_bytes_low = drain;
    d->monitor.queued_bytes_high = 3 * drain;

    SDL_ResumeAudioDevice(dev);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    if (d->monitor.stream) {
        SDL_DestroyAudioStream(d->monitor.stream);
    }
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    int16_t interleaved[6 * NUM_SAMPLES_PER_FRAME];

    if (d->is_5_1_active) {
        int16_t *planar = (int16_t *)d->monitor.surround_buf;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            interleaved[i * 6 + 0] = planar[0 * NUM_SAMPLES_PER_FRAME + i]; // FL
            interleaved[i * 6 + 1] = planar[1 * NUM_SAMPLES_PER_FRAME + i]; // FR
            interleaved[i * 6 + 2] = planar[2 * NUM_SAMPLES_PER_FRAME + i]; // FC
            interleaved[i * 6 + 3] = planar[3 * NUM_SAMPLES_PER_FRAME + i]; // LFE
            interleaved[i * 6 + 4] = planar[4 * NUM_SAMPLES_PER_FRAME + i]; // RL
            interleaved[i * 6 + 5] = planar[5 * NUM_SAMPLES_PER_FRAME + i]; // RR
        }
    } else {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            interleaved[i * 6 + 0] = d->monitor.frame_buf[i][0]; // FL
            interleaved[i * 6 + 1] = d->monitor.frame_buf[i][1]; // FR
            interleaved[i * 6 + 2] = 0;                          // FC
            interleaved[i * 6 + 3] = 0;                          // LFE
            interleaved[i * 6 + 4] = 0;                          // RL
            interleaved[i * 6 + 5] = 0;                          // RR
        }
    }

    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        SDL_PutAudioStreamData(d->monitor.stream, interleaved,
                               sizeof(interleaved));
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    memset(d->monitor.surround_buf, 0, sizeof(d->monitor.surround_buf));
}

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

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    bool eeprom_wants_surround = false;
    uint32_t audio_flags = 0;
    const char *eeprom_path = g_config.sys.files.eeprom_path;
    if (!eeprom_path || !eeprom_path[0]) {
        eeprom_path = xemu_settings_get_default_eeprom_path();
    }
    if (eeprom_path) {
        FILE *fp = fopen(eeprom_path, "rb");
        if (fp) {
            /* User section starts at offset 0x64 in Xbox EEPROM; audio flags at offset 0x2C (file offset 0x90) */
            if (fseek(fp, 0x64 + 0x2C, SEEK_SET) == 0 &&
                fread(&audio_flags, sizeof(audio_flags), 1, fp) == 1) {
                audio_flags = le32_to_cpu(audio_flags);
                if ((audio_flags & 0x00010000) || (audio_flags & 0x00020000) ||
                    ((audio_flags & 0xFFFF) == 2)) {
                    eeprom_wants_surround = true;
                }
            }
            fclose(fp);
        }
    }

    const char *env_surround = getenv("XEMU_SURROUND");
    bool surround_requested = eeprom_wants_surround;
    if (env_surround && (strcmp(env_surround, "1") == 0 || strcasecmp(env_surround, "true") == 0)) {
        surround_requested = true;
    }

    fprintf(stderr,
            "[APU MONITOR] EEPROM audio flags: 0x%08X (Surround requested: %s)\n",
            audio_flags, surround_requested ? "yes" : "no");

    /* Firmware-gated surround activation: verify EP P-RAM is populated */
    uint32_t reset_vec = dsp_read_memory(d->ep.dsp, 'P', 0x0000) & 0x00FFFFFF;
    if (reset_vec == 0 || reset_vec == 0x00CACACA) {
        dsp_bootstrap_ep_firmware(d->ep.dsp);
        reset_vec = dsp_read_memory(d->ep.dsp, 'P', 0x0000) & 0x00FFFFFF;
    }

    bool fw_present = (reset_vec != 0 && reset_vec != 0x00CACACA);
    d->is_5_1_active = (surround_requested && fw_present);

    fprintf(stderr, "[APU MONITOR] Audio mode: %s (%d channels)%s\n",
            d->is_5_1_active ? "5.1 Surround" : "Stereo",
            d->is_5_1_active ? 6 : 2,
            (!fw_present && surround_requested) ? " [Fallback: EP firmware missing]" : "");

    SDL_AudioSpec spec = {
        .freq = 48000,
        .format = SDL_AUDIO_S16LE,
        .channels = d->is_5_1_active ? 6 : 2,
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
    int frame_bytes = d->is_5_1_active ? sizeof(d->monitor.surround_buf)
                                       : sizeof(d->monitor.frame_buf);
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

    if (d->monitor.stream) {
        float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
        SDL_SetAudioStreamGain(d->monitor.stream, vu);
        if (d->is_5_1_active) {
            SDL_PutAudioStreamData(d->monitor.stream, d->monitor.surround_buf,
                                   sizeof(d->monitor.surround_buf));
        } else {
            SDL_PutAudioStreamData(d->monitor.stream, d->monitor.frame_buf,
                                   sizeof(d->monitor.frame_buf));
        }
    }

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    memset(d->monitor.surround_buf, 0, sizeof(d->monitor.surround_buf));
}

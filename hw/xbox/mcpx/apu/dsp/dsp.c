/*
 * MCPX DSP emulator
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Adapted from Hatari DSP M56001 emulation
 * (C) 2001-2008 ARAnyM developer team
 * Adaption to Hatari (C) 2008 by Thomas Huth
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "dsp_dma.h"
#include "dsp.h"
#include "dsp_internal.h"
#include "trace.h"
#include "ui/xemu-settings.h"

/* Defines */
#define BITMASK(x) ((1 << (x)) - 1)

#define INTERRUPT_ABORT_FRAME (1 << 0)
#define INTERRUPT_START_FRAME (1 << 1)
#define INTERRUPT_DMA_EOL (1 << 7)

static int g_gp_frame_count = 0;

/*
 * Shared peripheral I/O helpers, used by both backends via callbacks.
 */

uint32_t read_peripheral(DSPState *dsp, uint32_t address)
{
    uint32_t v = 0xababa;
    switch (address) {
    case 0xFFFFB3:
        v = 0; // core->num_inst; // ??
        break;
    case DSP_REG_PERIPH_HCR: /* 0xFFFFC2 */
        v = dsp->hcr;
        break;
    case DSP_REG_PERIPH_HSR: /* 0xFFFFC3 */
        v = dsp->hsr;
        break;
    case DSP_REG_PERIPH_HPCR: /* 0xFFFFC4 */
        v = dsp->hpcr;
        break;
    case DSP_REG_PERIPH_HBAR: /* 0xFFFFC5 */
        v = dsp->hbar;
        if (dsp->is_gp) {
            v |= dsp->interrupts;
            if (dsp->dma.eol) {
                v |= INTERRUPT_DMA_EOL;
            }
        }
        break;
    case DSP_REG_PERIPH_HORX: /* 0xFFFFC6 */
        v = dsp->horx;
        dsp->hsr &= ~DSP_HSR_HRDF;
        break;
    case DSP_REG_PERIPH_HOTX: /* 0xFFFFC7 */
        v = dsp->hotx;
        break;
    case DSP_REG_PERIPH_HDDR: /* 0xFFFFC8 */
    case DSP_REG_PERIPH_HDR:  /* 0xFFFFC9 */
        v = 0;
        break;
    case 0xFFFFD4:
        v = dsp_dma_read(&dsp->dma, DMA_NEXT_BLOCK);
        break;
    case 0xFFFFD5:
        v = dsp_dma_read(&dsp->dma, DMA_START_BLOCK);
        break;
    case 0xFFFFD6:
        v = dsp_dma_read(&dsp->dma, DMA_CONTROL);
        break;
    case 0xFFFFD7:
        v = dsp_dma_read(&dsp->dma, DMA_CONFIGURATION);
        break;
    }

    trace_dsp_read_peripheral(address, v);
    return v;
}

void write_peripheral(DSPState *dsp, uint32_t address, uint32_t value)
{
    switch (address) {
    case DSP_REG_PERIPH_HCR: /* 0xFFFFC2 */
        dsp->hcr = value & 0x00FFFFFF;
        break;
    case DSP_REG_PERIPH_HPCR: /* 0xFFFFC4 */
        dsp->hpcr = value;
        if (dsp->is_gp && (value & 1)) {
            dsp_set_halt_requested(dsp, true);
        }
        break;
    case DSP_REG_PERIPH_HBAR: /* 0xFFFFC5 */
        dsp->hbar = value;
        if (dsp->is_gp) {
            dsp->interrupts &= ~value;
            if (value & INTERRUPT_DMA_EOL) {
                dsp->dma.eol = false;
            }
        }
        break;
    case DSP_REG_PERIPH_HOTX: /* 0xFFFFC7 */
        dsp->hotx = value & 0x00FFFFFF;
        dsp->hsr &= ~DSP_HSR_HTDE;
        /* Host side automatically receives data, re-enabling transmit buffer empty */
        dsp->hsr |= DSP_HSR_HTDE;
        break;
    case DSP_REG_PERIPH_HDDR: /* 0xFFFFC8 */
    case DSP_REG_PERIPH_HDR:  /* 0xFFFFC9 */
        break;
    case 0xFFFFD4:
        dsp_dma_write(&dsp->dma, DMA_NEXT_BLOCK, value);
        break;
    case 0xFFFFD5:
        dsp_dma_write(&dsp->dma, DMA_START_BLOCK, value);
        break;
    case 0xFFFFD6:
        dsp_dma_write(&dsp->dma, DMA_CONTROL, value);
        break;
    case 0xFFFFD7:
        dsp_dma_write(&dsp->dma, DMA_CONFIGURATION, value);
        break;
    }

    trace_dsp_write_peripheral(address, value);
}

void dsp_start_frame_impl(DSPState *dsp)
{
    dsp->interrupts |= INTERRUPT_START_FRAME;
    dsp->hsr |= DSP_HSR_HRDF;
    if (dsp->hcr & DSP_HCR_HRIE) {
        dsp->interrupts |= (1 << 2);
    }
    if (dsp->hcr & DSP_HCR_HCIE) {
        dsp->interrupts |= (1 << 3);
    }
}

void dsp_host_write_horx(DSPState *dsp, uint32_t value)
{
    dsp->horx = value & 0x00FFFFFF;
    dsp->hsr |= DSP_HSR_HRDF;
}

uint32_t dsp_host_read_hotx(DSPState *dsp)
{
    dsp->hsr |= DSP_HSR_HTDE;
    return dsp->hotx;
}

DSPState *dsp_init(void *rw_opaque, dsp_scratch_rw_func scratch_rw,
                   dsp_fifo_rw_func fifo_rw, bool is_gp)
{
    DSPState *dsp = g_new0(DSPState, 1);
    dsp->is_gp = is_gp;
    dsp->core.is_gp = is_gp;

    dsp->dma.rw_opaque = rw_opaque;
    dsp->dma.scratch_rw = scratch_rw;
    dsp->dma.fifo_rw = fifo_rw;

    if (g_config.audio.use_dsp_jit) {
        dsp_jit_init(dsp);
    } else {
        dsp_c_init(dsp);
    }

    dsp_reset(dsp);

    return dsp;
}

void dsp_destroy(DSPState *dsp)
{
    dsp->ops->finalize(dsp);
    g_free(dsp);
}

void dsp_reset(DSPState *dsp)
{
    dsp->hcr = 0;
    dsp->hsr = DSP_HSR_HTDE;
    dsp->hpcr = 0;
    dsp->hbar = 0x80;
    dsp->horx = 0;
    dsp->hotx = 0;
    dsp->interrupts = 0;
    dsp->ops->reset(dsp);
}

void dsp_step(DSPState *dsp)
{
    dsp->ops->step(dsp);
}

void dsp_run(DSPState *dsp, int cycles)
{
    dsp->ops->run(dsp, cycles);
}

bool dsp_bootstrap_ep_firmware(DSPState *dsp)
{
    if (getenv("XEMU_NO_BYOF") != NULL) {
        return false;
    }

    FILE *f = NULL;
    const char *found_path = NULL;

    if (g_config.sys.files.ep_rom_path && g_config.sys.files.ep_rom_path[0] != '\0') {
        f = fopen(g_config.sys.files.ep_rom_path, "rb");
        if (f) {
            found_path = g_config.sys.files.ep_rom_path;
        } else {
            fprintf(stderr, "[APU EP] Warning: Failed to open configured EP ROM at '%s'\n",
                    g_config.sys.files.ep_rom_path);
        }
    }

    if (!f) {
        const char *candidates[] = {
            "./dolby_ep.bin",
            "./tools/dolby_ep.bin",
            "../tools/dolby_ep.bin",
            NULL
        };

        for (int i = 0; candidates[i] != NULL; i++) {
            f = fopen(candidates[i], "rb");
            if (f) {
                found_path = candidates[i];
                break;
            }
        }
    }

    if (!f) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[APU EP] Notice: dolby_ep.bin not found, running without EP firmware\n");
            warned = true;
        }
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "[APU EP] Warning: %s is empty\n", found_path);
        fclose(f);
        return false;
    }

    size_t total_words = file_size / 4;
    if (total_words < (0xC8 + 0x17F)) {
        fprintf(stderr, "[APU EP] Warning: %s is too small (%zu words, expected >= %d)\n",
                found_path, total_words, 0xC8 + 0x17F);
        fclose(f);
        return false;
    }

    uint32_t *buf = g_malloc(total_words * sizeof(uint32_t));
    if (fread(buf, sizeof(uint32_t), total_words, f) != total_words) {
        fprintf(stderr, "[APU EP] Warning: Failed reading %s\n", found_path);
        g_free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    size_t idx = 0;

    /* Segment 1: 0x0000 (0xC8 words) */
    for (size_t i = 0; i < 0xC8 && idx < total_words; i++, idx++) {
        uint32_t word = le32_to_cpu(buf[idx]);
        dsp_write_memory(dsp, 'P', 0x0000 + i, word & 0x00FFFFFF);
    }

    /* Segment 2: 0x0180 (0x17F words) */
    for (size_t i = 0; i < 0x17F && idx < total_words; i++, idx++) {
        uint32_t word = le32_to_cpu(buf[idx]);
        dsp_write_memory(dsp, 'P', 0x0180 + i, word & 0x00FFFFFF);
    }

    /* Segment 3: 0x0300 (remainder) */
    for (size_t i = 0; idx < total_words; i++, idx++) {
        uint32_t word = le32_to_cpu(buf[idx]);
        dsp_write_memory(dsp, 'P', 0x0300 + i, word & 0x00FFFFFF);
    }

    dsp_invalidate_opcache(dsp);
    g_free(buf);

    fprintf(stderr, "[APU EP] Loaded %zu firmware words from %s into EP P-RAM\n",
            total_words, found_path);
    return true;
}

void dsp_bootstrap(DSPState *dsp)
{
    if (!dsp->is_gp) {
        dsp_bootstrap_ep_firmware(dsp);
    } else {
        dsp->ops->bootstrap(dsp);
    }
}

void dsp_start_frame(DSPState *dsp)
{
    if (dsp->is_gp) {
        g_gp_frame_count++;
    }
    dsp->ops->start_frame(dsp);
}

uint32_t dsp_read_memory(DSPState *dsp, char space, uint32_t address)
{
    return dsp->ops->read_memory(dsp, space, address);
}

void dsp_write_memory(DSPState *dsp, char space, uint32_t address,
                      uint32_t value)
{
    dsp->ops->write_memory(dsp, space, address, value);
}

bool dsp_get_halt_requested(DSPState *dsp)
{
    return dsp->ops->get_halt_requested(dsp);
}

void dsp_set_halt_requested(DSPState *dsp, bool idle)
{
    dsp->ops->set_halt_requested(dsp, idle);
}

uint32_t dsp_get_cycle_count(DSPState *dsp)
{
    return dsp->ops->get_cycle_count(dsp);
}

void dsp_set_cycle_count(DSPState *dsp, uint32_t count)
{
    dsp->ops->set_cycle_count(dsp, count);
}

uint32_t dsp_get_pc(DSPState *dsp)
{
    return dsp->ops->get_pc ? dsp->ops->get_pc(dsp) : 0;
}

void dsp_invalidate_opcache(DSPState *dsp)
{
    dsp->ops->invalidate_opcache(dsp);
}

void dsp_sync_to_vm(DSPState *dsp)
{
    dsp->ops->sync_to_vm(dsp);
}

void dsp_sync_from_vm(DSPState *dsp)
{
    dsp->ops->sync_from_vm(dsp);
}

void dsp_set_engine(DSPState *dsp, bool use_jit)
{
    bool currently_jit = (dsp->ops == &jit_dsp_ops);
    if (use_jit == currently_jit) {
        return;
    }

    dsp_sync_to_vm(dsp);
    dsp->ops->finalize(dsp);

    if (use_jit) {
        dsp_jit_init(dsp);
    } else {
        dsp_c_init(dsp);
    }

    dsp_sync_from_vm(dsp);
}

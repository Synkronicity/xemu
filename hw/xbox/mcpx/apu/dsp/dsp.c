/*
 * MCPX DSP emulator
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2025 Matt Borgerson
 * Copyright (c) 2026 Will Bonnett
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
#include "interp/dsp_cpu.h"
#include "interp/dsp_cpu_regs.h"
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

static uint32_t c_dma_mem_read(void *opaque, int space, uint32_t addr)
{
    return dsp56k_read_memory((dsp_core_t *)opaque, space, addr);
}

static void c_dma_mem_write(void *opaque, int space, uint32_t addr,
                            uint32_t value)
{
    dsp56k_write_memory((dsp_core_t *)opaque, space, addr, value);
}

static uint32_t c_read_peripheral(dsp_core_t *core, uint32_t address)
{
    return read_peripheral((DSPState *)core->opaque, address);
}

static void c_write_peripheral(dsp_core_t *core, uint32_t address,
                               uint32_t value)
{
    write_peripheral((DSPState *)core->opaque, address, value);
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

    dsp_core_t *core = g_new0(dsp_core_t, 1);
    core->opaque = dsp;
    core->is_gp = is_gp;
    core->read_peripheral = c_read_peripheral;
    core->write_peripheral = c_write_peripheral;

    dsp->c_core = core;

    dsp->dma.mem_opaque = core;
    dsp->dma.mem_read = c_dma_mem_read;
    dsp->dma.mem_write = c_dma_mem_write;

    /* Ensure the interpreter's opcode decoder tables are initialized.
     * dsp56k_reset_cpu populates the static nonparallel_matches[] array
     * on first call. */
    dsp56k_reset_cpu(core);

    memset(core->pram, 0xCA, DSP_PRAM_SIZE * sizeof(uint32_t));
    memset(core->xram, 0xCA, DSP_XRAM_SIZE * sizeof(uint32_t));
    memset(core->yram, 0xCA, DSP_YRAM_SIZE * sizeof(uint32_t));
    dsp_invalidate_opcache(dsp);

    dsp_reset(dsp);

    return dsp;
}

void dsp_destroy(DSPState *dsp)
{
    g_free(dsp->c_core);
    dsp->c_core = NULL;
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
    dsp->save_cycles = 0;
    dsp56k_reset_cpu(dsp->c_core);
}

void dsp_step(DSPState *dsp)
{
    dsp56k_execute_instruction(dsp->c_core);
}

void dsp_run(DSPState *dsp, int cycles)
{
    dsp_core_t *core = dsp->c_core;

    dsp->save_cycles += cycles;

    if (dsp->save_cycles <= 0) {
        return;
    }

    while (dsp->save_cycles > 0) {
        dsp56k_execute_instruction(core);
        dsp->save_cycles -= core->instr_cycle;
        core->cycle_count += core->instr_cycle;

        if (core->is_idle) {
            break;
        }
    }
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
        dsp_core_t *core = dsp->c_core;

        // scratch memory is dma'd in to pram by the bootrom
        dsp->dma.scratch_rw(dsp->dma.rw_opaque, (uint8_t *)core->pram, 0, 0x800 * 4,
                            false);
        for (int i = 0; i < 0x800; i++) {
            if (core->pram[i] & 0xff000000) {
                core->pram[i] &= 0x00ffffff;
            }
        }
        memset(core->pram_opcache, 0, sizeof(core->pram_opcache));
    }
}

void dsp_start_frame(DSPState *dsp)
{
    if (dsp->is_gp) {
        g_gp_frame_count++;
    }
    dsp_start_frame_impl(dsp);
}

uint32_t dsp_read_memory(DSPState *dsp, char space, uint32_t address)
{
    int space_id;

    switch (space) {
    case 'X':
        space_id = DSP_SPACE_X;
        break;
    case 'Y':
        space_id = DSP_SPACE_Y;
        break;
    case 'P':
        space_id = DSP_SPACE_P;
        break;
    default:
        assert(!"Invalid dsp space when reading from memory");
        return 0;
    }

    return dsp56k_read_memory(dsp->c_core, space_id, address);
}

void dsp_write_memory(DSPState *dsp, char space, uint32_t address,
                      uint32_t value)
{
    int space_id;

    switch (space) {
    case 'X':
        space_id = DSP_SPACE_X;
        break;
    case 'Y':
        space_id = DSP_SPACE_Y;
        break;
    case 'P':
        space_id = DSP_SPACE_P;
        break;
    default:
        assert(!"Invalid dsp space when writing to memory");
        return;
    }

    dsp56k_write_memory(dsp->c_core, space_id, address, value);
}

bool dsp_get_halt_requested(DSPState *dsp)
{
    return dsp->c_core->is_idle;
}

void dsp_set_halt_requested(DSPState *dsp, bool idle)
{
    dsp->c_core->is_idle = idle;
}

uint32_t dsp_get_cycle_count(DSPState *dsp)
{
    return dsp->c_core->cycle_count;
}

void dsp_set_cycle_count(DSPState *dsp, uint32_t count)
{
    dsp->c_core->cycle_count = count;
}

uint32_t dsp_get_pc(DSPState *dsp)
{
    return dsp->c_core->pc;
}

void dsp_invalidate_opcache(DSPState *dsp)
{
    memset(dsp->c_core->pram_opcache, 0, sizeof(dsp->c_core->pram_opcache));
}

void dsp_sync_to_vm(DSPState *dsp)
{
    dsp_core_t *core = dsp->c_core;
    DspCoreState *vm = &dsp->core;

    vm->pc = core->pc;
    vm->cycle_count = core->cycle_count;
    vm->instr_cycle = core->instr_cycle;
    vm->halt_requested = core->is_idle;
    vm->is_gp = core->is_gp;
    vm->loop_rep = core->loop_rep;
    vm->pc_on_rep = core->pc_on_rep;
    vm->cur_inst = core->cur_inst;
    vm->cur_inst_len = core->cur_inst_len;

    memcpy(vm->registers, core->registers, sizeof(vm->registers));
    memcpy(vm->stack, core->stack, sizeof(vm->stack));
    memcpy(vm->xram, core->xram, sizeof(vm->xram));
    memcpy(vm->yram, core->yram, sizeof(vm->yram));
    memcpy(vm->pram, core->pram, sizeof(vm->pram));
    memcpy(vm->mixbuffer, core->mixbuffer, sizeof(vm->mixbuffer));
    memcpy(vm->periph, core->periph, sizeof(vm->periph));

    vm->interrupt_state = core->interrupt_state;
    vm->interrupt_instr_fetch = core->interrupt_instr_fetch;
    vm->interrupt_save_pc = core->interrupt_save_pc;
    vm->interrupt_counter = core->interrupt_counter;
    vm->interrupt_ipl_to_raise = core->interrupt_ipl_to_raise;
    vm->interrupt_pipeline_count = core->interrupt_pipeline_count;
    memcpy(vm->interrupt_ipl, core->interrupt_ipl, sizeof(core->interrupt_ipl));
    memcpy(vm->interrupt_is_pending, core->interrupt_is_pending,
           sizeof(core->interrupt_is_pending));
}

void dsp_sync_from_vm(DSPState *dsp)
{
    dsp_core_t *core = dsp->c_core;
    DspCoreState *vm = &dsp->core;

    core->pc = vm->pc;
    core->cycle_count = vm->cycle_count;
    core->instr_cycle = vm->instr_cycle;
    core->is_idle = vm->halt_requested;
    core->is_gp = vm->is_gp;
    core->loop_rep = vm->loop_rep;
    core->pc_on_rep = vm->pc_on_rep;
    core->cur_inst = vm->cur_inst;
    core->cur_inst_len = vm->cur_inst_len;

    memcpy(core->registers, vm->registers, sizeof(vm->registers));
    memcpy(core->stack, vm->stack, sizeof(vm->stack));
    memcpy(core->xram, vm->xram, sizeof(vm->xram));
    memcpy(core->yram, vm->yram, sizeof(vm->yram));
    memcpy(core->pram, vm->pram, sizeof(vm->pram));
    memcpy(core->mixbuffer, vm->mixbuffer, sizeof(vm->mixbuffer));
    memcpy(core->periph, vm->periph, sizeof(vm->periph));

    core->interrupt_state = vm->interrupt_state;
    core->interrupt_instr_fetch = vm->interrupt_instr_fetch;
    core->interrupt_save_pc = vm->interrupt_save_pc;
    core->interrupt_counter = vm->interrupt_counter;
    core->interrupt_ipl_to_raise = vm->interrupt_ipl_to_raise;
    core->interrupt_pipeline_count = vm->interrupt_pipeline_count;
    memcpy(core->interrupt_ipl, vm->interrupt_ipl, sizeof(core->interrupt_ipl));
    memcpy(core->interrupt_is_pending, vm->interrupt_is_pending,
           sizeof(core->interrupt_is_pending));

    memset(core->pram_opcache, 0, sizeof(core->pram_opcache));
}

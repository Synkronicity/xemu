/*
 * MCPX DSP emulator
 *
 * Copyright (c) 2015 espes
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

#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stdbool.h>

#include "interp/dsp_cpu_regs.h"
#include "dsp_dma.h"

typedef struct DSPState DSPState;
typedef struct dsp_core_s dsp_core_t;

/*
 * Shared VM state for save/load and backend synchronization.
 * The C interpreter core syncs to/from this struct for QEMU VMState snapshots.
 */
typedef struct DspCoreState {
    uint32_t pc;
    uint32_t registers[DSP_REG_MAX];
    uint32_t stack[2][16];

    uint32_t xram[DSP_XRAM_SIZE];
    uint32_t yram[DSP_YRAM_SIZE];
    uint32_t pram[DSP_PRAM_SIZE];
    uint32_t mixbuffer[DSP_MIXBUFFER_SIZE];
    uint32_t periph[DSP_PERIPH_SIZE];

    uint32_t cycle_count;
    uint16_t instr_cycle;
    bool halt_requested;
    bool is_gp;

    uint32_t loop_rep;
    uint32_t pc_on_rep;
    uint32_t cur_inst;
    uint32_t cur_inst_len;

    uint16_t interrupt_state;
    uint16_t interrupt_instr_fetch;
    uint16_t interrupt_save_pc;
    uint16_t interrupt_counter;
    uint16_t interrupt_ipl_to_raise;
    uint16_t interrupt_pipeline_count;
    int16_t interrupt_ipl[12]; /* only [0..3] used; size 12 for snapshot compat */
    uint16_t interrupt_is_pending[12]; /* only [0..3] used; size 12 for snapshot compat */
} DspCoreState;

struct DSPState {
    dsp_core_t *c_core;

    DspCoreState core;
    DSPDMAState dma;
    int save_cycles;

    uint32_t interrupts;

    /* Motorola DSP56362 HDI08 Host Interface registers */
    uint32_t hcr;
    uint32_t hsr;
    uint32_t hpcr;
    uint32_t hbar;
    uint32_t horx;
    uint32_t hotx;

    bool is_gp;
};

DSPState *dsp_init(void *rw_opaque, dsp_scratch_rw_func scratch_rw,
                   dsp_fifo_rw_func fifo_rw, bool is_gp);
void dsp_destroy(DSPState *dsp);
void dsp_reset(DSPState *dsp);

void dsp_step(DSPState *dsp);
void dsp_run(DSPState *dsp, int cycles);

void dsp_bootstrap(DSPState *dsp);
bool dsp_bootstrap_ep_firmware(DSPState *dsp);
void dsp_start_frame(DSPState *dsp);

uint32_t dsp_read_memory(DSPState *dsp, char space, uint32_t addr);
void dsp_write_memory(DSPState *dsp, char space, uint32_t address,
                      uint32_t value);

/* HDI08 Host Interface accessors */
void dsp_host_write_horx(DSPState *dsp, uint32_t value);
uint32_t dsp_host_read_hotx(DSPState *dsp);

/* Accessor functions for backend-independent state access */
bool dsp_get_halt_requested(DSPState *dsp);
void dsp_set_halt_requested(DSPState *dsp, bool idle);
uint32_t dsp_get_cycle_count(DSPState *dsp);
void dsp_set_cycle_count(DSPState *dsp, uint32_t count);
uint32_t dsp_get_pc(DSPState *dsp);
void dsp_invalidate_opcache(DSPState *dsp);

/* Backend synchronization - sync backend state to/from DspCoreState */
void dsp_sync_to_vm(DSPState *dsp);
void dsp_sync_from_vm(DSPState *dsp);

#endif /* DSP_H */

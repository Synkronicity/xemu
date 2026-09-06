#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h"
#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h"

#define DEFAULT_FW_PATH "halo2_dolby.bin"
#define MAX_CYCLES      50000

static uint32_t dma_control_reg = 0;
static int dma_transfer_countdown = -1;
static uint32_t hi08_c5_reg = 0;

static uint32_t bench_read_peripheral(dsp_core_t *core, uint32_t address)
{
    printf("[PERIPH READ ] Address: 0x%06X (PC: 0x%06X)\n", address, core->pc);

    /* HI08 Host Status Register (0xFFFFC5):
     * Return the active backing state written by the DSP, but ensure Bit 1
     * (Host Acknowledge) is set after the initial bootstrap poll count.
     */
    if (address == 0xFFFFC5) {
        static int c5_poll_count = 0;
        c5_poll_count++;
        if (c5_poll_count > 8) {
            return hi08_c5_reg | 0x000002;
        }
        return hi08_c5_reg;
    }

    /* DMA Status / Control Register (0xFFFFD6) */
    if (address == 0xFFFFD6) {
        if (dma_transfer_countdown > 0) {
            dma_transfer_countdown--;
            return dma_control_reg & ~0x000010;
        } else if (dma_transfer_countdown == 0) {
            dma_transfer_countdown = -1;
            return dma_control_reg | 0x000010;
        }
        return dma_control_reg & ~0x000010;
    }

    /* ESSI0 Status Register (0xFFFFB3): Return Transmitter Empty / Ready */
    if (address == 0xFFFFB3) {
        return 0x00000C;
    }

    return 0x000000;
}

static void bench_write_peripheral(dsp_core_t *core, uint32_t address, uint32_t value)
{
    printf("[PERIPH WRITE] Address: 0x%06X -> Value: 0x%06X (PC: 0x%06X, Cycle: %u)\n",
           address, value & 0xFFFFFF, core->pc, core->cycle_count);

    if (address == 0xFFFFC5) {
        hi08_c5_reg = value & 0xFFFFFF;
    }

    if (address == 0xFFFFD6) {
        dma_control_reg = value & 0xFFFFFF;
        if (value & 0x01) {
            dma_transfer_countdown = 2;
        }
    }
}

int main(int argc, char *argv[])
{
    const char *fw_path = (argc > 1) ? argv[1] : DEFAULT_FW_PATH;

    printf("====================================================\n");
    printf("  XEMU DSP56362 Offline Bench Harness (Phase 52)    \n");
    printf("====================================================\n");
    printf("[*] Target Firmware: %s\n", fw_path);

    FILE *f = fopen(fw_path, "rb");
    if (!f && argc <= 1) {
        fw_path = "tools/halo2_dolby.bin";
        f = fopen(fw_path, "rb");
    }
    if (!f) {
        fprintf(stderr, "[-] FATAL: Cannot open firmware file '%s'\n", fw_path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fw_size = ftell(f);
    rewind(f);

    printf("[+] Firmware Size: %ld bytes (0x%lX)\n", fw_size, fw_size);

    uint8_t *buffer = malloc(fw_size);
    if (!buffer) {
        fprintf(stderr, "[-] FATAL: Buffer allocation failed\n");
        fclose(f);
        return 1;
    }

    if (fread(buffer, 1, fw_size, f) != (size_t)fw_size) {
        fprintf(stderr, "[-] FATAL: Failed to read complete firmware\n");
        free(buffer);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Print header inspection (first 16 DWORDs) */
    printf("[*] Initial 16 DWORDs in payload:\n");
    for (int i = 0; i < 16 && (i * 4) < fw_size; i++) {
        uint32_t dw = (uint32_t)buffer[i*4] |
                      ((uint32_t)buffer[i*4 + 1] << 8) |
                      ((uint32_t)buffer[i*4 + 2] << 16) |
                      ((uint32_t)buffer[i*4 + 3] << 24);
        printf("    [%02d] Offset 0x%04X: 0x%08X (24-bit word: 0x%06X)\n",
               i, i * 4, dw, dw & 0xFFFFFF);
    }

    /* Instantiate DSP core state */
    dsp_core_t core;
    memset(&core, 0, sizeof(core));
    core.is_gp = false; /* EP mode */

    /* Hook peripheral callbacks */
    core.read_peripheral = bench_read_peripheral;
    core.write_peripheral = bench_write_peripheral;

    /* Initialize CPU registers and opcode tables */
    dsp56k_reset_cpu(&core);
    printf("[+] DSP Core Reset Complete. Initial PC: 0x%06X\n", core.pc);

    /* Zero out all P-RAM first */
    for (uint32_t addr = 0; addr < DSP_PRAM_SIZE; addr++) {
        dsp56k_write_memory(&core, DSP_SPACE_P, addr, 0x000000);
    }

    /* Segment 1: Boot vectors & hardware init
     * Source: File offset 0x0000
     * Size: 0x320 bytes (200 words)
     * Target: P:0x0000
     */
    uint32_t seg1_words = 0x320 / 4;
    printf("[*] Loading Segment 1: 0x%X words to P:0x0000...\n", seg1_words);
    for (uint32_t i = 0; i < seg1_words; i++) {
        uint32_t word = (uint32_t)buffer[i * 4] |
                        ((uint32_t)buffer[i * 4 + 1] << 8) |
                        ((uint32_t)buffer[i * 4 + 2] << 16);
        dsp56k_write_memory(&core, DSP_SPACE_P, 0x0000 + i, word);
    }

    /* Segment 2: Main processing kernel
     * Source: File offset 0x0320
     * Size: 0x5FC bytes (383 words)
     * Target: P:0x0180 (byte offset 0x600)
     */
    uint32_t seg2_offset = 0x320;
    uint32_t seg2_words = 0x5FC / 4;
    printf("[*] Loading Segment 2: 0x%X words to P:0x0180...\n", seg2_words);
    for (uint32_t i = 0; i < seg2_words; i++) {
        uint32_t file_idx = seg2_offset + (i * 4);
        uint32_t word = (uint32_t)buffer[file_idx] |
                        ((uint32_t)buffer[file_idx + 1] << 8) |
                        ((uint32_t)buffer[file_idx + 2] << 16);
        dsp56k_write_memory(&core, DSP_SPACE_P, 0x0180 + i, word);
    }

    /* Remaining segments (contiguous starting from P:0x0300) */
    uint32_t tail_offset = 0x320 + 0x5FC; // 0x91C
    if (fw_size > tail_offset) {
        uint32_t tail_words = (fw_size - tail_offset) / 4;
        printf("[*] Loading Tail Segments: 0x%X words to P:0x0300...\n", tail_words);
        for (uint32_t i = 0; i < tail_words; i++) {
            if ((0x0300 + i) >= DSP_PRAM_SIZE) break;
            uint32_t file_idx = tail_offset + (i * 4);
            uint32_t word = (uint32_t)buffer[file_idx] |
                            ((uint32_t)buffer[file_idx + 1] << 8) |
                            ((uint32_t)buffer[file_idx + 2] << 16);
            dsp56k_write_memory(&core, DSP_SPACE_P, 0x0300 + i, word);
        }
    }
    free(buffer);

    /* Preamble opcode peek: inspect bootstrap vector and jump instructions */
    printf("[*] Preamble P-RAM Peek (PC 0x000000 - 0x000025):\n");
    for (uint32_t p = 0; p <= 0x000025; p++) {
        printf("    P:0x%06X = 0x%06X\n", p, dsp56k_read_memory(&core, DSP_SPACE_P, p));
    }
    printf("[*] Segment 2 Entry Peek (PC 0x000180 - 0x000190):\n");
    for (uint32_t p = 0x000180; p <= 0x000190; p++) {
        printf("    P:0x%06X = 0x%06X\n", p, dsp56k_read_memory(&core, DSP_SPACE_P, p));
    }

    /* Execute controlled step loop */
    printf("[*] Beginning execution loop (Limit: %u cycles)...\n", MAX_CYCLES);

    uint32_t prev_pc = core.pc;
    uint32_t stall_count = 0;

    for (uint32_t step = 0; step < MAX_CYCLES; step++) {
        prev_pc = core.pc;

        dsp56k_execute_instruction(&core);

        if (core.pc == prev_pc) {
            stall_count++;
            if (stall_count == 1) {
                printf("[!] DSP entered self-loop / idle at PC: 0x%06X (Step: %u)\n", core.pc, step);
            }
            if (stall_count >= 100) {
                printf("[!] Halting execution: DSP remained stalled at PC 0x%06X for 100 iterations.\n", core.pc);
                break;
            }
        } else {
            stall_count = 0;
        }

        if (core.is_idle) {
            printf("[*] DSP asserted idle state at PC: 0x%06X (Step: %u)\n", core.pc, step);
            break;
        }
    }

    printf("====================================================\n");
    printf("  Execution Summary                                 \n");
    printf("====================================================\n");
    printf("  Final PC         : 0x%06X\n", core.pc);
    printf("  Total Cycles     : %u\n", core.cycle_count);
    printf("  Halt Requested   : %s\n", core.is_idle ? "true" : "false");
    printf("  Is Idle          : %s\n", core.is_idle ? "true" : "false");
    printf("====================================================\n");

    return 0;
}


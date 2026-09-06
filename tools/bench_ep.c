#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h"
#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h"

#define DEFAULT_FW_PATH "halo2_dolby.bin"
#define MAX_CYCLES      50000

static uint32_t bench_read_peripheral(dsp_core_t *core, uint32_t address)
{
    printf("[PERIPH READ ] Address: 0x%04X (PC: 0x%06X)\n", address, core->pc);
    /* By default, return 0 (or default HI08 register state if requested) */
    return 0;
}

static void bench_write_peripheral(dsp_core_t *core, uint32_t address, uint32_t value)
{
    printf("[PERIPH WRITE] Address: 0x%04X -> Value: 0x%06X (PC: 0x%06X, Cycle: %u)\n",
           address, value & 0xFFFFFF, core->pc, core->cycle_count);
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

    /* Flash P-RAM: 
     * If packed as DWORDs where every 4th byte is 00 padding,
     * inject word-by-word into P-RAM.
     */
    uint32_t words_to_load = fw_size / 4;
    if (words_to_load > DSP_PRAM_SIZE) {
        printf("[!] WARNING: Payload words (%u) exceed DSP_PRAM_SIZE (%u). Clamping load.\n",
               words_to_load, DSP_PRAM_SIZE);
        words_to_load = DSP_PRAM_SIZE;
    }

    printf("[*] Flashing %u words into DSP Program RAM (DSP_SPACE_P)...\n", words_to_load);
    for (uint32_t addr = 0; addr < words_to_load; addr++) {
        uint32_t word = (uint32_t)buffer[addr * 4] |
                        ((uint32_t)buffer[addr * 4 + 1] << 8) |
                        ((uint32_t)buffer[addr * 4 + 2] << 16);
        dsp56k_write_memory(&core, DSP_SPACE_P, addr, word);
    }
    free(buffer);

    /* Preamble opcode peek: inspect bootstrap vector and jump instructions */
    printf("[*] Preamble P-RAM Peek (PC 0x000000 - 0x000020):\n");
    for (uint32_t p = 0; p <= 0x000020; p++) {
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


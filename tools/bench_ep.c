#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h"
#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h"

#define DEFAULT_FW_PATH "halo2_dolby.bin"
#define TARGET_FRAMES   5

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

static uint32_t dma_src_addr = 0;
static uint32_t dma_dst_addr = 0;

static void bench_write_peripheral(dsp_core_t *core, uint32_t address, uint32_t value)
{
    if (address == 0xFFFFD5) dma_src_addr = value & 0xFFFFFF;
    if (address == 0xFFFFD4) dma_dst_addr = value & 0xFFFFFF;

    if (address == 0xFFFFD6) {
        dma_control_reg = value & 0xFFFFFF;
        if (value & 0x01) {
            dma_transfer_countdown = 2;
            printf("[DMA TRIGGER] Src: 0x%06X -> Dst: 0x%06X | Ctrl: 0x%06X (PC: 0x%06X, Cycle: %" PRIu64 ")\n",
                   dma_src_addr, dma_dst_addr, dma_control_reg, core->pc, (uint64_t)core->cycle_count);

            /* Active DMA Copy: Transfer 64 words from Src to Dst if within X-RAM bounds */
            if (dma_src_addr < DSP_XRAM_SIZE && dma_dst_addr < DSP_XRAM_SIZE) {
                for (uint32_t i = 0; i < 64; i++) {
                    if ((dma_src_addr + i < DSP_XRAM_SIZE) && (dma_dst_addr + i < DSP_XRAM_SIZE)) {
                        uint32_t val = dsp56k_read_memory(core, DSP_SPACE_X, dma_src_addr + i);
                        dsp56k_write_memory(core, DSP_SPACE_X, dma_dst_addr + i, val);
                    }
                }
            }
        }
    }

    if (address == 0xFFFFC5) {
        hi08_c5_reg = value & 0xFFFFFF;
    }
}

static FILE *wav_file = NULL;
static uint32_t total_pcm_bytes = 0;

static void wav_init(const char *filename)
{
    wav_file = fopen(filename, "wb");
    if (!wav_file) {
        fprintf(stderr, "[!] Failed to open %s for WAV export.\n", filename);
        return;
    }

    /* Write 44-byte placeholder header (patched at close) */
    uint8_t header[44] = {0};
    fwrite(header, 1, 44, wav_file);
    total_pcm_bytes = 0;
}

static void wav_write_frame_51(dsp_core_t *core)
{
    if (!wav_file) return;

    /* Extract 32 samples per channel across all 6 discrete buffers */
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t ch[6];
        ch[0] = dsp56k_read_memory(core, DSP_SPACE_X, 0x000000 + i); /* FL  */
        ch[1] = dsp56k_read_memory(core, DSP_SPACE_X, 0x000100 + i); /* FR  */
        ch[2] = dsp56k_read_memory(core, DSP_SPACE_X, 0x000200 + i); /* C   */
        ch[3] = dsp56k_read_memory(core, DSP_SPACE_X, 0x000300 + i); /* LFE */
        ch[4] = dsp56k_read_memory(core, DSP_SPACE_X, 0x000400 + i); /* SL  */
        ch[5] = dsp56k_read_memory(core, DSP_SPACE_X, 0x000500 + i); /* SR  */

        /* Interleave as 24-bit Little-Endian (3 bytes per sample) */
        for (int c = 0; c < 6; c++) {
            uint8_t b0 = ch[c] & 0xFF;
            uint8_t b1 = (ch[c] >> 8) & 0xFF;
            uint8_t b2 = (ch[c] >> 16) & 0xFF;
            fputc(b0, wav_file);
            fputc(b1, wav_file);
            fputc(b2, wav_file);
            total_pcm_bytes += 3;
        }
    }
}

static void wav_close(void)
{
    if (!wav_file) return;

    /* Patch valid 44-byte RIFF/WAVE header */
    fseek(wav_file, 0, SEEK_SET);

    uint32_t riff_size = 36 + total_pcm_bytes;
    uint32_t sample_rate = 48000;
    uint16_t num_channels = 6;
    uint16_t bits_per_sample = 24;
    uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
    uint16_t block_align = num_channels * (bits_per_sample / 8);

    uint8_t hdr[44] = {
        'R', 'I', 'F', 'F',
        riff_size & 0xFF, (riff_size >> 8) & 0xFF, (riff_size >> 16) & 0xFF, (riff_size >> 24) & 0xFF,
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        16, 0, 0, 0,             /* Subchunk1Size (16 for PCM) */
        1, 0,                    /* AudioFormat (1 = PCM) */
        num_channels & 0xFF, (num_channels >> 8) & 0xFF,
        sample_rate & 0xFF, (sample_rate >> 8) & 0xFF, (sample_rate >> 16) & 0xFF, (sample_rate >> 24) & 0xFF,
        byte_rate & 0xFF, (byte_rate >> 8) & 0xFF, (byte_rate >> 16) & 0xFF, (byte_rate >> 24) & 0xFF,
        block_align & 0xFF, (block_align >> 8) & 0xFF,
        bits_per_sample & 0xFF, (bits_per_sample >> 8) & 0xFF,
        'd', 'a', 't', 'a',
        total_pcm_bytes & 0xFF, (total_pcm_bytes >> 8) & 0xFF, (total_pcm_bytes >> 16) & 0xFF, (total_pcm_bytes >> 24) & 0xFF
    };

    fwrite(hdr, 1, 44, wav_file);
    fclose(wav_file);
    printf("[+] 5.1 LPCM WAV Export Complete: tools/ep_output_5_1.wav (%u bytes written)\n", total_pcm_bytes);
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
    printf("[*] AC-3 Kernel Exponent Extraction Peek (PC 0x000510 - 0x000525):\n");
    for (uint32_t p = 0x000510; p <= 0x000525; p++) {
        printf("    P:0x%06X = 0x%06X\n", p, dsp56k_read_memory(&core, DSP_SPACE_P, p));
    }

    /* Phase 58: Activate Full 5.1 Surround Streams in Host Mailbox */
    printf("[*] Activating 5.1 Surround Streams in Host Mailbox (FL/FR, C/LFE, SL/SR = 0x000300)...\n");
    dsp56k_write_memory(&core, DSP_SPACE_X, 0x000BC1, 0x000300); /* Front L/R */
    dsp56k_write_memory(&core, DSP_SPACE_X, 0x000BC2, 0x000300); /* Center / LFE */
    dsp56k_write_memory(&core, DSP_SPACE_X, 0x000BC3, 0x000300); /* Surround L/R */

    /* Phase 59: Pre-load Distinct Signatures for All 6 Discrete Audio Channels */
    printf("[*] Pre-loading 6-Channel Synthetic PCM Signatures into Apertures...\n");
    for (uint32_t i = 0; i < 32; i++) {
        /* Pair 0: Front Left (0x11xxxx) & Front Right (0x22xxxx) at 0x0029A2 */
        dsp56k_write_memory(&core, DSP_SPACE_X, 0x0029A2 + (i * 2),     0x110000 | (i << 8));
        dsp56k_write_memory(&core, DSP_SPACE_X, 0x0029A2 + (i * 2) + 1, 0x220000 | (i << 8));

        /* Pair 1: Center (0x33xxxx) & LFE Subwoofer (0x44xxxx) at 0x002AA2 (Offset +0x100) */
        dsp56k_write_memory(&core, DSP_SPACE_X, 0x002AA2 + (i * 2),     0x330000 | (i << 8));
        dsp56k_write_memory(&core, DSP_SPACE_X, 0x002AA2 + (i * 2) + 1, 0x440000 | (i << 8));

        /* Pair 2: Surround Left (0x55xxxx) & Surround Right (0x66xxxx) at 0x002BA2 (Offset +0x200) */
        dsp56k_write_memory(&core, DSP_SPACE_X, 0x002BA2 + (i * 2),     0x550000 | (i << 8));
        dsp56k_write_memory(&core, DSP_SPACE_X, 0x002BA2 + (i * 2) + 1, 0x660000 | (i << 8));
    }

    /* Initialize 5.1 LPCM WAV Exporter */
    wav_init("tools/ep_output_5_1.wav");

    /* Replace indefinite execution loop with frame-monitored runner */
    printf("[*] Beginning Audio Kernel Execution (Target: %d Active Frames)...\n", TARGET_FRAMES);

    uint32_t last_frame_count = 0;
    uint32_t total_instructions = 0;

    while (total_instructions < 1000000) {
        dsp56k_execute_instruction(&core);
        core.cycle_count += core.instr_cycle;
        core.instr_cycle = 0;
        total_instructions++;

        /* Monitor Master Audio Frame Counter at P:$0007 */
        uint32_t current_frame = dsp56k_read_memory(&core, DSP_SPACE_P, 0x0007);
        if (current_frame != last_frame_count) {
            uint32_t ping_pong = dsp56k_read_memory(&core, DSP_SPACE_P, 0x0116);
            printf("\n>>> [AUDIO FRAME %u COMPLETED] Instructions: %u | Cycle: %" PRIu64 " | Buffer P:$0116: 0x%06X | DMA Addr: 0x%06X <<<\n",
                   current_frame, total_instructions, (uint64_t)core.cycle_count, ping_pong, dma_control_reg);
            last_frame_count = current_frame;

            /* Capture 5.1 multichannel audio frame */
            wav_write_frame_51(&core);

            if (current_frame >= TARGET_FRAMES) {
                printf("[+] Target frame count (%u) reached cleanly. Halting execution.\n", TARGET_FRAMES);
                break;
            }
        }
    }

    /* Finalize WAV file header */
    wav_close();

    printf("\n[*] =================== POST-TRANSFORM 5.1 MEMORY MAP ===================\n");

    printf("\n--- FL/FR Input Aperture (X:0x0029A2 - X:0x0029C1) ---\n");
    for (uint32_t a = 0x0029A2; a <= 0x0029C1; a += 2) {
        printf("    X:0x%06X = FL: 0x%06X | FR: 0x%06X\n",
               a, dsp56k_read_memory(&core, DSP_SPACE_X, a), dsp56k_read_memory(&core, DSP_SPACE_X, a + 1));
    }

    printf("\n--- C/LFE Input Aperture (X:0x002AA2 - X:0x002AC1) ---\n");
    for (uint32_t a = 0x002AA2; a <= 0x002AC1; a += 2) {
        printf("    X:0x%06X = C:  0x%06X | LFE: 0x%06X\n",
               a, dsp56k_read_memory(&core, DSP_SPACE_X, a), dsp56k_read_memory(&core, DSP_SPACE_X, a + 1));
    }

    printf("\n--- SL/SR Input Aperture (X:0x002BA2 - X:0x002BC1) ---\n");
    for (uint32_t a = 0x002BA2; a <= 0x002BC1; a += 2) {
        printf("    X:0x%06X = SL: 0x%06X | SR:  0x%06X\n",
               a, dsp56k_read_memory(&core, DSP_SPACE_X, a), dsp56k_read_memory(&core, DSP_SPACE_X, a + 1));
    }

    printf("\n--- Mixer Accumulator / Staging Buffer (X:0x0670 - X:0x069F) ---\n");
    for (uint32_t x = 0x0670; x <= 0x069F; x += 4) {
        printf("    X:0x%06X: 0x%06X 0x%06X 0x%06X 0x%06X\n",
               x,
               dsp56k_read_memory(&core, DSP_SPACE_X, x),
               dsp56k_read_memory(&core, DSP_SPACE_X, x + 1),
               dsp56k_read_memory(&core, DSP_SPACE_X, x + 2),
               dsp56k_read_memory(&core, DSP_SPACE_X, x + 3));
    }

    printf("\n--- Internal Discrete Channel Buffers (First 8 Samples) ---\n");
    const char *chan_names[6] = { "FL ", "FR ", "C  ", "LFE", "SL ", "SR " };
    for (int c = 0; c < 6; c++) {
        uint32_t base = c * 0x0100;
        printf("    [%s @ 0x%06X]:", chan_names[c], base);
        for (uint32_t i = 0; i < 8; i++) {
            printf(" 0x%06X", dsp56k_read_memory(&core, DSP_SPACE_X, base + i));
        }
        printf("\n");
    }
    printf("[*] =====================================================================\n\n");

    printf("====================================================\n");
    printf("  Execution Summary                                 \n");
    printf("====================================================\n");
    printf("  Final PC             : 0x%06X\n", core.pc);
    printf("  Total Instructions   : %u\n", total_instructions);
    printf("  Total Cycles         : %u\n", core.cycle_count);
    printf("  Frames Completed     : %u\n", last_frame_count);
    printf("  Halt Requested       : %s\n", core.is_idle ? "true" : "false");
    printf("  Is Idle              : %s\n", core.is_idle ? "true" : "false");
    printf("====================================================\n");

    return 0;
}


# Xemu Motorola DSP56362 (EP) Low-Level Emulation (LLE) Audio Subsystem

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](file:///z:/xemu/LICENSE.txt)
[![Architecture: Original Xbox MCPX APU](https://img.shields.io/badge/Hardware-MCPX%20APU%20DSP56362-green.svg)](#system-architecture)
[![Audio Backend: SDL3 5.1 Multi-Channel](https://img.shields.io/badge/Audio-SDL3%205.1%20Surround-orange.svg)](#audio-channel-ordering)

A bit-accurate Low-Level Emulation (LLE) implementation of the **Motorola DSP56362 Encoding Processor (EP)** co-processor for the [Xemu](https://xemu.app) Original Xbox emulator. This subsystem restores authentic hardware-accelerated Dolby Digital AC-3 interactive surround sound processing.

---

## Overview

The original Xbox MCPX Audio Processing Unit (APU) contains two independent digital signal processing cores based on the Motorola DSP56300 family:
1. **Global Processor (GP)**: Responsible for voice summation, environmental reverberation, dynamic range compression, and front stereo mixing.
2. **Encode Processor (EP) - Motorola DSP56362**: A dedicated hardware co-processor responsible for discrete 5.1 multi-channel spatialization and interactive Dolby Digital AC-3 encoding.

Upstream emulation solutions historically stubbed or bypassed the EP co-processor due to missing instruction extensions, strict peripheral handshaking requirements, and lack of internal DMA emulation. This subsystem introduces a full execution interpreter, hardware peripheral register emulation, host mailbox synchronization, and seamless SDL3 5.1 audio streaming.

---

## Key Features

* **Authentic Motorola DSP56362 Execution**: Emulates 24-bit DSP56300 instruction extensions, including `EXTRACTU` (unsigned bitfield extraction) and `DO FOREVER` hardware looping.
* **Peripheral & DMA Emulation**: Implements hardware registers for ESSI0 serial communications (`$FFFFB3`), dynamic host mailbox synchronization (`$FFFFC5`), and internal DMA block transfers (`$FFFFD4`–`$FFFFD6`).
* **Expanded Memory Addressing**: 32,768 words ($0x8000$) of program RAM (P-RAM) backing the complete 64KB `NV_PAPU_EPPMEM` aperture.
* **Native SDL3 5.1 Multi-Channel Pipeline**: Replaces legacy stereo-locked SDL audio streams with discrete 6-channel LPCM streaming matching DirectSound3D surround channel topology.
* **Safe Halting & Graceful Fallback**: Replaces fatal guest memory assertions with virtual core halting. If discrete surround microcode is not present, audio automatically falls back to stereo mixing without hangs.
* **Standalone Verification Tooling**: Includes `tools/ep_harness`, a self-contained C99 utility to inspect and validate microcode dumps offline.

---

## Known Unfixable Issues Without Dolby EP Microcode

**Audio Artifacts & Stream Starvation:** Pre-rendered video tracks and software streams rely on the EP to generate periodic frame interrupts (`0xFFFFC5` Bit 1) to pace audio packet transfers. Without active EP pacing, host-side ring buffers underrun continuously, causing loud clicks, pops, and stuttering.

**Muddled Channel Separation:** Scraping raw mixbins without an active EP causes front/center channel bleeding, phase cancellation, and a center-biased soundstage. When the guest kernel detects an unbooted EP, DirectSound falls back to an analog 4-to-2 Dolby Pro Logic matrix across Front Left and Front Right.

---

## System Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Xemu MCPX APU Subsystem                         │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
    ┌───────────────────────────────┴───────────────────────────────┐
    ▼                                                               ▼
┌───────────────────────┐                               ┌───────────────────────┐
│  Voice Processor (VP) │                               │ Global Processor (GP) │
│  64 3D Voices + HRTF  ├───────────── Mixbins 0..5 ───►│ Master Effects & Mix  │
└───────────────────────┘                               └───────────┬───────────┘
                                                                    │
                                                  Multichannel DMA Stride ($004000)
                                                                    │
                                                                    ▼
                                                        ┌───────────────────────┐
                                                        │  Encode Processor     │
                                                        │  (EP / Motorola       │
                                                        │   DSP56362 LLE)       │
                                                        └───────────┬───────────┘
                                                                    │
                                                  Discrete 5.1 LPCM (L, R, C, LFE, LS, RS)
                                                                    │
                                                                    ▼
                                                        ┌───────────────────────┐
                                                        │    SDL3 AudioStream   │
                                                        │  (WASAPI / Direct)    │
                                                        └───────────────────────┘
```

---

## Building

### Prerequisites

* **Operating System**: Windows 10/11 (64-bit)
* **Build Environment**: [MSYS2](https://www.msys2.org/) with the `MINGW64` toolchain
* **Build System**: Meson and Ninja

### Full Emulator Build

Launch the **MSYS2 MINGW64** shell and execute:

```bash
cd /z/xemu
./build.sh
```

*(For incremental development builds: `ninja -C build`)*

The compiled binary will be generated at `./dist/xemu.exe`.

### Standalone Microcode Verification Harness Build

To compile the standalone verification utility without building the entire emulator:

```bash
gcc -O2 -s -o tools/ep_harness tools/bench_ep.c
```

---

## Firmware Configuration

Due to licensing restrictions, proprietary Dolby Digital microcode cannot be distributed with Xemu. While many titles upload this microcode dynamically into guest memory during execution, you can provide an offline firmware dump to ensure multi-channel surround encoding across all titles:

1. Obtain a clean 32-bit LE microcode dump from your authentic hardware or software title.
2. Rename the file to `dolby_ep.bin`.
3. Place `dolby_ep.bin` in the root emulator directory or in the `tools/` directory:
   * `./dolby_ep.bin`
   * `./tools/dolby_ep.bin`

### Validating Your Firmware Dump

Run the standalone verification utility to verify the file topology and hardware reset vector:

```bash
./tools/ep_harness tools/dolby_ep.bin
```

**Expected Output for Authentic Microcode**:
```text
Motorola DSP56362 Microcode Verification Utility
[+] Successfully loaded 3804 bytes (951 24-bit words) from: tools/dolby_ep.bin
[+] Firmware Topology Analysis:
    |- Vector Table Length : 0x00C8 words (Destination: P:0x0000)
    |- Transform Kernel    : 0x017F words (Destination: P:0x0180)
    \- Data Tables / Tail  : 0x01AE words (Destination: P:0x0300)
[+] Hardware Reset Vector : 0x050C08
[+] Status: Validated authentic Xbox Dolby Digital AC-3 interactive encoding microcode.
```

---

## Audio Channel Ordering

The internal monitor stream outputs audio in standard discrete DirectSound 5.1 surround ordering:

| Index | Channel Identifier | Speaker Destination |
| :---: | :--- | :--- |
| **0** | Front Left (`FL`) | Left Front Satellite |
| **1** | Front Right (`FR`) | Right Front Satellite |
| **2** | Center (`FC`) | Center Dialogue Channel |
| **3** | Low-Frequency Effects (`LFE`) | Subwoofer |
| **4** | Surround Left (`SL`) | Left Rear Surround |
| **5** | Surround Right (`SR`) | Right Rear Surround |

---

## Operational Guide: The Dual EEPROM Method

Because QEMU evaluates hardware audio and NVRAM flags strictly at cold boot, runtime toggling of surround states is unsupported by the guest OS. Testing requires maintaining dual EEPROM profiles:

1. **Create Base Profiles**: Boot the Xbox dashboard in Xemu, configure audio to **Stereo**, and shut down. Copy your `eeprom.bin` and rename it `eeprom.bin.stereo`.
2. **Create Surround Profile**: Boot the dashboard again, change settings to **Dolby Digital Surround**, and shut down. Copy the resulting file to `eeprom.bin.surround`.
3. **Execution**: When testing multi-channel paths, duplicate `eeprom.bin.surround` as `eeprom.bin`, ensure *Real-time DSP processing* is checked in Xemu settings, and launch.

---

## Roadmap
1. **Windows Alpha Testing:** Community validation on Windows across various multi-channel DACs, AVRs, and virtual surround headphones.
2. **Hybrid 2D/3D Bink Video audio streams** are pre-mastered with heavy dynamic range compression peaking directly at 0 dBFS. When those full-scale audio samples are routed through the APU's voice mixer and spatial transform matrices, any positional scaling or multi-voice summing that pushes $v > 1.0\text{f}$ hits that brick-wall clamp. Brick-wall digital clipping converts smooth sine waveforms into square-wave harmonic spikes, which produces high-frequency crackling and popping that vanishes the second the loud audio stream finishes. In the upcoming Beta milestone I'll be implementing a soft saturation knee or applying a -3 dB headroom pad across the multichannel aperture to hopefully resolve this.
3. **Upstream Architecture Review:** Collaborating with upstream maintainers to integrate EP C-interpreter state cleanly without regressing ongoing JIT milestones.
4. **macOS / Linux Support:** Deferred until Windows multichannel stability is fully baselined.

---

# [AI Disclosure and Project Log](https://github.com/Synkronicity/xemu/blob/master/AI_DISCLOSURE_LOG.md)

---

## License & Attribution

This project is licensed under the **GNU General Public License v2.0 (GPLv2)** to remain fully compatible with upstream QEMU and Xemu.

* **Motorola DSP56362 LLE Architecture & Reintegration**: Copyright (c) 2026 Will Bonnett
* **Xemu APU / Emulator Core**: Copyright (c) 2020-2025 Matt Borgerson, espes, and Xemu contributors
* **Hatari / ARAnyM DSP Engine Basis**: Copyright (c) 2001-2008 ARAnyM developer team, Thomas Huth

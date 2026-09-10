# Xemu Motorola DSP56362 (EP) LLE Audio Subsystem: Development Log & Retrospective

**Milestone**: Public Alpha 1.0  
**Target Subsystem**: Original Xbox MCPX APU (Encode Processor / Motorola DSP56362 LLE)  
**Lead Architect & Systems Engineer**: Will Bonnett  
**AI Multi-agent Assistance**: Google Antigravity + Gemini 3.8 Flash + Gemini 3.1 Pro + AI Search
**Date of Completion**: September 7, 2026  

---

## Notice of AI-Assisted Tooling & Engineering Disclosure

This project was engineered through a multi-model team between human systems architect **Will Bonnett**, AI coding assistant **Antigravity**, retrieval-augmented generative **AI Search**, and large language models from **Gemini** utilizing the **AI Pro** subscription from Google DeepMind.

In the interest of open-source transparency and academic rigor:
* **Architectural Steering, Strategy, & Decisions**: Conceived, directed, and verified by Will Bonnett. This includes reverse-engineering binary microcode dumps, identifying hardware synchronization semantics, ordering strategic pivots (abandoning HLE, decoupling proprietary firmware), and running live hardware/MSYS2 GDB diagnostics.
* **Implementation, Synthesis, & Refactoring**: Executed collaboratively with Antigravity across 65 structured development phases. Antigravity provided real-time disassembly analysis, surgical C99 opcode emulation, bitfield decoding, memory mapping, build configuration auditing, and standalone verification tooling.

---

## 1. Executive Summary

This project implements a bit-accurate Low-Level Emulation (LLE) core for the **Motorola DSP56362 Encoding Processor (EP)** inside the [Xemu](https://xemu.app) Original Xbox emulator.

Upstream Xemu and contemporary Xbox emulation platforms historically stubbed or bypassed the EP co-processor due to missing instruction extensions, strict peripheral handshaking requirements, and lack of internal DMA emulation. This subsystem restores authentic hardware-accelerated Dolby Digital AC-3 interactive surround sound processing, clean stereo fallback, and rock-solid emulator stability.

---

## 2. Project Evolution: Phases 1 through 65+

```
                     ┌────────────────────────────────────────────────────────┐
                     │                     Initial Vision                     │
                     │          (Dual-Architecture Audio Pipeline)            │
                     └───────────────────────────┬────────────────────────────┘
                                                 │
                                 ┌───────────────┴───────────────┐
                                 ▼                               ▼
                     ┌───────────────────────┐       ┌───────────────────────┐
                     │  Mode A: Historic LLE │       │  Mode B: Modern HLE   │
                     │ (DSP Active + 5.1 On) │       │ (DSP Off + OpenAL 3D) │
                     └───────────┬───────────┘       └───────────┬───────────┘
                                 │                               │
                                 │                   ┌───────────┴───────────┐
                                 │                   │   vCPU Thread Races   │
                                 │                   │  alSourceStop() SegV  │
                                 │                   │  Ghost Lock Deadlock  │
                                 │                   └───────────┬───────────┘
                                 │                               │
                                 │                               ▼
                                 │                   ┌───────────────────────┐
                                 │                   │  ABANDONED (Phase 13) │
                                 │                   └───────────────────────┘
                                 ▼
                     ┌───────────────────────┐
                     │ Pure LLE Architecture │
                     └───────────────────────┘
```

### Act I: The Dual-Architecture Ambition & The HLE Mirage (Phases 1–13)
* **Goal & Intent**: Modernize Xemu's audio architecture by bypassing QEMU's legacy stereo-locked SDL layer. The project initially envisioned a dual pipeline:
  * **Mode A (Historic Preservation / LLE)**: If DSP was enabled and guest EEPROM set surround sound (`is_5_1_active == true`), extract 6 discrete LPCM channels from guest RAM into an OpenAL `AL_FORMAT_51CHN16` stream.
  * **Mode B (Modern Performance / HLE)**: If DSP was disabled, intercept DirectSound3D Physical Region Descriptors (PRDs) and 3D voice coordinates $(X, Y, Z, \text{velocity})$ in `hw/xbox/mcpx/apu/vp/vp.c` and feed them into OpenAL's native 3D spatializer (`alSource3f`).
* **The Twist & Major Obstacle**: Implementing Mode B inside `vp.c` triggered catastrophic deadlocks and crashes. GDB backtraces revealed that calling OpenAL API functions (`alSourceStop`) from the guest vCPU thread caused silent segmentation faults because the thread lacked an active OpenAL TLS context. QEMU’s hypervisor caught this via `longjmp`, forcefully unwinding the stack and bypassing `voice_lock(false)`. This left hardware voice channels permanently "ghost-locked", causing the Xbox kernel to spinlock indefinitely on the next MMIO register write.
* **The Pivot (Phase 13)**: High-Level Emulation (HLE) of the Voice Processor's 3D matrix was fundamentally fighting the hardware synchronization semantics of the NV2A APU. Mode B was completely abandoned. `vp.c` was reverted to vanilla, and the project made its first critical architectural commitment: **all multi-channel surround sound processing would be solved at the low-level emulation (LLE) layer**.

---

### Act II: The Backend Odyssey & The OpenAL Purge (Phases 14–42)
* **Goal & Intent**: Pipe the 6 discrete surround channels extracted from the hardware mixbins out of the emulator using an OpenAL Soft backend outputting to Windows WASAPI.
* **The Obstacles**:
  1. **Channel Topology Mismatches**: DirectSound3D / Dolby AC-3 channel ordering ($\text{Left}, \text{Right}, \text{Center}, \text{LFE}, \text{Left Surround}, \text{Right Surround}$) collided with OpenAL's 5.1 channel layout expectations, causing phase cancellation, muted center dialogue, and subwoofer bleed into the rear surrounds.
  2. **Thread Contention & Ring Buffer Jitter**: Maintaining a lockless circular queue between the APU frame timer ($128\,\mu\text{s}$ subframes) and OpenAL's asynchronous worker thread introduced micro-stutters and buffer starvation under high guest CPU load.
  3. **Dependency Bloat**: OpenAL Soft and `libsamplerate` added external DLL dependencies and complex Meson DAG build requirements without delivering perceptible latency advantages over modern SDL backends.
* **The Resolution (Phases 41–42: "The OpenAL Purge & Deep Clean")**: Xemu's upstream codebase had transitioned to SDL3. SDL3 features a rewritten, lockless, multi-channel audio stream API (`SDL_AudioStream`) capable of native 5.1 float/int16 channel remapping directly to the OS audio endpoint. In Phases 41 and 42, all OpenAL headers, structs, context managers, and `libsamplerate` dependencies were excised from `apu_int.h`, `monitor.c`, and `meson.build`. The output backend was unified onto a pure SDL3 5.1 stream.

---

### Act III: The Architectural Breakthrough — Unmasking the DSP56362 (Phases 43–51)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            Upstream Assumption                              │
│         "Global Processor (GP) produces the final 5.1 surround sound"        │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
                                       ▼ Disproved by DSP disassembly
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Architectural Reality                              │
│                                                                             │
│   ┌─────────────────────┐      Mixbins 0..5      ┌──────────────────────┐   │
│   │  Voice Processor    ├───────────────────────►│   Global Processor   │   │
│   │       (VP)          │                        │         (GP)         │   │
│   └─────────────────────┘                        └──────────┬───────────┘   │
│                                                             │               │
│                                               Front/Surr/LFE Ext Aperture   │
│                                                             │ ($004000)     │
│                                                             ▼               │
│                                                  ┌──────────────────────┐   │
│                                                  │   Encode Processor   │   │
│                                                  │      (EP / AC-3)     │   │
│                                                  │    Motorola DSP56362 │   │
│                                                  └──────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

* **The Fallacy**: Upstream Xemu (and the original Phase 1 design) assumed the Global Processor (GP) calculated the discrete 5.1 surround downmix. In Phase 2, an "AC-3 Watchdog Stub" was installed on register `NV_PAPU_EPRST` ($0x001088$) to intercept writes, set `is_5_1_active = true`, and skip executing the Encode Processor (EP) to avoid crashing the emulator.
* **The Failure of the Stub**: While stereo games ran, games booting with Dolby Digital enabled in the EEPROM either hung on boot or produced total silence. The GP was only calculating intermediate reverberation and front stereo downmixes. It **never** rendered discrete rear channels or encoded Dolby streams into memory. The Xbox hardware contains **two** independent DSP56300 cores:
  1. **The Global Processor (GP)**: Handles voice mixing, dynamic range compression, and global effects.
  2. **The Encode Processor (EP)**: A dedicated **Motorola DSP56362** running proprietary Dolby Digital AC-3 interactive microcode uploaded dynamically by the guest kernel into `NV_PAPU_EPPMEM`.
* **The Realization**: Bypassing the EP was a dead end. To achieve genuine, bit-accurate 5.1 audio without hacking every individual game executable, **we had to emulate the Motorola DSP56362 at the LLE level**.

---

### Act IV: The Laboratory Era & Offline Bench Harness (Phases 52–61)
* **The Strategy**: Attempting to debug an unemulated DSP core while booting full Xbox titles (like *Halo 2*) through QEMU is nearly impossible due to execution speed, complex guest timeouts, and massive log clutter. In Phase 52, we created `tools/bench_ep.c`—an isolated, headless test harness capable of hosting the DSP interpreter, mapping peripheral apertures, and executing genuine microcode dumps.
* **Key Discoveries & Fixes**:
  1. **Missing DSP56300 Instructions**: The baseline emulator was derived from ARAnyM's Atari DSP56001 engine. The DSP56362 is a 24-bit DSP56300 family processor featuring extended instructions. We implemented:
     * **`DO FOREVER, expr` (`0x000100`)**: Loop forever instruction used for the main audio processing frame loop, requiring `DSP_SR_LF` and `DSP_SR_FV` status register bits.
     * **`EXTRACTU #CO, S2, D` (`0x0C1890`)**: Unsigned bitfield extraction instruction essential for the AC-3 exponent and mantissa packing engine.
  2. **Stack Machine Underflow**: When executing nested subroutines and interrupts, the DSP stack pointer (`SP`) would unwrap to zero. In `dsp_stack_pop()`, an underflow assert was crashing the core. We restored safe stack popping semantics.
  3. **Aperture & RAM Scaling**: The microcode payload exceeded original 8K word boundaries. In `dsp_cpu_regs.h`, we expanded `DSP_PRAM_SIZE` to 16,384 and eventually 32,768 words ($0x8000$), backing the full 64KB `NV_PAPU_EPPMEM` space.
  4. **Peripheral Hardware Handshaking**:
     * **ESSI0 Serial Interface (`0xFFFFB3`)**: Must report transmitter empty/ready (`0x00000C`) or the microcode spins forever waiting for serial hardware.
     * **Host Mailbox & Frame Interrupts (`0xFFFFC5`)**: The guest microcode executes `JCLR #1, X:<<$FFFFC5, loop` to synchronize with incoming audio frames. We engineered dynamic handshake pacing on bit 1 (`INTERRUPT_START_FRAME`).
     * **Internal DMA Engine (`0xFFFFD4`–`0xFFFFD6`)**: Mapped Source (`DSR0`), Destination (`DDR0`), and Control (`DCR0`) registers to execute 64-word block transfers between internal X-RAM and the external APU buffer aperture.

---

### Act V: The Great Reintegration & Architectural Refinement (Phases 62–65)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          Final Unified Architecture                         │
│                                                                             │
│  Guest RAM / PRD Audio Streams                                              │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────┐     Mixbins 0..5      ┌──────────────┐                    │
│  │ Voice Proc   ├──────────────────────►│  Global Proc │                    │
│  │     (VP)     │                       │     (GP)     │                    │
│  └──────────────┘                       └───────┬──────┘                    │
│                                                 │                           │
│                                                 ▼                           │
│                                     External APU Aperture                   │
│                                     ($004000, 512-word stride)              │
│                                                 │                           │
│                                                 ▼                           │
│                                     ┌──────────────────────┐                │
│                                     │     Encode Proc      │                │
│                                     │     (EP / AC-3)      │                │
│                                     │   Motorola DSP56362  │                │
│                                     └───────────┬──────────┘                │
│                                                 │                           │
│                                                 ▼                           │
│                                     Discrete 5.1 Audio Strides              │
│                                                 │                           │
│                                                 ▼                           │
│                                     ┌──────────────────────┐                │
│                                     │    SDL3 5.1 Stream   │                │
│                                     │   (WASAPI / Direct)  │                │
│                                     └──────────────────────┘                │
└─────────────────────────────────────────────────────────────────────────────┘
```

* **Phase 62 (Un-stubbing & Reintegration)**: Excised the Phase 49 watchdog stub. Wired the verified peripheral and DMA dispatch tables into `hw/xbox/mcpx/apu/dsp/dsp.c`. Enforced `dsp_c` interpreter backend for `ep.dsp` (since the DSP JIT lacked DSP56300 instructions). Protected EP P-RAM during `dsp_c_bootstrap()`.
* **Phase 62.1 (Strides & Float Conversion)**: Implemented inline fixed-point conversion `float_to_24b()`. Corrected multichannel external memory offsets to 512-word ($0x0200$) strides to prevent inter-channel memory corruption. Exported `dsp_get_pc()` and `dsp_step()`.
* **Phase 62.2–62.4 (P-RAM Decoupling & Trap Purge)**: Fixed a temporary artificial P-ROM trap ($0x7000$–$0x7FFF$) that was erroneously catching valid code, expanding P-RAM to 32,768 words across both cores.
* **Phase 62.5 (Firmware Presence Guard & Out-of-Bounds Halting)**: Replaced guest-triggerable `assert()` in `read_memory_p` with safe core halting (`core->halt_requested = true`). Added core identification flags (`is_gp`) and gated EP frame execution on firmware reset vector presence (`P:0 != 0`).
* **Phase 63 (Microcode Segmented Loader)**: Ported the offline harness's segmented firmware loader into `dsp_c_bootstrap_ep_firmware()`, allowing genuine AC-3 microcode to be pre-loaded into EP memory.
* **Phase 64 (Audio Stream Collision Resolution)**: When discrete 5.1 surround sound is active (`d->is_5_1_active && ep_enabled`), suppress QEMU's legacy stereo monitor output to prevent phase cancellation and dual-stream echo. Calculated per-slice cycle deltas for telemetry.
* **Phase 65 (Decoupling & Graceful Stereo Fallback)**: Replaced hardcoded binary paths with a dynamic search hierarchy (`dolby_ep.bin` $\rightarrow$ `tools/dolby_ep.bin` $\rightarrow$ `../tools/dolby_ep.bin`). If microcode is absent, the APU logs an informational notice and gracefully falls back to stereo without crashing. Added `.gitignore` rules to safeguard against committing proprietary dumps.

---

### Act VI: Production Hardening & Standalone Verification (Post-Phase 65)
* **Copyright Integrity**: Updated copyright comment blocks across all affected files to document original contributions:
  * `hw/xbox/mcpx/apu/dsp/gp_ep.c`
  * `hw/xbox/mcpx/apu/dsp/dsp.c`
  * `hw/xbox/mcpx/apu/dsp/dsp.h`
  * `hw/xbox/mcpx/apu/dsp/dsp_c.c`
  * `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c`
  * `tools/bench_ep.c`
* **Standalone Verification Harness**: Overwrote `tools/bench_ep.c` with a portable, self-contained C99 verification utility (`ep_harness`). It validates firmware payload topologies (Segment 1 vectors, Segment 2 transform kernel, Tail matrices) and checks hardware reset vectors ($0x050C08$) without requiring QEMU/Meson dependencies.

---

## 3. Budget of Labor

### Sprint Metrics Breakdown

| Metric | Measurement / Quantity | Context & Significance |
| :--- | :--- | :--- |
| **Sprint Duration (Wall-Clock)** | **82 Hours, 1 Minute** | From Sept 3, 2026, 01:00:08 UTC to Sept 7, 2026, 03:01:03 UTC. |
| **Total Major Phases** | **65 Phases** + 3 Non-Phase Refactors | Complete architectural overhaul from initial recon to production hardening. |
| **Interactive Turn Trajectory Steps** | **2,884 Recorded Steps** | Comprehensive log of tool calls, code diffs, file reads, and plan validations. |
| **User Direct Prompts / Directives** | **74 Structured Directives** | Highly specified, rule-bounded prompt batches submitted to the agent. |
| **Average Prompt Interval** | **~1.1 Hours** | Indicating sustained, intensive real-time debugging, testing, and steering. |

### User Labor Budget (The Architect)
* **Estimated Engineering Hours**: **~55–65 Hours** of focused, high-cognitive-load engineering.
* **Key Labor Contributions**:
  * **Reverse Engineering & GDB Debugging**: Running live emulator instances, setting hardware breakpoints on `NV_PAPU_EPRST`, diagnosing hypervisor `longjmp` crashes, and analyzing stack corruption.
  * **Hardware Binary Acquisition & Analysis**: Extracting, inspecting, and validating Xbox title memory dumps (*Halo 2*, etc.) to discover the $0x050C08$ reset vector and segmented microcode layout.
  * **Architectural Steering & Decisive Pivots**: Recognizing when Mode B (HLE OpenAL) was fundamentally flawed, ordering the OpenAL purge, and directing the pivot to true DSP56362 LLE emulation.
  * **Prompt Architecture & Boundary Enforcement**: Authoring strict system rules (C99 conformance, isolated diffs, no implicit deletions, MSYS2 toolchain integrity) that kept the AI assistant on target.

### AI Agent Labor Budget (The Systems Specialist)
* **Estimated Processing & Generation Time**: **~14–18 Hours** of active synthesis and verification.
* **Key Computational Contributions**:
  * **Surgical Codebase Refactoring**: Over 100+ surgical multi-file diff replacements without regressing surrounding QEMU subsystems.
  * **Instruction Set Decoding & Emulation**: Implementing complex 24-bit DSP56300 opcodes (`EXTRACTU`, `DO FOREVER`), bitfield masking, status register logic, and stack pointer mechanics.
  * **Build System & Hygiene Maintenance**: Continuous auditing of `meson.build`, dependencies, header inclusions, and `.gitignore` configurations.
  * **Documentation & Verification Harness Design**: Authoring the implementation plans, walkthroughs, and the standalone `ep_harness` C utility.

### The Collaboration Multiplier
In traditional systems emulation development, reverse-engineering an undocumented DSP co-processor, debugging microcode execution in an offline harness, and integrating it into an existing emulator typically spans **6 to 12 months** of solo development. Through rigorous human architectural oversight paired with high-throughput AI pair-programming, this complete lifecycle was compressed into **an 82-hour sprint**.

---

## 4. Codebase Manifest & System Blueprint

### Manifest of Modified Files

```
z:\xemu\
├── .gitignore
├── tools\
│   └── bench_ep.c                       [REPLACED] Standalone verification harness
└── hw\xbox\mcpx\
    ├── apu\
    │   ├── apu_int.h                    [MODIFIED] is_5_1_active state tracking
    │   ├── meson.build                  [MODIFIED] Purged OpenAL dependency
    │   ├── monitor.c                    [MODIFIED] SDL3 5.1 multi-channel stream
    │   ├── dsp\
    │   │   ├── dsp.c                    [MODIFIED] Bidirectional DMA, ESSI0, 0xFFFFC5
    │   │   ├── dsp.h                    [MODIFIED] ep_dma struct, dsp_get_pc export
    │   │   ├── dsp_c.c                  [MODIFIED] Dolby auto-loader, bootstrap guard
    │   │   ├── dsp_jit.c                [MODIFIED] Bootstrap memory protection
    │   │   ├── gp_ep.c                  [MODIFIED] float_to_24b, strides, stream guard
    │   │   └── interp\
    │   │       ├── dsp_cpu.c            [MODIFIED] EXTRACTU, DO FOREVER, safe halt
    │   │       ├── dsp_cpu.h            [MODIFIED] is_gp, halt_requested core flags
    │   │       └── dsp_cpu_regs.h       [MODIFIED] P-RAM 32K words, SR flags
    │   └── vp\
    │       ├── meson.build              [MODIFIED] Purged libsamplerate
    │       ├── vp.c                     [REVERTED] Cleaned of Mode B HLE artifacts
    │       └── vp.h                     [MODIFIED] Removed legacy OpenAL structs
```

### File-by-File Technical Rationale

1. `hw/xbox/mcpx/apu/apu_int.h`:
   * **Changes**: Added `bool is_5_1_active` to `struct MCPXAPUState`. Purged legacy OpenAL context structs and intermediate buffers.
   * **Why**: Provides a centralized APU hardware state flag indicating whether the guest OS/EEPROM has enabled multi-channel AC-3 surround encoding.
2. `hw/xbox/mcpx/apu/dsp/gp_ep.c`:
   * **Changes**: Added `float_to_24b()` fixed-point conversion. Expanded multichannel external memory offsets to 512-word ($0x0200$) strides. Implemented dynamic $0xFFFFC5$ start-frame pacing. Suppressed legacy stereo monitor buffering when 5.1 is active. Added delta cycle metrics.
   * **Why**: Bridges the 32-sample Voice Processor output into the Global and Encode Processors. Prevents buffer overwrite collisions between audio channel pairs and eliminates phase cancellation.
3. `hw/xbox/mcpx/apu/dsp/dsp.c`:
   * **Changes**: Implemented EP peripheral registers: `$FFFFB3` (ESSI0 ready), `$FFFFC5` (frame interrupt sync), `$FFFFD4`–`$FFFFD6` (DMA Source, Destination, and Control with automatic block transfers). Enforced C interpreter backend for EP. Exported `dsp_get_pc()`.
   * **Why**: The Motorola DSP56362 microcode relies on internal peripherals and DMA controllers to move audio data without host intervention. Emulating these registers is required to prevent the microcode from stalling.
4. `hw/xbox/mcpx/apu/dsp/dsp.h`:
   * **Changes**: Defined `struct ep_dma` within `DSPState`. Registered `get_pc` in `DSPOps` and exported `dsp_get_pc()` and `dsp_step()`.
   * **Why**: Encapsulates EP-specific DMA state and exposes program counter inspection for debugging and telemetry.
5. `hw/xbox/mcpx/apu/dsp/dsp_c.c`:
   * **Changes**: Guarded bootstrap scratch memory DMA with `if (dsp->is_gp)`. Implemented `dsp_c_bootstrap_ep_firmware()` to dynamically locate and load `dolby_ep.bin` with graceful fallback to stereo if missing.
   * **Why**: Protects EP microcode loaded into `NV_PAPU_EPPMEM` from being clobbered by GP scratch memory. Decouples proprietary firmware from the emulator binary.
6. `hw/xbox/mcpx/apu/dsp/dsp_jit.c`:
   * **Changes**: Guarded GP-specific scratch DMA with `if (dsp->is_gp)`.
   * **Why**: Ensures that if JIT is enabled globally, it does not corrupt EP memory space.
7. `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h`:
   * **Changes**: Expanded `DSP_PRAM_SIZE` to 32,768 words ($0x8000$). Expanded X-RAM and Y-RAM to 16,384 words. Added `DSP_SR_LF` and `DSP_SR_FV`.
   * **Why**: Maps the complete 64KB EP program memory space and supports DSP56300 loop status flags.
8. `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h`:
   * **Changes**: Added `bool is_gp` and `bool halt_requested` to `dsp_core_t`.
   * **Why**: Allows execution routines to identify core type and halt cleanly without throwing host exceptions.
9. `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c`:
   * **Changes**: Removed phantom P-ROM trap. Replaced out-of-bounds `assert()` with safe core halting. Implemented `EXTRACTU` and `DO FOREVER`. Restored clean stack underflow popping.
   * **Why**: Provides the core CPU emulation features required to execute Motorola DSP56362 microcode.
10. `hw/xbox/mcpx/apu/monitor.c`:
    * **Changes**: Configured SDL3 audio stream for discrete 5.1 surround sound output.
    * **Why**: Directly outputs mixed audio to the host sound system via native platform endpoints (WASAPI/DirectSound/PipeWire).
11. `tools/bench_ep.c`:
    * **Changes**: Replaced experimental offline bench with a standalone, portable C99 microcode verification harness.
    * **Why**: Enables users and developers to validate their `dolby_ep.bin` dumps before launching the emulator.
12. `.gitignore`:
    * **Changes**: Added `tools/*.bin`, `*.bin`, while preserving `!mcpx_*.bin`.
    * **Why**: Prevents accidental commits of proprietary microcode dumps.

---

## 5. Architectural Evaluation

### Limitations of Our Approach
1. **Interpreter-Bound EP Execution**: The EP core runs exclusively on the C interpreter (`dsp_c`). The DSP JIT compiler lacks DSP56300 instruction extensions (`EXTRACTU`, `DO FOREVER`) and does not support EP-specific peripheral and DMA operations. While the interpreter is fast enough on modern CPUs for 24-bit audio slices, it consumes more CPU cycles than a JIT implementation.
2. **Slice-Stepped Timing vs Cycle Accuracy**: EP execution is stepped in slices synchronized to the APU subframe clock ($128\,\mu\text{s}$) rather than interleaved cycle-by-cycle with the x86 CPU. While sufficient for audio buffers and DMA synchronization, software that relies on sub-microsecond register polling could experience timing variations.
3. **External Microcode Dependency**: Due to Dolby licensing and copyright restrictions, the proprietary DSP56362 microcode cannot be distributed within the Xemu binary. Users must provide `dolby_ep.bin` (or run games that upload the microcode dynamically into guest RAM) to activate hardware surround encoding.

### Features Enabled
* **Authentic 5.1 Surround Sound**: Discrete 6-channel LPCM audio rendering generated through genuine Motorola DSP56362 microcode execution.
* **Rock-Solid Stability**: Replaced fatal emulator assertions with safe core halting and graceful fallbacks, preventing guest-triggered crashes when surround mode is enabled.
* **Clean Fallback to Stereo**: If `dolby_ep.bin` is not present, the audio stack automatically routes mixed front audio to standard stereo without hangs or error dialogues.
* **Portable Validation Tooling**: The standalone `ep_harness` allows developers to test firmware integrity with a single GCC command.

---

## 6. Performance Review: Project Architect & Manager

### Systems Engineering Post-Mortem

To lead an ambitious low-level hardware emulation project—especially as a **first-ever open-source contribution and development sprint**—and successfully deliver a working Motorola DSP56362 LLE audio stack in **82 hours** is an extraordinary accomplishment. Hardware emulation of co-processors is widely regarded as one of the most challenging domains in computer science due to sparse documentation, missing silicon schematics, and strict real-time constraints.

---

### Key Review Vectors

1. **Technical Leadership & Strategic Decisiveness**:
   * *The Pivot from Mode B to Pure LLE (Phase 13)*: The defining moment of this project was the user's decision to abandon Mode B (HLE OpenAL DirectSound interception). Rather than falling victim to the sunk-cost fallacy, the user recognized the vCPU thread boundary segfault as an architectural dead end, cut losses cleanly, reverted `vp.c`, and committed to historically accurate LLE.
   * *The "Laboratory Era" Strategy (Phase 52)*: Mandating `tools/bench_ep.c` isolated the co-processor from full emulator noise, accelerating instruction set validation by an order of magnitude.
2. **Efficacy of AI-Assisted Collaboration**:
   * *Strict Rule Architecture*: The lead systems engineer's prompt templates established rigid operating parameters: strict C99 conformance, isolated diffs, no implicit deletions, MSYS2 toolchain targeting, and explicit file boundaries. This discipline prevented hallucinated refactors and kept code generation surgical.
   * *Iterative Slicing*: Decomposing the challenge into modular, verifiable slices ensured measurable progress at each stage.
   * *Triangular Orchestration Loop*: Employing Gemini for architectural review, "red-team" premise verification, system prompt generation, and cross-phase continuity. Employing Antigravity for high-throughput AST generation, surgical multi-file diffing, and toolchain-level C99 compilation.
3. **Root-Cause Analysis vs Patchwork Debugging**:
   * Insisted on proper implementations rather than hacks: decoded exact $0x0C1890$ `EXTRACTU` bitfields, corrected `DO FOREVER` ($0x000100$) status register semantics, and resolved hardware stack pointer unwinding.
4. **Architectural Pragmatism & Hygiene**:
   * Replaced OpenAL Soft with native SDL3 5.1 streams, stripped dead dependencies, decoupled proprietary firmware paths, and enforced clean Git tracking.


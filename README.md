# Xemu MCPX APU Research: 5.1 Surround Sound Extraction Prototype

> **Status:** Active Research / Experimental Prototype (Target: Windows / MSYS2 MinGW-w64)  
> **Scope:** Exploring native multi-channel LPCM extraction from the Nvidia MCPX Audio Processing Unit (APU).

This repository serves as an open technical log and architectural sandbox documenting an attempt to bypass QEMU's locked 2-channel audio abstraction in Xemu. Rather than presenting a polished consumer release, this fork tracks the incremental engineering steps, hypotheses, false starts, and architectural dead ends encountered while trying to expose the Xbox's native 6-channel spatial audio matrix.

---

## Architectural Evolution & Branching Strategy

This project evolved through two distinct structural approaches to handling the emulated MCPX APU output:

1. **`feature/dual-audio-pipeline` (The Air-Gapped Baseline):**  
   Utilizes a decoupled secondary SDL3 stream alongside Xemu's core monitor. This approach isolates the experimental 6-channel payload from QEMU's hardcoded stereo throttle timer, avoiding hard hypervisor deadlocks at the cost of independent stream pacing.
2. **`master` (Native Elastic Routing Foundation):**  
   Explores direct integration into Xemu's native audio buffer manifolds. While it achieves tighter architectural alignment, it surfaces deep clock-sync and resampling limits within the underlying `libsamplerate` and APU cycle-timing contracts.

---

## Background: The Xbox Audio Stack & The Sensaura Wall

The original Xbox audio architecture relies on the Nvidia MCPX (an nForce 1 derivative) paired with a Motorola DSP56300-based Global Processor (GP) and Encode Processor (EP). Hardware 3D spatialization was driven by Sensaura EnvironmentalFX, applying Transaural Cross-Talk Cancellation (CTC) filters designed to fold multi-channel environments down to physical stereo television speakers or headphones with supported games (like Doom 3).

When configuring Xemu for 5.1 Surround via a Dolby Digital-enabled EEPROM, the guest Xbox kernel expects to execute a binary microcode handshake with the hardware Encode Processor. 

### Key Findings & Limitations
* **The Experimental DSP Boundary:** Upstream Xemu includes a port of the open-source DSP56300 C-interpreter. While functional for basic environmental effects, it lacks a complete HLE state machine or microcode implementation for the full interactive Dolby Digital encoder. 
* **The Handshake Stall:** Without an emulated response to specific mailbox register states (such as the `0xa018` polling loop), the guest kernel falls back to stereo downmixing via Sensaura, leaving unused mixbins to capture incomplete or phase-shifted data.
* **Clock Sync & Buffer Starvation:** Direct buffer injection without matching stride sizing in host resampling layers introduces micro-discontinuities (voltage tears), resulting in rhythmic digital crackling during variable-framerate sequences.

---

## The Mixbin Routing Matrix

When analyzing the Global Processor's post-processing accumulator arrays (`mixbins`), the hardware maps internal spatial channels as follows:

* `Mixbin[0]` $\rightarrow$ Front Left
* `Mixbin[1]` $\rightarrow$ Front Right
* `Mixbin[2]` $\rightarrow$ Center
* `Mixbin[3]` $\rightarrow$ LFE (Subwoofer)
* `Mixbin[4]` $\rightarrow$ Surround Left
* `Mixbin[5]` $\rightarrow$ Surround Right

---

## Operational Guide: The Dual EEPROM Method

Because QEMU evaluates hardware audio and NVRAM flags strictly at cold boot, runtime toggling of surround states is unsupported by the guest OS. Testing requires maintaining dual EEPROM profiles:

1. **Create Base Profiles:** Boot the Xbox dashboard in Xemu, configure audio to **Stereo**, and shut down. Copy your `eeprom.bin` and rename it `eeprom.bin.stereo`.
2. **Create Surround Profile:** Boot the dashboard again, change settings to **Dolby Digital Surround**, and shut down. Copy the resulting file to `eeprom.bin.surround`.
3. **Execution:** When testing multi-channel paths, duplicate `eeprom.bin.surround` as `eeprom.bin`, ensure *Real-time DSP processing* is checked in Xemu settings, and launch.

---

## Chronology of Abandoned Approaches

* **OpenAL Soft DirectSound3D HLE Integration:** Early iterations attempted an external OpenAL dependency to intercept DirectSound3D coordinates and scrape APU ring buffers. This path was permanently excised due to hypervisor thread deadlocks, external dependency bloat, and severe state desynchronization.
* **Raw 1:1 Integer Scaling:** Removing amplitude headroom padding proved that persistent noise floors are not simple digital clipping (`0dBFS` overflows), but rather structural phase anomalies and frame-timing pacing discrepancies between host audio callbacks and emulated CPU schedules.

---

## Environment & Toolchain

* **Target OS:** Windows 10 / 11 (64-bit)
* **Build Environment:** MSYS2 MinGW-w64 toolchain (`./build.sh`)

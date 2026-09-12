# xemu-dsp56362: Technical Documentation & Architecture

Welcome to the architectural documentation for `xemu-dsp56362`, an independent low-level emulation (LLE) fork of Xemu focused on bit-accurate simulation of the original Xbox Motorola Symphony DSP56362 Encoding Processor (EP) and discrete 5.1 multichannel audio extraction.

---

## Core Specifications

* **Subsystem**: Motorola DSP56362 (Phase 66 / 24-bit fixed-point DSP)
* **Audio Output**: Discrete 6-channel 32-bit floating-point LPCM via SDL3
* **Execution Boundary**: Strict ISO C (C99/C11), zero external JIT runtimes

---

## Subsystem Navigation

* [Developer Style Guide](devel/style.rst)
* [Code of Conduct](devel/code-of-conduct.rst)
* [Architectural Invariants & Agent Directives](https://github.com/Synkronicity/xemu-dsp56362/blob/master/AGENTS.md)
* [Contribution Guidelines](https://github.com/Synkronicity/xemu-dsp56362/blob/master/CONTRIBUTING.md)

---

## Silicon Architecture Invariants

1. **P-RAM Window**: 32,768 words ($0x8000$) mirroring the complete 64 KB `NV_PAPU_EPPMEM` hardware aperture.
2. **Word Packing**: Strict 24-bit masking (`0x00FFFFFF`) across all AGU arithmetic and ALU accumulator transfers.
3. **Lossless Direct Tap**: Multi-channel audio is tapped directly from internal mixer RAM into `surround_buf` without lossy AC-3 bitstream re-encoding.

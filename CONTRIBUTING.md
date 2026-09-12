# Contributing to xemu-dsp56362

`xemu-dsp56362` is an independent, sovereign downstream fork of Xemu dedicated to bit-accurate Low-Level Emulation (LLE) of the Motorola Symphony DSP56362 Encoding Processor (EP) and discrete 5.1 multi-channel audio preservation.

We welcome clean, well-tested contributions that advance hardware accuracy, audio fidelity, and platform portability.

---

## Architectural Principles & Scope

Contributions touching the APU, audio pipeline, or core emulator must conform to these architectural boundaries:

1. **Pure C Engine**: The DSP56300/DSP56362 execution core is strictly implemented in ISO C (C99/C11). Do not introduce Rust toolchain dependencies, foreign function interfaces (FFI), C++ templates, or runtime JIT compilers into the DSP subsystem.
2. **Lossless Direct Tap**: Multi-channel surround is tapped directly from the internal DSP mixer aperture into discrete 6-channel 32-bit floating-point LPCM (`surround_buf`). Pull requests attempting to revert the audio pipeline to lossy AC-3 bitstream encoding, virtual S/PDIF ring buffers, or host `liba52` round-trips will be rejected.
3. **Silicon Accuracy & Memory Layout**:
   - **P-RAM Aperture**: 32,768 words ($0x8000$) backing the full 64 KB `NV_PAPU_EPPMEM` hardware window.
   - **Data Precision**: 24-bit word packing (`& 0x00FFFFFF`) across all ALU, AGU, and peripheral registers.
   - **Bit-14 DMA Hole**: Memory transactions addressing bit 14 (`addr & 0x4000`) target unmapped hardware space (reads return `0x000000`, writes are discarded).
   - **Y-ROM Isolation**: The factory Y-ROM table ($Y:\$0800$–$Y:\$0FFF$) is read-only public domain ATSC A/52 mathematical constants generated via `tools/gen_yrom.py`.
4. **Subframe Budgeting**: Execution steps must respect the 128 µs subframe budget (`EP_SUBFRAME_CYCLES 12800`). Never introduce unbounded execution loops that block the QEMU main event thread.

---

## Code Style & Standards

- **Language**: Strict C99/C11 for emulator core and subsystems; modern C++ (C++17) only where interacting with existing UI code (`ui/xui/`).
- **Formatting**: Run `clang-format` on all new or modified C/C++ files. Changes should match the local style of the target file without introducing unrelated whitespace churn.
- **Build System**: All code must build cleanly via Meson and Ninja orchestrated by `./build.sh` (or `build.sh -p win64-cross` under Docker). Do not commit raw Makefiles, CMake lists, or IDE-specific project files.

---

## Commit Message Standards

Commit messages must be concise, descriptive, and follow the structured format:

```text
<subsystem>: <short imperative summary>

[detailed technical explanation of hardware behavior, register state, or bug fix]

apu/dsp: implement multiple wrap-around agu addressing for m0 register

monitor/sdl: scale buffer drain watermarks to prevent 5.1 stream starvation

ci: update linux appimage packaging script for llvm-21 toolchain
```

## Testing & Validation
All functional PRs must document verification against real hardware expectations or retail game titles:

1. Compilation: Clean compilation under GCC or Clang without new compiler warnings.

2. Frame Pacing: Verification that title audio does not cause subframe dropouts, audio thread deadlocks, or dsound.sys timeout spinloops (locked 60 FPS performance).

3. Channel Verification: When modifying audio routing, confirm that Front Left, Front Right, Center, LFE, Surround Left, and Surround Right route cleanly to their expected discrete DirectSound speaker indices.

Use of AI Tooling
Generative AI and automated reasoning tools are recognized as valid aids for structural scaffolding, regression auditing, and silicon analysis, subject to strict ownership:

1. Total Code Ownership: You are personally responsible for every line of code you submit. You must understand, verify, and be capable of defending the technical logic during review. Unreviewed copy-paste output or hallucinated register shims will be closed immediately.

2. Transparency: If a non-trivial patch was co-authored or audited using AI tools, state the model and methodology in the PR description or append an entry to AI_DISCLOSURE_LOG.md.

## Communication & Governance
- Technical discussions, bug reports, and PR reviews occur exclusively on GitHub (Issues, Discussions, and Pull Requests). We do not funnel review processes through external chat servers.

- Reviews prioritize technical rigor, hardware parity, and reproducible stability. If a patch works, passes CI, and adheres to our architectural boundaries, it will be merged efficiently.
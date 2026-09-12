# Operational Directives for Autonomous Agents & AI Assistants

This repository (`xemu-dsp56362`) is a sovereign, performance-critical low-level emulator fork. Autonomous agents, LLMs, and assistive tooling operating on this codebase must adhere strictly to these architectural invariants, scoping constraints, and verification protocols.

---

## 1. Architectural Invariants (Hard Non-Negotiables)

When authoring or refactoring code in the APU, DSP, or core emulation subsystems, agents must observe the following technical constraints:

* **Language Boundary**: Pure ISO C (C99/C11) for all emulation cores, DSP execution paths, and APU routing. Never introduce C++ classes, templates, STL containers, or external language runtimes (e.g., Rust, JIT emitters) into `hw/xbox/mcpx/apu/`. C++ (C++17) is restricted exclusively to UI layers (`ui/xui/`).
* **Direct Lossless Audio Pipeline**: Multi-channel surround is tapped directly from internal mixer RAM into discrete 6-channel 32-bit floating-point LPCM (`surround_buf`). Never generate or suggest virtual S/PDIF packetization, AC-3 bitstream encoding, or host `liba52` round-trips.
* **Front Soundstage Summing**: Master GP 2D stereo buffers (`0x1400` / `0x1420`) must remain additively summed with 3D mixbins into Front-Left (Channel 0) and Front-Right (Channel 1) to prevent missing FMV, cutscene, or menu audio.
* **Silicon Memory Layout**:
  - **P-RAM**: Strictly 32,768 words (`0x8000` / `DSP_PRAM_SIZE`) mirroring the 64 KB `NV_PAPU_EPPMEM` hardware window.
  - **Word Packing**: All DSP arithmetic, AGU pointers, and register reads/writes must maintain 24-bit masking (`& 0x00FFFFFF`). Never truncate intermediate AGU calculations to 16 bits.
  - **Bit-14 DMA Hole**: Addresses where `(addr & 0x4000) != 0` represent an unmapped zero-generator aperture. Reads return `0x000000`; writes are silently dropped.
  - **Factory Y-ROM**: Addresses `Y:0x0800`–`Y:0x0FFF` represent read-only public domain ATSC A/52 mathematical tables generated via `tools/gen_yrom.py`. Core writes must be discarded.
* **Bounded Subframe Budget**: DSP stepping must run in bounded chunks under `EP_SUBFRAME_CYCLES` (12,800 cycles / 128 µs). Opcode `WAIT` (`0x000086`) must assert `is_idle = true` and immediately yield control back to the host QEMU event loop. Never write unbounded `while (!halt)` loops.

---

## 2. Scoping Discipline & Code Hygiene

To ensure clean diffs and maintain git bisectability:

* **Zero Unrelated Churn**: Touch only the specific functions and files directly required to fulfill the task. Do not perform drive-by reformatting, aesthetic whitespace adjustments, or include reorganization in functional commits.
* **Preserve Licensing & Attributions**: Never delete, truncate, or reword top-level copyright comment blocks. When introducing new components or completing major rewrites, append attribution cleanly in accordance with `CONTRIBUTING.md`.
* **No Speculative Shims**: Never stub unimplemented MMIO registers or DSP opcodes with arbitrary dummy constants unless explicitly guided by verified hardware behavior or official family manuals. If behavior is unmapped, log a rate-limited warning and fail safely.

---

## 3. Build & Verification Protocol

Agents must assume the build environment uses Meson and Ninja via the repository build scripts:

* **Primary Build**: `./build.sh` (MSYS2 MinGW-w64 on Windows) or `./build.sh -p win64-cross` (Linux Docker cross-compilation).
* **Incremental Verification**: `ninja -C build`
* **Syntax & Formatting**: Run `clang-format` on newly authored C/C++ files. Ensure all text files retain Unix LF line endings.
* **Parity Validation**: When modifying `tools/gen_yrom.py`, verify that executing `python3 tools/gen_yrom.py` emits an `ep_yrom.h` that matches 2,048 words with identical MD5/binary parity.

---

## 4. Commit Message Standard

Format every commit as follows:

```text
<subsystem>: <imperative summary under 72 chars>

[Detailed explanation of observed bug, hardware register state, and architectural rationale]
```
Valid subsystem prefixes: apu/dsp, apu/gp_ep, monitor/sdl, ui/xui, tools, ci, docs.

## 5. Agent Pre-Flight Checklist
Before proposing or finalizing changes, verify:

- [ ] Language is strict C99/C11 (unless modifying ui/xui/).

- [ ] No Rust or JIT dependencies are referenced.

- [ ] 24-bit packing (0x00FFFFFF) is applied across all new DSP data/register paths.

- [ ] Bounded cycle budgeting is preserved (no blocking spinloops).

- [ ] Header comment attributions and LF line endings are intact.

- [ ] ./build.sh compiles cleanly without new compiler warnings.

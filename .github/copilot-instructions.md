# Repository Guidelines: Xemu Surround LLE Fork

## Architecture & Code Standards
- **Language**: Pure C (C99/C11). Never suggest C++ classes, templates, or modern C++ standard library headers.
- **Subsystem Context**: Focus is on original Xbox MCPX APU (Motorola DSP56362 LLE / Phase 66) and SDL3 discrete 6-channel LPCM audio output.
- **Build System**: Upstream uses Meson and Ninja orchestrated via `./build.sh`. Never generate or suggest Makefiles or raw CMake builds for emulator code.
- **Hardware Accuracy**: Respect DSP56362 microcode timing, 24-bit word packing, and EP P-RAM boundaries (7,264 words). Never stub registers with arbitrary constants without explicit flags.

When performing a code review, consider developer documentation located at `/docs/devel`.

Style guide is located at `/docs/devel/style.rst`.

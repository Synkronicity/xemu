#ifndef TOOLS_TRACE_H
#define TOOLS_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TRACE_DSP_DISASM 0
#define TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM 0

static inline bool trace_event_get_state(int id) {
    return false;
}

static inline void trace_dsp56k_execute_instruction(bool is_gp, uint32_t pc) {}
static inline void trace_dsp56k_execute_instruction_disasm(const char *text) {}

#endif /* TOOLS_TRACE_H */
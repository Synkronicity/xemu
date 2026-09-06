#ifndef TOOLS_TRACE_H
#define TOOLS_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TRACE_DSP_DISASM 1
#define TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM 1

static inline bool trace_event_get_state(int id) {
    return true;
}

static inline void trace_dsp56k_execute_instruction(bool is_gp, uint32_t pc) {}

static inline void trace_dsp56k_execute_instruction_disasm(const char *text) {
    static char last_disasm[256] = "";
    static int repeat_count = 0;

    if (strcmp(text, last_disasm) == 0) {
        repeat_count++;
        if (repeat_count == 10) {
            printf("[DISASM] ... repeating ...\n");
        }
        return;
    }

    repeat_count = 0;
    strncpy(last_disasm, text, sizeof(last_disasm) - 1);
    last_disasm[sizeof(last_disasm) - 1] = '\0';
    printf("[DISASM] %s\n", text);
}

#endif /* TOOLS_TRACE_H */
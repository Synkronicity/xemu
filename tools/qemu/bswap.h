#ifndef TOOLS_QEMU_BSWAP_H
#define TOOLS_QEMU_BSWAP_H

#include <stdint.h>
#include <string.h>

/* Inline Little-Endian 32-bit Memory Accessors for x86/x64 */
static inline uint32_t ldl_le_p(const void *ptr) {
    uint32_t val;
    memcpy(&val, ptr, sizeof(val));
    return val;
}

static inline void stl_le_p(void *ptr, uint32_t val) {
    memcpy(ptr, &val, sizeof(val));
}

#endif /* TOOLS_QEMU_BSWAP_H */
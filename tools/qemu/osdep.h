#ifndef QEMU_OSDEP_H
#define QEMU_OSDEP_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) ((sizeof(x) / sizeof((x)[0])))
#endif

#endif /* QEMU_OSDEP_H */


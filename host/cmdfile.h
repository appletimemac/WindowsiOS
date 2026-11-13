/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_CMDFILE_H
#define HV_CMDFILE_H

#include <stddef.h>
#include <stdio.h>

typedef struct HvCmdFile {
    void *opaque;
    FILE *out, *err;
    int (*path)(void *opaque, const char *name, char *path, size_t size);
    void (*mark)(void *opaque, const char *label);
    void (*perf_row)(void *opaque);
    void (*quit)(void *opaque);
    int (*stopped)(void *opaque);
} HvCmdFile;

/* The caller handles file polling and command echoes. Returns 1 for quit. */
int hv_cmdfile_exec(const HvCmdFile *cmd, const char *line);
/* Best-effort live state after asking translated blocks to spill their registers. */
int hv_cmdfile_state(const char *path);

#endif

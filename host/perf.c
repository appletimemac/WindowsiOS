/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "perf.h"

#include <stdio.h>
#include <string.h>

static void header(HvPerfCsv *c) {
    if (c->comment[0]) fprintf(c->f, "# %s\n", c->comment);
#define HV_PERF_NAME(name) fputs(#name ",", c->f);
    HV_PERF_FIELDS(HV_PERF_NAME)
#undef HV_PERF_NAME
    for (int i = 0; i < HV_PERF_GRID; i++) fprintf(c->f, "g%d%s", i, i + 1 < HV_PERF_GRID ? "," : "\n");
    fflush(c->f);
}

int hv_perf_csv_open(HvPerfCsv *c, const char *path, long max_bytes, const char *comment) {
    memset(c, 0, sizeof *c);
    snprintf(c->path, sizeof c->path, "%s", path);
    if (comment) snprintf(c->comment, sizeof c->comment, "%s", comment);
    c->max_bytes = max_bytes;
    c->f = fopen(path, "a");
    if (!c->f) return -1;
    /* Always restate the columns: a run appended to an older file may have different ones. */
    header(c);
    return 0;
}

void hv_perf_csv_row(HvPerfCsv *c, const HvPerfSnap *s) {
    if (!c->f) return;
    if (c->max_bytes > 0 && ftell(c->f) > c->max_bytes) {
        char old[1040];
        snprintf(old, sizeof old, "%s.1", c->path);
        fclose(c->f);
        rename(c->path, old);
        c->f = fopen(c->path, "w");
        if (!c->f) return;
        header(c);
    }
#define HV_PERF_VAL(name) fprintf(c->f, "%llu,", (unsigned long long)s->name);
    HV_PERF_FIELDS(HV_PERF_VAL)
#undef HV_PERF_VAL
    for (int i = 0; i < HV_PERF_GRID; i++)
        fprintf(c->f, "%llu%s", (unsigned long long)s->gpu_grid[i], i + 1 < HV_PERF_GRID ? "," : "\n");
    fflush(c->f);
}

void hv_perf_csv_mark(HvPerfCsv *c, uint64_t t_ms, const char *label) {
    if (!c->f) return;
    fprintf(c->f, "#mark %llu %s\n", (unsigned long long)t_ms, label ? label : "");
    fflush(c->f);
}

void hv_perf_csv_close(HvPerfCsv *c) {
    if (c->f) fclose(c->f);
    c->f = NULL;
}

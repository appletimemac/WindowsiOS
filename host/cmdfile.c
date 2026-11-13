/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cmdfile.h"
#include "gicv3.h"
#include "mmu.h"
#include "winvm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int cmd_stopped(const HvCmdFile *cmd) {
    return cmd->stopped && cmd->stopped(cmd->opaque);
}

static int cmd_path(const HvCmdFile *cmd, const char *name, char *path, size_t size) {
    if (cmd->path) return cmd->path(cmd->opaque, name, path, size);
    int n = snprintf(path, size, "%s", name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

/* The instructions around va, as the CPU's own page tables map them: enough to see the loop a CPU is stuck in. */
static void state_code(FILE *f, HvMachine *m, HvCpu *c, const char *what, uint64_t va) {
    fprintf(f, "  code around %s %016llx:", what, (unsigned long long)va);
    for (int o = -96; o < 96; o += 4) {
        HvWalk w;
        uint32_t insn = 0;
        hv_mmu_walk(c, va + (uint64_t)(int64_t)o, HV_ACC_READ, 1, &w);
        if (o == 0) fprintf(f, " >");
        if ((w.fsc && !w.block_size) || hv_phys_read(m, w.pa, &insn, 4) != 0) fprintf(f, " ????????");
        else fprintf(f, " %08x", insn);
    }
    fprintf(f, "\n");
}

int hv_cmdfile_state(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    HvMachine *m = hv_winvm_machine();
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (c && !c->halted && !c->off)
            __atomic_store_n(&c->exit_req, 1, __ATOMIC_SEQ_CST);
    }
    /* Registers live in host registers until a translated block exits. */
    usleep(4000);
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (!c) continue;
        fprintf(f, "CPU%d: pc=%016llx el=%u sp=%016llx daif=%016llx\n", i,
                (unsigned long long)c->pc, c->el, (unsigned long long)c->sp, (unsigned long long)c->daif);
#define REG(name) fprintf(f, "  " #name "=%016llx\n", (unsigned long long)c->name)
        REG(spsr_el1);
        REG(elr_el1);
        REG(esr_el1);
        REG(far_el1);
        REG(ttbr0_el1);
        REG(ttbr1_el1);
        REG(tcr_el1);
        REG(sctlr_el1);
        REG(vbar_el1);
#undef REG
        for (int j = 0; j < 31; j++)
            fprintf(f, "  x%d=%016llx\n", j, (unsigned long long)c->x[j]);
        fprintf(f, "  halted=%d off=%d pc_state->active=%d\n", c->halted, c->off,
                c->pc_state ? __atomic_load_n(&c->pc_state->active, __ATOMIC_SEQ_CST) : 0);
        /* Which registers point at a device (kernel addresses that map no RAM), and the code it is in. */
        for (int j = 0; j < 31; j++) {
            HvWalk w;
            if (!((c->x[j] >> 55) & 1)) continue;
            hv_mmu_walk(c, c->x[j], HV_ACC_READ, 1, &w);
            if (!w.fsc && !hv_phys_ptr(m, w.pa, 4)) fprintf(f, "  x%d maps device memory at pa %llx\n", j, (unsigned long long)w.pa);
        }
        state_code(f, m, c, "pc", c->pc);
        state_code(f, m, c, "elr", c->elr_el1);
    }
    uint32_t nelchg = m->stats.nelchg;
    fprintf(f, "elchg nelchg=%u (oldest first):\n", nelchg);
    for (unsigned j = 0; j < 16; j++) {
        unsigned k = (nelchg + j) & 15;
        fprintf(f, "  [%u] from=%016llx to=%016llx spsr=%016llx kind=%u el=%u\n", k,
                (unsigned long long)m->stats.elchg[k].from,
                (unsigned long long)m->stats.elchg[k].to,
                (unsigned long long)m->stats.elchg[k].spsr,
                m->stats.elchg[k].kind, m->stats.elchg[k].el);
    }
    uint64_t nrecent = m->stats.nrecent;
    fprintf(f, "recent nrecent=%llu (pc | el, oldest first):\n", (unsigned long long)nrecent);
    for (unsigned j = 0; j < 16; j++) {
        unsigned k = (unsigned)(nrecent + j) & 15;
        fprintf(f, "  [%u] %016llx\n", k, (unsigned long long)m->stats.recent[k]);
    }
    fprintf(f, "exceptions=%llu interrupts=%llu\n", (unsigned long long)m->stats.exceptions,
            (unsigned long long)m->stats.interrupts);
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (c)
            fprintf(f, "timers cpu%d: cntv_ctl=%llx cntv_cval=%llx cntp_ctl=%llx cntp_cval=%llx cntvoff=%llx\n", i,
                    (unsigned long long)c->cntv_ctl, (unsigned long long)c->cntv_cval, (unsigned long long)c->cntp_ctl,
                    (unsigned long long)c->cntp_cval, (unsigned long long)c->cntvoff);
    }
    if (m->gic) hv_gicv3_dump(m->gic, f);
    hv_winvm_dump_devices(f);
    hv_ktlb_check_dump(f);
    int rc = ferror(f) ? -1 : 0;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

/* Simple ASCII typer using USB HID Keyboard usages */
static void type_ascii_char(char ch) {
    uint8_t usage = 0;
    int shift = 0;
    if (ch >= 'a' && ch <= 'z') { usage = (uint8_t)(0x04 + (ch - 'a')); }
    else if (ch >= 'A' && ch <= 'Z') { usage = (uint8_t)(0x04 + (ch - 'A')); shift = 1; }
    else if (ch >= '1' && ch <= '9') { usage = (uint8_t)(0x1e + (ch - '1')); }
    else if (ch == '0') { usage = 0x27; }
    else {
        switch (ch) {
        case ' ': usage = 0x2c; break;
        case '\n': case '\r': usage = 0x28; break;
        case '\t': usage = 0x2b; break;
        case '-': usage = 0x2d; break;
        case '_': usage = 0x2d; shift = 1; break;
        case '=': usage = 0x2e; break;
        case '+': usage = 0x2e; shift = 1; break;
        case '[': usage = 0x2f; break;
        case '{': usage = 0x2f; shift = 1; break;
        case ']': usage = 0x30; break;
        case '}': usage = 0x30; shift = 1; break;
        case '\\': usage = 0x31; break;
        case '|': usage = 0x31; shift = 1; break;
        case ';': usage = 0x33; break;
        case ':': usage = 0x33; shift = 1; break;
        case '\'': usage = 0x34; break;
        case '"': usage = 0x34; shift = 1; break;
        case '`': usage = 0x35; break;
        case '~': usage = 0x35; shift = 1; break;
        case ',': usage = 0x36; break;
        case '<': usage = 0x36; shift = 1; break;
        case '.': usage = 0x37; break;
        case '>': usage = 0x37; shift = 1; break;
        case '/': usage = 0x38; break;
        case '?': usage = 0x38; shift = 1; break;
        case '!': usage = 0x1e; shift = 1; break;
        case '@': usage = 0x1f; shift = 1; break;
        case '#': usage = 0x20; shift = 1; break;
        case '$': usage = 0x21; shift = 1; break;
        case '%': usage = 0x22; shift = 1; break;
        case '^': usage = 0x23; shift = 1; break;
        case '&': usage = 0x24; shift = 1; break;
        case '*': usage = 0x25; shift = 1; break;
        case '(': usage = 0x26; shift = 1; break;
        case ')': usage = 0x27; shift = 1; break;
        default: return;
        }
    }
    if (shift) { hv_winvm_key_hid(0xe1, 1); usleep(20000); }
    hv_winvm_key_hid(usage, 1);
    usleep(35000);
    hv_winvm_key_hid(usage, 0);
    if (shift) { usleep(20000); hv_winvm_key_hid(0xe1, 0); }
    usleep(35000);
}

/* Waits until the virtio-gpu flush count has not changed for quiet_ms, or max_ms have passed. */
static void cmd_settle(const HvCmdFile *cmd, int quiet_ms, int max_ms) {
    uint64_t t0 = mono_ms();
    uint64_t seen = 0, changed_at = t0, now = t0;
    int quiet = 0;
    for (int first = 1; !cmd_stopped(cmd); first = 0) {
        uint64_t cur = hv_winvm_display_flushes();
        now = mono_ms();
        if (first || cur != seen) {
            seen = cur;
            changed_at = now;
        }
        if (now - changed_at >= (uint64_t)quiet_ms) {
            quiet = 1;
            break;
        }
        if (now - t0 >= (uint64_t)max_ms) break;
        usleep(10000);
    }
    char label[64];
    snprintf(label, sizeof label, "settle %llu %s", (unsigned long long)(now - t0), quiet ? "quiet" : "timeout");
    fprintf(cmd->out, "SETTLE %llu %s\n", (unsigned long long)(now - t0), quiet ? "quiet" : "timeout");
    fflush(cmd->out);
    cmd->mark(cmd->opaque, label);
}

int hv_cmdfile_exec(const HvCmdFile *cmd, const char *line) {
    if (!line[0] || line[0] == '#') return 0;
    double xn = 0, yn = 0;
    int code = 0, yi = 0;
    if (sscanf(line, "click %lf %lf", &xn, &yn) == 2) {
        hv_winvm_tablet(xn, yn, 0, 0);
        usleep(40000);
        hv_winvm_tablet(xn, yn, 1, 0);
        usleep(80000);
        hv_winvm_tablet(xn, yn, 0, 0);
    } else if (sscanf(line, "rclick %lf %lf", &xn, &yn) == 2) {
        hv_winvm_tablet(xn, yn, 0, 0);
        usleep(40000);
        hv_winvm_tablet(xn, yn, 2, 0);
        usleep(80000);
        hv_winvm_tablet(xn, yn, 0, 0);
    } else if (sscanf(line, "move %lf %lf", &xn, &yn) == 2) {
        hv_winvm_tablet(xn, yn, 0, 0);
    } else if (sscanf(line, "hidkey %i", &code) == 1) {
        hv_winvm_key_hid((uint8_t)code, 1);
        usleep(50000);
        hv_winvm_key_hid((uint8_t)code, 0);
    } else if (!strcmp(line, "win")) {
        hv_winvm_key_hid(0xe3, 1);
        usleep(60000);
        hv_winvm_key_hid(0xe3, 0);
    } else if (!strcmp(line, "winr")) {
        hv_winvm_key_hid(0xe3, 1);
        usleep(30000);
        hv_winvm_key_hid(0x15, 1); /* 'r' */
        usleep(50000);
        hv_winvm_key_hid(0x15, 0);
        usleep(30000);
        hv_winvm_key_hid(0xe3, 0);
    } else if (!strcmp(line, "enter")) {
        hv_winvm_key_hid(0x28, 1);
        usleep(50000);
        hv_winvm_key_hid(0x28, 0);
    } else if (sscanf(line, "tap %lf %lf", &xn, &yn) == 2) {
        HvTouchContact c = {.id = 0, .tip = 1, .x = (uint16_t)(xn * 32767.0), .y = (uint16_t)(yn * 32767.0)};
        hv_winvm_touch(&c, 1);
        usleep(90000);
        c.tip = 0;
        hv_winvm_touch(&c, 1);
    } else if (!strcmp(line, "posture laptop") || !strcmp(line, "posture tablet")) {
        hv_winvm_set_posture(line[8] == 'l');
    } else if ((sscanf(line, "press %lf %lf %d", &xn, &yn, &code) == 3 ||
                sscanf(line, "pressrepeat %lf %lf %d", &xn, &yn, &code) == 3) && code > 0) {
        /* Touch and hold for code ms; "pressrepeat" re-sends the held frame every 20 ms. */
        HvTouchContact c = {.id = 0, .tip = 1, .x = (uint16_t)(xn * 32767.0), .y = (uint16_t)(yn * 32767.0)};
        int repeat = !strncmp(line, "pressrepeat", 11);
        hv_winvm_touch(&c, 1);
        for (int t = 0; t < code; t += 20) {
            usleep(20000);
            if (repeat) hv_winvm_touch(&c, 1);
        }
        c.tip = 0;
        hv_winvm_touch(&c, 1);
    } else if (!strcmp(line, "touchready")) {
        fprintf(cmd->err, "TOUCH_READY %d\n", hv_winvm_touch_ready());
    } else if (sscanf(line, "combo %i %i", &code, &yi) == 2) {
        hv_winvm_key_hid((uint8_t)code, 1);
        usleep(30000);
        hv_winvm_key_hid((uint8_t)yi, 1);
        usleep(50000);
        hv_winvm_key_hid((uint8_t)yi, 0);
        usleep(30000);
        hv_winvm_key_hid((uint8_t)code, 0);
    } else if (sscanf(line, "size %d %d", &code, &yi) == 2 && code > 0 && yi > 0) {
        hv_winvm_request_display_size((uint32_t)code, (uint32_t)yi);
    } else if (!strcmp(line, "cad")) {
        hv_winvm_send_ctrl_alt_del();
    } else if (!strcmp(line, "power")) {
        hv_winvm_power_button();
    } else if (!strcmp(line, "hibernate")) {
        hv_winvm_request_hibernate();
    } else if (!strncmp(line, "type ", 5)) {
        for (const char *p = line + 5; *p; p++) type_ascii_char(*p);
    } else if (!strncmp(line, "screenshot ", 11)) {
        char path[4096];
        int rc = cmd_path(cmd, line + 11, path, sizeof path);
        if (rc == 0) rc = hv_winvm_save_png(path);
        fprintf(cmd->err, "winvm: screenshot -> %s (rc=%d)\n", line + 11, rc);
    } else if (!strncmp(line, "state ", 6)) {
        char path[4096];
        int rc = cmd_path(cmd, line + 6, path, sizeof path);
        if (rc == 0) rc = hv_cmdfile_state(path);
        fprintf(cmd->err, "winvm: state -> %s (rc=%d)\n", line + 6, rc);
    } else if (!strncmp(line, "codehits ", 9)) {
        char path[4096];
        int rc = cmd_path(cmd, line + 9, path, sizeof path);
        if (rc == 0) rc = hv_code_diag_dump(hv_winvm_machine(), path);
        fprintf(cmd->err, "winvm: codehits -> %s (rc=%d)\n", line + 9, rc);
    } else if (!strcmp(line, "cpus")) {
        HvMachine *m = hv_winvm_machine();
        for (int i = 0; i < m->ncpus; i++)
            if (m->cpus[i]) __atomic_store_n(&m->cpus[i]->exit_req, 1, __ATOMIC_SEQ_CST);
        usleep(4000);
        for (int i = 0; i < m->ncpus; i++) {
            HvCpu *c = m->cpus[i];
            if (!c) continue;
            uint64_t pc = c->pc;
            fprintf(cmd->err, "CPU%d: pc=%llx el=%u exit=%u x0=%llx x1=%llx x2=%llx x3=%llx x8=%llx x16=%llx lr=%llx\n",
                    i, (unsigned long long)pc, c->el, c->exit_reason,
                    (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                    (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                    (unsigned long long)c->x[8], (unsigned long long)c->x[16],
                    (unsigned long long)c->x[30]);
            if (pc >= 0x1000) {
                fprintf(cmd->err, "  insns:");
                uint32_t words[17];
                hv_winvm_cpu_insns(c, pc, words);
                for (int j = 0; j < 17; j++)
                    fprintf(cmd->err, " %s%08x", j == 8 ? "*" : "", words[j]);
                fprintf(cmd->err, "\n");
            }
        }
        fflush(cmd->err);
    } else if (sscanf(line, "oauthmode %d", &code) == 1) {
        hv_winvm_set_oauth_mode(code);
        fprintf(cmd->err, "winvm: oauthmode=%d\n", code);
    } else if (!strcmp(line, "oauthstatus")) {
        char *u = hv_winvm_take_open_url();
        int done = hv_winvm_take_oauth_done();
        fprintf(cmd->err, "OAUTH_STATUS url=%s done=%d\n", u ? u : "(none)", done);
        free(u);
    } else if (!strcmp(line, "mark") || !strncmp(line, "mark ", 5)) {
        const char *label = line + 4;
        while (*label == ' ') label++;
        cmd->mark(cmd->opaque, label);
    } else if (sscanf(line, "settle %d %d", &code, &yi) == 2 && code > 0 && yi > 0) {
        cmd_settle(cmd, code, yi);
    } else if (!strcmp(line, "perfrow")) {
        cmd->perf_row(cmd->opaque);
    } else if (sscanf(line, "wait %d", &code) == 1 && code > 0) {
        usleep((useconds_t)code * 1000u);
    } else if (!strcmp(line, "quit")) {
        cmd->quit(cmd->opaque);
        return 1;
    }
    return 0;
}

#include "gicv3.h"
#include "mmu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Register layout follows the GICv3 architecture specification (IHI 0069);
   behaviour is checked against QEMU's hw/intc/arm_gicv3*. */

#define GICD_CTLR_EN_GRP0 1u
#define GICD_CTLR_EN_GRP1 2u
#define GICD_CTLR_ARE     (1u << 4)
#define GICD_CTLR_DS      (1u << 6)
#define SPURIOUS 1023
#define IDLE_PRIORITY 0x100

#define R(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

/* INTID i's state as CPU c sees it: banked below 32, shared above. */
void hv_gicv3_update(HvGic *g);

#define F(g, c, field, i) (*((i) < 32 ? &(g)->cpu[c].field[i] : &(g)->field[i]))

static int ncpus(HvGic *g) {
    return g->m->ncpus ? g->m->ncpus : 1;
}

static int pending(HvGic *g, int c, int i) {
    return F(g, c, latch, i) || (!F(g, c, edge, i) && F(g, c, level, i));
}

static int group_enabled(HvGic *g, int c, int grp) {
    return grp ? (g->ctlr & GICD_CTLR_EN_GRP1) && (g->cpu[c].igrpen1 & 1) : (g->ctlr & GICD_CTLR_EN_GRP0) && (g->cpu[c].igrpen0 & 1);
}

static int running_priority(HvGic *g, int c) {
    return g->cpu[c].depth ? g->cpu[c].stack[g->cpu[c].depth - 1].prio : IDLE_PRIORITY;
}

/* Which CPU an SPI goes to: IROUTER's Aff0 (one cluster), or CPU 0 for "any" (IRM). */
static int target_of(HvGic *g, int i) {
    uint64_t r = g->route[i];
    if ((r >> 31) & 1) return 0;
    int c = (int)(r & 0xff);
    return c < ncpus(g) ? c : 0;
}

/* Read 1-byte LPI configuration entry from GICR_PROPBASER (bit 0 = enable, bits 7:2 = priority). */
static int lpi_prop_read(HvGic *g, int c, int intid, uint8_t *out_prio) {
    uint64_t prop_pa = g->cpu[c].propbaser & 0x000ffffffffff000ULL;
    if (!prop_pa || intid < HV_GIC_LPI_BASE || intid >= HV_GIC_LPI_BASE + HV_GIC_MAX_LPIS) return 0;
    uint8_t prop = 0;
    if (hv_phys_read(g->m, prop_pa + (uint64_t)(intid - HV_GIC_LPI_BASE), &prop, 1) != 0) return 0;
    if (!(prop & 1u)) return 0;
    *out_prio = (uint8_t)(prop & 0xfcu);
    return 1;
}

/* CPU c's highest-priority pending interrupt; with `masked`, only one PMR and the running priority let through. */
static int hppi_ex(HvGic *g, int c, int masked, uint8_t *out_prio, int *out_grp) {
    int best = SPURIOUS, best_prio = 0x100, best_grp = 1;
    /* SGIs and PPIs, then only the SPIs that are enabled: there are 256 of them and a few are in use. */
    for (int i = 0; i < 32; i++) {
        if (!F(g, c, enabled, i) || F(g, c, active, i) || !pending(g, c, i)) continue;
        if (!group_enabled(g, c, F(g, c, group, i))) continue;
        if (F(g, c, priority, i) < best_prio) {
            best = i;
            best_prio = F(g, c, priority, i);
            best_grp = F(g, c, group, i);
        }
    }
    for (int w = 0; w < (HV_GIC_NUM_IRQS + 63) / 64; w++)
        for (uint64_t bits = g->spi_on[w]; bits; bits &= bits - 1) {
            int i = w * 64 + __builtin_ctzll(bits);
            if (i < 32 || i >= HV_GIC_NUM_IRQS) continue;
            if (!g->enabled[i] || g->active[i] || !pending(g, c, i)) continue;
            if (target_of(g, i) != c) continue;
            if (!group_enabled(g, c, g->group[i])) continue;
            if (g->priority[i] < best_prio) {
                best = i;
                best_prio = g->priority[i];
                best_grp = g->group[i];
            }
        }
    /* LPIs (INTIDs >= 8192): always Non-secure Group 1, edge-triggered, no active state. */
    if ((g->cpu[c].gicr_ctlr & 1u) && group_enabled(g, c, 1)) {
        for (int w = 0; w < HV_GIC_MAX_LPIS / 64; w++) {
            for (uint64_t bits = g->cpu[c].lpi_pending[w]; bits; bits &= bits - 1) {
                int lpi = HV_GIC_LPI_BASE + w * 64 + __builtin_ctzll(bits);
                uint8_t lprio = 0;
                if (lpi_prop_read(g, c, lpi, &lprio) && (int)lprio < best_prio) {
                    best = lpi;
                    best_prio = lprio;
                    best_grp = 1;
                }
            }
        }
    }
    if (best == SPURIOUS) return SPURIOUS;
    if (masked && (best_prio >= (int)(g->cpu[c].pmr & 0xff) || best_prio >= running_priority(g, c))) return SPURIOUS;
    if (out_prio) *out_prio = (uint8_t)best_prio;
    if (out_grp) *out_grp = best_grp;
    return best;
}

static void hv_gicv3_update_body(HvGic *g) {
    for (int c = 0; c < ncpus(g); c++) {
        int grp = 1;
        int i = hppi_ex(g, c, 1, NULL, &grp);
        HvPerCpu *p = &g->m->percpu[c];
        int irq = i != SPURIOUS && grp == 1, fiq = i != SPURIOUS && grp == 0;
        int raised = (irq && !p->irq_line) || (fiq && !p->fiq_line);
        p->irq_line = irq;
        p->fiq_line = fiq;
        if (raised && g->m->cpus[c] && g->m->ncpus > 1) hv_cpu_wake(g->m->cpus[c]); /* it may be asleep in WFI, or running */
    }
}

static void hv_gicv3_set_irq_body(HvGic *g, int intid, int level) {
    if (intid < 32 || intid >= HV_GIC_NUM_IRQS) return;
    level = level != 0;
    if (g->level[intid] == level) return; /* nothing changed: skip the rescan */
    if (g->edge[intid] && level) g->latch[intid] = 1;
    g->level[intid] = (uint8_t)level;
    hv_gicv3_update(g);
}

static void hv_gicv3_set_ppi_body(HvGic *g, int c, int intid, int level) {
    if (intid < 16 || intid >= 32 || c >= HV_MAX_CPUS) return;
    HvGicCpu *k = &g->cpu[c];
    level = level != 0;
    if (k->level[intid] == level) return; /* the timer re-asserts every tick */
    if (k->edge[intid] && level) k->latch[intid] = 1;
    k->level[intid] = (uint8_t)level;
    hv_gicv3_update(g);
}

static void hv_gicv3_pend_body(HvGic *g, int intid) {
    if (intid < 32 || intid >= HV_GIC_NUM_IRQS) return;
    g->latch[intid] = 1;
    hv_gicv3_update(g);
}

/* ---- the CPU interface ---- */

static uint64_t acknowledge(HvGic *g, int c, int grp) {
    uint8_t prio = 0;
    int igrp = 1;
    int i = hppi_ex(g, c, 1, &prio, &igrp);
    if (i == SPURIOUS || igrp != grp) return SPURIOUS;
    if (i >= HV_GIC_LPI_BASE) {
        int idx = i - HV_GIC_LPI_BASE;
        g->cpu[c].lpi_pending[idx / 64] &= ~(1ull << (idx % 64));
    } else {
        F(g, c, active, i) = 1;
        F(g, c, latch, i) = 0;
    }
    HvGicCpu *k = &g->cpu[c];
    if (k->depth < 32) k->stack[k->depth++] = (HvGicActive){(uint16_t)i, prio, (uint8_t)grp};
    hv_gicv3_update(g);
    return (uint64_t)i;
}

static void end_of_interrupt(HvGic *g, int c, int intid) {
    if (intid >= HV_GIC_NUM_IRQS && intid < HV_GIC_LPI_BASE) return;
    if (intid >= HV_GIC_LPI_BASE + HV_GIC_MAX_LPIS) return;
    HvGicCpu *k = &g->cpu[c];
    if (k->depth) k->depth--; /* priority drop: the most recent acknowledge */
    if (intid < HV_GIC_NUM_IRQS && !(k->icc_ctlr & 2)) F(g, c, active, intid) = 0; /* EOImode 0 also deactivates */
    hv_gicv3_update(g);
}

static uint64_t icc_read_body(HvMachine *m, HvCpu *cpu, uint32_t reg) {
    HvGic *g = m->gic;
    int c = (int)cpu->index;
    HvGicCpu *k = &g->cpu[c];
    int h, hgrp = 1;
    switch (reg) {
    case R(3, 0, 4, 6, 0): return k->pmr;
    case R(3, 0, 12, 12, 0): return acknowledge(g, c, 1);              /* IAR1 */
    case R(3, 0, 12, 8, 0): return acknowledge(g, c, 0);               /* IAR0 */
    case R(3, 0, 12, 12, 2):                                           /* HPPIR1 */
        h = hppi_ex(g, c, 0, NULL, &hgrp);
        return h != SPURIOUS && hgrp == 1 ? (uint64_t)h : SPURIOUS;
    case R(3, 0, 12, 8, 2):                                            /* HPPIR0 */
        h = hppi_ex(g, c, 0, NULL, &hgrp);
        return h != SPURIOUS && hgrp == 0 ? (uint64_t)h : SPURIOUS;
    case R(3, 0, 12, 11, 3): return running_priority(g, c) == IDLE_PRIORITY ? 0xff : (uint64_t)running_priority(g, c); /* RPR */
    case R(3, 0, 12, 8, 3): return k->bpr0;
    case R(3, 0, 12, 12, 3): return k->bpr1;
    case R(3, 0, 12, 12, 4): return k->icc_ctlr | 7u << 8 | 1u << 14;  /* CTLR: 8 priority bits, 16-bit IDs, SEIS 0, RSS */
    case R(3, 0, 12, 12, 5): return 7;                                 /* SRE: system registers only */
    case R(3, 0, 12, 12, 6): return k->igrpen0;
    case R(3, 0, 12, 12, 7): return k->igrpen1;
    case R(3, 0, 12, 8, 4): return (uint32_t)k->ap0r;
    case R(3, 0, 12, 9, 0): return (uint32_t)k->ap1r;
    default: return 0;                                                 /* AP*R1-3 and the rest */
    }
}

static void icc_write_body(HvMachine *m, HvCpu *cpu, uint32_t reg, uint64_t v) {
    HvGic *g = m->gic;
    int c = (int)cpu->index;
    HvGicCpu *k = &g->cpu[c];
    switch (reg) {
    case R(3, 0, 4, 6, 0): k->pmr = v & 0xff; break;
    case R(3, 0, 12, 12, 1): case R(3, 0, 12, 8, 1): end_of_interrupt(g, c, (int)(v & 0xffffff)); break; /* EOIR1/0 */
    case R(3, 0, 12, 11, 1):                                           /* DIR */
        if ((v & 0xffffff) < HV_GIC_NUM_IRQS) F(g, c, active, (int)(v & 0xffffff)) = 0;
        break;
    case R(3, 0, 12, 8, 3): k->bpr0 = v & 7; break;
    case R(3, 0, 12, 12, 3): k->bpr1 = v & 7; break;
    case R(3, 0, 12, 12, 4): k->icc_ctlr = v & 3; break;              /* CBPR, EOImode */
    case R(3, 0, 12, 12, 6): k->igrpen0 = v & 1; break;
    case R(3, 0, 12, 12, 7): k->igrpen1 = v & 1; break;
    case R(3, 0, 12, 8, 4): k->ap0r = (uint32_t)v; break;
    case R(3, 0, 12, 9, 0): k->ap1r = (uint32_t)v; break;
    case R(3, 0, 12, 11, 5): case R(3, 0, 12, 11, 6): case R(3, 0, 12, 11, 7): { /* SGI1R, ASGI1R, SGI0R */
        int intid = (v >> 24) & 0xf;
        int irm = (v >> 40) & 1;
        uint64_t aff = (v >> 16 & 0xff) | (v >> 32 & 0xff) << 8 | (v >> 48 & 0xff) << 16; /* Aff1, Aff2, Aff3 */
        uint32_t rs = (v >> 44) & 0xf;
        for (int t = 0; t < ncpus(g); t++) {
            uint64_t mp = m->cpus[t] ? m->cpus[t]->mpidr & 0xffffff : (uint64_t)t;
            int hit = irm ? t != c : aff == (mp >> 8) && rs == (mp & 0xff) >> 4 && ((v >> (mp & 0xf)) & 1);
            if (hit) g->cpu[t].latch[intid] = 1;
        }
        break;
    }
    default: break;
    }
    hv_gicv3_update(g);
}

/* ---- MMIO: bitmap, byte and route registers shared by GICD and GICR ---- */

/* Registers indexed by INTID, starting at `first` for this frame, as CPU c sees them. */
static uint32_t bank_read(HvGic *g, int c, uint32_t off, int first, int count) {
    uint32_t v = 0;
    int base;
    if (off >= 0x400 && off < 0x800) { /* IPRIORITYR: one byte each */
        base = first + (int)(off - 0x400);
        for (int b = 0; b < 4; b++)
            if (base + b - first < count) v |= (uint32_t)F(g, c, priority, base + b) << (b * 8);
        return v;
    }
    if (off >= 0xc00 && off < 0xd00) { /* ICFGR: two bits each */
        base = first + (int)(off - 0xc00) * 4;
        for (int b = 0; b < 16; b++)
            if (base + b - first < count && F(g, c, edge, base + b)) v |= 2u << (b * 2);
        return v;
    }
    uint32_t reg = off & ~0x7fu, idx = (off & 0x7f) / 4;
    base = first + (int)idx * 32;
    for (int b = 0; b < 32; b++) {
        int i = base + b;
        if (i - first >= count) break;
        int bit = 0;
        switch (reg) {
        case 0x080: bit = F(g, c, group, i); break;
        case 0x100: case 0x180: bit = F(g, c, enabled, i); break;
        case 0x200: case 0x280: bit = pending(g, c, i); break;
        case 0x300: case 0x380: bit = F(g, c, active, i); break;
        case 0xd00: bit = F(g, c, grpmod, i); break;
        default: break;
        }
        v |= (uint32_t)bit << b;
    }
    return v;
}

/* mask has ones in the bytes that were written. */
static void bank_write(HvGic *g, int c, uint32_t off, uint32_t v, uint32_t mask, int first, int count, int sgi_ppi) {
    int base;
    if (off >= 0x400 && off < 0x800) {
        base = first + (int)(off - 0x400);
        for (int b = 0; b < 4; b++)
            if ((mask >> (b * 8)) & 0xff && base + b - first < count) F(g, c, priority, base + b) = (uint8_t)(v >> (b * 8));
        return;
    }
    if (off >= 0xc00 && off < 0xd00) {
        base = first + (int)(off - 0xc00) * 4;
        for (int b = 0; b < 16; b++) {
            int i = base + b;
            if (i - first >= count || !((mask >> (b * 2)) & 3)) continue;
            if (sgi_ppi && i < 16) continue; /* SGIs are always edge */
            F(g, c, edge, i) = (v >> (b * 2 + 1)) & 1;
        }
        return;
    }
    uint32_t reg = off & ~0x7fu, idx = (off & 0x7f) / 4;
    base = first + (int)idx * 32;
    for (int b = 0; b < 32; b++) {
        int i = base + b;
        if (i - first >= count) break;
        if (!((mask >> b) & 1)) continue;
        int bit = (v >> b) & 1;
        switch (reg) {
        case 0x080: F(g, c, group, i) = (uint8_t)bit; break;
        case 0x100:
            if (bit) {
                F(g, c, enabled, i) = 1;
                if (i >= 32) g->spi_on[i / 64] |= 1ull << (i % 64);
            }
            break;
        case 0x180:
            if (bit) {
                F(g, c, enabled, i) = 0;
                if (i >= 32) g->spi_on[i / 64] &= ~(1ull << (i % 64));
            }
            break;
        case 0x200: if (bit) F(g, c, latch, i) = 1; break;
        case 0x280: if (bit) F(g, c, latch, i) = 0; break;
        case 0x300: if (bit) F(g, c, active, i) = 1; break;
        case 0x380: if (bit) F(g, c, active, i) = 0; break;
        case 0xd00: F(g, c, grpmod, i) = (uint8_t)bit; break;
        default: break;
        }
    }
}

static uint32_t size_mask(uint64_t off, unsigned size) {
    uint32_t m = size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
    return m << ((off & 3) * 8);
}

/* Registers for INTIDs 0-31. In the distributor they are reserved (ARE is set);
   the redistributor's SGI frame has only these. */
static int bank0(uint32_t a) {
    if (a >= 0x400 && a < 0x800) return a < 0x420;
    if (a >= 0xc00 && a < 0xd00) return a < 0xc08;
    return (a & 0x7f) < 4;
}

static uint64_t dist_read_body(void *opaque, uint64_t off, unsigned size) {
    HvGic *g = opaque;
    if (off >= 0x6100 && off < 0x6100 + 8 * HV_GIC_NUM_IRQS) { /* IROUTER<n>, n >= 32 */
        uint64_t v = g->route[(off - 0x6000) / 8];
        return size == 8 ? v : (uint32_t)(v >> ((off & 4) * 8));
    }
    uint32_t word, a = (uint32_t)off & ~3u;
    switch (a) {
    case 0x0000: word = g->ctlr | GICD_CTLR_ARE | GICD_CTLR_DS; break;
    case 0x0004: word = (HV_GIC_NUM_IRQS / 32 - 1) | (1u << 17) | (15u << 19); break; /* ITLinesNumber, LPIS, 16 ID bits */
    case 0x0008: word = 0x0300043b; break;                             /* IIDR: Arm, GICv3 */
    case 0xffe8: word = 0x3b; break;                                   /* PIDR2: ArchRev 3 */
    default:
        word = a >= 0x80 && a < 0x1000 && !bank0(a) ? bank_read(g, 0, a, 0, HV_GIC_NUM_IRQS) : 0;
        break;
    }
    return (word >> ((off & 3) * 8)) & (size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void dist_write_body(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvGic *g = opaque;
    if (off >= 0x6100 && off < 0x6100 + 8 * HV_GIC_NUM_IRQS) {
        uint64_t *r = &g->route[(off - 0x6000) / 8];
        if (size == 8) *r = v;
        else if (off & 4) *r = (*r & 0xffffffffull) | v << 32;
        else *r = (*r & ~0xffffffffull) | (uint32_t)v;
        return;
    }
    uint32_t a = (uint32_t)off & ~3u, mask = size_mask(off, size);
    uint32_t word = (uint32_t)(v << ((off & 3) * 8));
    if (a == 0) {
        g->ctlr = (g->ctlr & ~mask) | (word & mask & (GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1));
    } else if (a >= 0x80 && a < 0x1000) {
        if (!bank0(a)) bank_write(g, 0, a, word, mask, 0, HV_GIC_NUM_IRQS, 0);
    }
    hv_gicv3_update(g);
}

static void write_slice64(uint64_t *dst, uint64_t off, uint64_t val, unsigned size) {
    if (size >= 8) {
        *dst = val;
        return;
    }
    unsigned sh = (unsigned)((off & 7ull) * 8u);
    uint64_t m = ((1ull << (size * 8u)) - 1ull) << sh;
    *dst = (*dst & ~m) | ((val << sh) & m);
}

/* One redistributor per CPU, 128 KB apart: RD_base at 0, SGI_base at 0x10000. */
static uint64_t redist_read_body(void *opaque, uint64_t off, unsigned size) {
    HvGic *g = opaque;
    uint64_t v = 0;
    int c = (int)(off >> 17);
    off &= 0x1ffff;
    if (c >= ncpus(g)) return 0;
    if (off < 0x10000) {
        uint64_t mp = g->m->cpus[c] ? g->m->cpus[c]->mpidr : (uint64_t)c;
        switch (off & ~7ull) {
        case 0x0000: v = ((uint64_t)0x0300043b << 32) | g->cpu[c].gicr_ctlr; break; /* CTLR, IIDR */
        case 0x0008: /* TYPER: PLPIS=1, Last on final frame, processor number, affinity */
            v = (mp & 0xffffff) << 32 | (uint64_t)c << 8 | (c == ncpus(g) - 1 ? 1u << 4 : 0) | 1ull;
            break;
        case 0x0010: v = (uint64_t)g->cpu[c].waker << 32; break;        /* STATUSR, WAKER */
        case 0x0070: v = g->cpu[c].propbaser; break;                    /* PROPBASER */
        case 0x0078: v = g->cpu[c].pendbaser; break;                    /* PENDBASER */
        case 0x00c0: v = 0; break;                                      /* SYNCR */
        case 0xffe8: v = 0x3b; break;
        default: break;
        }
        v >>= (off & 7) * 8;
    } else {
        uint32_t a = (uint32_t)(off - 0x10000) & ~3u;
        if (a >= 0x80 && a < 0x1000 && bank0(a))
            v = bank_read(g, c, a, 0, 32) >> ((off & 3) * 8);
    }
    return size == 8 ? v : v & ((1ull << (size * 8)) - 1);
}

static void redist_write_body(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvGic *g = opaque;
    int c = (int)(off >> 17);
    off &= 0x1ffff;
    if (c >= ncpus(g)) return;
    if (off < 0x10000) {
        uint64_t a8 = off & ~7ull;
        if (off < 4) {
            g->cpu[c].gicr_ctlr = (uint32_t)(v & 1u);
            hv_gicv3_update(g);
        } else if ((off & ~3ull) == 0x14) {
            /* WAKER: clearing ProcessorSleep wakes it; ChildrenAsleep follows at once. */
            uint32_t sleep = (v >> (8 * (off & 3))) & 2;
            g->cpu[c].waker = sleep ? 6 : 0;
        } else if (a8 == 0x0070) {
            write_slice64(&g->cpu[c].propbaser, off, v, size);
            hv_gicv3_update(g);
        } else if (a8 == 0x0078) {
            write_slice64(&g->cpu[c].pendbaser, off, v, size);
            if (g->cpu[c].pendbaser & (1ull << 62)) {
                memset(g->cpu[c].lpi_pending, 0, sizeof g->cpu[c].lpi_pending);
                g->cpu[c].pendbaser &= ~(1ull << 62);
            }
            hv_gicv3_update(g);
        } else if (a8 == 0x00a0 || a8 == 0x00b0) {
            hv_gicv3_update(g);
        }
        return;
    }
    uint32_t a = (uint32_t)(off - 0x10000) & ~3u, mask = size_mask(off, size);
    uint32_t word = (uint32_t)(v << ((off & 3) * 8));
    if (a >= 0x80 && a < 0x1000 && bank0(a)) bank_write(g, c, a, word, mask, 0, 32, 1);
    hv_gicv3_update(g);
}

/* Every way in takes the GIC's own lock (recursive: the bodies call hv_gicv3_update).
   Devices call in holding the device lock; the CPUs' ICC registers and timers do not need it. */
void hv_gicv3_update(HvGic *g) {
    pthread_mutex_lock(&g->mu);
    hv_gicv3_update_body(g);
    pthread_mutex_unlock(&g->mu);
}

void hv_gicv3_set_irq(HvGic *g, int intid, int level) {
    pthread_mutex_lock(&g->mu);
    hv_gicv3_set_irq_body(g, intid, level);
    pthread_mutex_unlock(&g->mu);
}

void hv_gicv3_set_ppi(HvGic *g, int c, int intid, int level) {
    /* The timer re-asserts its level at every tick, and only CPU c sets its own
       PPIs: an unchanged level needs no lock (the GIC's is shared by all). */
    if (intid >= 16 && intid < 32 && c < HV_MAX_CPUS &&
        __atomic_load_n(&g->cpu[c].level[intid], __ATOMIC_RELAXED) == (level != 0))
        return;
    pthread_mutex_lock(&g->mu);
    hv_gicv3_set_ppi_body(g, c, intid, level);
    pthread_mutex_unlock(&g->mu);
}

void hv_gicv3_pend(HvGic *g, int intid) {
    pthread_mutex_lock(&g->mu);
    hv_gicv3_pend_body(g, intid);
    pthread_mutex_unlock(&g->mu);
}

static uint64_t icc_read(HvMachine *m, HvCpu *cpu, uint32_t reg) {
    pthread_mutex_lock(&((HvGic *)m->gic)->mu);
    uint64_t v_ = icc_read_body(m, cpu, reg);
    pthread_mutex_unlock(&((HvGic *)m->gic)->mu);
    return v_;
}

static void icc_write(HvMachine *m, HvCpu *cpu, uint32_t reg, uint64_t v) {
    pthread_mutex_lock(&((HvGic *)m->gic)->mu);
    icc_write_body(m, cpu, reg, v);
    pthread_mutex_unlock(&((HvGic *)m->gic)->mu);
}

static uint64_t dist_read(void *opaque, uint64_t off, unsigned size) {
    pthread_mutex_lock(&((HvGic *)opaque)->mu);
    uint64_t v_ = dist_read_body(opaque, off, size);
    pthread_mutex_unlock(&((HvGic *)opaque)->mu);
    return v_;
}

static void dist_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    pthread_mutex_lock(&((HvGic *)opaque)->mu);
    dist_write_body(opaque, off, v, size);
    pthread_mutex_unlock(&((HvGic *)opaque)->mu);
}

static uint64_t redist_read(void *opaque, uint64_t off, unsigned size) {
    pthread_mutex_lock(&((HvGic *)opaque)->mu);
    uint64_t v_ = redist_read_body(opaque, off, size);
    pthread_mutex_unlock(&((HvGic *)opaque)->mu);
    return v_;
}

static void redist_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    pthread_mutex_lock(&((HvGic *)opaque)->mu);
    redist_write_body(opaque, off, v, size);
    pthread_mutex_unlock(&((HvGic *)opaque)->mu);
}

typedef struct HvGicV2m {
    HvGic *g;
    int spi_base; /* GIC INTID of first MSI SPI (>= 32) */
    int num_spi;
} HvGicV2m;

static uint64_t v2m_read(void *opaque, uint64_t off, unsigned size) {
    HvGicV2m *v = opaque;
    uint32_t word = 0;
    switch (off & ~3ull) {
    case 0x008: word = ((uint32_t)v->spi_base << 16) | (uint32_t)v->num_spi; break; /* MSI_TYPER */
    case 0xfcc: word = 0x0002043bu; break; /* MSI_IIDR: ARM GICv2m */
    case 0xfe0: word = 0x22; break;
    case 0xfe4: word = 0xb0; break;
    case 0xfe8: word = 0x0b; break;
    case 0xff0: word = 0x0d; break;
    case 0xff4: word = 0xf0; break;
    case 0xff8: word = 0x05; break;
    case 0xffc: word = 0xb1; break;
    default: break;
    }
    return (word >> ((off & 3) * 8)) & (size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void v2m_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    (void)size;
    HvGicV2m *v = opaque;
    if ((off & ~3ull) == 0x040) { /* MSI_SETSPI_NS */
        int intid = (int)(val & 0x3ff);
        if (intid >= v->spi_base && intid < v->spi_base + v->num_spi)
            hv_gicv3_pend(v->g, intid);
    }
}

void hv_gicv2m_create(HvMachine *m, HvGic *g, uint64_t pa, int spi_base, int num_spi) {
    HvGicV2m *v = calloc(1, sizeof *v);
    v->g = g;
    v->spi_base = spi_base;
    v->num_spi = num_spi;
    hv_machine_add_mmio(m, pa, 0x10000, v, v2m_read, v2m_write, "gicv2m");
    m->mmio[m->nmmio - 1].self_locked = 1;
}

/* ---- GICv3 Interrupt Translation Service (ITS, 128 KB frame: 0x00000 ctrl + 0x10000 translater) ---- */

#define HV_ITS_MAX_ITES  1024
#define HV_ITS_MAX_COLLS 1024

typedef struct HvItsIte {
    uint32_t devid;
    uint32_t eventid;
    uint32_t pintid;
    uint16_t icid;
    uint8_t valid;
} HvItsIte;

struct HvGicIts {
    HvMachine *m;
    HvGic *g;
    uint32_t ctlr;       /* bit 0: Enabled, bit 31: Quiescent (always 1) */
    uint64_t typer;
    uint64_t cbaser;
    uint64_t cwriter;
    uint64_t creadr;
    uint64_t baser[8];
    uint16_t coll_cpu[HV_ITS_MAX_COLLS];
    uint8_t coll_valid[HV_ITS_MAX_COLLS];
    HvItsIte ite[HV_ITS_MAX_ITES];
};

/* The device, collection and event tables live in guest memory the OS hands over (GITS_BASER, MAPD's ITT),
   as on real hardware, in this model's own 8-byte formats (their layout is implementation defined):
     device entry:     bit 63 valid | ITT address (bits 51:8) | EventID bits - 1 (bits 4:0)
     collection entry: bit 63 valid | target CPU (bits 15:0)
     event entry:      bit 63 valid | ICID (bits 47:32) | pINTID (bits 31:0)
   The arrays above cache them. Windows resuming from hibernation restores those tables with the rest of RAM
   and never maps its devices again, so a cache miss reads the tables. */
#define ITS_TYPE_DEVICE 1
#define ITS_TYPE_COLLECTION 4

static int its_entry_pa(HvGicIts *its, int type, uint32_t id, uint64_t *pa) {
    for (int i = 0; i < 8; i++) {
        uint64_t b = its->baser[i];
        if (!(b >> 63) || (int)((b >> 56) & 7) != type) continue;
        uint32_t ps = (b >> 8) & 3;
        uint64_t page = ps == 0 ? 4096 : ps == 1 ? 16384 : 65536;
        uint64_t base = page == 65536 ? (b & 0x0000ffffffff0000ull) | ((b >> 12 & 0xf) << 48) : b & 0x0000fffffffff000ull;
        uint64_t bytes = ((b & 0xff) + 1) * page;
        if (!((b >> 62) & 1)) { /* flat */
            if ((uint64_t)id * 8 >= bytes) return 0;
            *pa = base + (uint64_t)id * 8;
            return 1;
        }
        uint64_t per = page / 8, l1 = (uint64_t)id / per, l2;
        if (l1 * 8 >= bytes || hv_phys_read(its->m, base + l1 * 8, &l2, 8) != 0 || !(l2 >> 63)) return 0;
        *pa = (l2 & 0x000ffffffffff000ull) + ((uint64_t)id % per) * 8;
        return 1;
    }
    return 0;
}

static int its_entry_read(HvGicIts *its, int type, uint32_t id, uint64_t *v) {
    uint64_t pa;
    return its_entry_pa(its, type, id, &pa) && hv_phys_read(its->m, pa, v, 8) == 0 && (*v >> 63);
}

static void its_entry_write(HvGicIts *its, int type, uint32_t id, uint64_t v) {
    uint64_t pa;
    if (its_entry_pa(its, type, id, &pa)) hv_phys_write(its->m, pa, &v, 8);
}

/* The event entry's address, from the device entry; 0 if the device is not mapped or the event is out of range. */
static uint64_t its_ite_pa(HvGicIts *its, uint32_t devid, uint32_t eventid) {
    uint64_t dte;
    if (!its_entry_read(its, ITS_TYPE_DEVICE, devid, &dte)) return 0;
    uint32_t bits = (uint32_t)(dte & 0x1f) + 1;
    if (bits < 32 && eventid >> bits) return 0;
    return (dte & 0x000fffffffffff00ull) + (uint64_t)eventid * 8;
}

static void its_ite_write(HvGicIts *its, uint32_t devid, uint32_t eventid, uint64_t v) {
    uint64_t pa = its_ite_pa(its, devid, eventid);
    if (pa) hv_phys_write(its->m, pa, &v, 8);
}

static int its_coll_cpu(HvGicIts *its, uint16_t icid) {
    if (icid >= HV_ITS_MAX_COLLS) return 0;
    if (!its->coll_valid[icid]) {
        uint64_t cte;
        if (its_entry_read(its, ITS_TYPE_COLLECTION, icid, &cte)) {
            its->coll_cpu[icid] = (uint16_t)cte;
            its->coll_valid[icid] = 1;
        }
    }
    return its->coll_cpu[icid];
}

static int rdbase_to_cpu(HvGicIts *its, uint64_t rdbase) {
    int n = ncpus(its->g);
    uint64_t pa = rdbase << 16;
    if (pa >= 0x080a0000ULL && pa < 0x080a0000ULL + (uint64_t)n * 0x20000ULL)
        return (int)((pa - 0x080a0000ULL) >> 17);
    return (int)rdbase < n ? (int)rdbase : 0;
}

static void its_dropped(uint32_t devid, uint32_t eventid, const char *why) {
    static int logged;
    if (logged < 20) {
        logged++;
        fprintf(stderr, "its: MSI from device %#x event %u dropped: %s\n", devid, eventid, why);
    }
}

static void its_map_ite(HvGicIts *its, uint32_t devid, uint32_t eventid, uint32_t pintid, uint16_t icid);

static void its_set_pending(HvGicIts *its, uint32_t pintid, uint16_t icid) {
    int c = its_coll_cpu(its, icid);
    if (c >= ncpus(its->g)) c = 0;
    if (pintid >= HV_GIC_LPI_BASE && pintid < HV_GIC_LPI_BASE + HV_GIC_MAX_LPIS) {
        uint32_t idx = pintid - HV_GIC_LPI_BASE;
        its->g->cpu[c].lpi_pending[idx / 64] |= 1ull << (idx % 64);
        hv_gicv3_update_body(its->g);
    }
}

static void its_deliver_locked(HvGicIts *its, uint32_t devid, uint32_t eventid) {
    if (!(its->ctlr & 1u)) {
        its_dropped(devid, eventid, "ITS disabled");
        return;
    }
    for (int i = 0; i < HV_ITS_MAX_ITES; i++) {
        if (its->ite[i].valid && its->ite[i].devid == devid && its->ite[i].eventid == eventid) {
            its_set_pending(its, its->ite[i].pintid, its->ite[i].icid);
            return;
        }
    }
    /* Not cached: the tables in memory (after a resume from hibernation, the only copy). Delivered from what
       was read even if the cache has no room left for it. */
    uint64_t pa = its_ite_pa(its, devid, eventid), ite;
    if (!pa || hv_phys_read(its->m, pa, &ite, 8) != 0 || !(ite >> 63)) {
        its_dropped(devid, eventid, "no mapping");
        return;
    }
    its_map_ite(its, devid, eventid, (uint32_t)ite, (uint16_t)(ite >> 32));
    its_set_pending(its, (uint32_t)ite, (uint16_t)(ite >> 32));
}

void hv_gicv3_its_msi(HvGicIts *its, uint32_t devid, uint32_t eventid) {
    if (!its) return;
    pthread_mutex_lock(&its->g->mu);
    its_deliver_locked(its, devid, eventid);
    pthread_mutex_unlock(&its->g->mu);
}

static void its_map_ite(HvGicIts *its, uint32_t devid, uint32_t eventid, uint32_t pintid, uint16_t icid) {
    int free_slot = -1;
    for (int i = 0; i < HV_ITS_MAX_ITES; i++) {
        if (its->ite[i].valid && its->ite[i].devid == devid && its->ite[i].eventid == eventid) {
            its->ite[i].pintid = pintid;
            its->ite[i].icid = icid;
            return;
        }
        if (!its->ite[i].valid && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        its->ite[free_slot] = (HvItsIte){.devid = devid, .eventid = eventid, .pintid = pintid, .icid = icid, .valid = 1};
    }
}

static void its_clear_ite(HvGicIts *its, uint32_t devid, uint32_t eventid, int discard) {
    for (int i = 0; i < HV_ITS_MAX_ITES; i++) {
        if (!its->ite[i].valid || its->ite[i].devid != devid || its->ite[i].eventid != eventid) continue;
        uint32_t pintid = its->ite[i].pintid;
        uint16_t icid = its->ite[i].icid;
        int c = its_coll_cpu(its, icid);
        if (c < ncpus(its->g) && pintid >= HV_GIC_LPI_BASE && pintid < HV_GIC_LPI_BASE + HV_GIC_MAX_LPIS) {
            uint32_t idx = pintid - HV_GIC_LPI_BASE;
            its->g->cpu[c].lpi_pending[idx / 64] &= ~(1ull << (idx % 64));
        }
        if (discard) its->ite[i].valid = 0;
        hv_gicv3_update_body(its->g);
        return;
    }
}

static void its_process_commands(HvGicIts *its) {
    if (!(its->ctlr & 1u) || !(its->cbaser >> 63)) return;
    uint64_t ring_pa = its->cbaser & 0x000ffffffffff000ULL;
    uint64_t ring_sz = ((its->cbaser & 0xffull) + 1ull) * 4096ull;
    uint64_t target = its->cwriter & 0xfffe0ull;
    if (target >= ring_sz) target %= ring_sz;

    int guard = 0;
    while (its->creadr != target && guard++ < 8192) {
        uint64_t dw[4] = {0};
        if (hv_phys_read(its->m, ring_pa + its->creadr, dw, 32) != 0) break;
        its->creadr = (its->creadr + 32ull) % ring_sz;

        uint8_t cmd = (uint8_t)(dw[0] & 0xffu);
        uint32_t devid = (uint32_t)(dw[0] >> 32);
        uint32_t eventid = (uint32_t)(dw[1] & 0xffffffffu);
        if (getenv("HV_ITS_LOG"))
            fprintf(stderr, "its cmd 0x%02x: dw0=%llx dw1=%llx dw2=%llx dw3=%llx\n",
                    cmd, (unsigned long long)dw[0], (unsigned long long)dw[1],
                    (unsigned long long)dw[2], (unsigned long long)dw[3]);
        switch (cmd) {
        case 0x01: { /* MOVI: the event now belongs to another collection */
            uint16_t icid = (uint16_t)(dw[2] & 0xffffu);
            for (int i = 0; i < HV_ITS_MAX_ITES; i++)
                if (its->ite[i].valid && its->ite[i].devid == devid && its->ite[i].eventid == eventid)
                    its->ite[i].icid = icid;
            uint64_t pa = its_ite_pa(its, devid, eventid), ite;
            if (pa && hv_phys_read(its->m, pa, &ite, 8) == 0 && (ite >> 63)) {
                ite = (ite & ~(0xffffull << 32)) | (uint64_t)icid << 32;
                hv_phys_write(its->m, pa, &ite, 8);
            }
            hv_gicv3_update_body(its->g);
            break;
        }
        case 0x03: /* INT */
            its_deliver_locked(its, devid, eventid);
            break;
        case 0x04: /* CLEAR */
            its_clear_ite(its, devid, eventid, 0);
            break;
        case 0x05: /* SYNC */
        case 0x0c: /* INV */
        case 0x0d: /* INVALL */
            hv_gicv3_update_body(its->g);
            break;
        case 0x08: /* MAPD */
            if (!((dw[2] >> 63) & 1u)) {
                for (int i = 0; i < HV_ITS_MAX_ITES; i++)
                    if (its->ite[i].valid && its->ite[i].devid == devid) its->ite[i].valid = 0;
                its_entry_write(its, ITS_TYPE_DEVICE, devid, 0);
            } else {
                its_entry_write(its, ITS_TYPE_DEVICE, devid, 1ull << 63 | (dw[2] & 0x000fffffffffff00ull) | (dw[1] & 0x1f));
            }
            break;
        case 0x09: { /* MAPC */
            uint16_t icid = (uint16_t)(dw[2] & 0xffffu);
            if (icid < HV_ITS_MAX_COLLS && ((dw[2] >> 63) & 1u)) {
                its->coll_cpu[icid] = (uint16_t)rdbase_to_cpu(its, (dw[2] >> 16) & 0x7ffffffffULL);
                its->coll_valid[icid] = 1;
                its_entry_write(its, ITS_TYPE_COLLECTION, icid, 1ull << 63 | its->coll_cpu[icid]);
            } else if (icid < HV_ITS_MAX_COLLS) {
                its->coll_valid[icid] = 0;
                its->coll_cpu[icid] = 0;
                its_entry_write(its, ITS_TYPE_COLLECTION, icid, 0);
            }
            break;
        }
        case 0x0a: /* MAPTI */
        case 0x0b: { /* MAPI */
            uint32_t pintid = cmd == 0x0a ? (uint32_t)(dw[1] >> 32) : eventid;
            uint16_t icid = (uint16_t)(dw[2] & 0xffffu);
            its_map_ite(its, devid, eventid, pintid, icid);
            its_ite_write(its, devid, eventid, 1ull << 63 | (uint64_t)icid << 32 | pintid);
            break;
        }
        case 0x0e: { /* MOVALL: pending interrupts move from one redistributor to another */
            int c1 = rdbase_to_cpu(its, (dw[2] >> 16) & 0x7ffffffffULL);
            int c2 = rdbase_to_cpu(its, (dw[3] >> 16) & 0x7ffffffffULL);
            if (c1 != c2 && c1 < ncpus(its->g) && c2 < ncpus(its->g)) {
                for (int k = 0; k < HV_GIC_MAX_LPIS / 64; k++) {
                    its->g->cpu[c2].lpi_pending[k] |= its->g->cpu[c1].lpi_pending[k];
                    its->g->cpu[c1].lpi_pending[k] = 0;
                }
            }
            hv_gicv3_update_body(its->g);
            break;
        }
        case 0x0f: /* DISCARD */
            its_clear_ite(its, devid, eventid, 1);
            its_ite_write(its, devid, eventid, 0);
            break;
        default:
            break;
        }
    }
}

static uint64_t its_read(void *opaque, uint64_t off, unsigned size) {
    HvGicIts *its = opaque;
    if (off >= 0x10000) return 0;
    pthread_mutex_lock(&its->g->mu);
    uint64_t v = 0, a8 = off & ~7ull;
    if (a8 == 0x0000) v = ((uint64_t)0x0000043bu << 32) | its->ctlr; /* CTLR, IIDR */
    else if (a8 == 0x0008) v = its->typer;
    else if (a8 == 0x0080) v = its->cbaser;
    else if (a8 == 0x0088) v = its->cwriter;
    else if (a8 == 0x0090) v = its->creadr;
    else if (a8 >= 0x0100 && a8 < 0x0140) v = its->baser[(a8 - 0x0100) / 8];
    else if (a8 == 0xffe8) v = 0x30ull; /* GITS_PIDR2: ArchRev 3 */
    pthread_mutex_unlock(&its->g->mu);
    v >>= (off & 7ull) * 8u;
    uint64_t ret = size >= 8 ? v : v & ((1ull << (size * 8u)) - 1ull);
    if (getenv("HV_ITS_LOG"))
        fprintf(stderr, "its read %04llx (%u) = %llx\n", (unsigned long long)off, size, (unsigned long long)ret);
    return ret;
}

static void its_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvGicIts *its = opaque;
    if (getenv("HV_ITS_LOG"))
        fprintf(stderr, "its write %04llx (%u) = %llx\n", (unsigned long long)off, size, (unsigned long long)val);
    pthread_mutex_lock(&its->g->mu);
    if (off == 0x10040) { /* GITS_TRANSLATER direct MMIO fallback */
        its_deliver_locked(its, 0, (uint32_t)val);
        pthread_mutex_unlock(&its->g->mu);
        return;
    }
    if (off >= 0x10000) {
        pthread_mutex_unlock(&its->g->mu);
        return;
    }
    uint64_t a8 = off & ~7ull;
    if (off < 4) {
        its->ctlr = ((uint32_t)val & 1u) | (1u << 31);
        its_process_commands(its);
    } else if (a8 == 0x0080) {
        write_slice64(&its->cbaser, off, val, size);
        its->creadr = 0;
    } else if (a8 == 0x0088) {
        write_slice64(&its->cwriter, off, val, size);
        its->cwriter &= ~0x1full;
        its_process_commands(its);
    } else if (a8 >= 0x0100 && a8 < 0x0110) {
        int idx = (int)((a8 - 0x0100) / 8);
        const uint64_t ro = (7ull << 56) | (0x1full << 48);
        uint64_t cur = its->baser[idx];
        write_slice64(&cur, off, val, size);
        its->baser[idx] = (its->baser[idx] & ro) | (cur & ~ro);
        /* New tables: the caches may describe the old ones (the tables are the truth; a miss reads them). */
        memset(its->ite, 0, sizeof its->ite);
        memset(its->coll_valid, 0, sizeof its->coll_valid);
    }
    pthread_mutex_unlock(&its->g->mu);
}

HvGicIts *hv_gicv3_its_create(HvMachine *m, HvGic *g, uint64_t pa) {
    HvGicIts *its = calloc(1, sizeof *its);
    its->m = m;
    its->g = g;
    its->ctlr = 1u << 31; /* Quiescent = 1, Enabled = 0 */
    /* Physical=1, ITT_entry_size=7 (8B), IDbits=15 (16b), Devbits=15 (16b), PTA=0, CIDbits=15 (16b), CIL=1 */
    its->typer = 1ull | (7ull << 4) | (15ull << 8) | (15ull << 13) | (15ull << 32) | (1ull << 36);
    its->baser[0] = (1ull << 56) | (7ull << 48); /* Type 1: Devices, EntrySize=8B */
    its->baser[1] = (4ull << 56) | (7ull << 48); /* Type 4: Interrupt Collections, EntrySize=8B */
    hv_machine_add_mmio(m, pa, 0x20000, its, its_read, its_write, "gicv3-its");
    m->mmio[m->nmmio - 1].self_locked = 1;
    return its;
}

void hv_gicv3_its_reset(HvGicIts *its) {
    if (!its) return;
    pthread_mutex_lock(&its->g->mu);
    its->ctlr = 1u << 31;
    its->cbaser = 0;
    its->cwriter = 0;
    its->creadr = 0;
    its->baser[0] = (1ull << 56) | (7ull << 48);
    its->baser[1] = (4ull << 56) | (7ull << 48);
    memset(its->coll_cpu, 0, sizeof its->coll_cpu);
    memset(its->coll_valid, 0, sizeof its->coll_valid);
    memset(its->ite, 0, sizeof its->ite);
    pthread_mutex_unlock(&its->g->mu);
}

HvGic *hv_gicv3_create(HvMachine *m, uint64_t dist_pa, uint64_t redist_pa) {
    HvGic *g = calloc(1, sizeof *g);
    pthread_mutexattr_t ra;
    pthread_mutexattr_init(&ra);
    pthread_mutexattr_settype(&ra, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g->mu, &ra);
    g->m = m;
    for (int c = 0; c < HV_MAX_CPUS; c++) {
        g->cpu[c].waker = 6; /* ProcessorSleep and ChildrenAsleep until the guest wakes it */
        for (int i = 0; i < 16; i++) g->cpu[c].edge[i] = 1;
    }
    m->gic = g;
    m->icc_read = icc_read;
    m->icc_write = icc_write;
    hv_machine_add_mmio(m, dist_pa, 0x10000, g, dist_read, dist_write, "gicv3-dist");
    m->mmio[m->nmmio - 1].self_locked = 1;
    hv_machine_add_mmio(m, redist_pa, 0xf60000, g, redist_read, redist_write, "gicv3-redist");
    m->mmio[m->nmmio - 1].self_locked = 1;
    return g;
}

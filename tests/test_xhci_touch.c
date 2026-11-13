// xHCI USB HID touchscreen (port 3): drives devices/xhci.c through its MMIO interface with a scripted guest
// (rings in plain RAM) and checks the root hub, enumeration, the HID report descriptor (parsed item by item),
// the control requests Windows uses (GET_REPORT, SET_REPORT) and the touch frame queue.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "machine.h"
#include "pcie.h"
#include "xhci.h"

#define GPA 0x40000000ull
#define MEM_SIZE (1u << 20)

/* BAR 0 layout of the controller. */
#define OP 0x40
#define PORTREGS 0x440
#define RT 0x1000
#define DB 0x2000
#define XECP 0x800

/* Guest memory map. */
#define DCBAA (GPA + 0x0000)
#define ERST (GPA + 0x0100)
#define CMD_RING (GPA + 0x1000)
#define EVENT_RING (GPA + 0x2000)
#define EVENT_TRBS 256
#define DEV_CTX(slot) (GPA + 0x10000 + (uint64_t)(slot) * 0x400)
#define INPUT_CTX (GPA + 0x14000)
#define EP0_RING(slot) (GPA + 0x20000 + (uint64_t)(slot) * 0x2000)
#define IN_RING(slot) (EP0_RING(slot) + 0x1000)
#define DATA_BUF (GPA + 0x30000)
#define IN_BUF(i) (GPA + 0x38000 + (uint64_t)(i) * 64)

enum { TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_ENABLE_SLOT = 9, TRB_ADDRESS_DEVICE = 11,
       TRB_CONFIGURE_EP = 12, TRB_STOP_EP = 15, TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33 };
enum { CC_SUCCESS = 1, CC_STALL = 6, CC_SHORT_PACKET = 13 };

static HvMachine m;
static HvXhci *x;
static uint8_t *mem;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint8_t *host(uint64_t pa) {
    return mem + (pa - GPA);
}

static void mmio_w(uint64_t off, uint64_t v, unsigned size) {
    x->pci.bar_write(&x->pci, 0, off, v, size);
}

static uint64_t mmio_r(uint64_t off, unsigned size) {
    return x->pci.bar_read(&x->pci, 0, off, size);
}

/* ---- scripted guest: rings, commands, control transfers ---- */

typedef struct Ring {
    uint64_t base;
    int idx;
} Ring;

static void ring_push(Ring *r, uint64_t param, uint32_t status, uint32_t control) {
    uint32_t *t = (uint32_t *)host(r->base + 16ull * (uint64_t)r->idx++);
    t[0] = (uint32_t)param, t[1] = (uint32_t)(param >> 32), t[2] = status;
    __atomic_store_n(&t[3], control | 1u, __ATOMIC_RELEASE); /* cycle state is 1 throughout: rings never wrap */
}

typedef struct Event {
    uint64_t param;
    uint32_t status, control;
} Event;

static int ev_idx;

static int next_event(Event *e) {
    if (ev_idx >= EVENT_TRBS) return 0;
    const uint32_t *t = (const uint32_t *)host(EVENT_RING + 16ull * (uint64_t)ev_idx);
    if (!(t[3] & 1)) return 0;
    e->param = (uint64_t)t[1] << 32 | t[0];
    e->status = t[2];
    e->control = t[3];
    ev_idx++;
    return 1;
}

static Ring cmd_ring = {CMD_RING, 0};
static Ring ep0[HV_XHCI_SLOTS + 1], in_ring[HV_XHCI_SLOTS + 1];

/* Runs one command; returns its completion code and the slot the event carries. */
static int run_cmd(uint32_t type, int slot, uint64_t param, uint32_t extra, int *slot_out) {
    uint64_t at = cmd_ring.base + 16ull * (uint64_t)cmd_ring.idx;
    ring_push(&cmd_ring, param, 0, type << 10 | (uint32_t)slot << 24 | extra);
    mmio_w(DB, 0, 4);
    int cc = -1;
    Event e;
    while (next_event(&e)) {
        if (((e.control >> 10) & 0x3f) == TRB_CMD_COMPLETION && e.param == at) {
            cc = (int)(e.status >> 24);
            if (slot_out) *slot_out = (int)(e.control >> 24);
        }
    }
    return cc;
}

static void le32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void le64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

static int enumerate(int port) {
    int slot = 0;
    check("enable slot", (uint64_t)run_cmd(TRB_ENABLE_SLOT, 0, 0, 0, &slot), CC_SUCCESS);
    if (slot < 1) return 0;
    ep0[slot].base = EP0_RING(slot);
    in_ring[slot].base = IN_RING(slot);
    le64(host(DCBAA) + 8 * slot, DEV_CTX(slot));
    uint8_t *in = host(INPUT_CTX);
    memset(in, 0, 0x420);
    le32(in + 4, 3);                    /* add slot and EP0 contexts */
    in[32 + 6] = (uint8_t)port;         /* slot context: root hub port number */
    le64(in + 64 + 8, EP0_RING(slot) | 1);
    check("address device", (uint64_t)run_cmd(TRB_ADDRESS_DEVICE, slot, INPUT_CTX, 0, NULL), CC_SUCCESS);
    return slot;
}

static void configure_in_endpoint(int slot) {
    uint8_t *in = host(INPUT_CTX);
    memset(in, 0, 0x420);
    le32(in + 4, 1u | 1u << 3); /* add slot context and DCI 3 (endpoint 1 IN) */
    le64(in + 32 * 4 + 8, IN_RING(slot) | 1);
    check("configure endpoint", (uint64_t)run_cmd(TRB_CONFIGURE_EP, slot, INPUT_CTX, 0, NULL), CC_SUCCESS);
}

/* One control transfer. in_len < 0 on a stall; returns the status stage's completion code (CC_STALL on a stall).
   An OUT data stage carries out_data (immediate data when idt). */
static int ctrl(int slot, uint8_t bm, uint8_t req, uint16_t value, uint16_t index, uint16_t wlen,
                const uint8_t *out_data, int idt, uint8_t *in_data, int *in_len) {
    int dir_in = bm & 0x80;
    uint8_t setup[8] = {bm, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
                        (uint8_t)wlen, (uint8_t)(wlen >> 8)};
    uint64_t setup_param;
    memcpy(&setup_param, setup, 8);
    uint32_t trt = wlen ? (dir_in ? 3u : 2u) : 0u;
    Ring *r = &ep0[slot];
    ring_push(r, setup_param, 8, TRB_SETUP << 10 | 1u << 6 | trt << 16);
    uint64_t data_at = 0, status_at;
    if (wlen) {
        data_at = r->base + 16ull * (uint64_t)r->idx;
        if (dir_in) {
            memset(host(DATA_BUF), 0, 0x1000);
            ring_push(r, DATA_BUF, wlen, TRB_DATA << 10 | 1u << 16 | 1u << 2); /* IN, ISP */
        } else if (idt) {
            uint64_t imm = 0;
            memcpy(&imm, out_data, wlen < 8 ? wlen : 8);
            ring_push(r, imm, wlen, TRB_DATA << 10 | 1u << 6);
        } else {
            memcpy(host(DATA_BUF), out_data, wlen);
            ring_push(r, DATA_BUF, wlen, TRB_DATA << 10);
        }
    }
    status_at = r->base + 16ull * (uint64_t)r->idx;
    ring_push(r, 0, 0, TRB_STATUS << 10 | 1u << 5 | (wlen && dir_in ? 0u : 1u << 16));
    mmio_w(DB + 4 * slot, 1, 4);

    int cc = -1, data_cc = -1;
    uint32_t residual = 0;
    Event e;
    while (next_event(&e)) {
        if (((e.control >> 10) & 0x3f) != TRB_TRANSFER_EVENT) continue;
        if (wlen && e.param == data_at) data_cc = (int)(e.status >> 24), residual = e.status & 0xffffff;
        if (e.param == status_at) cc = (int)(e.status >> 24);
    }
    if (data_cc == CC_STALL) {
        if (in_len) *in_len = -1;
        return CC_STALL;
    }
    if (dir_in && wlen) {
        uint32_t got = data_cc == CC_SHORT_PACKET ? wlen - residual : wlen;
        if (in_len) *in_len = (int)got;
        if (in_data) memcpy(in_data, host(DATA_BUF), got);
    }
    return cc;
}

static int get_descriptor(int slot, uint8_t bm, uint8_t type, uint8_t idx, uint16_t wlen, uint8_t *out) {
    int n = 0;
    int cc = ctrl(slot, bm, 6, (uint16_t)(type << 8 | idx), 0, wlen, NULL, 0, out, &n);
    return cc == CC_SUCCESS ? n : -1;
}

/* ---- HID report descriptor walker ---- */

typedef struct Field {
    int kind; /* 0 input, 1 output, 2 feature */
    int rid, page, usage, bit, size, constant;
    int depth, top, finger; /* collection depth (1 = top level), top-level index, enclosing finger index or -1 */
    int lmin, lmax, pmin, pmax, unit, exp;
} Field;

#define MAX_FIELDS 512
static Field fields[MAX_FIELDS];
static int nfields;
static int ncoll_open, ncoll_close, balanced, ntop, nfinger;
static int top_page[8], top_usage[8], top_type[8];

static int32_t item_value(const uint8_t *p, int size, int sign) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    if (sign && size && size < 4 && (v & (1u << (8 * size - 1)))) v |= ~0u << (8 * size);
    return (int32_t)v;
}

static void parse_descriptor(const uint8_t *d, int len) {
    int page = 0, lmin = 0, lmax = 0, pmin = 0, pmax = 0, unit = 0, exp = 0, size = 0, count = 0, rid = 0;
    int usages[64], nusages = 0, umin = -1, umax = -1;
    int depth = 0, top = -1, finger = -1;
    int bits[3][256];
    memset(bits, 0, sizeof bits);
    nfields = ncoll_open = ncoll_close = ntop = nfinger = 0;
    balanced = 1;
    for (int i = 0; i < len;) {
        uint8_t b = d[i];
        int sz = b & 3, type = (b >> 2) & 3, tag = b >> 4;
        if (b == 0xfe) break; /* long items are not used */
        if (sz == 3) sz = 4;
        int sv = item_value(d + i + 1, sz, 1);
        uint32_t uv = (uint32_t)item_value(d + i + 1, sz, 0);
        i += 1 + sz;
        if (type == 1) { /* global */
            switch (tag) {
            case 0: page = (int)uv; break;
            case 1: lmin = sv; break;
            case 2: lmax = sv; break;
            case 3: pmin = sv; break;
            case 4: pmax = sv; break;
            case 5: exp = (int)(uv & 0xf) >= 8 ? (int)(uv & 0xf) - 16 : (int)(uv & 0xf); break;
            case 6: unit = (int)uv; break;
            case 7: size = (int)uv; break;
            case 8: rid = (int)uv; break;
            case 9: count = (int)uv; break;
            }
        } else if (type == 2) { /* local */
            if (tag == 0 && nusages < 64) usages[nusages++] = sz > 2 ? (int)(uv & 0xffff) : (int)uv;
            else if (tag == 1) umin = (int)uv;
            else if (tag == 2) umax = (int)uv;
        } else if (type == 0) { /* main */
            if (tag == 0xa) { /* collection */
                ncoll_open++;
                int usage = nusages ? usages[0] : 0;
                if (depth == 0) {
                    top = ntop++;
                    top_page[top] = page, top_usage[top] = usage, top_type[top] = (int)uv;
                } else if (usage == 0x22 && page == 0x0d && (int)uv == 2) {
                    finger = nfinger++;
                }
                depth++;
            } else if (tag == 0xc) { /* end collection */
                ncoll_close++;
                if (--depth < 0) {
                    balanced = 0;
                    depth = 0;
                }
                if (depth == 1) finger = -1;
            } else if (tag == 8 || tag == 9 || tag == 0xb) {
                int kind = tag == 8 ? 0 : tag == 9 ? 1 : 2;
                int constant = uv & 1;
                if (umin >= 0 && umax >= umin)
                    for (int u = umin; u <= umax && nusages < 64; u++) usages[nusages++] = u;
                for (int n = 0; n < count && nfields < MAX_FIELDS; n++) {
                    Field *f = &fields[nfields++];
                    f->kind = kind, f->rid = rid, f->page = page;
                    f->usage = constant || !nusages ? 0 : usages[n < nusages ? n : nusages - 1];
                    f->bit = bits[kind][rid & 255] + n * size;
                    f->size = size, f->constant = constant;
                    f->depth = depth, f->top = top, f->finger = finger;
                    f->lmin = lmin, f->lmax = lmax, f->pmin = pmin, f->pmax = pmax, f->unit = unit, f->exp = exp;
                }
                bits[kind][rid & 255] += count * size;
            }
            nusages = 0, umin = umax = -1;
        }
    }
    if (depth != 0) balanced = 0;
}

static int report_bits(int kind, int rid) {
    int total = 0;
    for (int i = 0; i < nfields; i++)
        if (fields[i].kind == kind && fields[i].rid == rid && fields[i].bit + fields[i].size > total)
            total = fields[i].bit + fields[i].size;
    return total;
}

/* The nth (from 0) field with this usage in a report. */
static const Field *find_field(int kind, int rid, int page, int usage, int nth) {
    for (int i = 0; i < nfields; i++) {
        const Field *f = &fields[i];
        if (f->kind == kind && f->rid == rid && f->page == page && f->usage == usage && !f->constant && nth-- == 0)
            return f;
    }
    return NULL;
}

/* Value of a field in a report as the host extracts it (report[0] is the id byte). */
static uint32_t field_value(const uint8_t *report, const Field *f) {
    uint32_t v = 0;
    for (int b = 0; b < f->size; b++) {
        int bit = f->bit + b;
        if (report[1 + bit / 8] >> (bit % 8) & 1) v |= 1u << b;
    }
    return v;
}

static int physical_hundredth_cm(const Field *f) {
    /* physical extent in 0.01 cm for a centimetre unit (0x11) */
    int e = f->exp + 2, v = f->pmax;
    for (; e > 0; e--) v *= 10;
    for (; e < 0; e++) v /= 10;
    return v;
}

static void test_descriptor(const uint8_t *d, int len) {
    printf("  report descriptor: %d bytes\n", len);
    parse_descriptor(d, len);
    check("collections balanced", balanced && ncoll_open == ncoll_close, 1);
    check("two top-level collections", ntop, 2);
    check("touch screen collection page", top_page[0], 0x0d);
    check("touch screen collection usage", top_usage[0], 0x04);
    check("touch screen collection is application", top_type[0], 1);
    check("device configuration collection page", top_page[1], 0x0d);
    check("device configuration collection usage", top_usage[1], 0x0e);
    check("device configuration collection is application", top_type[1], 1);
    check("five finger collections", nfinger, HV_TOUCH_MAX_CONTACTS);

    /* Input report 1 is exactly the frame hv_xhci_touch sends: id byte plus the sum of the field sizes. */
    int in_bits = report_bits(0, 1);
    check("input report 1 length", 1 + in_bits / 8, HV_TOUCH_REPORT_LEN);
    check("input report 1 is whole bytes", in_bits % 8, 0);
    int other_inputs = 0;
    for (int i = 0; i < nfields; i++) other_inputs += fields[i].kind == 0 && fields[i].rid != 1;
    check("no other input report", other_inputs, 0);
    check("no output report", report_bits(1, 1) + report_bits(1, 2) + report_bits(1, 3), 0);

    for (int k = 0; k < HV_TOUCH_MAX_CONTACTS; k++) {
        const Field *tip = find_field(0, 1, 0x0d, 0x42, k), *conf = find_field(0, 1, 0x0d, 0x47, k);
        const Field *cid = find_field(0, 1, 0x0d, 0x51, k), *fx = find_field(0, 1, 0x01, 0x30, k);
        const Field *fy = find_field(0, 1, 0x01, 0x31, k);
        if (!tip || !conf || !cid || !fx || !fy) {
            check("finger fields present", 0, 1);
            continue;
        }
        check("tip switch size", tip->size, 1);
        check("tip switch in finger collection", tip->finger, k);
        check("confidence size", conf->size, 1);
        check("contact id size", cid->size, 8);
        check("contact id logical max", cid->lmax >= 127, 1);
        check("x size", fx->size, 16);
        check("y size", fy->size, 16);
        check("x logical min", fx->lmin, 0);
        check("x logical max", fx->lmax, 32767);
        check("y logical min", fy->lmin, 0);
        check("y logical max", fy->lmax, 32767);
        check("x unit is centimetre", fx->unit, 0x11);
        check("y unit is centimetre", fy->unit, 0x11);
        check("x physical extent is at least an inch", physical_hundredth_cm(fx) >= 254, 1);
        check("y physical extent is at least an inch", physical_hundredth_cm(fy) >= 254, 1);
        check("x extent about 17.3 cm", physical_hundredth_cm(fx), 1730);
        check("y extent about 12.2 cm", physical_hundredth_cm(fy), 1220);
        check("tip switch has no unit", tip->unit, 0);
    }

    const Field *scan = find_field(0, 1, 0x0d, 0x56, 0), *cnt = find_field(0, 1, 0x0d, 0x54, 0);
    check("scan time present", scan != NULL, 1);
    check("contact count present", cnt != NULL, 1);
    if (scan) {
        check("scan time is 16 bits", scan->size, 16);
        check("scan time logical max", scan->lmax >= 65535, 1);
        check("scan time unit is seconds", scan->unit, 0x1001);
        check("scan time exponent is 100 us", scan->exp, -4);
        check("scan time in the top-level collection", scan->depth, 1);
    }
    if (cnt) {
        check("contact count is 8 bits", cnt->size, 8);
        check("contact count in the top-level collection", cnt->depth, 1);
        check("contact count logical max", cnt->lmax >= HV_TOUCH_MAX_CONTACTS, 1);
    }

    /* Contact Count Maximum: a feature of the touch collection itself, never of a child collection. */
    const Field *ccm = find_field(2, 2, 0x0d, 0x55, 0);
    check("contact count maximum present", ccm != NULL, 1);
    if (ccm) {
        check("contact count maximum in the touch collection", ccm->top, 0);
        check("contact count maximum at top level", ccm->depth, 1);
        check("contact count maximum is 8 bits", ccm->size, 8);
        check("contact count maximum within logical range", ccm->lmax >= HV_TOUCH_MAX_CONTACTS, 1);
    }
    check("contact count maximum is 5", HV_TOUCH_MAX_CONTACTS, 5);
    check("contact count maximum below 250", HV_TOUCH_MAX_CONTACTS <= 250, 1);

    /* Device Configuration: Input Mode and Device Index as a feature in the second top-level collection. */
    const Field *mode = find_field(2, 3, 0x0d, 0x52, 0), *index = find_field(2, 3, 0x0d, 0x53, 0);
    check("input mode present", mode != NULL, 1);
    check("device index present", index != NULL, 1);
    if (mode && index) {
        check("input mode in the configuration collection", mode->top, 1);
        check("device index in the configuration collection", index->top, 1);
        check("configuration feature length", report_bits(2, 3), 16);
        check("input mode logical max", mode->lmax >= 2, 1);
    }

    /* Every data field has a logical range. */
    int unranged = 0;
    for (int i = 0; i < nfields; i++)
        if (!fields[i].constant && fields[i].lmax == fields[i].lmin) unranged++;
    check("every field has a logical range", unranged, 0);
    /* No THQA certification feature (vendor page 0xff00). */
    int vendor = 0;
    for (int i = 0; i < nfields; i++) vendor += fields[i].page == 0xff00;
    check("no THQA blob", vendor, 0);
}

/* ---- the checks ---- */

static void test_root_hub(void) {
    uint32_t hcs1 = (uint32_t)mmio_r(4, 4);
    check("HCSPARAMS1 ports", hcs1 >> 24, 3);
    check("supported protocol port count", (mmio_r(XECP + 8, 4) >> 8) & 0xff, 3);
    check("supported protocol port offset", mmio_r(XECP + 8, 4) & 0xff, 1);
    for (int p = 0; p < 3; p++) {
        uint32_t sc = (uint32_t)mmio_r(PORTREGS + 16 * (uint64_t)p, 4);
        check("port connected", sc & 1, 1);
        check("port powered", (sc >> 9) & 1, 1);
    }
    check("no fourth port", mmio_r(PORTREGS + 16 * 3, 4), 0);
}

static uint8_t rdesc[2048];
static int rdesc_len;

static void test_enumeration(void) {
    static const struct { int port; uint16_t pid; } devs[] = {{1, 0x0002}, {2, 0x0001}, {3, 0x0003}};
    for (int i = 0; i < 3; i++) {
        int port = devs[i].port;
        int slot = enumerate(port);
        check("slot assigned", slot >= 1, 1);
        if (slot < 1) continue;
        uint8_t dd[64], cfg[256];
        check("device descriptor length", (uint64_t)get_descriptor(slot, 0x80, 1, 0, 64, dd), 18);
        check("vendor id", (uint16_t)(dd[8] | dd[9] << 8), 0x0627);
        check("product id", (uint16_t)(dd[10] | dd[11] << 8), devs[i].pid);
        int clen = get_descriptor(slot, 0x80, 2, 0, 255, cfg);
        check("config descriptor length", (uint64_t)clen, 34);
        check("HID interface class", cfg[9 + 5], 3);
        check("HID descriptor type", cfg[18 + 1], 0x21);
        int want_len = cfg[18 + 7] | cfg[18 + 8] << 8;
        int got = get_descriptor(slot, 0x81, 0x22, 0, 0x7ff, rdesc);
        check("report descriptor length matches the HID descriptor", (uint64_t)got, (uint64_t)want_len);
        check("endpoint address", cfg[27 + 2], 0x81);
        if (port == 3) {
            check("touch interface is not a boot device", cfg[9 + 6], 0);
            check("touch interrupt endpoint max packet", cfg[27 + 4] | cfg[27 + 5] << 8, 64);
            check("touch report descriptor is longer than 255 bytes", got > 255, 1);
            check("touch report fits one interrupt packet", HV_TOUCH_REPORT_LEN <= 64, 1);
            rdesc_len = got;
            /* A whole-descriptor read with the host's 255-byte ceiling would have cut it: ask in pieces too. */
            uint8_t piece[600];
            int n = get_descriptor(slot, 0x81, 0x22, 0, 600, piece);
            check("report descriptor read twice is identical", n == got && memcmp(piece, rdesc, (size_t)got) == 0, 1);
        } else {
            check("boot keyboard and tablet descriptors still served", got > 0 && rdesc[0] == 0x05, 1);
        }
        uint8_t str[64];
        int sl = get_descriptor(slot, 0x80, 3, port == 3 ? 6 : port == 1 ? 2 : 4, 64, str);
        check("product string", sl > 2, 1);
        if (port == 3) {
            char name[64] = {0};
            for (int k = 0; k < (sl - 2) / 2; k++) name[k] = (char)str[2 + 2 * k];
            check("touchscreen product string", strcmp(name, "WindowsiOS USB Touchscreen"), 0);
        }
    }
    check("keyboard slot", x->kbd_slot, 1);
    check("tablet slot", x->tab_slot, 2);
    check("touch slot", x->touch_slot, 3);
}

static void test_get_set_report(void) {
    int slot = x->touch_slot;
    uint8_t b[64];
    int n;
    check("get feature contact count max", (uint64_t)ctrl(slot, 0xa1, 1, 0x0302, 0, 2, NULL, 0, b, &n), CC_SUCCESS);
    check("contact count max length", n, 2);
    check("contact count max report id", b[0], 2);
    check("contact count max value", b[1], 5);

    check("get feature configuration", (uint64_t)ctrl(slot, 0xa1, 1, 0x0303, 0, 3, NULL, 0, b, &n), CC_SUCCESS);
    check("configuration length", n, 3);
    check("configuration report id", b[0], 3);
    check("default input mode is multiple input", b[1], 2);
    check("default device index", b[2], 0);

    /* SET_REPORT(Feature 3), once with the payload in the data buffer and once as immediate data. */
    uint8_t set1[3] = {3, 1, 4}, set2[3] = {3, 2, 0};
    check("set report", (uint64_t)ctrl(slot, 0x21, 9, 0x0303, 0, 3, set1, 0, NULL, NULL), CC_SUCCESS);
    ctrl(slot, 0xa1, 1, 0x0303, 0, 3, NULL, 0, b, &n);
    check("input mode written", b[1], 1);
    check("device index written", b[2], 4);
    check("set report immediate", (uint64_t)ctrl(slot, 0x21, 9, 0x0303, 0, 3, set2, 1, NULL, NULL), CC_SUCCESS);
    ctrl(slot, 0xa1, 1, 0x0303, 0, 3, NULL, 0, b, &n);
    check("input mode written (immediate)", b[1], 2);
    check("device index written (immediate)", b[2], 0);

    /* A SET_REPORT for another report must not disturb the configuration. */
    uint8_t junk[3] = {2, 9, 9};
    ctrl(slot, 0x21, 9, 0x0302, 0, 3, junk, 0, NULL, NULL);
    ctrl(slot, 0xa1, 1, 0x0303, 0, 3, NULL, 0, b, &n);
    check("other report id ignored", b[1] == 2 && b[2] == 0, 1);

    check("get input report", (uint64_t)ctrl(slot, 0xa1, 1, 0x0101, 0, HV_TOUCH_REPORT_LEN, NULL, 0, b, &n), CC_SUCCESS);
    check("input report length", n, HV_TOUCH_REPORT_LEN);
    check("input report id", b[0], 1);
    check("no contacts before the first frame", b[HV_TOUCH_REPORT_LEN - 1], 0);

    /* Unknown report id: the control pipe stalls (last, it halts EP0). */
    check("unknown feature stalls", (uint64_t)ctrl(slot, 0xa1, 1, 0x0309, 0, 2, NULL, 0, b, &n), CC_STALL);
}

static int in_next;
static void post_in(int slot, int n) {
    for (int i = 0; i < n; i++, in_next++)
        ring_push(&in_ring[slot], IN_BUF(in_next), 64, TRB_NORMAL << 10 | 1u << 5);
    mmio_w(DB + 4 * slot, 3, 4);
}

static void drain(void) {
    Event e;
    while (next_event(&e)) {}
}

static HvTouchContact tc(int id, int tip, int x_, int y_) {
    return (HvTouchContact){(uint8_t)id, (uint8_t)tip, (uint16_t)x_, (uint16_t)y_};
}

static uint32_t queued(void) {
    return x->ctail - x->chead;
}

static const uint8_t *queued_at(uint32_t i) {
    return x->touch_reports[(x->chead + i) % HV_XHCI_REPORTS];
}

static uint32_t contact_field(const uint8_t *rep, int k, int page, int usage) {
    const Field *f = find_field(0, 1, page, usage, k);
    return f ? field_value(rep, f) : 0xdeadbeef;
}

static void test_touch_queue(void) {
    int slot = x->touch_slot;
    HvTouchContact down[2] = {tc(0, 1, 1000, 2000), tc(1, 1, 30000, 15000)};

    /* Not configured yet: frames are dropped, never queued. */
    check("not ready before configure", (uint64_t)hv_xhci_touch_ready(x), 0);
    hv_xhci_touch(x, down, 2);
    check("frame dropped while the endpoint is not running", queued(), 0);

    configure_in_endpoint(slot);
    check("ready after configure", (uint64_t)hv_xhci_touch_ready(x), 1);

    /* No TRBs posted, so the queue backs up: motion frames collapse, a landing or a lift never does. */
    hv_xhci_touch(x, down, 2);
    check("first frame queued", queued(), 1);
    HvTouchContact move[2] = {tc(0, 1, 1010, 2010), tc(1, 1, 30010, 15010)};
    hv_xhci_touch(x, move, 2);
    move[0].x = 1020, move[1].y = 15020;
    hv_xhci_touch(x, move, 2);
    check("motion frames coalesce", queued(), 1);
    check("coalesced frame keeps the latest x", contact_field(queued_at(0), 0, 0x01, 0x30), 1020);
    check("coalesced frame keeps the latest y", contact_field(queued_at(0), 1, 0x01, 0x31), 15020);

    HvTouchContact lift[2] = {tc(0, 0, 1020, 2010), tc(1, 1, 30010, 15020)};
    hv_xhci_touch(x, lift, 2);
    check("a lifting contact is never coalesced", queued(), 2);
    hv_xhci_touch(x, lift, 2);
    check("repeat of a lifting frame only moves", queued(), 2);
    HvTouchContact one[1] = {tc(1, 1, 30020, 15030)};
    hv_xhci_touch(x, one, 1);
    check("a changed contact set is not coalesced", queued(), 3);
    HvTouchContact land[2] = {tc(1, 1, 30020, 15030), tc(2, 1, 500, 500)};
    hv_xhci_touch(x, land, 2);
    check("a landing contact is not coalesced", queued(), 4);

    /* Hand the frames over and decode them with the descriptor's own field offsets. */
    post_in(slot, 4);
    hv_xhci_poll(x);
    check("queue drained", queued(), 0);
    const uint8_t *r0 = host(IN_BUF(0)), *r1 = host(IN_BUF(1)), *r2 = host(IN_BUF(2)), *r3 = host(IN_BUF(3));
    check("report id", r0[0], 1);
    check("contact count", field_value(r0, find_field(0, 1, 0x0d, 0x54, 0)), 2);
    check("contact 0 tip", contact_field(r0, 0, 0x0d, 0x42), 1);
    check("contact 0 confidence", contact_field(r0, 0, 0x0d, 0x47), 1);
    check("contact 0 id", contact_field(r0, 0, 0x0d, 0x51), 0);
    check("contact 0 x", contact_field(r0, 0, 0x01, 0x30), 1020);
    check("contact 0 y", contact_field(r0, 0, 0x01, 0x31), 2010);
    check("contact 1 tip", contact_field(r0, 1, 0x0d, 0x42), 1);
    check("contact 1 id", contact_field(r0, 1, 0x0d, 0x51), 1);
    check("contact 1 x", contact_field(r0, 1, 0x01, 0x30), 30010);
    check("contact 1 y", contact_field(r0, 1, 0x01, 0x31), 15020);
    check("unused contact slot is empty", contact_field(r0, 2, 0x0d, 0x42) | contact_field(r0, 2, 0x01, 0x30), 0);
    check("lift frame: contact 0 tip", contact_field(r1, 0, 0x0d, 0x42), 0);
    check("lift frame: contact 0 confidence", contact_field(r1, 0, 0x0d, 0x47), 1);
    check("lift frame: contact 0 keeps its position", contact_field(r1, 0, 0x01, 0x30), 1020);
    check("lift frame: contact 1 tip", contact_field(r1, 1, 0x0d, 0x42), 1);
    check("single contact frame count", field_value(r2, find_field(0, 1, 0x0d, 0x54, 0)), 1);
    check("single contact frame id", contact_field(r2, 0, 0x0d, 0x51), 1);
    check("landing frame count", field_value(r3, find_field(0, 1, 0x0d, 0x54, 0)), 2);
    check("landing frame id", contact_field(r3, 1, 0x0d, 0x51), 2);

    /* Scan time: 100 us units off a monotonic clock, one stamp per frame, non-decreasing modulo 16 bits. */
    const Field *scan = find_field(0, 1, 0x0d, 0x56, 0);
    uint32_t s0 = field_value(r0, scan), s3 = field_value(r3, scan);
    check("scan time advances", ((s3 - s0) & 0xffff) < 0x8000, 1);
    check("scan time fits 16 bits", s0 <= 0xffff && s3 <= 0xffff, 1);

    /* Delivery events: a 34-byte report in a 64-byte TRB is a short packet. */
    Event e;
    int shorts = 0;
    while (next_event(&e))
        shorts += ((e.control >> 10) & 0x3f) == TRB_TRANSFER_EVENT && (e.status >> 24) == CC_SHORT_PACKET &&
                  (e.status & 0xffffff) == 64 - HV_TOUCH_REPORT_LEN;
    check("four short packet events", shorts, 4);

    /* Clamping and limits. */
    HvTouchContact many[7];
    for (int i = 0; i < 7; i++) many[i] = tc(i, 1, 100 * i, 100 * i);
    many[0].x = 40000, many[0].y = 65535;
    hv_xhci_touch(x, many, 7);
    check("more than five contacts are clamped", queued(), 1);
    check("clamped count", queued_at(0)[HV_TOUCH_REPORT_LEN - 1], 5);
    check("x clamps to 32767", contact_field(queued_at(0), 0, 0x01, 0x30), 32767);
    check("y clamps to 32767", contact_field(queued_at(0), 0, 0x01, 0x31), 32767);
    hv_xhci_touch(x, many, 0);
    hv_xhci_touch(x, many, -1);
    hv_xhci_touch(x, NULL, 3);
    check("empty frames are ignored", queued(), 1);

    /* GET_REPORT(Input) shows the last frame. */
    /* (EP0 is halted by the stall test above, so read the state directly.) */
    check("state mirrors the last frame", x->touch_state[HV_TOUCH_REPORT_LEN - 1], 5);

    /* Stopping the endpoint drops what is queued, and nothing is queued while it is stopped. */
    check("stop endpoint", (uint64_t)run_cmd(TRB_STOP_EP, slot, 0, 3u << 16, NULL), CC_SUCCESS);
    check("stop endpoint flushes the queue", queued(), 0);
    check("not ready while stopped", (uint64_t)hv_xhci_touch_ready(x), 0);
    hv_xhci_touch(x, down, 2);
    check("frames dropped while stopped", queued(), 0);

    /* Restart: the burst sent while stopped is not replayed, new frames flow again. */
    int posted = in_next;
    post_in(slot, 1);
    hv_xhci_poll(x);
    check("nothing replayed after restart", host(IN_BUF(posted))[0], 0);
    hv_xhci_touch(x, down, 2);
    check("frames flow again after restart", queued(), 1);
    hv_xhci_poll(x);
    check("restart delivery", host(IN_BUF(posted))[0], 1);
    drain();
}

/* Windows times press-and-hold (its touch right-click) off the digitizer's report stream, so a finger
   resting still must keep being reported at the scan rate, not only when the host sends a new frame. */
static void test_touch_hold(void) {
    int slot = x->touch_slot;
    int first = in_next;
    HvTouchContact press[1] = {tc(0, 1, 16000, 12000)};
    hv_xhci_touch(x, press, 1);
    post_in(slot, 4);
    hv_xhci_poll(x);
    check("press delivered", host(IN_BUF(first))[0], 1);

    usleep(15000);
    hv_xhci_poll(x);
    const uint8_t *again = host(IN_BUF(first + 1));
    check("a resting finger is reported again", again[0], 1);
    check("repeat keeps the tip down", contact_field(again, 0, 0x0d, 0x42), 1);
    check("repeat keeps the position", contact_field(again, 0, 0x01, 0x30), 16000);
    const Field *scan = find_field(0, 1, 0x0d, 0x56, 0);
    uint32_t s0 = field_value(host(IN_BUF(first)), scan), s1 = field_value(again, scan);
    check("repeat carries a later scan time", s1 != s0 && ((s1 - s0) & 0xffff) < 0x8000, 1);

    usleep(15000);
    hv_xhci_poll(x);
    check("and again while it rests", host(IN_BUF(first + 2))[0], 1);

    HvTouchContact lift[1] = {tc(0, 0, 16000, 12000)};
    hv_xhci_touch(x, lift, 1);
    hv_xhci_poll(x);
    check("lift delivered", contact_field(host(IN_BUF(first + 3)), 0, 0x0d, 0x42), 0);
    usleep(15000);
    post_in(slot, 1);
    hv_xhci_poll(x);
    check("nothing repeats after the lift", host(IN_BUF(first + 4))[0], 0);

    /* Two fingers, one lifts: the lift goes out once and repeats carry only the finger still down. */
    int k = first + 4; /* the buffer posted for "nothing repeats after the lift" is still waiting */
    HvTouchContact two[2] = {tc(0, 1, 1000, 1000), tc(1, 1, 2000, 2000)};
    HvTouchContact partial[2] = {tc(0, 1, 1000, 1000), tc(1, 0, 2000, 2000)};
    hv_xhci_touch(x, two, 2);
    hv_xhci_touch(x, partial, 2);
    post_in(slot, 2);
    hv_xhci_poll(x);
    check("partial lift delivered", contact_field(host(IN_BUF(k + 1)), 1, 0x0d, 0x42), 0);
    usleep(15000);
    hv_xhci_poll(x);
    const uint8_t *rest = host(IN_BUF(k + 2));
    check("repeat after a partial lift has one contact", field_value(rest, find_field(0, 1, 0x0d, 0x54, 0)), 1);
    check("that contact is the finger still down", contact_field(rest, 0, 0x0d, 0x51), 0);
    check("and its tip is down", contact_field(rest, 0, 0x0d, 0x42), 1);

    /* A halted controller manufactures nothing. */
    mmio_w(OP, 4, 4);
    usleep(15000);
    hv_xhci_poll(x);
    check("no repeat while the controller is halted", queued(), 0);
    mmio_w(OP, 1 | 4, 4);
    HvTouchContact up[1] = {tc(0, 0, 1000, 1000)};
    hv_xhci_touch(x, up, 1);
    post_in(slot, 1);
    hv_xhci_poll(x);
    drain();

    /* A finger held while the endpoint stops is not replayed when it comes back. */
    hv_xhci_touch(x, press, 1);
    hv_xhci_poll(x);
    check("stop endpoint while held", (uint64_t)run_cmd(TRB_STOP_EP, slot, 0, 3u << 16, NULL), CC_SUCCESS);
    int after = in_next;
    usleep(15000);
    post_in(slot, 1);
    hv_xhci_poll(x);
    check("a hold does not survive an endpoint stop", host(IN_BUF(after))[0], 0);
    drain();
}

/* A machine reset (the guest rebooted) must keep input out of the rings of the OS that is gone. */
static void test_machine_reset(void) {
    int slot = x->touch_slot;
    int first = in_next;
    post_in(slot, 2);
    hv_xhci_reset(x);
    check("reset halts the controller", mmio_r(OP + 4, 4) & 1, 1);
    check("reset stops it", mmio_r(OP, 4) & 1, 0);
    check("touch is not ready after a reset", hv_xhci_touch_ready(x), 0);
    HvTouchContact press[1] = {tc(0, 1, 100, 100)};
    hv_xhci_touch(x, press, 1);
    hv_xhci_poll(x);
    usleep(15000);
    hv_xhci_poll(x);
    check("no report reaches the old rings", host(IN_BUF(first))[0], 0);
    check("nor the one after", host(IN_BUF(first + 1))[0], 0);
}

int main(void) {
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    mem = aligned_alloc(4096, MEM_SIZE);
    memset(mem, 0, MEM_SIZE);
    hv_machine_add_ram(&m, GPA, MEM_SIZE, mem, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x50000000, 0x10000000, 0x20000000, 0x10000000, 35);
    x = hv_xhci_create(&m, bus);
    if (!x) return 1;

    le64(host(DCBAA) + 0, 0);
    le64(host(ERST), EVENT_RING);
    le32(host(ERST) + 8, EVENT_TRBS);
    mmio_w(OP + 0x30, DCBAA, 8);
    mmio_w(OP + 0x18, CMD_RING | 1, 8);
    mmio_w(RT + 0x20 + 0x00, 2, 4);
    mmio_w(RT + 0x20 + 0x08, 1, 4);
    mmio_w(RT + 0x20 + 0x18, EVENT_RING, 8);
    mmio_w(RT + 0x20 + 0x10, ERST, 8);
    mmio_w(OP, 1 | 4, 4); /* run, interrupts on: raises the port status change events */
    drain();

    printf("== root hub\n");
    test_root_hub();
    printf("== enumeration\n");
    test_enumeration();
    printf("== report descriptor\n");
    test_descriptor(rdesc, rdesc_len);
    printf("== GET_REPORT / SET_REPORT\n");
    test_get_set_report();
    printf("== touch queue\n");
    test_touch_queue();

    printf("== touch hold\n");
    test_touch_hold();
    printf("== machine reset\n");
    test_machine_reset();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

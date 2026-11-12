#ifndef HENGEVM_XHCI_H
#define HENGEVM_XHCI_H

#include <pthread.h>

#include "pcie.h"

/* xHCI 1.0 USB host controller (MSI-X + INTx) with:
   - Port 1: USB HID Boot Keyboard
   - Port 2: USB HID Absolute Tablet (0..32767 X/Y, L/R/M buttons, scroll wheel)
   - Port 3: USB HID multi-touch digitizer (Windows Touchscreen, 5 contacts, parallel mode)
   Zero guest drivers required in EDK2 UEFI or Windows 11 ARM64. */
#define HV_XHCI_SLOTS 8
#define HV_XHCI_PORTS 3
#define HV_XHCI_INTRS 4
#define HV_XHCI_EPS 32 /* device context index 1..31 */
#define HV_XHCI_REPORTS 256
#define HV_TOUCH_MAX_CONTACTS 5
#define HV_TOUCH_REPORT_LEN 34 /* report id, 5 x (flags, contact id, x, y), scan time, contact count */

typedef struct HvXhciRing {
    uint64_t dequeue; /* guest PA of the next TRB */
    int ccs;          /* consumer cycle state */
} HvXhciRing;

typedef struct HvXhciSlot {
    int used;
    int port;         /* 1 = keyboard, 2 = tablet, 3 = touchscreen */
    int configured;
    uint8_t protocol; /* 0 = boot, 1 = report */
    uint8_t idle;
    HvXhciRing ep[HV_XHCI_EPS];
    int ep_running[HV_XHCI_EPS];
} HvXhciSlot;

typedef struct HvXhci {
    HvPciDev pci;
    HvMachine *m;
    /* operational */
    uint32_t usbcmd, usbsts, dnctrl, config;
    uint64_t dcbaap;
    HvXhciRing cmd;
    int cmd_running;
    uint32_t portsc[HV_XHCI_PORTS];
    /* interrupters and their event rings */
    struct HvXhciIntr {
        uint32_t iman, imod, erstsz;
        uint64_t erstba, erdp;
        uint64_t seg_base;
        uint32_t seg_size, seg_index, index;
        int pcs;
    } intr[HV_XHCI_INTRS];

    HvXhciSlot slots[HV_XHCI_SLOTS + 1];
    int kbd_slot;
    int tab_slot;
    int touch_slot;

    pthread_mutex_t mu;
    /* Port 1 keyboard reports (8 bytes each) */
    uint8_t reports[HV_XHCI_REPORTS][8];
    uint32_t rhead, rtail;
    uint8_t state[8];
    /* Port 2 absolute tablet reports (6 bytes: buttons, x_lo, x_hi, y_lo, y_hi, wheel) */
    uint8_t tab_reports[HV_XHCI_REPORTS][6];
    uint32_t thead, ttail;
    uint8_t tab_state[6];
    /* Port 3 touchscreen reports (HV_TOUCH_REPORT_LEN bytes, scan time stamped when queued) */
    uint8_t touch_reports[HV_XHCI_REPORTS][HV_TOUCH_REPORT_LEN];
    uint32_t chead, ctail;
    uint8_t touch_state[HV_TOUCH_REPORT_LEN];
    uint8_t touch_mode;  /* Device Configuration feature: Input Mode (0 mouse, 1 single, 2 multiple input) */
    uint8_t touch_index; /* Device Configuration feature: Device Index */

    uint64_t delivered;
} HvXhci;

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus);
/* Any thread: HID usage page 7 key down/up (e.g. 4 = A; 0xe0-0xe7 modifiers). */
void hv_xhci_key(HvXhci *x, uint8_t usage, int down);
/* Any thread: Linux input key code down/up. */
void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down);
/* Any thread: USB HID Absolute Tablet event (x, y in 0..32767; buttons bit0=L, bit1=R, bit2=M; wheel -127..127). */
void hv_xhci_tablet(HvXhci *x, int abs_x, int abs_y, uint8_t buttons, int8_t wheel);
/* One touch contact. x, y span the whole Windows desktop (0..32767). */
typedef struct HvTouchContact {
    uint8_t id;
    uint8_t tip; /* 1 = on the surface, 0 = lifting (report it once) */
    uint16_t x, y;
} HvTouchContact;
/* Any thread: one touch frame. contacts is the full set for this frame (count 1..HV_TOUCH_MAX_CONTACTS),
   including contacts lifting (tip 0) once. Frames are dropped while Windows has not configured the
   touchscreen; a count of 0 is ignored. */
void hv_xhci_touch(HvXhci *x, const HvTouchContact *contacts, int count);
/* Any thread: 1 once the guest has configured the touchscreen's interrupt endpoint. */
int hv_xhci_touch_ready(HvXhci *x);
/* CPU thread: hand waiting keyboard, tablet & touch reports to the guest. */
void hv_xhci_poll(HvXhci *x);

#endif

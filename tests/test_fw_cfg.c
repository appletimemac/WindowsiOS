/* devices/fw_cfg.c, read the way the firmware reads it: the boot menu's wait. QEMU's EDK2 waits five seconds
   for a key before it starts the first boot option, on every boot, unless the boot-menu item is set and the file
   etc/boot-menu-wait says how many milliseconds to wait instead (QemuBootOrderLib, GetFrontPageTimeoutFromQemu). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fw_cfg.h"
#include "machine.h"

static HvMachine m;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void select_item(uint64_t pa, uint16_t key) {
    HvMmio *e = hv_mmio_lookup(&m, pa);
    e->write(e->opaque, 0x08, __builtin_bswap16(key), 2); /* the selector register is big-endian */
}

static uint8_t byte(uint64_t pa) {
    HvMmio *e = hv_mmio_lookup(&m, pa);
    return (uint8_t)e->read(e->opaque, 0, 1);
}

static uint16_t item16(uint64_t pa, uint16_t key) {
    select_item(pa, key);
    uint16_t lo = byte(pa);
    return (uint16_t)(lo | byte(pa) << 8);
}

/* The file's selector and size from the directory (item 0x19), 0 if it is not there. */
static uint16_t file(uint64_t pa, const char *name, uint32_t *size) {
    select_item(pa, 0x19);
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) n = n << 8 | byte(pa);
    for (uint32_t f = 0; f < n; f++) {
        uint8_t e[64];
        for (int i = 0; i < 64; i++) e[i] = byte(pa);
        if (strcmp((const char *)e + 8, name)) continue;
        *size = (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
        return (uint16_t)(e[4] << 8 | e[5]);
    }
    return 0;
}

int main(void) {
    if (hv_machine_init(&m, 1 << 20) != 0) return 2;
    uint32_t size = 0;

    printf("as the app runs it\n");
    unsetenv("HV_BOOT_MENU_WAIT_MS");
    hv_fw_cfg_create(&m, 0x09020000, 4);
    check("the boot menu item is set", item16(0x09020000, 0x0e) != 0, 1);
    uint16_t sel = file(0x09020000, "etc/boot-menu-wait", &size);
    check("etc/boot-menu-wait is there", sel != 0, 1);
    check("two bytes of it", size, 2);
    check("and it says no wait", item16(0x09020000, sel), 0);
    check("the CPU count is still read", item16(0x09020000, 0x05), 4);

    printf("a wait asked for (HV_BOOT_MENU_WAIT_MS=3000)\n");
    setenv("HV_BOOT_MENU_WAIT_MS", "3000", 1);
    hv_fw_cfg_create(&m, 0x09030000, 4);
    check("the boot menu item is set", item16(0x09030000, 0x0e) != 0, 1);
    sel = file(0x09030000, "etc/boot-menu-wait", &size);
    check("the file says 3000 ms", sel ? item16(0x09030000, sel) : 0xffff, 3000);
    setenv("HV_BOOT_MENU_WAIT_MS", "999999", 1);
    hv_fw_cfg_create(&m, 0x09040000, 4);
    sel = file(0x09040000, "etc/boot-menu-wait", &size);
    check("a wait too long for the file is its largest", sel ? item16(0x09040000, sel) : 0, 65535);

    printf("the firmware's own default (HV_BOOT_MENU_WAIT_MS=-1)\n");
    setenv("HV_BOOT_MENU_WAIT_MS", "-1", 1);
    hv_fw_cfg_create(&m, 0x09050000, 4);
    check("the boot menu item reads 0", item16(0x09050000, 0x0e), 0);
    check("and the file is not offered", file(0x09050000, "etc/boot-menu-wait", &size), 0);

    printf("test_fw_cfg: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

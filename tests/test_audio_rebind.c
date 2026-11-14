/* host/audio_mac.c: the output unit pulls from the sound device it was made for, and goes on pulling from that one
   when another device starts a stream at the same format. So a VM that has ended must have its audio shut down
   before the next one's device is attached, or the next one plays into nothing: that is what winvm.c does before
   it sets up a VM again, and what this pins. It plays silence for a moment and needs an output device; without
   one it says so and passes. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_mac.h"
#include "gicv3.h"
#include "hda.h"
#include "machine.h"
#include "pcie.h"

static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

/* Half a second of silence is waiting in the device's ring: does the host take any of it within a second? */
static int pulled(HvHda *d) {
    uint64_t before = atomic_load(&d->tx_consumed);
    atomic_fetch_add(&d->tx_written, 48000 * 4 / 2);
    for (int i = 0; i < 100; i++) {
        if (atomic_load(&d->tx_consumed) != before) return 1;
        usleep(10000);
    }
    return 0;
}

int main(void) {
    static HvMachine m;
    if (hv_machine_init(&m, 16u << 20) != 0) return 2;
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x50000000, 0x10000000, 0x20000000, 0x10000000, 35);
    HvHda *first = hv_hda_create(&m, bus), *second = hv_hda_create(&m, bus);
    if (!first || !second) return 2;

    hv_audio_attach_hda(first);
    if (first->backend.start_out(first->backend.opaque, 2, 48000) != 0) {
        printf("test_audio_rebind: no audio output here, nothing checked\n");
        return 0;
    }
    check("the first device's stream is pulled", (uint64_t)pulled(first), 1);
    first->backend.stop_out(first->backend.opaque);

    printf("the next device without a shutdown in between (what a VM started again used to get)\n");
    hv_audio_attach_hda(second);
    check("its stream starts", (uint64_t)second->backend.start_out(second->backend.opaque, 2, 48000), 0);
    check("but nothing pulls from it", (uint64_t)pulled(second), 0);
    second->backend.stop_out(second->backend.opaque);

    printf("the next device after the shutdown\n");
    hv_audio_shutdown();
    hv_audio_attach_hda(second);
    check("its stream starts", (uint64_t)second->backend.start_out(second->backend.opaque, 2, 48000), 0);
    uint64_t first_before = atomic_load(&first->tx_consumed);
    check("and is pulled", (uint64_t)pulled(second), 1);
    check("the first device is left alone", atomic_load(&first->tx_consumed), first_before);
    second->backend.stop_out(second->backend.opaque);
    hv_audio_shutdown();

    printf("test_audio_rebind: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

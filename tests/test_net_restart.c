/* SPDX-License-Identifier: GPL-2.0-or-later */
/* host/net_slirp.c across VM starts in one process (Windows shut down and started again without leaving the app):
   each hv_net_attach has to leave one network thread on one libslirp, with nothing of the last one left open, and
   the network has to work again. Stubs stand in for the machine and the virtio-net device. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_slirp.h"

static int checks, failures;

static void check(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    }
}

static HvVirtioNet dev;
static uint8_t got_frame[2048];
static uint32_t got_len;
static int got_frames;

void hv_machine_wake(HvMachine *m) { (void)m; }

int hv_virtio_net_receive(HvVirtioNet *n, const uint8_t *frame, uint32_t len) {
    (void)n;
    got_len = len < sizeof got_frame ? len : (uint32_t)sizeof got_frame;
    memcpy(got_frame, frame, got_len);
    got_frames++;
    return 1;
}

static int thread_count(void) {
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof *list);
    return (int)n;
}

/* A thread that was just joined can still be listed for a moment. */
static int settled_thread_count(int want) {
    int n = thread_count();
    for (int i = 0; i < 100 && n != want; i++) {
        usleep(10000);
        n = thread_count();
    }
    return n;
}

static int fd_count(void) {
    int n = 0;
    for (int fd = 0; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

/* The guest (10.0.2.15) asks who has the host's address (10.0.2.2); libslirp answers that itself. 1 when the answer
   came back through the device within two seconds. */
static int arp_answered(void) {
    static const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t f[42] = {0};
    memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6);
    f[12] = 0x08, f[13] = 0x06;                                       /* ARP */
    f[15] = 1, f[16] = 0x08, f[18] = 6, f[19] = 4, f[21] = 1;         /* Ethernet, IPv4, a request */
    memcpy(f + 22, mac, 6);
    f[28] = 10, f[30] = 2, f[31] = 15;
    f[38] = 10, f[40] = 2, f[41] = 2;
    got_frames = 0;
    dev.transmit(dev.opaque, f, sizeof f);
    for (int i = 0; i < 200 && !got_frames; i++) {
        usleep(10000);
        hv_net_poll();
    }
    return got_frames == 1 && got_len >= 42 && got_frame[12] == 0x08 && got_frame[13] == 0x06 && got_frame[21] == 2 &&
           got_frame[28] == 10 && got_frame[31] == 2 && !memcmp(got_frame, mac, 6);
}

/* Gives the next network a forward from a free port on this machine (HV_NET_FWD), so that its libslirp holds a
   socket of its own: one that is still open after the next start was not cleaned up. */
static void forward_a_free_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof a;
    char v[32] = "0:80";
    if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr *)&a, &len) == 0)
        snprintf(v, sizeof v, "%d:80", ntohs(a.sin_port));
    if (s >= 0) close(s);
    setenv("HV_NET_FWD", v, 1);
}

int main(void) {
    const int fds0 = fd_count(), threads0 = thread_count();
    forward_a_free_port();
    check("first start", hv_net_attach(NULL, &dev), 0);
    check("the first network answers", arp_answered(), 1);
    const int fds1 = fd_count(), threads1 = thread_count();
    check("one thread for the network", threads1, threads0 + 1);
    check("a network holds its wake pipe and its forward open", fds1, fds0 + 3);
    for (int start = 2; start <= 6; start++) {
        forward_a_free_port();
        check("another start", hv_net_attach(NULL, &dev), 0);
        check("still one network thread", settled_thread_count(threads1), threads1);
        check("nothing of the last network left open", fd_count(), fds1);
        check("the new network answers", arp_answered(), 1);
        check("and answers again", arp_answered(), 1);
    }
    /* Ending it by hand, as the VM's creation does before it sets the machine up again. */
    hv_net_detach();
    check("the device is cut off from the ended network", dev.transmit == NULL, 1);
    check("no network thread after a detach", settled_thread_count(threads0), threads0);
    check("nothing open after a detach", fd_count(), fds0);
    hv_net_detach();
    check("a second detach has nothing to do", fd_count(), fds0);
    forward_a_free_port();
    check("a start after a detach", hv_net_attach(NULL, &dev), 0);
    check("one network thread again", settled_thread_count(threads1), threads1);
    check("its pipe and forward again", fd_count(), fds1);
    check("and it answers", arp_answered(), 1);
    printf("%s: %d checks, %d failures\n", "test_net_restart", checks, failures);
    return failures ? 1 : 0;
}

#ifndef HENGEVM_NET_SLIRP_H
#define HENGEVM_NET_SLIRP_H

#include "machine.h"
#include "virtio_net.h"

/* Before hv_net_attach: the Unix socket behind the guest's 10.0.2.4:80 (share.c). */
void hv_net_set_share_socket(const char *path);
/* User-mode NAT for the guest's virtio-net (see net_slirp.c). Returns 0 on success. One network exists at a time:
   it first ends the one attached before (hv_net_detach). */
int hv_net_attach(HvMachine *m, HvVirtioNet *n);
/* Ends the attached network, if there is one: its thread, libslirp and queues. The VM's CPU threads must have
   stopped. Its thread wakes the machine it was attached with, so this comes before that machine is torn down or
   set up again. */
void hv_net_detach(void);
/* CPU thread, from the board's tick: hand frames for the guest to the device. */
void hv_net_poll(void);
/* Any thread: dynamically forward [::1]/127.0.0.1:host_port -> 10.0.2.15:guest_port (e.g. OAuth callbacks). */
void hv_net_add_hostfwd(int host_port, int guest_port);

extern uint64_t hv_net_tx_frames, hv_net_rx_frames;

#endif

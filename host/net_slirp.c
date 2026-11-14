/* The guest's network: user-mode NAT through libslirp (as QEMU's -netdev user),
   so it needs no root and no entitlement. 10.0.2.0/24: the host is 10.0.2.2,
   DNS 10.0.2.3, the guest gets 10.0.2.15 by DHCP.

   libslirp is not thread-safe, so it lives on one thread with its sockets and
   timers. Frames the guest sends are queued to it (and the thread woken through
   a pipe); frames for the guest are queued the other way and handed to the
   virtio-net device on the CPU thread (hv_net_poll), which may only take them
   when the guest has posted receive buffers.

   There is one network at a time: hv_net_detach ends it, and attaching ends the
   one before first, so a VM started again in the same process does not leave the
   last one's thread running on the new one's state. */
#include "net_slirp.h"
#include "share.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <libslirp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define QUEUE 512
#define MAX_FRAME 65550
#define MAX_TIMERS 64
#define MAX_POLL 1024

typedef struct Frame {
    uint32_t len;
    uint8_t *data;
} Frame;

typedef struct Queue {
    Frame f[QUEUE];
    uint32_t head, tail;
    pthread_mutex_t mu;
} Queue;

typedef struct Timer {
    int used;
    SlirpTimerId id;
    void *cb_opaque;
    int64_t expire_ms; /* -1: not armed */
} Timer;

static Slirp *slirp;
static pthread_t net_tid;
static int net_started; /* net_tid is a thread that has to be joined */
static int net_quit;    /* atomic: asks that thread to end */
static int queues_made; /* the two queues' mutexes exist */
static HvVirtioNet *net;
static HvMachine *machine;
static Queue to_guest, to_host;
static int wake_pipe[2];
static Timer timers[MAX_TIMERS];
static struct pollfd pfds[MAX_POLL];
static int npfd;
uint64_t hv_net_tx_frames, hv_net_rx_frames;

static char share_socket[256];

typedef struct FwdReq {
    int host_port, guest_port;
} FwdReq;
static pthread_mutex_t fwd_mu = PTHREAD_MUTEX_INITIALIZER;
static FwdReq pending_fwds[32];
static int npending_fwds;
static int active_fwds[64];
static int nactive_fwds;

typedef struct V6ConnPair {
    int a, b;
} V6ConnPair;

static void *v6_pump_thread(void *arg) {
    V6ConnPair p = *(V6ConnPair *)arg;
    free(arg);
    uint8_t buf[16384];
    struct pollfd fds[2] = {{p.a, POLLIN, 0}, {p.b, POLLIN, 0}};
    while (poll(fds, 2, 120000) > 0) {
        for (int i = 0; i < 2; i++) {
            int src = i == 0 ? p.a : p.b;
            int dst = i == 0 ? p.b : p.a;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = recv(src, buf, sizeof buf, 0);
                if (n <= 0) goto done;
                size_t off = 0;
                while (off < (size_t)n) {
                    ssize_t w = send(dst, buf + off, (size_t)n - off, 0);
                    if (w <= 0) goto done;
                    off += (size_t)w;
                }
            }
        }
    }
done:
    close(p.a);
    close(p.b);
    return NULL;
}

typedef struct V6AcceptArg {
    int listen_fd;
    int port;
} V6AcceptArg;

static void *v6_accept_thread(void *arg) {
    V6AcceptArg a = *(V6AcceptArg *)arg;
    free(arg);
    for (;;) {
        int cfd = accept(a.listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int v4 = socket(AF_INET, SOCK_STREAM, 0);
        if (v4 < 0) { close(cfd); continue; }
        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port = htons((uint16_t)a.port);
        inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
        if (connect(v4, (struct sockaddr *)&dst, sizeof dst) != 0) {
            close(cfd);
            close(v4);
            continue;
        }
        V6ConnPair *pair = malloc(sizeof *pair);
        if (!pair) { close(cfd); close(v4); continue; }
        pair->a = cfd;
        pair->b = v4;
        pthread_t pt;
        if (pthread_create(&pt, NULL, v6_pump_thread, pair) == 0) pthread_detach(pt);
        else { free(pair); close(cfd); close(v4); }
    }
    close(a.listen_fd);
    return NULL;
}

static void start_ipv6_loopback_bridge(int port) {
    int lfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (lfd < 0) return;
    int one = 1;
    setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((uint16_t)port);
    addr.sin6_addr = in6addr_loopback;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(lfd, 16) != 0) {
        close(lfd);
        return;
    }
    V6AcceptArg *a = malloc(sizeof *a);
    if (!a) { close(lfd); return; }
    a->listen_fd = lfd;
    a->port = port;
    pthread_t t;
    if (pthread_create(&t, NULL, v6_accept_thread, a) == 0) pthread_detach(t);
    else { free(a); close(lfd); }
}

void hv_net_add_hostfwd(int host_port, int guest_port) {
    if (host_port < 1024 || host_port > 65535 || guest_port < 1 || guest_port > 65535) return;
    pthread_mutex_lock(&fwd_mu);
    int dup = 0;
    for (int i = 0; i < npending_fwds; i++)
        if (pending_fwds[i].host_port == host_port) dup = 1;
    if (!dup && npending_fwds < 32)
        pending_fwds[npending_fwds++] = (FwdReq){host_port, guest_port};
    if (wake_pipe[1] > 0) { /* under the lock: hv_net_detach closes the pipe under it */
        char c = 0;
        (void)!write(wake_pipe[1], &c, 1);
    }
    pthread_mutex_unlock(&fwd_mu);
}

void hv_net_set_share_socket(const char *path) {
    snprintf(share_socket, sizeof share_socket, "%s", path ? path : "");
}

static int push(Queue *q, const uint8_t *data, uint32_t len) {
    pthread_mutex_lock(&q->mu);
    int ok = q->tail - q->head < QUEUE;
    if (ok) {
        Frame *f = &q->f[q->tail % QUEUE];
        f->data = malloc(len);
        memcpy(f->data, data, len);
        f->len = len;
        q->tail++;
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

/* HV_NET_LOG: one line per frame (ethertype, IP protocol, ports). */
static void log_frame(const char *dir, const uint8_t *p, uint32_t len) {
    static int on = -1;
    if (on < 0) on = getenv("HV_NET_LOG") != NULL;
    if (!on || len < 14) return;
    unsigned et = p[12] << 8 | p[13];
    if (et == 0x0800 && len >= 34) {
        unsigned ihl = (p[14] & 15) * 4, proto = p[23];
        unsigned sp = 0, dp = 0;
        if ((proto == 6 || proto == 17) && len >= 14 + ihl + 4) {
            sp = p[14 + ihl] << 8 | p[15 + ihl];
            dp = p[16 + ihl] << 8 | p[17 + ihl];
        }
        fprintf(stderr, "net %s ipv4 %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u proto %u len %u\n", dir, p[26], p[27], p[28],
                p[29], sp, p[30], p[31], p[32], p[33], dp, proto, len);
    } else {
        fprintf(stderr, "net %s ethertype %#06x len %u\n", dir, et, len);
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- slirp callbacks (all on the network thread) ---- */

static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque) {
    (void)opaque;
    if (len > MAX_FRAME) return (slirp_ssize_t)len;
    log_frame("rx", buf, (uint32_t)len);
    if (push(&to_guest, buf, (uint32_t)len)) hv_machine_wake(machine);
    return (slirp_ssize_t)len;
}

static void guest_error(const char *msg, void *opaque) {
    (void)opaque;
    fprintf(stderr, "hengevm: net: %s\n", msg);
}

static int64_t clock_get_ns(void *opaque) {
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque) {
    (void)opaque;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (!timers[i].used) {
            timers[i] = (Timer){1, id, cb_opaque, -1};
            return &timers[i];
        }
    return NULL;
}

static void timer_free(void *timer, void *opaque) {
    (void)opaque;
    if (timer) ((Timer *)timer)->used = 0;
}

static void timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    if (timer) ((Timer *)timer)->expire_ms = expire_ms;
}

static void register_poll_socket(slirp_os_socket fd, void *opaque) {
    (void)fd, (void)opaque;
}

static void unregister_poll_socket(slirp_os_socket fd, void *opaque) {
    (void)fd, (void)opaque;
}

static void notify(void *opaque) {
    (void)opaque;
    char c = 0;
    (void)!write(wake_pipe[1], &c, 1);
}

static int add_poll(slirp_os_socket fd, int events, void *opaque) {
    (void)opaque;
    if (npfd == MAX_POLL) return -1;
    short ev = 0;
    if (events & SLIRP_POLL_IN) ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    pfds[npfd] = (struct pollfd){fd, ev, 0};
    return npfd++;
}

static int get_revents(int idx, void *opaque) {
    (void)opaque;
    short r = pfds[idx].revents;
    int ev = 0;
    if (r & POLLIN) ev |= SLIRP_POLL_IN;
    if (r & POLLOUT) ev |= SLIRP_POLL_OUT;
    if (r & POLLPRI) ev |= SLIRP_POLL_PRI;
    if (r & POLLERR) ev |= SLIRP_POLL_ERR;
    if (r & POLLHUP) ev |= SLIRP_POLL_HUP;
    return ev;
}

static const SlirpCb callbacks = {
    .send_packet = send_packet,
    .guest_error = guest_error,
    .clock_get_ns = clock_get_ns,
    .timer_free = timer_free,
    .timer_mod = timer_mod,
    .notify = notify,
    .timer_new_opaque = timer_new_opaque,
    .register_poll_socket = register_poll_socket,
    .unregister_poll_socket = unregister_poll_socket,
};

/* ---- the network thread ---- */

static void *net_thread(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&net_quit, __ATOMIC_ACQUIRE)) {
        uint32_t timeout = 1000;
        int64_t now = now_ms();
        for (int i = 0; i < MAX_TIMERS; i++)
            if (timers[i].used && timers[i].expire_ms >= 0) {
                int64_t left = timers[i].expire_ms - now;
                if (left < 0) left = 0;
                if ((uint64_t)left < timeout) timeout = (uint32_t)left;
            }
        npfd = 0;
        pfds[npfd++] = (struct pollfd){wake_pipe[0], POLLIN, 0};
        slirp_pollfds_fill_socket(slirp, &timeout, add_poll, NULL);
        int r = poll(pfds, (nfds_t)npfd, (int)timeout);
        if (pfds[0].revents & POLLIN) {
            char buf[64];
            while (read(wake_pipe[0], buf, sizeof buf) > 0) {}
        }
        for (;;) {
            FwdReq req = {0, 0};
            pthread_mutex_lock(&fwd_mu);
            /* Not once it is asked to end (the flag is set under this lock): that forward is the next network's. */
            if (!__atomic_load_n(&net_quit, __ATOMIC_ACQUIRE) && npending_fwds > 0) req = pending_fwds[--npending_fwds];
            pthread_mutex_unlock(&fwd_mu);
            if (!req.host_port) break;
            int exists = 0;
            for (int i = 0; i < nactive_fwds; i++)
                if (active_fwds[i] == req.host_port) exists = 1;
            if (!exists) {
                struct in_addr h, g;
                inet_pton(AF_INET, "127.0.0.1", &h);
                inet_pton(AF_INET, "10.0.2.15", &g);
                if (slirp_add_hostfwd(slirp, 0, h, req.host_port, g, req.guest_port) == 0) {
                    if (nactive_fwds < 64) active_fwds[nactive_fwds++] = req.host_port;
                    start_ipv6_loopback_bridge(req.host_port);
                    fprintf(stderr, "winvm: net: forwarded [::1]/127.0.0.1:%d -> 10.0.2.15:%d\n", req.host_port, req.guest_port);
                }
            }
        }
        slirp_pollfds_poll(slirp, r < 0, get_revents, NULL);
        now = now_ms();
        for (int i = 0; i < MAX_TIMERS; i++)
            if (timers[i].used && timers[i].expire_ms >= 0 && timers[i].expire_ms <= now) {
                timers[i].expire_ms = -1;
                slirp_handle_timer(slirp, timers[i].id, timers[i].cb_opaque);
            }
        /* What the guest sent since. */
        for (;;) {
            pthread_mutex_lock(&to_host.mu);
            Frame f = {0, NULL};
            if (to_host.head != to_host.tail) f = to_host.f[to_host.head++ % QUEUE];
            pthread_mutex_unlock(&to_host.mu);
            if (!f.data) break;
            slirp_input(slirp, f.data, (int)f.len);
            free(f.data);
        }
    }
    return NULL;
}

/* CPU thread: the guest sent a frame. */
static void transmit(void *opaque, const uint8_t *frame, uint32_t len) {
    (void)opaque;
    hv_net_tx_frames++;
    log_frame("tx", frame, len);
    if (push(&to_host, frame, len)) {
        char c = 0;
        (void)!write(wake_pipe[1], &c, 1);
    }
}

void hv_net_poll(void) {
    if (!net) return;
    for (;;) {
        pthread_mutex_lock(&to_guest.mu);
        Frame f = {0, NULL};
        if (to_guest.head != to_guest.tail) f = to_guest.f[to_guest.head % QUEUE];
        pthread_mutex_unlock(&to_guest.mu);
        if (!f.data) return;
        if (!hv_virtio_net_receive(net, f.data, f.len)) return; /* no buffer posted yet: try again later */
        hv_net_rx_frames++;
        pthread_mutex_lock(&to_guest.mu);
        to_guest.head++;
        pthread_mutex_unlock(&to_guest.mu);
        free(f.data);
    }
}

static void queue_empty(Queue *q) {
    for (; q->head != q->tail; q->head++) free(q->f[q->head % QUEUE].data);
    q->head = q->tail = 0;
    pthread_mutex_destroy(&q->mu);
}

/* Ends the network, or what a failed attach got as far as making: its thread, its libslirp with every socket and
   timer it had, and what was queued either way. The VM's CPU threads have stopped (the caller sees to it), so
   nothing else is using any of it. Without this each start added a thread, and from the second start on two of
   them ran one libslirp (a double free in if_start within half a minute on a phone). The thread looks at net_quit
   at least once a second, its longest poll. */
void hv_net_detach(void) {
    if (net_started) {
        pthread_mutex_lock(&fwd_mu);
        __atomic_store_n(&net_quit, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&fwd_mu);
        char c = 0;
        (void)!write(wake_pipe[1], &c, 1);
        pthread_join(net_tid, NULL);
        net_started = 0;
        __atomic_store_n(&net_quit, 0, __ATOMIC_RELEASE);
    }
    if (net) net->transmit = NULL; /* the device stays with its VM: nothing of this network may be called through it */
    net = NULL;
    if (slirp) slirp_cleanup(slirp);
    slirp = NULL;
    memset(timers, 0, sizeof timers);
    if (queues_made) {
        queue_empty(&to_guest);
        queue_empty(&to_host);
        queues_made = 0;
    }
    pthread_mutex_lock(&fwd_mu);
    if (wake_pipe[1] > 0) { /* a pipe's writing end is never descriptor 0 */
        close(wake_pipe[0]);
        close(wake_pipe[1]);
    }
    wake_pipe[0] = wake_pipe[1] = 0;
    nactive_fwds = 0; /* they were the old libslirp's: a port asked for again is forwarded again */
    pthread_mutex_unlock(&fwd_mu);
}

int hv_net_attach(HvMachine *m, HvVirtioNet *n) {
    hv_net_detach();
    machine = m;
    pthread_mutex_init(&to_guest.mu, NULL);
    pthread_mutex_init(&to_host.mu, NULL);
    queues_made = 1;
    int ends[2];
    if (pipe(ends) != 0) {
        hv_net_detach();
        return -1;
    }
    pthread_mutex_lock(&fwd_mu);
    wake_pipe[0] = ends[0], wake_pipe[1] = ends[1];
    pthread_mutex_unlock(&fwd_mu);
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    SlirpConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.version = 6; /* below 6 it calls the old register_poll_fd, which we leave unset */
    cfg.in_enabled = true;
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
    cfg.vhostname = "hengevm";
    cfg.if_mtu = 1500;
    cfg.if_mru = 1500;
    slirp = slirp_new(&cfg, &callbacks, NULL);
    if (!slirp) {
        hv_net_detach();
        return -1;
    }
    if (getenv("HV_NET_FWD")) { /* debugging: "hostport:guestport", from this Mac's loopback into the guest */
        int hp, gp;
        struct in_addr h, g;
        if (sscanf(getenv("HV_NET_FWD"), "%d:%d", &hp, &gp) == 2) {
            inet_pton(AF_INET, "127.0.0.1", &h);
            inet_pton(AF_INET, "10.0.2.15", &g);
            if (slirp_add_hostfwd(slirp, 0, h, hp, g, gp) != 0) fprintf(stderr, "hengevm: no forward for port %d\n", hp);
        }
    }
    if (share_socket[0]) { /* 10.0.2.4:80 is the shared folder and clipboard: see share.c */
        struct in_addr addr;
        inet_pton(AF_INET, HV_SHARE_ADDR, &addr);
        if (slirp_add_unix(slirp, share_socket, &addr, 80) != 0) fprintf(stderr, "hengevm: net: no route to the shared folder\n");
    }
    if (pthread_create(&net_tid, NULL, net_thread, NULL) != 0) {
        hv_net_detach();
        return -1;
    }
    net_started = 1;
    net = n;
    n->transmit = transmit; /* last: the guest can only send into a network that is all there */
    return 0;
}

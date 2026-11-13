/* host/share.c's text focus channel: GET /.henge/focus-wait (the long poll the agent in Windows sits in) and
   GET /.henge/focus (its report), over a real Unix socket. The wait and staleness limits are shortened. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "share.h"

extern unsigned hv_share_focus_wait_ms, hv_share_focus_stale_ms, hv_share_focus_report_ms;

void hv_net_add_hostfwd(int host_port, int guest_port) { (void)host_port, (void)guest_port; }

static int checks, failures;
static char sock_path[128];

static void check_str(const char *what, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want)) {
        printf("  FAIL %s: \"%s\", expected \"%s\"\n", what, got, want);
        failures++;
    }
}

static void check(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %ld, expected %ld\n", what, got, want);
        failures++;
    }
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One GET on a fresh connection. Returns the status; the body is copied to body. */
static int get(const char *target, char *body, size_t cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("connect");
        exit(2);
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 10.0.2.4\r\nConnection: close\r\n\r\n", target);
    if (write(fd, req, (size_t)n) != n) exit(2);
    char resp[2048];
    size_t have = 0;
    ssize_t got;
    while (have < sizeof resp - 1 && (got = read(fd, resp + have, sizeof resp - 1 - have)) > 0) have += (size_t)got;
    close(fd);
    resp[have] = 0;
    body[0] = 0;
    char *split = strstr(resp, "\r\n\r\n");
    if (split) snprintf(body, cap, "%s", split + 4);
    int status = 0;
    sscanf(resp, "HTTP/1.1 %d", &status);
    return status;
}

static void *tap_later(void *arg) {
    (void)arg;
    usleep(150000);
    hv_share_note_tap(5, 7);
    return NULL;
}

static int calls, last_editable, last_inside;
static unsigned long last_seq;
static void *last_ctx;
static void handler(unsigned long seq, int editable, int inside, void *ctx) {
    calls++;
    last_seq = seq;
    last_editable = editable;
    last_inside = inside;
    last_ctx = ctx;
}

int main(void) {
    char dir[] = "/tmp/test_share_focus.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(sock_path, sizeof sock_path, "%s/s", dir);

    /* Noting events before the server is up must be safe. */
    hv_share_note_look();
    hv_share_note_look();
    if (hv_share_start(dir, sock_path, NULL) != 0) { printf("  cannot start the share\n"); return 2; }
    hv_share_focus_wait_ms = 400;
    hv_share_focus_stale_ms = 300;

    char body[256];
    check("agent never seen", hv_share_focus_agent_seen(), -1);

    long t0 = now_ms();
    check("wait without after: status", get("/.henge/focus-wait", body, sizeof body), 200);
    check_str("wait without after learns the seq", body, "2 none -1 -1");
    check("wait without after answers at once", now_ms() - t0 < 250, 1);
    check("agent seen after a wait", hv_share_focus_agent_seen(), 0);
    get("/.henge/focus-wait?after=abc", body, sizeof body);
    check_str("wait with a malformed after", body, "2 none -1 -1");
    get("/.henge/focus-wait?after=", body, sizeof body);
    check_str("wait with an empty after", body, "2 none -1 -1");

    pthread_t th;
    pthread_create(&th, NULL, tap_later, NULL);
    t0 = now_ms();
    check("blocking wait: status", get("/.henge/focus-wait?after=2", body, sizeof body), 200);
    long waited = now_ms() - t0;
    check_str("a wait blocks until a tap and returns it", body, "3 tap 5 7");
    check("the wait did block", waited >= 100, 1);
    pthread_join(th, NULL);

    hv_share_note_tap(10, 20);
    t0 = now_ms();
    get("/.henge/focus-wait?after=3", body, sizeof body);
    check_str("a tap noted before the wait is returned", body, "4 tap 10 20");
    check("and at once", now_ms() - t0 < 250, 1);

    hv_share_note_look();
    get("/.henge/focus-wait?after=4", body, sizeof body);
    check_str("a look", body, "5 look -1 -1");

    hv_share_note_tap(1, 1);
    usleep(400000);
    t0 = now_ms();
    get("/.henge/focus-wait?after=5", body, sizeof body);
    check_str("a stale tap is not returned", body, "6 none -1 -1");
    check("and does not block", now_ms() - t0 < 250, 1);

    t0 = now_ms();
    get("/.henge/focus-wait?after=6", body, sizeof body);
    waited = now_ms() - t0;
    check_str("a wait with nothing new times out", body, "6 none -1 -1");
    check("after the wait limit", waited >= 350 && waited < 1500, 1);

    /* Reports. */
    void *ctx = &calls;
    hv_share_set_text_focus_handler(handler, ctx);
    check("noting a tap returns its number", (long)hv_share_note_tap(3, 4), 7);
    check("report: status", get("/.henge/focus?seq=7&editable=1&inside=0", body, sizeof body), 204);
    check("report with the current seq reaches the handler", calls, 1);
    check("the event's number passed", (long)last_seq, 7);
    check("editable passed", last_editable, 1);
    check("inside passed", last_inside, 0);
    check("ctx passed", last_ctx == ctx, 1);
    get("/.henge/focus?seq=7&editable=0&inside=1", body, sizeof body);
    check("second report", calls, 2);
    check("editable passed (0)", last_editable, 0);
    check("inside passed (1)", last_inside, 1);

    check("report for an older seq: status", get("/.henge/focus?seq=6&editable=1&inside=1", body, sizeof body), 204);
    check("report for an older seq is ignored", calls, 2);
    get("/.henge/focus?seq=8&editable=1&inside=1", body, sizeof body);
    check("report for a seq not yet issued is ignored", calls, 2);

    check("missing seq", get("/.henge/focus?editable=1&inside=1", body, sizeof body), 400);
    check("missing editable", get("/.henge/focus?seq=7&inside=1", body, sizeof body), 400);
    check("missing inside", get("/.henge/focus?seq=7&editable=1", body, sizeof body), 400);
    check("no query", get("/.henge/focus", body, sizeof body), 400);
    check("seq not a number", get("/.henge/focus?seq=x&editable=1&inside=1", body, sizeof body), 400);
    check("editable out of range", get("/.henge/focus?seq=7&editable=2&inside=1", body, sizeof body), 400);
    check("inside with junk", get("/.henge/focus?seq=7&editable=1&inside=1x", body, sizeof body), 400);
    check("a field name that only has the right prefix", get("/.henge/focus?seq=7&editable=1&insidex=1", body, sizeof body), 400);
    check("malformed reports never reach the handler", calls, 2);

    /* A report that comes long after its event (the agent was held up) is not acted on. */
    hv_share_focus_report_ms = 200;
    check("noting a look returns its number", (long)hv_share_note_look(), 8);
    get("/.henge/focus?seq=8&editable=1&inside=0", body, sizeof body);
    check("a report in time reaches the handler", calls, 3);
    usleep(350000);
    check("late report: status", get("/.henge/focus?seq=8&editable=1&inside=0", body, sizeof body), 204);
    check("a late report is ignored", calls, 3);
    hv_share_focus_report_ms = 4000;

    hv_share_set_text_focus_handler(NULL, NULL);
    check("no handler: status", get("/.henge/focus?seq=8&editable=1&inside=1", body, sizeof body), 204);
    check("a removed handler is not called", calls, 3);

    unlink(sock_path);
    rmdir(dir);
    printf("%s: %d checks, %d failures\n", "test_share_focus", checks, failures);
    return failures ? 1 : 0;
}

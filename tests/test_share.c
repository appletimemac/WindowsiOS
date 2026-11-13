/* host/share.c's channels to the agent in Windows, over a real Unix socket: the text focus channel (GET
   /.henge/focus-wait, the long poll the agent sits in, and GET /.henge/focus, its report; the wait and staleness
   limits are shortened) and GET /.henge/posture with the display scale. */
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
#include "../tools/winios_agent_scale.h"

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
/* What the agent makes of the part of a posture reply after the posture word. */
static int agent_reads(const char *tail) { return agent_scale_parse(tail, (int)strlen(tail)); }

static int scale_calls, scale_heard, scale_ctx_wrong;

/* Reads the scale as a real handler does: a handler called with the share's lock held would hang here. */
static void on_agent_scale(void *ctx) {
    if (ctx != &scale_calls) scale_ctx_wrong++;
    scale_calls++;
    scale_heard = hv_share_agent_display_scale();
}

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
    char dir[] = "/tmp/test_share.XXXXXX";
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

    /* Posture: "laptop" or "tablet", then " scale=NNN" once the app has set a display scale. The first reply is
       before any scale was set. */
    check("posture: status", get("/.henge/posture", body, sizeof body), 200);
    check_str("posture without a scale", body, "tablet");
    hv_share_set_display_scale(150);
    get("/.henge/posture", body, sizeof body);
    check_str("posture with a scale", body, "tablet scale=150");
    hv_share_set_display_scale(100);
    get("/.henge/posture", body, sizeof body);
    check_str("the lowest scale is kept", body, "tablet scale=100");
    hv_share_set_display_scale(500);
    get("/.henge/posture", body, sizeof body);
    check_str("the highest scale is kept", body, "tablet scale=500");
    hv_share_set_display_scale(50);
    get("/.henge/posture", body, sizeof body);
    check_str("a scale below 100 is clamped up", body, "tablet scale=100");
    hv_share_set_display_scale(-30);
    get("/.henge/posture", body, sizeof body);
    check_str("a negative scale is clamped up", body, "tablet scale=100");
    hv_share_set_display_scale(900);
    get("/.henge/posture", body, sizeof body);
    check_str("a scale above 500 is clamped down", body, "tablet scale=500");
    hv_share_set_display_scale(0);
    get("/.henge/posture", body, sizeof body);
    check_str("a scale of 0 is none again", body, "tablet");
    hv_share_set_posture(1);
    get("/.henge/posture", body, sizeof body);
    check_str("laptop without a scale", body, "laptop");
    hv_share_set_display_scale(200);
    get("/.henge/posture", body, sizeof body);
    check_str("laptop with a scale", body, "laptop scale=200");
    hv_share_set_display_scale(0);
    hv_share_set_posture(0);
    get("/.henge/posture", body, sizeof body);
    check_str("back to tablet without a scale", body, "tablet");

    /* The agent's end of that reply (tools/winios_agent_scale.h): the number it would make Windows' scale. Every
       scale the app's settings can ask for gets through; one that would leave a phone's display no room does not. */
    static const int asked[] = {100, 125, 150, 175, 200, 225, 250};
    for (size_t i = 0; i < sizeof asked / sizeof asked[0]; i++) {
        hv_share_set_display_scale(asked[i]);
        get("/.henge/posture", body, sizeof body);
        check("the agent reads the scale the host sent", agent_reads(body + 6), asked[i]);
    }
    hv_share_set_display_scale(251);
    get("/.henge/posture", body, sizeof body);
    check_str("the host passes on a scale past the agent's limit", body, "tablet scale=251");
    check("and the agent does not take it", agent_reads(body + 6), 0);
    hv_share_set_display_scale(500);
    get("/.henge/posture", body, sizeof body);
    check("nor the host's highest", agent_reads(body + 6), 0);
    hv_share_set_display_scale(0);
    get("/.henge/posture", body, sizeof body);
    check("no scale in the reply is none for the agent", agent_reads(body + 6), 0);
    check("agent: the lowest scale", agent_reads(" scale=100"), 100);
    check("agent: under the lowest", agent_reads(" scale=99"), 0);
    check("agent: zero", agent_reads(" scale=0"), 0);
    check("agent: a negative number", agent_reads(" scale=-150"), 0);
    check("agent: no number", agent_reads(" scale="), 0);
    check("agent: letters after the number", agent_reads(" scale=150x"), 0);
    check("agent: a number too long to be a scale", agent_reads(" scale=15000000000000000000"), 0);
    check("agent: another word after the scale", agent_reads(" scale=150 later=1"), 150);
    check("agent: another word in its place", agent_reads(" later=150"), 0);
    check("agent: a word one letter off", agent_reads(" scald=150"), 0);
    check("agent: the key without its space", agent_reads("scale=150"), 0);
    check("agent: a reply cut short", agent_scale_parse(" scale=150", 5), 0);
    check("agent: reads no further than the reply is long", agent_scale_parse(" scale=1500", 10), 150);
    static const struct { int percent; unsigned dpi; } dots[] = {{100, 96}, {125, 120}, {150, 144}, {175, 168},
                                                                 {200, 192}, {225, 216}, {250, 240}};
    for (size_t i = 0; i < sizeof dots / sizeof dots[0]; i++) {
        check("agent: dots per inch for a scale", agent_scale_dpi(dots[i].percent), dots[i].dpi);
        check("agent: the scale of a session at those", agent_scale_percent(dots[i].dpi), dots[i].percent);
    }
    check("agent: a scale between steps keeps its dots", agent_scale_dpi(110), 106);
    check("agent: and reads back as itself", agent_scale_percent(106), 110);
    check("agent: a session's scale is rounded to the nearest percent", agent_scale_percent(115), 120);

    /* The agent says what scale Windows is running at with its poll. The polls above carried none. */
    hv_share_set_agent_display_scale_handler(on_agent_scale, &scale_calls);
    check("no report yet", hv_share_agent_display_scale(), 0);
    check("poll with a report: status", get("/.henge/posture?scale=150", body, sizeof body), 200);
    check_str("a poll with a report is still answered", body, "tablet");
    check("the report is kept", hv_share_agent_display_scale(), 150);
    check("the handler hears the first report", scale_calls, 1);
    check("with its scale", scale_heard, 150);
    get("/.henge/posture?scale=150", body, sizeof body);
    check("the same report again is not news", scale_calls, 1);
    get("/.henge/posture", body, sizeof body);
    check("a poll without one leaves it", hv_share_agent_display_scale(), 150);
    check("and is not news", scale_calls, 1);
    get("/.henge/posture?scale=350", body, sizeof body);
    check("a newer report replaces it", hv_share_agent_display_scale(), 350);
    check("the handler hears the change", scale_calls, 2);
    check("with the new scale", scale_heard, 350);
    get("/.henge/posture?scale=99", body, sizeof body);
    check("a report under 100 is ignored", hv_share_agent_display_scale(), 350);
    get("/.henge/posture?scale=501", body, sizeof body);
    check("a report over 500 is ignored", hv_share_agent_display_scale(), 350);
    get("/.henge/posture?scale=1x", body, sizeof body);
    check("a malformed report is ignored", hv_share_agent_display_scale(), 350);
    get("/.henge/posture?scale=", body, sizeof body);
    check("an empty report is ignored", hv_share_agent_display_scale(), 350);
    check("an ignored report is not news", scale_calls, 2);
    get("/.henge/posture?scale=100", body, sizeof body);
    check("the lowest report is kept", hv_share_agent_display_scale(), 100);
    get("/.henge/posture?scale=500", body, sizeof body);
    check("the highest report is kept", hv_share_agent_display_scale(), 500);
    hv_share_set_display_scale(200);
    get("/.henge/posture?scale=125", body, sizeof body);
    check_str("what the app wants is answered whatever Windows is at", body, "tablet scale=200");
    check("and what Windows is at is kept apart from it", hv_share_agent_display_scale(), 125);
    int before = scale_calls;
    hv_share_clear_agent_display_scale();
    check("a new VM starts without a report", hv_share_agent_display_scale(), 0);
    check("the handler hears it forgotten", scale_calls, before + 1);
    check("as no scale", scale_heard, 0);
    check("the handler got the context it was set with every time", scale_ctx_wrong, 0);
    get("/.henge/posture", body, sizeof body);
    check_str("what the app wants outlives the report", body, "tablet scale=200");
    hv_share_set_display_scale(0);
    hv_share_clear_agent_display_scale();
    check("forgetting nothing is not news", scale_calls, before + 1);
    hv_share_set_agent_display_scale_handler(NULL, NULL);
    get("/.henge/posture?scale=175", body, sizeof body);
    check("a removed handler is not called", scale_calls, before + 1);
    check("the report is still kept", hv_share_agent_display_scale(), 175);
    hv_share_clear_agent_display_scale();

    unlink(sock_path);
    rmdir(dir);
    printf("%s: %d checks, %d failures\n", "test_share", checks, failures);
    return failures ? 1 : 0;
}

/* The folder and clipboard a virtual Mac shares with the device it runs on.

   The folder is a small WebDAV server, because macOS has a WebDAV client built in, so it mounts like any
   network drive with nothing to install for that part. The network hands each connection the guest makes to
   10.0.2.4:80 to a Unix socket in the app's own storage: no TCP port is open, so other apps can't reach this.

   /.henge/ is not part of the folder. It has the clipboard, where the helper inside macOS (tools/henge-agent.m)
   waits for the device's clipboard and sends the guest's, and what it takes to install that helper.

   Locks are accepted but not enforced: macOS only writes to a server that offers them, and there is one client. */
#include "share.h"
#include "net_slirp.h"

#include <TargetConditionals.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <removefile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HEAD_MAX (32 * 1024)
#define SMALL_BODY_MAX (16 * 1024 * 1024) /* a clipboard, or a request's XML */
#define TEMP_PREFIX ".henge-put-"

static char root[PATH_MAX];
static char agent_path[PATH_MAX];

/* ---- the clipboard ---- */

static pthread_mutex_t clip_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clip_cond = PTHREAD_COND_INITIALIZER;
static char *device_clip; /* the device's, for the guest */
static unsigned long device_seq;
static char *guest_clip; /* the guest's, until the device takes it */
static char *guest_open_url;
static char *last_oauth_url;
static time_t last_oauth_seen;
static int guest_oauth_done;
static int url_handler_enabled = -1;
static time_t agent_seen;
static int posture_laptop; /* atomic; 0 = tablet */

void hv_share_set_posture(int laptop) { __atomic_store_n(&posture_laptop, laptop ? 1 : 0, __ATOMIC_RELAXED); }

/* The Windows display scale the app wants, in percent (0 = it has not said), atomic. GET /.henge/posture appends it
   as " scale=NNN"; the agent in Windows writes it down for Windows' next sign-in, which is when Windows takes a scale. */
static int display_scale;

void hv_share_set_display_scale(int percent) {
    if (percent != 0) percent = percent < 100 ? 100 : percent > 500 ? 500 : percent;
    __atomic_store_n(&display_scale, percent, __ATOMIC_RELAXED);
}

/* The scale Windows is running at, in percent, as the agent last said with its posture poll ("?scale=NNN"); 0 until
   one has (no agent yet, or one from before it said this). The handler is told of each change outside the lock, and
   only that there was one: it reads the scale itself, so calls that arrive out of order still leave it the latest. */
static pthread_mutex_t agent_scale_mu = PTHREAD_MUTEX_INITIALIZER;
static int agent_display_scale;
static void (*agent_scale_handler)(void *);
static void *agent_scale_handler_ctx;

static void agent_scale_set(int percent) {
    pthread_mutex_lock(&agent_scale_mu);
    int changed = agent_display_scale != percent;
    agent_display_scale = percent;
    void (*fn)(void *) = agent_scale_handler;
    void *ctx = agent_scale_handler_ctx;
    pthread_mutex_unlock(&agent_scale_mu);
    if (changed && fn) fn(ctx);
}

int hv_share_agent_display_scale(void) {
    pthread_mutex_lock(&agent_scale_mu);
    int percent = agent_display_scale;
    pthread_mutex_unlock(&agent_scale_mu);
    return percent;
}

void hv_share_clear_agent_display_scale(void) { agent_scale_set(0); }

void hv_share_set_agent_display_scale_handler(void (*fn)(void *ctx), void *ctx) {
    pthread_mutex_lock(&agent_scale_mu);
    agent_scale_handler = fn;
    agent_scale_handler_ctx = ctx;
    pthread_mutex_unlock(&agent_scale_mu);
}

/* When the app asked Windows to hibernate (CLOCK_UPTIME_RAW ns, 0 for none), atomic. GET /.henge/power, which
   WinIOSAgent polls twice a second, answers it once. One the agent did not take within HIBERNATE_TTL_NS (no agent
   installed, Windows still booting, a second tap while it saves) is dropped rather than left for a later agent,
   which would hibernate a session nobody asked to. */
#define HIBERNATE_TTL_NS (10ull * 1000000000ull)
static uint64_t hibernate_requested_at;
void hv_share_request_hibernate(void) {
    __atomic_store_n(&hibernate_requested_at, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
}
void hv_share_clear_hibernate(void) { __atomic_store_n(&hibernate_requested_at, 0, __ATOMIC_RELAXED); }

/* Text focus. The agent inside Windows long-polls GET /.henge/focus-wait; the app calls hv_share_note_tap when a
   touch lands on the picture (or hv_share_note_look after Enter) and the poll answers with that event. The agent
   then asks UI Automation what has focus and reports it at GET /.henge/focus, which reaches the app's handler. Only
   the newest event is kept: a report for an older one is dropped because a newer tap is already being looked at.
   The limits are variables so tests/test_share.c can shorten them. */
unsigned hv_share_focus_wait_ms = 8000;   /* how long a poll waits for an event before answering "none" */
unsigned hv_share_focus_stale_ms = 2000;  /* an event older than this is not worth popping a keyboard for */
unsigned hv_share_focus_report_ms = 4000; /* nor is a report this long after its event (a hung app held the agent) */

enum { FOCUS_NONE, FOCUS_TAP, FOCUS_LOOK };
static pthread_mutex_t focus_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t focus_cond = PTHREAD_COND_INITIALIZER;
static unsigned long focus_seq;
static int focus_kind, focus_x, focus_y;
static uint64_t focus_at; /* CLOCK_UPTIME_RAW ns */
static void (*focus_handler)(unsigned long, int, int, void *);
static void *focus_handler_ctx;
static uint64_t focus_agent_at; /* atomic; CLOCK_UPTIME_RAW ns, 0 for never */

static unsigned long focus_note(int kind, int x, int y) {
    pthread_mutex_lock(&focus_mu);
    unsigned long seq = ++focus_seq;
    focus_kind = kind;
    focus_x = x;
    focus_y = y;
    focus_at = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_cond_broadcast(&focus_cond);
    pthread_mutex_unlock(&focus_mu);
    return seq;
}

unsigned long hv_share_note_tap(int x, int y) { return focus_note(FOCUS_TAP, x, y); }
unsigned long hv_share_note_look(void) { return focus_note(FOCUS_LOOK, -1, -1); }

void hv_share_set_text_focus_handler(void (*fn)(unsigned long seq, int editable, int inside, void *ctx), void *ctx) {
    pthread_mutex_lock(&focus_mu);
    focus_handler = fn;
    focus_handler_ctx = ctx;
    pthread_mutex_unlock(&focus_mu);
}

long hv_share_focus_agent_seen(void) {
    uint64_t at = __atomic_load_n(&focus_agent_at, __ATOMIC_RELAXED);
    if (!at) return -1;
    return (long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - at) / 1000000000ull);
}

static int is_url_handler_on(void) {
    if (url_handler_enabled < 0) {
#if TARGET_OS_IPHONE
        url_handler_enabled = 1;
#else
        url_handler_enabled = getenv("HV_OAUTH_BRIDGE") != NULL ? 1 : 0;
#endif
    }
    return url_handler_enabled;
}

void hv_share_set_url_handler_enabled(int enabled) {
    pthread_mutex_lock(&clip_mu);
    int prev = is_url_handler_on();
    url_handler_enabled = enabled ? 1 : 0;
    /* If user just switched OAuth: Edge -> OAuth: Safari while an OAuth login is in flight, pop SafariSheet immediately */
    if (!prev && url_handler_enabled && !guest_open_url && last_oauth_url &&
        (time(NULL) - last_oauth_seen) <= 300) {
        guest_open_url = last_oauth_url;
        last_oauth_url = NULL;
    }
    pthread_mutex_unlock(&clip_mu);
}

int hv_share_url_handler_enabled(void) {
    pthread_mutex_lock(&clip_mu);
    int v = is_url_handler_on();
    pthread_mutex_unlock(&clip_mu);
    return v;
}

char *hv_share_take_open_url(void) {
    pthread_mutex_lock(&clip_mu);
    char *url = guest_open_url;
    guest_open_url = NULL;
    pthread_mutex_unlock(&clip_mu);
    return url;
}

int hv_share_take_oauth_done(void) {
    pthread_mutex_lock(&clip_mu);
    int done = guest_oauth_done;
    guest_oauth_done = 0;
    pthread_mutex_unlock(&clip_mu);
    return done;
}

void hv_share_set_clipboard(const char *text) {
    pthread_mutex_lock(&clip_mu);
    free(device_clip);
    device_clip = strdup(text);
    device_seq++;
    pthread_cond_broadcast(&clip_cond);
    pthread_mutex_unlock(&clip_mu);
}

char *hv_share_take_clipboard(void) {
    pthread_mutex_lock(&clip_mu);
    char *text = guest_clip;
    guest_clip = NULL;
    pthread_mutex_unlock(&clip_mu);
    return text;
}

long hv_share_agent_seen(void) {
    pthread_mutex_lock(&clip_mu);
    long ago = agent_seen ? (long)(time(NULL) - agent_seen) : -1;
    pthread_mutex_unlock(&clip_mu);
    return ago;
}

/* ---- connections ---- */

typedef struct Conn {
    int fd;
    char buf[64 * 1024]; /* read ahead */
    size_t at, have;
} Conn;

typedef struct Request {
    char method[16];
    char target[HEAD_MAX]; /* as sent, without the query */
    char path[PATH_MAX];   /* decoded */
    char query[128];
    char destination[PATH_MAX]; /* decoded path */
    char if_header[256];
    long long length;
    int chunked, expect_continue, close, overwrite, depth;
    long long range_from, range_to; /* -1: none */
    long chunk_left;
    int body_done;
} Request;

/* A growing string for XML. */
typedef struct Text {
    char *s;
    size_t len, cap;
} Text;

static void add(Text *t, const char *s, size_t n) {
    if (t->len + n + 1 > t->cap) {
        t->cap = (t->len + n + 1) * 2;
        t->s = realloc(t->s, t->cap);
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void adds(Text *t, const char *s) { add(t, s, strlen(s)); }

static void addf(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void addf(Text *t, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) add(t, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

static void add_xml(Text *t, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': adds(t, "&amp;"); break;
        case '<': adds(t, "&lt;"); break;
        case '>': adds(t, "&gt;"); break;
        case '"': adds(t, "&quot;"); break;
        default: add(t, s, 1);
        }
    }
}

static void add_url(Text *t, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/", c)) add(t, s, 1);
        else addf(t, "%%%02X", c);
    }
}

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w, n -= (size_t)w;
    }
    return 0;
}

static ssize_t conn_read(Conn *c, void *out, size_t n) {
    if (c->at < c->have) {
        size_t take = c->have - c->at < n ? c->have - c->at : n;
        memcpy(out, c->buf + c->at, take);
        c->at += take;
        return (ssize_t)take;
    }
    for (;;) {
        ssize_t r = recv(c->fd, out, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

/* A line of a chunked body's framing. */
static int conn_line(Conn *c, char *line, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        if (conn_read(c, &ch, 1) != 1) return -1;
        if (ch == '\n') {
            if (n && line[n - 1] == '\r') n--;
            line[n] = 0;
            return 0;
        }
        line[n++] = ch;
    }
    return -1;
}

/* The next piece of the request's body: >0 bytes, 0 at its end, <0 if the connection broke. */
static ssize_t body_read(Conn *c, Request *r, void *out, size_t n) {
    if (r->body_done) return 0;
    if (r->chunked) {
        if (r->chunk_left == 0) {
            char line[64];
            if (conn_line(c, line, sizeof line) != 0) return -1;
            if (!line[0] && conn_line(c, line, sizeof line) != 0) return -1; /* the CRLF ending the last chunk */
            r->chunk_left = strtol(line, NULL, 16);
            if (r->chunk_left < 0) return -1;
            if (r->chunk_left == 0) {
                while (conn_line(c, line, sizeof line) == 0 && line[0]) {} /* trailers */
                r->body_done = 1;
                return 0;
            }
        }
        size_t take = (size_t)r->chunk_left < n ? (size_t)r->chunk_left : n;
        ssize_t got = conn_read(c, out, take);
        if (got <= 0) return -1;
        r->chunk_left -= got;
        return got;
    }
    if (r->length <= 0) {
        r->body_done = 1;
        return 0;
    }
    size_t take = (unsigned long long)r->length < n ? (size_t)r->length : n;
    ssize_t got = conn_read(c, out, take);
    if (got <= 0) return -1;
    r->length -= got;
    return got;
}

static void body_ready(Conn *c, Request *r) {
    if (r->expect_continue) {
        r->expect_continue = 0;
        static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(c->fd, go, sizeof go - 1);
    }
}

/* Reads what is left of a body, so the connection can carry on. Returns -1 if it can't. */
static int body_discard(Conn *c, Request *r) {
    char junk[8192];
    body_ready(c, r);
    for (;;) {
        ssize_t got = body_read(c, r, junk, sizeof junk);
        if (got == 0) return 0;
        if (got < 0) return -1;
    }
}

static char *body_small(Conn *c, Request *r, size_t *len) {
    Text t = {0};
    char piece[16384];
    body_ready(c, r);
    for (;;) {
        ssize_t got = body_read(c, r, piece, sizeof piece);
        if (got < 0 || t.len + (size_t)got > SMALL_BODY_MAX) {
            free(t.s);
            return NULL;
        }
        if (got == 0) break;
        add(&t, piece, (size_t)got);
    }
    add(&t, "", 0);
    *len = t.len;
    return t.s;
}

static int respond(Conn *c, Request *r, int status, const char *reason, const char *headers, const char *body, size_t len) {
    char head[2048];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nServer: Henge\r\nContent-Length: %zu\r\n%s%s\r\n", status, reason, len,
                     headers ? headers : "", r->close ? "Connection: close\r\n" : "");
    if (send_all(c->fd, head, (size_t)n) != 0) return -1;
    return (body && len && strcmp(r->method, "HEAD") != 0) ? send_all(c->fd, body, len) : 0;
}

static int respond_empty(Conn *c, Request *r, int status, const char *reason) {
    return respond(c, r, status, reason, NULL, NULL, 0);
}

/* ---- paths ---- */

static int hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decodes a URL path, and refuses any that could leave the folder. */
static int decode_path(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int a = hex(p[1]), b = a < 0 ? -1 : hex(p[2]);
            if (a < 0 || b < 0) return -1;
            c = a * 16 + b;
            p += 2;
            if (c == 0) return -1;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (out[0] != '/') return -1;
    for (const char *s = out; s; s = strchr(s + 1, '/')) {
        const char *part = s + 1, *end = strchr(part, '/');
        size_t len = end ? (size_t)(end - part) : strlen(part);
        if (len == 2 && part[0] == '.' && part[1] == '.') return -1;
    }
    return 0;
}

static int local_path(const char *path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", root, path);
    if (n < 0 || (size_t)n >= cap) return -1;
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    return 0;
}

/* ---- the folder ---- */

static void add_entry(Text *t, const char *url_path, const char *name, const struct stat *st, int is_root) {
    int dir = S_ISDIR(st->st_mode);
    char modified[64], created[64];
    struct tm tm;
    gmtime_r(&st->st_mtimespec.tv_sec, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    gmtime_r(&st->st_birthtimespec.tv_sec, &tm);
    strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", &tm);
    adds(t, "<D:response><D:href>");
    add_url(t, url_path);
    if (dir && url_path[strlen(url_path) - 1] != '/') adds(t, "/");
    adds(t, "</D:href><D:propstat><D:prop><D:displayname>");
    add_xml(t, name);
    addf(t, "</D:displayname><D:getlastmodified>%s</D:getlastmodified><D:creationdate>%s</D:creationdate>", modified, created);
    if (dir) {
        adds(t, "<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        addf(t, "<D:resourcetype/><D:getcontentlength>%lld</D:getcontentlength><D:getcontenttype>application/octet-stream</D:getcontenttype>"
                "<D:getetag>\"%llx-%llx\"</D:getetag>",
             (long long)st->st_size, (unsigned long long)st->st_size, (unsigned long long)st->st_mtimespec.tv_sec);
    }
    adds(t, "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>"
            "<D:lockdiscovery/>");
    if (is_root) {
        struct statfs fs;
        if (statfs(root, &fs) == 0) {
            addf(t, "<D:quota-available-bytes>%llu</D:quota-available-bytes><D:quota-used-bytes>%llu</D:quota-used-bytes>",
                 (unsigned long long)fs.f_bavail * fs.f_bsize, (unsigned long long)(fs.f_blocks - fs.f_bfree) * fs.f_bsize);
        }
    }
    adds(t, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

static int do_propfind(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0 || stat(local, &st) != 0) return respond_empty(c, r, 404, "Not Found");
    Text t = {0};
    adds(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">");
    const char *slash = strrchr(r->path, '/');
    int is_root = strcmp(r->path, "/") == 0;
    add_entry(&t, r->path, is_root ? "Henge" : (slash && slash[1] ? slash + 1 : r->path), &st, is_root);
    if (S_ISDIR(st.st_mode) && r->depth != 0) {
        DIR *d = opendir(local);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, TEMP_PREFIX, strlen(TEMP_PREFIX))) continue;
            char child[PATH_MAX], url[PATH_MAX];
            struct stat cs;
            if (snprintf(child, sizeof child, "%s/%s", local, e->d_name) >= (int)sizeof child) continue;
            if (snprintf(url, sizeof url, "%s%s%s", r->path, r->path[strlen(r->path) - 1] == '/' ? "" : "/", e->d_name) >= (int)sizeof url) continue;
            if (lstat(child, &cs) != 0 || !(S_ISREG(cs.st_mode) || S_ISDIR(cs.st_mode))) continue; /* no links out of the folder */
            add_entry(&t, url, e->d_name, &cs, 0);
        }
        if (d) closedir(d);
    }
    adds(&t, "</D:multistatus>");
    int rc = respond(c, r, 207, "Multi-Status", "Content-Type: application/xml; charset=utf-8\r\n", t.s, t.len);
    free(t.s);
    return rc;
}

static int do_proppatch(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    Text t = {0};
    adds(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    add_url(&t, r->path);
    adds(&t, "</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>");
    int rc = respond(c, r, 207, "Multi-Status", "Content-Type: application/xml; charset=utf-8\r\n", t.s, t.len);
    free(t.s);
    return rc;
}

static int do_get(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 404, "Not Found");
    int fd = open(local, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        return respond_empty(c, r, 404, "Not Found");
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return respond_empty(c, r, 405, "Method Not Allowed");
    }
    long long from = 0, to = st.st_size - 1;
    int partial = 0;
    if (r->range_from >= 0 || r->range_to >= 0) {
        if (r->range_from < 0) { /* the last N bytes */
            from = st.st_size > r->range_to ? st.st_size - r->range_to : 0;
        } else {
            from = r->range_from;
            if (r->range_to >= 0 && r->range_to < to) to = r->range_to;
        }
        if (from >= st.st_size && st.st_size > 0) {
            close(fd);
            char h[80];
            snprintf(h, sizeof h, "Content-Range: bytes */%lld\r\n", (long long)st.st_size);
            return respond(c, r, 416, "Range Not Satisfiable", h, NULL, 0);
        }
        partial = 1;
    }
    long long len = st.st_size ? to - from + 1 : 0;
    char modified[64];
    struct tm tm;
    gmtime_r(&st.st_mtimespec.tv_sec, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nServer: Henge\r\nContent-Length: %lld\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                     "Last-Modified: %s\r\nETag: \"%llx-%llx\"\r\n",
                     partial ? "206 Partial Content" : "200 OK", len, modified, (unsigned long long)st.st_size,
                     (unsigned long long)st.st_mtimespec.tv_sec);
    if (partial) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", from, to, (long long)st.st_size);
    n += snprintf(head + n, sizeof head - (size_t)n, "%s\r\n", r->close ? "Connection: close\r\n" : "");
    int rc = send_all(c->fd, head, (size_t)n);
    if (rc == 0 && strcmp(r->method, "HEAD") != 0) {
        static const size_t piece = 256 * 1024;
        char *buf = malloc(piece);
        long long at = from, left = len;
        while (rc == 0 && left > 0) {
            ssize_t got = pread(fd, buf, (size_t)left < piece ? (size_t)left : piece, at);
            if (got <= 0) rc = -1;
            else {
                rc = send_all(c->fd, buf, (size_t)got);
                at += got, left -= got;
            }
        }
        free(buf);
    }
    close(fd);
    return rc;
}

/* Into a temporary file beside it, then renamed, so a transfer cut short leaves the old file alone. */
static int do_put(Conn *c, Request *r) {
    char local[PATH_MAX], temp[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0) {
        body_discard(c, r);
        return respond_empty(c, r, 403, "Forbidden");
    }
    int existed = stat(local, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 405, "Method Not Allowed");
    }
    char *slash = strrchr(local, '/');
    size_t dir_len = (size_t)(slash - local);
    if (snprintf(temp, sizeof temp, "%.*s/" TEMP_PREFIX "XXXXXX", (int)dir_len, local) >= (int)sizeof temp) {
        body_discard(c, r);
        return respond_empty(c, r, 414, "URI Too Long");
    }
    int fd = mkstemp(temp);
    if (fd < 0) {
        int gone = errno == ENOENT;
        if (body_discard(c, r) != 0) return -1;
        return gone ? respond_empty(c, r, 409, "Conflict") : respond_empty(c, r, 507, "Insufficient Storage");
    }
    fchmod(fd, 0644);
    body_ready(c, r);
    static const size_t piece = 256 * 1024;
    char *buf = malloc(piece);
    int full = 0;
    for (;;) {
        ssize_t got = body_read(c, r, buf, piece);
        if (got < 0) { /* the client went away: no response to give */
            close(fd);
            unlink(temp);
            free(buf);
            return -1;
        }
        if (got == 0) break;
        if (!full) {
            for (ssize_t done = 0; done < got;) {
                ssize_t w = write(fd, buf + done, (size_t)(got - done));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    full = 1;
                    break;
                }
                done += w;
            }
        }
    }
    free(buf);
    close(fd);
    if (full || rename(temp, local) != 0) {
        unlink(temp);
        return respond_empty(c, r, 507, "Insufficient Storage");
    }
    return existed ? respond_empty(c, r, 204, "No Content") : respond_empty(c, r, 201, "Created");
}

static int remove_path(const char *local) {
    struct stat st;
    if (lstat(local, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return removefile(local, NULL, REMOVEFILE_RECURSIVE);
    return unlink(local);
}

static int do_delete(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    if (strcmp(r->path, "/") == 0 || local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 403, "Forbidden");
    if (remove_path(local) != 0) return errno == ENOENT ? respond_empty(c, r, 404, "Not Found") : respond_empty(c, r, 403, "Forbidden");
    return respond_empty(c, r, 204, "No Content");
}

static int do_mkcol(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    char local[PATH_MAX];
    if (local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 403, "Forbidden");
    if (mkdir(local, 0755) == 0) return respond_empty(c, r, 201, "Created");
    if (errno == EEXIST) return respond_empty(c, r, 405, "Method Not Allowed");
    if (errno == ENOENT) return respond_empty(c, r, 409, "Conflict");
    return respond_empty(c, r, 403, "Forbidden");
}

static int do_move_copy(Conn *c, Request *r, int move) {
    if (body_discard(c, r) != 0) return -1;
    char from[PATH_MAX], to[PATH_MAX];
    struct stat st;
    if (!r->destination[0] || strcmp(r->path, "/") == 0 || local_path(r->path, from, sizeof from) != 0 ||
        local_path(r->destination, to, sizeof to) != 0)
        return respond_empty(c, r, 400, "Bad Request");
    if (stat(from, &st) != 0) return respond_empty(c, r, 404, "Not Found");
    int existed = stat(to, &st) == 0;
    if (existed && !r->overwrite) return respond_empty(c, r, 412, "Precondition Failed");
    if (existed && strcmp(from, to) != 0 && remove_path(to) != 0) return respond_empty(c, r, 403, "Forbidden");
    int rc = move ? rename(from, to) : copyfile(from, to, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE);
    if (rc != 0) return errno == ENOENT ? respond_empty(c, r, 409, "Conflict") : respond_empty(c, r, 403, "Forbidden");
    return existed ? respond_empty(c, r, 204, "No Content") : respond_empty(c, r, 201, "Created");
}

static int do_lock(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    static unsigned long counter;
    char token[128];
    const char *open_ = strchr(r->if_header, '<'), *end = open_ ? strchr(open_, '>') : NULL;
    if (open_ && end && (size_t)(end - open_) < sizeof token) { /* a refresh keeps its token */
        snprintf(token, sizeof token, "%.*s", (int)(end - open_ - 1), open_ + 1);
    } else {
        snprintf(token, sizeof token, "opaquelocktoken:henge-%lx-%lx", (unsigned long)time(NULL), __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
    }
    int status = 200;
    char local[PATH_MAX];
    struct stat st;
    if (local_path(r->path, local, sizeof local) != 0) return respond_empty(c, r, 403, "Forbidden");
    if (stat(local, &st) != 0) { /* locking a name that isn't there makes an empty file */
        int fd = open(local, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) return respond_empty(c, r, 409, "Conflict");
        close(fd);
        status = 201;
    }
    Text t = {0};
    addf(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
             "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>0</D:depth>"
             "<D:timeout>Second-3600</D:timeout><D:locktoken><D:href>%s</D:href></D:locktoken></D:activelock></D:lockdiscovery></D:prop>",
         token);
    char headers[256];
    snprintf(headers, sizeof headers, "Content-Type: application/xml; charset=utf-8\r\nLock-Token: <%s>\r\n", token);
    int rc = respond(c, r, status, status == 201 ? "Created" : "OK", headers, t.s, t.len);
    free(t.s);
    return rc;
}

/* ---- /.henge/ ---- */

/* What "curl -s http://10.0.2.4/.henge/install | sh" runs in macOS. */
static const char install_script[] =
    "#!/bin/sh\n"
    "D=\"$HOME/Library/Application Support/Henge\"\n"
    "P=\"$HOME/Library/LaunchAgents/com.henge.agent.plist\"\n"
    "mkdir -p \"$D\" \"$HOME/Library/LaunchAgents\"\n"
    "launchctl bootout \"gui/$(id -u)/com.henge.agent\" 2>/dev/null\n"
    "curl -fsS http://" HV_SHARE_ADDR "/.henge/agent | gunzip > \"$D/henge-agent\" || { echo 'Could not reach Henge.'; exit 1; }\n"
    "chmod 755 \"$D/henge-agent\"\n"
    "cat > \"$P\" <<EOF\n"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>\n"
    "<key>Label</key><string>com.henge.agent</string>\n"
    "<key>ProgramArguments</key><array><string>$D/henge-agent</string></array>\n"
    "<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>\n"
    "<key>LimitLoadToSessionType</key><string>Aqua</string>\n"
    "<key>ProcessType</key><string>Background</string>\n"
    "</dict></plist>\n"
    "EOF\n"
    "launchctl bootstrap \"gui/$(id -u)\" \"$P\"\n"
    "echo 'Henge is set up. You can close this window.'\n";

static int serve_agent(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    int fd = agent_path[0] ? open(agent_path, O_RDONLY) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        return respond_empty(c, r, 404, "Not Found");
    }
    char *data = malloc((size_t)st.st_size);
    ssize_t got = data ? pread(fd, data, (size_t)st.st_size, 0) : -1;
    close(fd);
    int rc = got == st.st_size ? respond(c, r, 200, "OK", "Content-Type: application/octet-stream\r\n", data, (size_t)got)
                               : respond_empty(c, r, 500, "Internal Server Error");
    free(data);
    return rc;
}

/* GET ?since=N waits up to 25 s for a newer clipboard than N. */
static int clipboard_get(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    unsigned long since = 0;
    const char *q = strstr(r->query, "since=");
    if (q) since = strtoul(q + 6, NULL, 10);
    struct timespec until;
    struct timeval now;
    gettimeofday(&now, NULL);
    until.tv_sec = now.tv_sec + 25;
    until.tv_nsec = now.tv_usec * 1000;
    pthread_mutex_lock(&clip_mu);
    agent_seen = time(NULL);
    while (device_seq <= since && pthread_cond_timedwait(&clip_cond, &clip_mu, &until) != ETIMEDOUT) {}
    agent_seen = time(NULL);
    unsigned long seq = device_seq;
    char *text = seq > since && device_clip ? strdup(device_clip) : NULL;
    pthread_mutex_unlock(&clip_mu);
    char headers[64];
    snprintf(headers, sizeof headers, "X-Henge-Seq: %lu\r\n", seq);
    int rc = text ? respond(c, r, 200, "OK", headers, text, strlen(text)) : respond(c, r, 204, "No Content", headers, NULL, 0);
    free(text);
    return rc;
}

static int clipboard_put(Conn *c, Request *r) {
    size_t len = 0;
    char *text = body_small(c, r, &len);
    if (!text) return -1;
    pthread_mutex_lock(&clip_mu);
    free(guest_clip);
    guest_clip = text;
    agent_seen = time(NULL);
    pthread_mutex_unlock(&clip_mu);
    return respond_empty(c, r, 204, "No Content");
}

static void auto_forward_localhost_ports(const char *url) {
    static const char *keys[] = {"localhost:", "localhost%3A", "localhost%3a", "127.0.0.1:", "127.0.0.1%3A", "127.0.0.1%3a"};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        const char *p = url;
        size_t klen = strlen(keys[k]);
        while ((p = strcasestr(p, keys[k])) != NULL) {
            p += klen;
            int port = atoi(p);
            if (port >= 1024 && port <= 65535) hv_net_add_hostfwd(port, port);
        }
    }
}

static int open_url_put(Conn *c, Request *r) {
    size_t len = 0;
    char *url = body_small(c, r, &len);
    if (!url) return -1;
    while (len > 0 && (url[len - 1] == '\n' || url[len - 1] == '\r' || url[len - 1] == ' ')) url[--len] = 0;
    if (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0) {
        auto_forward_localhost_ports(url);
        pthread_mutex_lock(&clip_mu);
        if (!is_url_handler_on()) {
            if (strcasestr(url, "oauth") || strcasestr(url, "redirect_uri=")) {
                free(last_oauth_url);
                last_oauth_url = url;
                last_oauth_seen = time(NULL);
            } else {
                free(url);
            }
            pthread_mutex_unlock(&clip_mu);
            return respond_empty(c, r, 409, "Conflict");
        }
        free(last_oauth_url);
        last_oauth_url = NULL;
        free(guest_open_url);
        guest_open_url = url;
        agent_seen = time(NULL);
        pthread_mutex_unlock(&clip_mu);
        return respond_empty(c, r, 204, "No Content");
    }
    free(url);
    return respond_empty(c, r, 400, "Bad Request");
}

/* The value of key in a query string ("a=1&b=2") as a plain decimal, 1 if it is there and well formed. */
static int query_number(const char *query, const char *key, unsigned long *out) {
    size_t n = strlen(key);
    for (const char *p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, n) != 0 || p[n] != '=') continue;
        const char *v = p + n + 1;
        size_t digits = 0;
        unsigned long value = 0;
        for (; v[digits] >= '0' && v[digits] <= '9'; digits++) {
            if (digits >= 18) return 0;
            value = value * 10 + (unsigned long)(v[digits] - '0');
        }
        if (!digits || (v[digits] && v[digits] != '&')) return 0;
        *out = value;
        return 1;
    }
    return 0;
}

static int focus_wait(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    __atomic_store_n(&focus_agent_at, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
    unsigned long after = 0;
    int have_after = query_number(r->query, "after", &after);
    unsigned long seq;
    int kind = FOCUS_NONE, x = -1, y = -1;
    pthread_mutex_lock(&focus_mu);
    if (have_after) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += hv_share_focus_wait_ms / 1000;
        deadline.tv_nsec += (long)(hv_share_focus_wait_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) deadline.tv_sec++, deadline.tv_nsec -= 1000000000L;
        while (focus_seq == after) {
            if (pthread_cond_timedwait(&focus_cond, &focus_mu, &deadline) != 0) break;
        }
        uint64_t age_ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - focus_at) / 1000000ull;
        if (focus_seq != after && age_ms <= hv_share_focus_stale_ms) {
            kind = focus_kind;
            x = focus_x;
            y = focus_y;
        }
    }
    seq = focus_seq;
    pthread_mutex_unlock(&focus_mu);
    __atomic_store_n(&focus_agent_at, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
    char text[64];
    int len = snprintf(text, sizeof text, "%lu %s %d %d", seq, kind == FOCUS_TAP ? "tap" : kind == FOCUS_LOOK ? "look" : "none", x, y);
    return respond(c, r, 200, "OK", "Content-Type: text/plain\r\nCache-Control: no-store\r\n", text, (size_t)len);
}

static int focus_report(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    unsigned long seq, editable, inside;
    if (!query_number(r->query, "seq", &seq) || !query_number(r->query, "editable", &editable) ||
        !query_number(r->query, "inside", &inside) || editable > 1 || inside > 1)
        return respond_empty(c, r, 400, "Bad Request");
    void (*fn)(unsigned long, int, int, void *) = NULL;
    void *ctx = NULL;
    pthread_mutex_lock(&focus_mu);
    uint64_t age_ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - focus_at) / 1000000ull;
    if (seq == focus_seq && age_ms <= hv_share_focus_report_ms) {
        fn = focus_handler;
        ctx = focus_handler_ctx;
    }
    pthread_mutex_unlock(&focus_mu);
    if (fn) fn(seq, (int)editable, (int)inside, ctx);
    return respond_empty(c, r, 204, "No Content");
}

static int serve_henge(Conn *c, Request *r) {
    const char *what = r->path + strlen("/.henge/");
    if (!strcmp(what, "install") && !strcmp(r->method, "GET")) {
        if (body_discard(c, r) != 0) return -1;
        return respond(c, r, 200, "OK", "Content-Type: text/plain\r\n", install_script, sizeof install_script - 1);
    }
    if (!strcmp(what, "agent") && !strcmp(r->method, "GET")) return serve_agent(c, r);
    if (!strcmp(what, "posture") && !strcmp(r->method, "GET")) {
        if (body_discard(c, r) != 0) return -1;
        unsigned long running;
        if (query_number(r->query, "scale", &running) && running >= 100 && running <= 500) agent_scale_set((int)running);
        const char *word = __atomic_load_n(&posture_laptop, __ATOMIC_RELAXED) ? "laptop" : "tablet";
        int scale = __atomic_load_n(&display_scale, __ATOMIC_RELAXED);
        char text[32];
        int n = scale ? snprintf(text, sizeof text, "%s scale=%d", word, scale) : snprintf(text, sizeof text, "%s", word);
        return respond(c, r, 200, "OK", "Content-Type: text/plain\r\nCache-Control: no-store\r\n", text, (size_t)n);
    }
    if (!strcmp(what, "power") && !strcmp(r->method, "GET")) {
        if (body_discard(c, r) != 0) return -1;
        uint64_t at = __atomic_exchange_n(&hibernate_requested_at, 0, __ATOMIC_RELAXED);
        const char *text = at && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - at < HIBERNATE_TTL_NS ? "hibernate" : "none";
        return respond(c, r, 200, "OK", "Content-Type: text/plain\r\nCache-Control: no-store\r\n", text, strlen(text));
    }
    if (!strcmp(what, "focus-wait") && !strcmp(r->method, "GET")) return focus_wait(c, r);
    if (!strcmp(what, "focus") && !strcmp(r->method, "GET")) return focus_report(c, r);
    if (!strcmp(what, "clipboard") && !strcmp(r->method, "GET")) return clipboard_get(c, r);
    if (!strcmp(what, "clipboard") && !strcmp(r->method, "PUT")) return clipboard_put(c, r);
    if (!strcmp(what, "open-url") && (!strcmp(r->method, "PUT") || !strcmp(r->method, "POST"))) return open_url_put(c, r);
    if (!strcmp(what, "oauth-done") && (!strcmp(r->method, "PUT") || !strcmp(r->method, "POST"))) {
        if (body_discard(c, r) != 0) return -1;
        pthread_mutex_lock(&clip_mu);
        free(last_oauth_url);
        last_oauth_url = NULL;
        guest_oauth_done = 1;
        agent_seen = time(NULL);
        pthread_mutex_unlock(&clip_mu);
        return respond_empty(c, r, 204, "No Content");
    }
    if (!strcmp(what, "expose-port") && (!strcmp(r->method, "PUT") || !strcmp(r->method, "POST"))) {
        size_t len = 0;
        char *pstr = body_small(c, r, &len);
        if (!pstr) return -1;
        int port = atoi(pstr);
        free(pstr);
        if (port >= 1024 && port <= 65535) {
            hv_net_add_hostfwd(port, port);
            return respond_empty(c, r, 204, "No Content");
        }
        return respond_empty(c, r, 400, "Bad Request");
    }
    if (body_discard(c, r) != 0) return -1;
    return respond_empty(c, r, 404, "Not Found");
}

/* ---- requests ---- */

static int header_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return 0;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return 1;
}

/* 1 with a request, 0 when the client is done, -1 on a malformed one. */
static int read_request(Conn *c, Request *r) {
    static __thread char head[HEAD_MAX];
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t got = conn_read(c, &ch, 1);
        if (got <= 0) return n ? -1 : 0;
        if (n + 1 >= sizeof head) return -1;
        head[n++] = ch;
        if (n >= 4 && !memcmp(head + n - 4, "\r\n\r\n", 4)) break;
        if (n == 2 && !memcmp(head, "\r\n", 2)) n = 0; /* blank lines between requests */
    }
    head[n] = 0;
    memset(r, 0, sizeof *r);
    r->range_from = r->range_to = -1;
    r->overwrite = 1;
    r->depth = 1;
    char *line = head, *eol = strstr(line, "\r\n");
    *eol = 0;
    char version[16];
    if (sscanf(line, "%15s %32767s %15s", r->method, r->target, version) != 3) return -1;
    char *query = strchr(r->target, '?');
    if (query) {
        *query++ = 0;
        snprintf(r->query, sizeof r->query, "%s", query);
    }
    /* A client may send the whole URL. */
    if (!strncasecmp(r->target, "http://", 7)) {
        char *p = strchr(r->target + 7, '/');
        memmove(r->target, p ? p : "/", strlen(p ? p : "/") + 1);
    }
    if (decode_path(r->target, r->path, sizeof r->path) != 0) return -1;
    if (!strcmp(version, "HTTP/1.0")) r->close = 1;
    for (line = eol + 2; *line; line = eol + 2) {
        eol = strstr(line, "\r\n");
        if (!eol) break;
        *eol = 0;
        const char *v;
        if (header_is(line, "Content-Length", &v)) r->length = strtoll(v, NULL, 10);
        else if (header_is(line, "Transfer-Encoding", &v)) r->chunked = strcasestr(v, "chunked") != NULL;
        else if (header_is(line, "Expect", &v)) r->expect_continue = strcasestr(v, "100-continue") != NULL;
        else if (header_is(line, "Connection", &v)) r->close = strcasestr(v, "close") != NULL || r->close;
        else if (header_is(line, "Depth", &v)) r->depth = v[0] == '0' ? 0 : 1;
        else if (header_is(line, "Overwrite", &v)) r->overwrite = v[0] != 'F' && v[0] != 'f';
        else if (header_is(line, "If", &v)) snprintf(r->if_header, sizeof r->if_header, "%s", v);
        else if (header_is(line, "Destination", &v)) {
            const char *p = v;
            if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
                p = strchr(strstr(p, "//") + 2, '/');
                if (!p) p = "/";
            }
            if (decode_path(p, r->destination, sizeof r->destination) != 0) r->destination[0] = 0;
        } else if (header_is(line, "Range", &v) && !strncasecmp(v, "bytes=", 6)) {
            long long a = -1, b = -1;
            const char *s = v + 6;
            if (*s == '-') b = strtoll(s + 1, NULL, 10);
            else {
                char *end;
                a = strtoll(s, &end, 10);
                if (*end == '-' && end[1]) b = strtoll(end + 1, NULL, 10);
            }
            r->range_from = a, r->range_to = b;
        }
    }
    return 1;
}

static int handle(Conn *c, Request *r) {
    if (!strncmp(r->path, "/.henge/", 8)) return serve_henge(c, r);
    const char *m = r->method;
    if (!strcmp(m, "OPTIONS")) {
        if (body_discard(c, r) != 0) return -1;
        return respond(c, r, 200, "OK",
                       "DAV: 1, 2\r\nMS-Author-Via: DAV\r\nAllow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK\r\n", NULL, 0);
    }
    if (!strcmp(m, "PROPFIND")) return do_propfind(c, r);
    if (!strcmp(m, "PROPPATCH")) return do_proppatch(c, r);
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) return do_get(c, r);
    if (!strcmp(m, "PUT")) return do_put(c, r);
    if (!strcmp(m, "DELETE")) return do_delete(c, r);
    if (!strcmp(m, "MKCOL")) return do_mkcol(c, r);
    if (!strcmp(m, "MOVE")) return do_move_copy(c, r, 1);
    if (!strcmp(m, "COPY")) return do_move_copy(c, r, 0);
    if (!strcmp(m, "LOCK")) return do_lock(c, r);
    if (!strcmp(m, "UNLOCK")) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 204, "No Content");
    }
    if (body_discard(c, r) != 0) return -1;
    return respond_empty(c, r, 405, "Method Not Allowed");
}

static void *connection(void *arg) {
    Conn *c = arg;
    Request *r = malloc(sizeof *r);
    for (;;) {
        int got = read_request(c, r);
        if (got <= 0) {
            if (got < 0) {
                memset(r, 0, sizeof *r);
                strcpy(r->method, "GET");
                r->close = 1;
                respond_empty(c, r, 400, "Bad Request");
            }
            break;
        }
        if (getenv("HV_SHARE_LOG")) fprintf(stderr, "share: %s %s\n", r->method, r->path);
        if (handle(c, r) != 0 || r->close) break;
    }
    close(c->fd);
    free(r);
    free(c);
    return NULL;
}

static void *listener(void *arg) {
    int server = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            usleep(100000);
            continue;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        Conn *c = calloc(1, sizeof *c);
        c->fd = fd;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, connection, c) != 0) {
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&a);
    }
    return NULL;
}

int hv_share_start(const char *folder, const char *socket_path, const char *agent) {
    static int started;
    if (started) return 0;
    if (!realpath(folder, root)) return -1;
    if (agent) snprintf(agent_path, sizeof agent_path, "%s", agent);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "hengevm: share: socket path too long: %s\n", socket_path);
        return -1;
    }
    strcpy(addr.sun_path, socket_path);
    unlink(socket_path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(server, 16) != 0) {
        fprintf(stderr, "hengevm: share: %s: %s\n", socket_path, strerror(errno));
        if (server >= 0) close(server);
        return -1;
    }
    chmod(socket_path, 0600);
    pthread_t t;
    pthread_create(&t, NULL, listener, (void *)(intptr_t)server);
    pthread_detach(t);
    started = 1;
    return 0;
}

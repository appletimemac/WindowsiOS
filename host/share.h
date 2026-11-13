#ifndef HENGEVM_SHARE_H
#define HENGEVM_SHARE_H

#include <stddef.h>

/* What the Mac and the device share: a folder and the clipboard (see share.c). The guest reaches this
   at HV_SHARE_ADDR, port 80, which the network hands to the Unix socket. Returns 0, or -1 if the socket
   can't be made. agent is the gzipped helper that runs inside macOS, served to the guest; it may be NULL. */
#define HV_SHARE_ADDR "10.0.2.4"
int hv_share_start(const char *folder, const char *socket_path, const char *agent);

/* The device's clipboard, for the guest to pick up. */
void hv_share_set_clipboard(const char *text);
/* What the guest last copied, once: a malloc'd string, or NULL if nothing is new. */
char *hv_share_take_clipboard(void);
/* Seconds since the helper in macOS was last heard from, or -1 if it never has been. */
long hv_share_agent_seen(void);

/* The device's posture for the guest's tablet mode: 1 laptop, 0 tablet (the default). Served at GET /.henge/posture. */
void hv_share_set_posture(int laptop);
/* The Windows display scale the app wants, in percent; 0 (the default) means the app has not said. Anything else is
   clamped to 100..500. Served after the posture at GET /.henge/posture as " scale=NNN"; the agent writes it down as
   Windows' scale, which Windows takes at its next sign-in. Safe from any thread, and before hv_share_start. */
void hv_share_set_display_scale(int percent);
/* The scale Windows is running at, in percent, as the agent last reported it (its posture poll carries "?scale=NNN");
   0 when none has, which is also what an agent older than the report gives. */
int hv_share_agent_display_scale(void);
/* Forget the report (a new VM is starting, and Windows may come up at another scale). */
void hv_share_clear_agent_display_scale(void);
/* fn is called each time that scale changes or is forgotten, on a share thread or on the thread that forgot it, and
   reads the scale with hv_share_agent_display_scale. NULL removes it; a call already on its way may still arrive. */
void hv_share_set_agent_display_scale_handler(void (*fn)(void *ctx), void *ctx);
/* Ask Windows to hibernate: the WindowsiOS Tools agent picks it up within a second and runs shutdown /h. */
void hv_share_request_hibernate(void);
/* Forget an untaken request (a new VM is starting). */
void hv_share_clear_hibernate(void);

/* A tap landed on the Windows picture at guest pixel (x, y). Wakes the agent's GET /.henge/focus-wait. Returns
   the event's number, which its reports carry. */
unsigned long hv_share_note_tap(int x, int y);
/* Nothing was tapped, but focus may have moved (Enter was typed): the agent looks again. */
unsigned long hv_share_note_look(void);
/* Each report from the agent, on a share server thread: seq = the event it answers (a caller that has noted a
   newer one since drops it), editable = the focused element takes text, inside = the tap was inside it (0 for a
   look). fn NULL removes the handler. */
void hv_share_set_text_focus_handler(void (*fn)(unsigned long seq, int editable, int inside, void *ctx), void *ctx);
/* Seconds since the agent last waited for a tap (so it is one that reports text focus), or -1 if never. */
long hv_share_focus_agent_seen(void);

/* Enable/disable host SafariSheet handling of guest `/.henge/open-url` requests (0 -> guest falls back to Edge). */
void hv_share_set_url_handler_enabled(int enabled);
int hv_share_url_handler_enabled(void);
/* URL the guest asked the host browser to open (e.g. Google OAuth sign-in), once: malloc'd or NULL. */
char *hv_share_take_open_url(void);
/* Returns 1 (once) when the guest OAuth callback relay completed an auth redirect. */
int hv_share_take_oauth_done(void);

#endif

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
/* Ask Windows to hibernate: the WindowsiOS Tools agent picks it up within a second and runs shutdown /h. */
void hv_share_request_hibernate(void);
/* Forget an untaken request (a new VM is starting). */
void hv_share_clear_hibernate(void);

/* Enable/disable host SafariSheet handling of guest `/.henge/open-url` requests (0 -> guest falls back to Edge). */
void hv_share_set_url_handler_enabled(int enabled);
int hv_share_url_handler_enabled(void);
/* URL the guest asked the host browser to open (e.g. Google OAuth sign-in), once: malloc'd or NULL. */
char *hv_share_take_open_url(void);
/* Returns 1 (once) when the guest OAuth callback relay completed an auth redirect. */
int hv_share_take_oauth_done(void);

#endif

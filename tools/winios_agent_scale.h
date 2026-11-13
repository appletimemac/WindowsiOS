/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The agent's side of the display scale's wire format and arithmetic, kept apart from the agent so that
   tests/test_share.c can run it against the host's replies: the agent itself is freestanding Win32 code that no
   test here runs. No library calls. */
#ifndef WINIOS_AGENT_SCALE_H
#define WINIOS_AGENT_SCALE_H

/* What a scale from the app has to be within to become Windows' scale. The app's settings ask for 100 to 225; much
   more on a phone's display would leave the sign-in screen no room. */
#define AGENT_SCALE_MIN_PERCENT 100
#define AGENT_SCALE_MAX_PERCENT 250

/* The percent after " scale=" at the start of s (what follows the posture word in the host's reply), or 0 when there
   is none or it is not a scale Windows may be given: this number goes into the registry, so nothing else gets through. */
static inline int agent_scale_parse(const char *s, int len) {
    static const char key[] = " scale=";
    const int k = (int)sizeof(key) - 1;
    if (len < k) return 0;
    for (int i = 0; i < k; i++)
        if (s[i] != key[i]) return 0;
    s += k, len -= k;
    int v = 0, i = 0;
    for (; i < len && s[i] >= '0' && s[i] <= '9' && v <= AGENT_SCALE_MAX_PERCENT; i++) v = v * 10 + (s[i] - '0');
    if (i < len && s[i] != ' ') return 0;
    return v >= AGENT_SCALE_MIN_PERCENT && v <= AGENT_SCALE_MAX_PERCENT ? v : 0;
}

/* LogPixels for a scale: Windows counts dots per inch, 96 at 100 percent. */
static inline unsigned agent_scale_dpi(int percent) { return ((unsigned)percent * 96u + 50u) / 100u; }

/* And back: the scale, in percent, of a session at dpi dots per inch. */
static inline unsigned agent_scale_percent(unsigned dpi) { return (dpi * 100u + 48u) / 96u; }

#endif

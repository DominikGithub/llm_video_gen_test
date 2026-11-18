/* WATCHER -- a 60 second terminal demo. No files, no assets: everything is
 * synthesized at runtime. Pixels are drawn as truecolor half-blocks, sound is
 * a tiny realtime synth sent to PulseAudio (or ALSA) loaded with dlopen. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SR 44100
#define TAU 6.2831853f
#define PI 3.1415927f
#define END 60.9f

typedef struct { float x, y, z; } v3;

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float sm(float a, float b, float x) { x = clampf((x - a) / (b - a), 0, 1); return x * x * (3 - 2 * x); }
static float fr(float x) { return x - floorf(x); }
static float mixf(float a, float b, float k) { return a + (b - a) * k; }
static float hsh(unsigned n) { n = (n << 13) ^ n; n = n * (n * n * 15731u + 789221u) + 1376312589u; return (n & 0x7fffffff) / 2147483648.f; }
static float mtof(int m) { return 440.f * powf(2.f, (m - 69) / 12.f); }
static v3 nrm(v3 a) { float l = 1.f / sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); return (v3){a.x * l, a.y * l, a.z * l}; }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b) { return (v3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static v3 pal(float x, float d0, float d1, float d2) {
    return (v3){.5f + .5f * cosf(TAU * (x + d0)), .5f + .5f * cosf(TAU * (x + d1)), .5f + .5f * cosf(TAU * (x + d2))};
}
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

/* ------------------------------------------------------------------ score */
/* 120 bpm, 16th-note steps (8 per second), 16 steps per bar, 30 bars. */
static const unsigned char CH[5][3] = {{57, 60, 64}, {53, 57, 60}, {50, 53, 57}, {52, 56, 59}, {55, 59, 62}}; /* Am F Dm E G */
#define _ 99
static const signed char MEL[6][16] = {
    {7, _, _, 3, _, _, 0, _, 2, _, 3, _, _, _, _, _},
    {0, _, _, 3, _, _, 8, _, 7, _, _, _, 3, _, _, _},
    {5, _, _, 8, _, _, 12, _, 10, _, 8, _, 7, _, _, _},
    {11, _, _, _, 14, _, _, _, 7, _, _, _, 8, _, 11, _},
    {12, _, _, 10, _, _, 8, _, 7, _, 8, _, 10, _, _, _},
    {14, _, _, 12, _, _, 10, _, 14, _, 17, _, 19, _, _, _}};

static int on(int b, int lo, int hi) { return b >= lo && b <= hi; }
static int groove(int b) { return on(b, 9, 25) || on(b, 27, 28); }
static int chord(int b) {
    if (b < 4) return 0;
    if (b < 22) return (b - 4) & 3;
    if (b < 24) return 2;
    if (b < 27) return 3;
    return b == 27 ? 1 : b == 28 ? 4 : 0;
}
static float p_heart(int b, int p) {
    if (b < 9) { if ((p & 7) == 0) return 1; if ((p & 7) == 2) return .6f; }
    return b == 29 && p == 12;
}
static float p_kick(int b, int p) {
    if (p & 3) return 0;
    if (groove(b)) return 1;
    return b == 8 && p >= 8 ? .7f : 0;
}
static float p_snare(int b, int p) {
    if (on(b, 15, 21) || on(b, 27, 28)) return p == 4 || p == 12;
    if (b == 22) return (p & 3) ? 0 : .5f;
    if (b == 23) return (p & 1) ? 0 : .6f;
    return on(b, 24, 25) ? .75f : 0;
}
static float p_hat(int b, int p) {
    if (on(b, 9, 14)) return (p & 3) == 2 ? .7f : 0;
    if (on(b, 15, 25) || on(b, 27, 28)) return (p & 3) == 2 ? .8f : .3f;
    return 0;
}
static float p_arp(int b, int p) {
    if (on(b, 4, 8)) return !(p & 1);
    return on(b, 9, 25) || on(b, 27, 28);
}
typedef float (*patfn)(int, int);
/* seconds since the last hit of a pattern; bar 26 is the tape-stop echo of 25 */
static float since(patfn f, double tt, float *v) {
    int st = (int)floor(tt * 8);
    for (int k = 0; k < 24; k++) {
        int s = st - k, b = s >> 4;
        if (s < 0) break;
        if (b == 26) b = 25;
        float x = f(b, s & 15);
        if (x > 0) { *v = x; return (float)(tt - s / 8.0); }
    }
    *v = 0;
    return 9;
}
static float snare_since(double tt, float *v) {
    int b = (int)(tt / 2);
    if (b == 25 || b == 26) { *v = .85f; return fmodf((float)tt, .0625f); }
    return since(p_snare, tt, v);
}
static int melbar(int b) {
    if (on(b, 15, 21)) return chord(b);
    return b == 27 ? 4 : b == 28 ? 5 : -1;
}
static int lead(double tt, float *tau) {
    int st = (int)(tt * 8);
    for (int k = 0; k < 16 && st - k >= 0; k++) {
        int s = st - k, m = melbar(s >> 4);
        if (m < 0) return -1;
        if (MEL[m][s & 15] != _) { *tau = (float)(tt - s / 8.0); return 69 + MEL[m][s & 15]; }
    }
    return -1;
}
/* the fake crash: song time slows to a halt at 52s (tape stop), silence till 54s */
static double warp(double r, float *sp) {
    if (r < 52) { *sp = 1; return r; }
    if (r < 52.75) { double u = r - 52; *sp = (float)(1 - u / .75); return 52 + u - u * u / 1.5; }
    if (r < 54) { *sp = 0; return 52.375; }
    *sp = 1;
    return r;
}

/* ------------------------------------------------------------------ synth */
#define DLN 16538
static float dlb[2][DLN];
static float osc(float *p, float f) { *p += f; *p -= (int)*p; return *p; }
static float lpc(float fc) { return 1.f - expf(-TAU * fc / SR); }

static void synth(short *o, int n) {
    static double r;
    static float ph[16], lp[12], hpv, svl, svb;
    static int dp;
    static unsigned ns = 22222;
    for (int i = 0; i < n; i++, r += 1.0 / SR) {
        float sp, v, tau, s, L = 0, R = 0, send = 0, duck = 1;
        double tt = warp(r, &sp);
        if (r >= 52.75 && r < 54) { o[2 * i] = o[2 * i + 1] = 0; continue; }
        ns = ns * 1664525u + 1013904223u;
        float nz = (int)ns * (1.f / 2147483648.f), dt = sp / SR, ft = (float)tt;
        int st = (int)floor(tt * 8), b = st >> 4, p = st & 15;
        if (b == 26) b = 25;
        int c = chord(b);

        /* heartbeat */
        tau = since(p_heart, tt, &v);
        if (v > 0 && tau < 1) {
            float hv = b < 4 ? .35f + b * .15f : .9f;
            s = sinf(TAU * (52 * tau + 90 * (1 - expf(-tau * 25)) / 25)) * expf(-tau * 9);
            s = tanhf(s * 3) * v * hv * .5f;
            L += s; R += s;
        }
        /* drone: slowly opening filter = rising tension */
        if (tt < 52.4) {
            float x = 2 * osc(&ph[0], 110 * dt) + 2 * osc(&ph[1], 110.7f * dt) + 1.2f * osc(&ph[2], 164.9f * dt) - 2.6f;
            lp[0] += lpc(220 + 1400 * sm(0, 52, ft) + 100 * sinf(ft * .7f)) * (x - lp[0]);
            s = lp[0] * (.05f + .07f * sm(0, 50, ft)) * sm(0, 3, ft);
            L += s; R += s;
        }
        /* pad */
        float pv = 0;
        if (on(b, 4, 8)) pv = .06f * sm(8, 12, ft);
        else if (on(b, 9, 25)) pv = .055f;
        else if (on(b, 27, 28)) pv = .075f;
        else if (b == 29) pv = .14f * expf(-(ft - 58) * 1.1f);
        if (pv > 0) {
            float xl = 0, xr = 0;
            for (int k = 0; k < 3; k++) {
                float f = mtof(CH[c][k]);
                float a = 2 * osc(&ph[3 + k], f * 1.004f * dt) - 1, bb = 2 * osc(&ph[6 + k], f * .996f * dt) - 1;
                xl += a + .4f * bb; xr += bb + .4f * a;
            }
            float k = lpc(b >= 27 ? 5000 : 350 + 2600 * sm(8, 52, ft));
            lp[1] += k * (xl - lp[1]); lp[2] += k * (xr - lp[2]);
            L += lp[1] * pv; R += lp[2] * pv; send += (lp[1] + lp[2]) * pv * .25f;
        }
        /* arpeggio */
        tau = since(p_arp, tt, &v);
        if (v > 0) {
            static const char AP[8] = {0, 1, 2, 3, 4, 5, 4, 2};
            int hs = (int)lround((tt - tau) * 8), k = AP[hs & 7], hb = hs >> 4;
            if (hb == 26) hb = 25;
            int nn = CH[chord(hb)][k % 3] + 12 * (k / 3) + 12 + (on(hb, 22, 25) ? 12 : 0);
            float x = osc(&ph[9], mtof(nn) * dt) < .5f ? 1.f : -1.f;
            lp[3] += lpc(2500) * (x - lp[3]);
            s = lp[3] * expf(-tau * 13) * .07f;
            L += s; R += s; send += s * .9f;
        }
        /* kick + sidechain */
        tau = since(p_kick, tt, &v);
        if (v > 0 && tau < .6f) {
            s = sinf(TAU * (48 * tau + 140 * (1 - expf(-tau * 28)) / 28)) * expf(-tau * 7);
            s = tanhf(s * 2.5f) * v * .5f;
            L += s; R += s;
            duck = 1 - .75f * expf(-tau * 9);
        }
        /* bass */
        if (groove(b)) {
            float tb = (float)(tt - st / 8.0);
            float x = 2 * osc(&ph[10], mtof(CH[c][0] - 12 + ((p & 1) ? 12 : 0)) * dt) - 1;
            lp[4] += lpc(150 + 1900 * expf(-tb * 14) * (.4f + .6f * sm(18, 52, ft))) * (x - lp[4]);
            s = lp[4] * .26f * duck;
            L += s; R += s;
        }
        /* snare (rolls up to 32nds during the build) */
        tau = snare_since(tt, &v);
        if (v > 0 && tau < .4f) {
            float f = 190 * (1 + clampf((ft - 44) / 8, 0, 1));
            s = (nz * .7f * expf(-tau * 20) + sinf(TAU * f * tau) * .5f * expf(-tau * 30)) * v * .33f;
            L += s * .9f; R += s;
        }
        /* hihat */
        tau = since(p_hat, tt, &v);
        { float h = nz - hpv; hpv = nz;
          if (v > 0) { s = h * expf(-tau * 55) * v * .07f; L += s * .8f; R += s; } }
        /* lead */
        int ln = lead(tt, &tau);
        if (ln > 0) {
            float f = mtof(ln) * (1 + .006f * sinf(TAU * 5.5f * tau) * sm(0, .3f, tau));
            float x = 2 * osc(&ph[11], f * dt) - 1 + .7f * (2 * osc(&ph[12], f * 1.007f * dt) - 1) + (osc(&ph[13], f * .5f * dt) < .5f ? .4f : -.4f);
            lp[5] += lpc(3000) * (x - lp[5]);
            s = lp[5] * fminf(tau * 60, 1) * (.55f + .45f * expf(-tau * 5)) * .065f;
            L += s; R += s; send += s * .8f;
        }
        /* riser: resonant noise sweep + rising tone */
        if (tt >= 44 && tt < 52.4) {
            float x = (ft - 44) / 8, f = 2 * sinf(PI * 250 * powf(24, x) / SR);
            svl += f * svb; svb += f * (nz - svl - .12f * svb);
            s = svb * .12f * x * x + sinf(TAU * osc(&ph[14], 90 * powf(14, x) * dt)) * .05f * x;
            L += s; R += s;
        }
        /* booms at 54 (the eye opens) and 58 (the end) */
        for (int k = 0; k < 2; k++) {
            float bt = ft - (k ? 58 : 54);
            if (bt >= 0 && bt < 4) {
                s = tanhf(3 * sinf(TAU * (34 * bt + 180 * (1 - expf(-bt * 12)) / 12)) * expf(-bt * 1.7f)) * .6f;
                lp[6] += lpc(3000 * expf(-bt * 2)) * (nz - lp[6]);
                s += lp[6] * expf(-bt * 2.5f) * .9f;
                L += s; R += s;
            }
        }
        /* ping-pong delay */
        float a = dlb[0][dp], d2 = dlb[1][dp];
        dlb[0][dp] = send + d2 * .45f; dlb[1][dp] = a * .45f;
        if (++dp == DLN) dp = 0;
        L += a * .5f; R += d2 * .5f;
        /* overall dynamics follow the arc of suspense */
        float mg = tt < 18 ? 1 : tt < 52.5 ? .6f + .4f * (ft - 18) / 34 : 1.05f;
        L *= mg; R *= mg;
        /* tape stop: everything darkens and dies */
        if (sp < 1) {
            float k = lpc(100 + 12000 * sp * sp);
            lp[7] += k * (L - lp[7]); lp[8] += k * (R - lp[8]);
            L = lp[7] * fminf(sp * 5, 1); R = lp[8] * fminf(sp * 5, 1);
        }
        if (r > END - .3) { L *= (END - r) / .3f; R *= (END - r) / .3f; }
        o[2 * i] = (short)(tanhf(L * 1.1f) * 29000);
        o[2 * i + 1] = (short)(tanhf(R * 1.1f) * 29000);
    }
}

/* ------------------------------------------------------------------ audio out */
typedef struct { int format; unsigned rate; unsigned char channels; } pa_ss;
typedef struct { unsigned maxlength, tlength, prebuf, minreq, fragsize; } pa_ba;
static void *(*pa_new)(const char *, const char *, int, const char *, const char *, const pa_ss *, const void *, const pa_ba *, int *);
static int (*pa_write)(void *, const void *, size_t, int *);
static uint64_t (*pa_lat)(void *, int *);
static int (*pa_drain)(void *, int *);
static int (*sn_open)(void **, const char *, int, int);
static int (*sn_params)(void *, int, int, unsigned, unsigned, int, unsigned);
static long (*sn_writei)(void *, const void *, unsigned long);
static int (*sn_recover)(void *, int, int);
static int (*sn_delay)(void *, long *);
static int (*sn_drain)(void *);
static void *pa, *pcm;
static volatile double a_pos = -1, a_stamp;

static int audio_open(void) {
    void *h = dlopen("libpulse-simple.so.0", RTLD_NOW);
    if (h) {
        pa_new = dlsym(h, "pa_simple_new"); pa_write = dlsym(h, "pa_simple_write");
        pa_lat = dlsym(h, "pa_simple_get_latency"); pa_drain = dlsym(h, "pa_simple_drain");
        pa_ss ss = {3, SR, 2};
        pa_ba ba = {~0u, SR * 4 / 12, ~0u, ~0u, ~0u};
        int e;
        if (pa_new && pa_write && pa_lat && (pa = pa_new(0, "watcher", 1, 0, "watcher", &ss, 0, &ba, &e))) return 1;
    }
    h = dlopen("libasound.so.2", RTLD_NOW);
    if (h) {
        sn_open = dlsym(h, "snd_pcm_open"); sn_params = dlsym(h, "snd_pcm_set_params");
        sn_writei = dlsym(h, "snd_pcm_writei"); sn_recover = dlsym(h, "snd_pcm_recover");
        sn_delay = dlsym(h, "snd_pcm_delay"); sn_drain = dlsym(h, "snd_pcm_drain");
        if (sn_open && sn_open(&pcm, "default", 0, 0) == 0 && sn_params(pcm, 2, 3, 2, SR, 1, 80000) == 0) return 1;
        pcm = 0;
    }
    return 0;
}
static void *audio_thread(void *x) {
    static short buf[2 * 512];
    long done = 0;
    (void)x;
    while (done < (long)(END * SR)) {
        synth(buf, 512);
        double lat = 0;
        int e;
        if (pa) {
            if (pa_write(pa, buf, sizeof buf, &e) < 0) break;
            lat = pa_lat(pa, &e) / 1e6;
        } else {
            long w = sn_writei(pcm, buf, 512), d;
            if (w < 0) sn_recover(pcm, (int)w, 1);
            if (sn_delay && sn_delay(pcm, &d) == 0) lat = d / (double)SR;
        }
        done += 512;
        a_stamp = now();
        a_pos = done / (double)SR - lat;
    }
    if (pa && pa_drain) pa_drain(pa, &(int){0});
    return 0;
}

/* ------------------------------------------------------------------ screen */
static int W, H, COLS, ROWS, frame, crash;
static float *fb;
static char *tx;
static unsigned *tc;
static uint64_t *prev;
static char *ob;
static size_t olen;
static struct termios tio0;

static void oc(const char *s) { size_t l = strlen(s); memcpy(ob + olen, s, l); olen += l; }
static void oi(int v) { char t[12]; int n = 0; do t[n++] = '0' + v % 10; while (v /= 10); while (n) ob[olen++] = t[--n]; }
static void flush(void) {
    size_t p = 0;
    while (p < olen) {
        ssize_t w = write(1, ob + p, olen - p);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (w <= 0) break;
        p += w;
    }
    olen = 0;
}
static void raw(const char *s) { olen = 0; oc(s); flush(); }
static void bye(int sig) {
    (void)sig;
    if (crash == 1) raw("\033[A\r\033[J");
    raw("\033[0m\033[?2026l\033[?1049l\033[?25h");
    tcsetattr(0, TCSANOW, &tio0);
    _exit(0);
}
static int resize(void) {
    struct winsize ws;
    int c = 80, r = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) c = ws.ws_col, r = ws.ws_row;
    if (c == COLS && r == ROWS) return 0;
    COLS = c; ROWS = r; W = c; H = r * 2;
    fb = realloc(fb, sizeof(float) * 3 * W * H);
    tx = realloc(tx, COLS * ROWS);
    tc = realloc(tc, sizeof(unsigned) * COLS * ROWS);
    prev = realloc(prev, sizeof(uint64_t) * COLS * ROWS);
    ob = realloc(ob, (size_t)COLS * ROWS * 64 + 4096);
    memset(prev, 0xff, sizeof(uint64_t) * COLS * ROWS);
    return 1;
}
static unsigned qc(const float *p) {
    unsigned r = (unsigned)(clampf(p[0], 0, 1) * 255) & 0xf8, g = (unsigned)(clampf(p[1], 0, 1) * 255) & 0xf8,
             b = (unsigned)(clampf(p[2], 0, 1) * 255) & 0xf8;
    return r << 16 | g << 8 | b;
}
static void color(int bg, unsigned c) {
    oc(bg ? "\033[48;2;" : "\033[38;2;");
    oi(c >> 16); ob[olen++] = ';'; oi(c >> 8 & 255); ob[olen++] = ';'; oi(c & 255); ob[olen++] = 'm';
}
static void present(void) {
    unsigned lf = ~0u, lb = ~0u;
    int cr = -1, cc = -1;
    oc("\033[?2026h");
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int i = r * COLS + c;
            unsigned t = qc(fb + ((2 * r) * W + c) * 3), u = qc(fb + ((2 * r + 1) * W + c) * 3);
            unsigned char ch = tx[i];
            uint64_t key = t | (uint64_t)u << 24 | (uint64_t)ch << 48;
            if (ch) key ^= (uint64_t)tc[i] << 8;
            if (key == prev[i]) continue;
            prev[i] = key;
            if (cr != r || cc != c) { oc("\033["); oi(r + 1); ob[olen++] = ';'; oi(c + 1); ob[olen++] = 'H'; }
            if (ch) {
                unsigned bg = ((t >> 1) & 0x7f7f7f) + ((u >> 1) & 0x7f7f7f);
                if (tc[i] != lf) color(0, lf = tc[i]);
                if (bg != lb) color(1, lb = bg);
                ob[olen++] = ch;
            } else if (t == u) {
                if (u != lb) color(1, lb = u);
                ob[olen++] = ' ';
            } else {
                if (t != lf) color(0, lf = t);
                if (u != lb) color(1, lb = u);
                oc("\xe2\x96\x80");
            }
            cr = r; cc = c + 1;
        }
    oc("\033[?2026l");
    flush();
}

/* ------------------------------------------------------------------ helpers */
#define PIX for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { \
    float u = (x + .5f - W * .5f) / (H * .5f), w = (H * .5f - y - .5f) / (H * .5f); \
    float *P = fb + (y * W + x) * 3; (void)u; (void)w;
#define END_PIX }
static void plot(float sx, float sy, float r, float g, float b) {
    int x = (int)floorf(W * .5f + sx * H * .5f), y = (int)floorf(H * .5f - sy * H * .5f);
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    float *P = fb + (y * W + x) * 3;
    P[0] += r; P[1] += g; P[2] += b;
}
static void text(int r, int c, const char *s, int n, unsigned col) {
    for (int i = 0; i < n && s[i]; i++)
        if (r >= 0 && r < ROWS && c + i >= 0 && c + i < COLS) tx[r * COLS + c + i] = s[i], tc[r * COLS + c + i] = col;
}
static void typed(float t, float t0, const char *s) {
    int n = (int)((t - t0) * 11), l = (int)strlen(s);
    if (t < t0) return;
    if (n > l) n = l;
    text(ROWS - 3, 3, s, n, 0xb4b4b4);
    if (n >= l && fr(t * 2) < .5f) text(ROWS - 3, 3 + n, "_", 1, 0xb4b4b4);
}
static float heart(float t) { float v, tau = since(p_heart, t, &v); return v * expf(-tau * 7); }
static void glowdot(float t, float fade, float grow) {
    float e = heart(t);
    PIX
        float r2 = u * u + w * w, I = fade * .0012f * (1 + 3 * e) * grow / (r2 + .0006f * (1 + e) * grow) + fade * .04f * e * expf(-r2 * 5);
        P[0] += I; P[1] += I * .3f + I * I * .4f; P[2] += I * .25f + I * I * .4f;
    END_PIX
}

/* ------------------------------------------------------------------ 0-8s: the void */
static void s_void(float t) {
    glowdot(t, sm(0, 2.5f, t), 1 + sm(6.5f, 8, t) * 4);
    if (t > 3.5f)
        for (int i = 0; i < 220; i++) {
            float f = fr(t * .12f + hsh(i)), rad = (1 - f) * (1.1f + hsh(i + 99)), a = hsh(i + 33) * TAU + f * 4;
            float br = f * f * sm(3.5f, 6.5f, t) * .9f;
            plot(cosf(a) * rad, sinf(a) * rad, br * .9f, br * .45f, br * .4f);
        }
    if (t < 7) typed(t, 1.5f, "> is anyone there?");
}

/* ------------------------------------------------------------------ 8-18s: point cloud */
#define NP 1100
static void shape(int k, int i, float t, float *p) {
    float f = (i + .5f) / NP, a = i * 2.39996323f, y, r;
    switch (k) {
    case 0: y = 1 - 2 * f; r = sqrtf(1 - y * y); p[0] = r * cosf(a); p[1] = y; p[2] = r * sinf(a); break;
    case 1: { float A = f * TAU, q = .75f + .32f * cosf(a); p[0] = q * cosf(A); p[1] = .32f * sinf(a); p[2] = q * sinf(A); } break;
    case 2: { float s = f * TAU;
        p[0] = (sinf(s) + 2 * sinf(2 * s)) * .33f + .12f * cosf(a);
        p[1] = (cosf(s) - 2 * cosf(2 * s)) * .33f + .12f * sinf(a);
        p[2] = -sinf(3 * s) * .33f + .12f * cosf(a * 1.3f); } break;
    default: { y = 1 - 2 * f; r = sqrtf(1 - y * y);
        float x = r * cosf(a), z = r * sinf(a);
        float m = 1 + .45f * powf(fabsf(sinf(4 * atan2f(z, x)) * sinf(4 * acosf(y))), 3) * (1.2f + sinf(t * 5));
        p[0] = x * m; p[1] = y * m; p[2] = z * m; }
    }
}
static void s_cloud(float t) {
    float lt = t - 8, v, at = since(p_arp, t, &v), ae = v * expf(-at * 10);
    float ex = 1 - expf(-lt * 5) * cosf(lt * 9), ph = lt / 2.5f, m;
    int k = (int)ph, ps = H / 60 + 1;
    m = sm(.65f, 1, ph - k);
    if (k >= 3) k = 3, m = 0;
    float zoom = 1 + powf(sm(16.6f, 18, t), 3) * 9, sc = ex * (1 + .08f * ae) * zoom;
    float ca = cosf(t * .7f), sa = sinf(t * .7f), cb = cosf(t * .43f), sb = sinf(t * .43f);
    for (int i = 0; i < 380; i++) {
        float z = fr(hsh(i * 7 + 1) - lt * (.06f + lt * .012f)), br = (1 - z) * .7f * sm(8, 10, t);
        plot((hsh(i * 7 + 2) * 2 - 1) * .5f / (z + .03f), (hsh(i * 7 + 3) * 2 - 1) * .3f / (z + .03f), br * .7f, br * .8f, br);
    }
    for (int i = 0; i < NP; i++) {
        float p[3], q[3];
        shape(k, i, t, p);
        if (m > 0) { shape(k + 1, i, t, q); for (int j = 0; j < 3; j++) p[j] = mixf(p[j], q[j], m); }
        float x = p[0] * sc, y = p[1] * sc, z = p[2] * sc;
        float x2 = x * ca + z * sa, z2 = -x * sa + z * ca;
        float y2 = y * cb - z2 * sb, z3 = y * sb + z2 * cb + 3;
        if (z3 < .15f) continue;
        float br = clampf((4.3f - z3) / 2.2f, .08f, 1) * (.55f + .6f * ae);
        v3 c = pal(i / (float)NP * .5f + t * .05f, 0, .33f, .67f);
        for (int dy = 0; dy < ps; dy++)
            for (int dx = 0; dx < ps; dx++)
                plot(x2 / z3 * 1.6f + dx * 2.f / H, y2 / z3 * 1.6f - dy * 2.f / H, c.x * br, c.y * br, c.z * br);
    }
}

/* ------------------------------------------------------------------ 18-30s: retro horizon + eclipse */
static void s_horizon(float t) {
    float lt = t - 18, kv, kt = since(p_kick, t, &kv), ke = kv * expf(-kt * 6);
    float roll = .07f * sinf(t * .8f), cr = cosf(roll), sr = sinf(roll), hz = -.1f;
    float zpos = lt * 1.6f + lt * lt * .09f, camx = sinf(t * .35f) * 1.5f;
    float sx = 0, sy = hz + .4f, R = .48f, ec = sm(23, 29, t);
    float mx = sx + (1 - ec) * 1.3f, my = sy + (1 - ec) * .2f;
    PIX
        float uu = u * cr - w * sr, ww = u * sr + w * cr, r, g, b;
        if (ww > hz) {
            float k = sm(0, .9f, ww - hz);
            r = mixf(.95f, .05f, k); g = mixf(.25f, .02f, k); b = mixf(.45f, .16f, k);
            if (hsh(x * 131 + y * 7919) > .985f) { float s = (.4f + .4f * sinf(t * 3 + x)) * k; r += s; g += s; b += s; }
            float dx = uu - sx, dy = ww - sy, d = sqrtf(dx * dx + dy * dy);
            float glow = .08f / (1 + fmaxf(d - R, 0) * 14) * (1 + ke);
            r += glow; g += glow * .3f; b += glow * .5f;
            int insun = d < R;
            if (insun && dy < .05f && fr(dy * 14 + lt * .6f) < sm(.05f, -.45f, dy) * .65f) insun = 0;
            if (insun) { float gy = (dy + R) / (2 * R); r = 1; g = mixf(.15f, .9f, gy); b = mixf(.5f, .25f, gy); }
            float dm = sqrtf((uu - mx) * (uu - mx) + (ww - my) * (ww - my));
            if (dm < R * .98f && ec > .01f) {
                float rim = sm(R * .9f, R * .98f, dm) * ec;
                r = .02f + rim; g = rim * .4f; b = .05f + rim * .6f;
            }
            float cor = ec * ec * .03f / (fabsf(d - R) + .02f) * (1 + 2 * ke);
            r += cor; g += cor * .6f; b += cor * .9f;
            float mh = hz + .11f * (fabsf(sinf(uu * 2.3f + camx * .3f + 1)) * .7f + fabsf(sinf(uu * 5.7f + 2 + camx * .3f)) * .3f) * (.35f + sm(.2f, 1.2f, fabsf(uu)));
            if (ww < mh) {
                float e = sm(mh - .015f, mh, ww);
                r = .07f + e * .9f; g = .01f + e * .25f; b = .12f + e * .6f;
            }
        } else {
            float d = hz - ww, z = .25f / d, gx = uu * z + camx, gz = z + zpos;
            float fw = z * 2 / H, fz = z * z * 8 / H;
            float lx = 1 - sm(0, fw * 1.5f + .02f, fminf(fr(gx), 1 - fr(gx)));
            float lz = 1 - sm(0, fz * 1.5f + .02f, fminf(fr(gz), 1 - fr(gz)));
            float fog = expf(-z * .13f), fade = fminf(1, .4f / (fz + .001f));
            float L = (.8f + 1.5f * ke) * fog;
            r = .06f + (lx + lz * fade) * L * .9f + (1 - fog) * .8f;
            g = .0f + lz * fade * L * .3f + lx * L * .15f + (1 - fog) * .22f;
            b = .1f + (lx * .8f + lz * fade) * L + (1 - fog) * .4f;
        }
        P[0] = r; P[1] = g; P[2] = b;
    END_PIX
}

/* ------------------------------------------------------------------ 30-44s: tunnel */
static void s_tunnel(float t) {
    float lt = t - 30, v, kt = since(p_kick, t, &v), ke = v * expf(-kt * 7), se, st = snare_since(t, &se);
    se *= expf(-st * 9);
    float zt = lt * (2 + lt * .15f), amp = sm(30, 33, t), cx = .35f * sinf(t * .9f) * amp, cy = .25f * cosf(t * .6f) * amp;
    float hue = (int)(t / 2) * .13f, k1 = sm(36.5f, 37.5f, t), k2 = sm(40, 41, t);
    PIX
        float du = u - cx, dv = w - cy, a = atan2f(dv, du);
        float rc = sqrtf(du * du + dv * dv), rs = fmaxf(fabsf(du), fabsf(dv)), rf = rc * (1 + .25f * sinf(a * 6 + t * 2));
        float r = mixf(mixf(rc, rs, k1), rf, k2) + 1e-3f, depth = .5f / r, z = depth + zt;
        float aa = a / TAU + .05f * z * sinf(t * .3f) + t * .05f;
        float tu = fr(aa * 12), tv = fr(z * 1.5f);
        float h = hsh((unsigned)(int)floorf(aa * 12) * 31u + (unsigned)(int)floorf(z * 1.5f) * 977u);
        float e = fminf(fminf(tu, 1 - tu), fminf(tv, 1 - tv));
        v3 c = pal(h * .3f + hue, 0, .1f, .2f);
        float lit = .2f + (h > .85f ? .45f + ke : 0), line = sm(.07f, 0, e) * .65f;
        float ring = expf(-fabsf(fr(z * .25f - lt * .5f) - .5f) * 30) * (.15f + ke);
        float fog = expf(-depth * .33f), cl = .02f / (r * r + .02f) * (.6f + .6f * ke);
        P[0] = (c.x * lit + line * .9f + ring) * fog + cl + se * .25f;
        P[1] = (c.y * lit + line * .3f + ring * .5f) * fog + cl * .9f + se * .25f;
        P[2] = (c.z * lit + line * 1.f + ring * .8f) * fog + cl * .8f + se * .3f;
    END_PIX
}

/* ------------------------------------------------------------------ 44-52s: cube storm (raymarched) */
static float map5(v3 p, float t) {
    float cx = floorf(p.x * .5f), cy = floorf(p.y * .5f), cz = floorf(p.z * .5f);
    float qx = p.x - cx * 2 - 1, qy = p.y - cy * 2 - 1, qz = p.z - cz * 2 - 1;
    float h = hsh((unsigned)(int)cx * 73u + (unsigned)(int)cy * 1009u + (unsigned)(int)cz * 7919u);
    float an = h * 6.28f + t * (1 + h * 2), c = cosf(an), s = sinf(an), tx_;
    tx_ = qx * c - qy * s; qy = qx * s + qy * c; qx = tx_;
    tx_ = qy * c - qz * s; qz = qy * s + qz * c; qy = tx_;
    float dx = fabsf(qx) - .3f, dy = fabsf(qy) - .3f, dz = fabsf(qz) - .3f;
    float ox = fmaxf(dx, 0), oy = fmaxf(dy, 0), oz = fmaxf(dz, 0);
    return sqrtf(ox * ox + oy * oy + oz * oz) + fminf(fmaxf(dx, fmaxf(dy, dz)), 0) - .04f;
}
static const unsigned short FONT[6] = {0x7927, 0x4927, 0x7b6f, 0x79cf, 0x79a7, 0x6bad}; /* CLOSER */
static void s_cubes(float t, float sp) {
    float lt = t - 44, v, kt = since(p_kick, t, &v), ke = v * expf(-kt * 7), se, st = snare_since(t, &se);
    se *= expf(-st * 14) * sm(44, 52, t);
    float zp = lt * 3 + lt * lt * .55f, roll = lt * .35f + .3f * sinf(t * .5f), cr = cosf(roll), sr = sinf(roll);
    float shk = .04f * lt * lt / 64, shx = (hsh(frame * 3) - .5f) * shk, shy = (hsh(frame * 3 + 1) - .5f) * shk;
    int beat = (int)(t * 2);
    v3 ro = {.25f * sinf(t * .9f), .25f * cosf(t * .7f), zp};
    PIX
        if ((x & 1) && W * H > 9000) { P[0] = P[-3]; P[1] = P[-2]; P[2] = P[-1]; continue; }
        float uu = u + shx, ww = w + shy;
        v3 rd = nrm((v3){uu * cr - ww * sr, uu * sr + ww * cr, 1.3f});
        float T = .1f, d = 1;
        int i;
        for (i = 0; i < 44; i++) {
            d = map5((v3){ro.x + rd.x * T, ro.y + rd.y * T, ro.z + rd.z * T}, t);
            if (d < .003f * T || T > 24) break;
            T += d * .85f;
        }
        float glow = i / 44.f, fog = expf(-T * .11f), r = 0, g = 0, b = 0;
        if (T <= 24 && d < .05f) {
            v3 p = {ro.x + rd.x * T, ro.y + rd.y * T, ro.z + rd.z * T};
            float e = .01f, a1 = map5((v3){p.x + e, p.y - e, p.z - e}, t), a2 = map5((v3){p.x - e, p.y - e, p.z + e}, t),
                  a3 = map5((v3){p.x - e, p.y + e, p.z - e}, t), a4 = map5((v3){p.x + e, p.y + e, p.z + e}, t);
            v3 n = nrm((v3){a1 - a2 - a3 + a4, -a1 - a2 + a3 + a4, -a1 + a2 - a3 + a4});
            unsigned id = (unsigned)(int)floorf(p.x * .5f) * 73u + (unsigned)(int)floorf(p.y * .5f) * 1009u + (unsigned)(int)floorf(p.z * .5f) * 7919u;
            float h = hsh(id), dif = fmaxf(-dot(n, rd), 0), rim = powf(1 - dif, 3);
            v3 c = pal(h * .25f + lt * .04f, .5f, .6f, .7f);
            float em = hsh(id + beat * 101u) < .2f ? ke * 2.5f : 0;
            r = c.x * (.15f + .85f * dif + em) + rim * .3f;
            g = c.y * (.15f + .85f * dif + em) + rim * .1f;
            b = c.z * (.15f + .85f * dif + em) + rim * .45f;
        }
        glow *= glow * glow;
        P[0] = (r * fog + glow * .5f + se * .4f) * sp;
        P[1] = (g * fog + glow * .2f + se * .4f) * sp;
        P[2] = (b * fog + glow * .9f + se * .5f) * sp;
    END_PIX
    if (t > 48 && t < 52 && fr(t * 2) < .3f) {
        int s = H / 14 > 0 ? H / 14 : 1, x0 = (W - 23 * s) / 2, y0 = (H - 5 * s) / 2;
        for (int l = 0; l < 6; l++)
            for (int ry = 0; ry < 5; ry++)
                for (int rx = 0; rx < 3; rx++)
                    if (FONT[l] >> (14 - ry * 3 - rx) & 1)
                        for (int yy = 0; yy < s; yy++)
                            for (int xx = 0; xx < s; xx++) {
                                int X = x0 + (l * 4 + rx) * s + xx, Y = y0 + ry * s + yy;
                                if (X >= 0 && X < W && Y >= 0 && Y < H) { float *P = fb + (Y * W + X) * 3; P[0] = 1; P[1] = P[2] = .95f - .85f * (t > 50); }
                            }
    }
}
static void glitch(float amt) {
    float tmp[3 * 512];
    for (int y = 0; y < H; y++) {
        if (hsh(y / 3 * 17 + frame * 131) > amt * .35f) continue;
        int sh = (int)((hsh(y + frame * 7) - .5f) * W * .4f * amt), n = W < 512 ? W : 512;
        float *row = fb + y * W * 3;
        for (int x = 0; x < n; x++) {
            float *s = row + ((x - sh + n * 4) % n) * 3;
            tmp[x * 3] = s[2]; tmp[x * 3 + 1] = s[0]; tmp[x * 3 + 2] = s[1];
        }
        memcpy(row, tmp, n * 3 * sizeof(float));
    }
}

/* ------------------------------------------------------------------ 54-59s: it was watching */
static v3 gazeat(int i) { return nrm((v3){(hsh(i) * 2 - 1) * .5f, (hsh(i + 50) * 2 - 1) * .3f, -1}); }
static void s_eye(float t) {
    float v, kt = since(p_kick, t, &v), ke = v * expf(-kt * 7), se, st = snare_since(t, &se);
    se *= expf(-st * 12);
    float open = sm(54.02f, 54.3f, t) * (.03f + .97f * sm(.03f, .14f, fabsf(t - 56.25f))) * (1 - sm(58.55f, 59.1f, t));
    int bi = (int)(t * 2);
    float bp = t * 2 - bi, k = sm(0, .12f, bp);
    v3 g0 = gazeat(bi - 1), g1 = gazeat(bi), g = nrm((v3){mixf(g0.x, g1.x, k), mixf(g0.y, g1.y, k), mixf(g0.z, g1.z, k)});
    float lk = sm(57.4f, 57.7f, t);
    g = nrm((v3){g.x * (1 - lk), g.y * (1 - lk), mixf(g.z, -1, lk)});
    float pc = mixf(.972f - .012f * ke, .994f, sm(57.5f, 57.9f, t));
    pc = mixf(pc, .885f, sm(58, 58.2f, t));
    float ci = .86f, blood = sm(54, 58.5f, t), hue = t * .05f + (int)(t / 2) * .2f;
    v3 e1 = nrm(cross(g, (v3){0, 1, 0})), e2 = cross(e1, g), Lt = nrm((v3){-.5f, .6f, -1});
    v3 ro = {0, 0, -2.4f + .2f * sm(54, 58, t)};
    PIX
        float sc = 1 - .05f * ke, uu = u * sc, ww = w * sc, rr = sqrtf(uu * uu + ww * ww), a = atan2f(ww, uu);
        float ray = powf(.5f + .5f * sinf(a * 14 + t * 3), 6) * (.12f + .5f * ke) / (1 + rr) * (1 - sm(59.1f, 59.4f, t));
        float r = ray * .8f + .02f, gg = ray * .08f, b = ray * .15f + .02f;
        float hw = fmaxf(0, 1 - uu * uu), hh = open * .62f * powf(hw, .7f);
        if (ww < hh && ww > -hh * .8f) {
            v3 rd = nrm((v3){uu * .45f, ww * .45f, 1});
            float bb = dot(ro, rd), cc = dot(ro, ro) - 1, hd = bb * bb - cc;
            if (hd > 0) {
                float T = -bb - sqrtf(hd);
                v3 n = {ro.x + rd.x * T, ro.y + rd.y * T, ro.z + rd.z * T};
                float c = dot(n, g), x1 = dot(n, e1), x2 = dot(n, e2), an = atan2f(x2, x1);
                if (c > pc) { r = .01f; gg = .01f; b = .02f; }
                else if (c > ci) {
                    float q = (pc - c) / (pc - ci), fib = .5f + .5f * sinf(an * 37 + sinf(an * 5 + t) * 2) * sinf(an * 11 + q * 6);
                    v3 col = pal(hue + q * .25f, 0, .15f, .3f);
                    float l = (.35f + .65f * fib) * (1.2f - .8f * sm(.7f, 1, q)) * (q < .15f ? 1.5f : 1);
                    r = col.x * l; gg = col.y * l; b = col.z * l;
                } else {
                    float lat = acosf(clampf(c, -1, 1)), vein = sm(.1f, 0, fabsf(sinf(an * 7 + sinf(lat * 9) * 1.5f))) * sm(.4f, 1.3f, lat);
                    float vk = vein * (.25f + .75f * blood);
                    r = mixf(.93f, .75f, vk); gg = mixf(.88f, .05f, vk); b = mixf(.84f, .06f, vk);
                }
                v3 rf = {rd.x - 2 * dot(rd, n) * n.x, rd.y - 2 * dot(rd, n) * n.y, rd.z - 2 * dot(rd, n) * n.z};
                float dif = .3f + .7f * fmaxf(dot(n, Lt), 0), spc = powf(fmaxf(dot(rf, Lt), 0), 60) * 1.3f;
                float lid = sm(0, .12f, hh - ww) * sm(0, .1f, ww + hh * .8f);
                r = (r * dif + spc) * (.3f + .7f * lid); gg = (gg * dif + spc) * (.3f + .7f * lid); b = (b * dif + spc) * (.3f + .7f * lid);
            }
        } else {
            float e = fminf(fabsf(ww - hh), fabsf(ww + hh * .8f));
            float rim = expf(-e * 30) * open * (hw > 0);
            r += rim * .7f; gg += rim * .12f; b += rim * .15f;
        }
        P[0] = r + se * .3f; P[1] = gg + se * .25f; P[2] = b + se * .25f;
    END_PIX
}

/* ------------------------------------------------------------------ main */
static void render(float t) {
    float sp, tw = (float)warp(t, &sp);
    memset(fb, 0, sizeof(float) * 3 * W * H);
    memset(tx, 0, COLS * ROWS);
    if (t < 8) s_void(t);
    else if (t < 18) s_cloud(t);
    else if (t < 30) s_horizon(t);
    else if (t < 44) s_tunnel(t);
    else if (t < 52.75f) s_cubes(tw, sp);
    else if (t < 59.3f) s_eye(t);
    else { glowdot(t, 1, 1); typed(t, 59.5f, "> yes."); }
    if (t > 50 && t < 52.75f) glitch(sm(50, 52, t) * .7f + (1 - sp) * .8f);
    static const float FT[6] = {8, 18, 30, 44, 54, 58}, FV[6] = {.9f, .7f, .6f, .6f, 1.2f, .9f};
    float fl = 0;
    for (int i = 0; i < 6; i++) if (t >= FT[i]) fl += FV[i] * expf(-(t - FT[i]) * 6);
    PIX
        float vg = 1 - .18f * (u * u * .25f + w * w);
        P[0] = P[0] * vg + fl; P[1] = P[1] * vg + fl; P[2] = P[2] * vg + fl;
    END_PIX
}

int main(void) {
    struct termios tio;
    int audio = audio_open();
    pthread_t th;
    double t0, last = 0;
    tcgetattr(0, &tio0);
    tio = tio0;
    tio.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(0, TCSANOW, &tio);
    signal(SIGINT, bye); signal(SIGTERM, bye); signal(SIGHUP, bye);
    resize();
    raw("\033[?1049h\033[?25l\033[0m\033[2J");
    if (audio) pthread_create(&th, 0, audio_thread, 0);
    t0 = now();
    for (;;) {
        double t, fs = now();
        if (audio) {
            t = a_pos > -.5 ? a_pos + (fs - a_stamp) : 0;
            if (fs - t0 > 3 && a_pos < -.5) audio = 0;
        } else t = fs - t0;
        if (t < last) t = last;
        last = t;
        if (t > END) break;
        if (t >= 52.75 && t < 54) {
            /* "crash" back to the shell for a moment */
            if (!crash) { crash = 1; raw("\033[?2026l\033[0m\033[?1049l\033[?25hSegmentation fault (core dumped)\r\n"); }
            usleep(5000);
            continue;
        }
        if (crash == 1) { crash = 2; raw("\033[A\r\033[J\033[?25l\033[?1049h\033[2J"); memset(prev, 0xff, sizeof(uint64_t) * COLS * ROWS); }
        if (resize()) raw("\033[2J");
        render((float)t);
        present();
        frame++;
        double sl = 1 / 30. - (now() - fs);
        if (sl > 0) usleep((useconds_t)(sl * 1e6));
    }
    bye(0);
}

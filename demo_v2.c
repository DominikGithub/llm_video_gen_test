/* WATCHER v2 -- a 60 second demo. No files, no assets: everything is
 * synthesized at runtime. Rendered in software on all CPU cores at 1024x576,
 * shown in an X11 window (scaled to fit / fullscreen). Sound is a tiny realtime
 * synth sent to PulseAudio (or ALSA). Xlib, Pulse and ALSA are loaded with dlopen. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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
static float fatan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y), a = fminf(ax, ay) / (fmaxf(ax, ay) + 1e-20f), s = a * a;
    float r = ((-.0464964749f * s + .15931422f) * s - .327622764f) * s * a + a;
    if (ay > ax) r = 1.5707963f - r;
    if (x < 0) r = PI - r;
    return y < 0 ? -r : r;
}

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


/* ------------------------------------------------------------------ frame buffer + worker threads */
#define W 1024
#define H 576
static float *fb, U[W], V[H], T;
static uint32_t *px;
static int frame;
static void (*job)(int), (*rowf)(int);
static int jobn, nextj;
static pthread_barrier_t bar0, bar1;
static void runjobs(void) {
    int j;
    while ((j = __atomic_fetch_add(&nextj, 1, __ATOMIC_RELAXED)) < jobn) job(j);
}
static void *worker(void *x) {
    for (;;) { pthread_barrier_wait(&bar0); runjobs(); pthread_barrier_wait(&bar1); }
    return x;
}
static void par(void (*f)(int), int n) { job = f; jobn = n; nextj = 0; pthread_barrier_wait(&bar0); runjobs(); pthread_barrier_wait(&bar1); }
static void bandjob(int b) { for (int y = b * 8; y < b * 8 + 8; y++) rowf(y); }
static void rows(void (*f)(int)) { rowf = f; par(bandjob, H / 8); }

static void splat(float sx, float sy, float rad, float r, float g, float b) {
    float cx = W * .5f + sx * H * .5f, cy = H * .5f - sy * H * .5f, ir = 1 / (rad * rad);
    int x0 = (int)(cx - rad), x1 = (int)(cx + rad) + 1, y0 = (int)(cy - rad), y1 = (int)(cy + rad) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, q = 1 - (dx * dx + dy * dy) * ir;
            if (q > 0) { float *P = fb + (y * W + x) * 3; q *= q; P[0] += r * q; P[1] += g * q; P[2] += b * q; }
        }
}
static const char GLY[] = ">isanyoethr?._CLOSER";
static const uint64_t FONT[] = {0x410411110ull, 0x100c2108eull, 0xf8383eull, 0xe0be2full, 0x16cc631ull, 0x118bc2eull, 0xe8c62eull,
                                0xe8fe0eull, 0x211c42126ull, 0x4216cc631ull, 0x16cc210ull, 0x3a2111004ull, 0x18cull, 0x1full,
                                0x3a308422eull, 0x42108421full, 0x3a318c62eull, 0x3e107043eull, 0x7e10f421full, 0x7a31f5251ull};
static void text(int x0, int y0, int s, const char *str, int n, float r, float g, float b) {
    for (int i = 0; i < n && str[i]; i++) {
        const char *p = strchr(GLY, str[i]);
        if (!p || !*p) continue;
        for (int k = 0; k < 35; k++)
            if (FONT[p - GLY] >> (34 - k) & 1)
                for (int yy = 0; yy < s; yy++)
                    for (int xx = 0; xx < s; xx++) {
                        int X = x0 + (i * 6 + k % 5) * s + xx, Y = y0 + k / 5 * s + yy;
                        if (X >= 0 && X < W && Y >= 0 && Y < H) { float *P = fb + (Y * W + X) * 3; P[0] = r; P[1] = g; P[2] = b; }
                    }
    }
}
static void typed(float t, float t0, const char *s) {
    int n = (int)((t - t0) * 11), l = (int)strlen(s);
    if (t < t0) return;
    if (n > l) n = l;
    text(40, H - 64, 3, s, n, .72f, .72f, .72f);
    if (n >= l && fr(t * 2) < .5f) text(40 + n * 18, H - 64, 3, "_", 1, .72f, .72f, .72f);
}
static float heart(float t) { float v, tau = since(p_heart, t, &v); return v * expf(-tau * 7); }

/* ------------------------------------------------------------------ 0-8s: the void */
static float vE, vF, vG;
static void r_dot(int y) {
    float w = V[y], *P = fb + y * W * 3, k = vF * .0012f * (1 + 3 * vE) * vG, c = .0006f * (1 + vE) * vG;
    for (int x = 0; x < W; x++, P += 3) {
        float u = U[x], r2 = u * u + w * w, q = 1 + r2 * 5, I = k / (r2 + c) + vF * .04f * vE / (q * q);
        P[0] = I; P[1] = I * .3f + I * I * .4f; P[2] = I * .25f + I * I * .4f;
    }
}
static void s_dot(float t, float fade, float grow) { vE = heart(t); vF = fade; vG = grow; rows(r_dot); }
static void s_void(float t) {
    s_dot(t, sm(0, 2.5f, t), 1 + sm(6.5f, 8, t) * 4);
    if (t > 3.5f)
        for (int i = 0; i < 260; i++) {
            float f = fr(t * .12f + hsh(i)), rad = (1 - f) * (1.1f + hsh(i + 99)), a = hsh(i + 33) * TAU + f * 4;
            float br = f * f * sm(3.5f, 6.5f, t) * .9f;
            splat(cosf(a) * rad, sinf(a) * rad, 1.5f + 2 * f, br * .9f, br * .45f, br * .4f);
        }
    if (t < 7) typed(t, 1.5f, "> is anyone there?");
}

/* ------------------------------------------------------------------ 8-18s: point cloud */
#define NP 1600
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
static float nebA;
static void r_neb(int y) {
    float w = V[y], *P = fb + y * W * 3;
    for (int x = 0; x < W; x++, P += 3) {
        float u = U[x], q = 1 / (1 + (u * u + w * w) * 3);
        P[0] = q * .03f * nebA; P[1] = q * .01f * nebA; P[2] = q * .07f * nebA;
    }
}
static void s_cloud(float t) {
    float lt = t - 8, v, at = since(p_arp, t, &v), ae = v * expf(-at * 10);
    float ex = 1 - expf(-lt * 5) * cosf(lt * 9), ph = lt / 2.5f, m;
    int k = (int)ph;
    m = sm(.65f, 1, ph - k);
    if (k >= 3) k = 3, m = 0;
    nebA = (1 + ae) * sm(8, 10, t);
    rows(r_neb);
    float zoom = 1 + powf(sm(16.6f, 18, t), 3) * 9, sc = ex * (1 + .08f * ae) * zoom;
    float ca = cosf(t * .7f), sa = sinf(t * .7f), cb = cosf(t * .43f), sb = sinf(t * .43f);
    for (int i = 0; i < 500; i++) {
        float hx = (hsh(i * 7 + 2) * 2 - 1) * .5f, hy = (hsh(i * 7 + 3) * 2 - 1) * .3f;
        for (int j = 0; j < 4; j++) { /* streaks: a few samples back in time */
            float tt = lt - j * .012f, z = fr(hsh(i * 7 + 1) - tt * (.06f + tt * .012f)) + .03f, br = (1 - z) * .45f * sm(8, 10, t) * (1 - j * .2f);
            splat(hx / z, hy / z, 1.6f, br * .7f, br * .8f, br);
        }
    }
    for (int i = 0; i < NP; i++) {
        float p[3], q[3];
        shape(k, i, t, p);
        if (m > 0) { shape(k + 1, i, t, q); for (int j = 0; j < 3; j++) p[j] = mixf(p[j], q[j], m); }
        float x = p[0] * sc, y = p[1] * sc, z = p[2] * sc;
        float x2 = x * ca + z * sa, z2 = -x * sa + z * ca;
        float y2 = y * cb - z2 * sb, z3 = y * sb + z2 * cb + 3;
        if (z3 < .15f) continue;
        float near = clampf((4.3f - z3) / 2.2f, .08f, 1), br = near * (.5f + .6f * ae);
        v3 c = pal(i / (float)NP * .5f + t * .05f, 0, .33f, .67f);
        splat(x2 / z3 * 1.6f, y2 / z3 * 1.6f, (1.6f + 3 * near) * (z3 < 1 ? 1 / z3 : 1), c.x * br, c.y * br, c.z * br);
    }
}

/* ------------------------------------------------------------------ 18-30s: retro horizon + eclipse */
static float hLt, hKe, hCr, hSr, hZ, hCam, hEc, hMx, hMy, mLUT[1024];
#define HZ -.1f
#define SY (HZ + .4f)
#define SR_ .48f
static void r_horizon(int y) {
    float *P = fb + y * W * 3;
    for (int x = 0; x < W; x++, P += 3) {
        float u = U[x], w = V[y], uu = u * hCr - w * hSr, ww = u * hSr + w * hCr, r, g, b;
        if (ww > HZ) {
            float k = sm(0, .9f, ww - HZ);
            r = mixf(.95f, .05f, k); g = mixf(.25f, .02f, k); b = mixf(.45f, .16f, k);
            if (hsh(x * 131 + y * 7919) > .996f) { float s = (.5f + .5f * sinf(T * 3 + x)) * k; r += s; g += s; b += s; }
            float dy = ww - SY, d = sqrtf(uu * uu + dy * dy);
            float glow = .08f / (1 + fmaxf(d - SR_, 0) * 14) * (1 + hKe);
            r += glow; g += glow * .3f; b += glow * .5f;
            int insun = d < SR_;
            if (insun && dy < .05f && fr(dy * 14 + hLt * .6f) < sm(.05f, -.45f, dy) * .65f) insun = 0;
            if (insun) { float gy = (dy + SR_) / (2 * SR_); r = 1; g = mixf(.15f, .9f, gy); b = mixf(.5f, .25f, gy); }
            if (hEc > .01f) {
                float dm = sqrtf((uu - hMx) * (uu - hMx) + (ww - hMy) * (ww - hMy));
                if (dm < SR_ * .98f) {
                    float rim = sm(SR_ * .9f, SR_ * .98f, dm) * hEc;
                    r = .02f + rim; g = rim * .4f; b = .05f + rim * .6f;
                }
                float cor = hEc * hEc * .03f / (fabsf(d - SR_) + .02f) * (1 + 2 * hKe);
                r += cor; g += cor * .6f; b += cor * .9f;
            }
            int mi = (int)((uu + 2.5f) * 204.6f);
            float mh = mLUT[mi < 0 ? 0 : mi > 1023 ? 1023 : mi];
            if (ww < mh) {
                float e = sm(mh - .008f, mh, ww);
                r = .07f + e * .9f; g = .01f + e * .25f; b = .12f + e * .6f;
            }
        } else {
            float d = HZ - ww, z = .25f / d, gx = uu * z + hCam, gz = z + hZ;
            float fw = z * 2 / H, fz = z * z * 8 / H;
            float lx = 1 - sm(0, fw * 1.5f + .012f, fminf(fr(gx), 1 - fr(gx)));
            float lz = 1 - sm(0, fz * 1.5f + .012f, fminf(fr(gz), 1 - fr(gz)));
            float fog = expf(-z * .13f), fade = fminf(1, .4f / (fz + .001f)), L = (.8f + 1.5f * hKe) * fog;
            r = .06f + (lx + lz * fade) * L * .9f + (1 - fog) * .8f;
            g = lz * fade * L * .3f + lx * L * .15f + (1 - fog) * .22f;
            b = .1f + (lx * .8f + lz * fade) * L + (1 - fog) * .4f;
        }
        P[0] = r; P[1] = g; P[2] = b;
    }
}
static void s_horizon(float t) {
    float kv, kt = since(p_kick, t, &kv), roll = .07f * sinf(t * .8f);
    hLt = t - 18; hKe = kv * expf(-kt * 6); hCr = cosf(roll); hSr = sinf(roll);
    hZ = hLt * 1.6f + hLt * hLt * .09f; hCam = sinf(t * .35f) * 1.5f;
    hEc = sm(23, 29, t); hMx = (1 - hEc) * 1.3f; hMy = SY + (1 - hEc) * .2f;
    for (int i = 0; i < 1024; i++) {
        float uu = i / 204.6f - 2.5f;
        mLUT[i] = HZ + .11f * (fabsf(sinf(uu * 2.3f + hCam * .3f + 1)) * .7f + fabsf(sinf(uu * 5.7f + 2 + hCam * .3f)) * .3f) * (.35f + sm(.2f, 1.2f, fabsf(uu)));
    }
    rows(r_horizon);
}

/* ------------------------------------------------------------------ 30-44s: tunnel */
static float tKe, tSe, tZt, tCx, tCy, tK1, tK2, tLt, tPal[256][3];
static void r_tunnel(int y) {
    float *P = fb + y * W * 3, w = V[y];
    for (int x = 0; x < W; x++, P += 3) {
        float du = U[x] - tCx, dv = w - tCy, a = fatan2(dv, du);
        float rc = sqrtf(du * du + dv * dv), r = rc;
        if (tK1 > 0) r = mixf(rc, fmaxf(fabsf(du), fabsf(dv)), tK1);
        if (tK2 > 0) r = mixf(r, rc * (1 + .25f * sinf(a * 6 + T * 2)), tK2);
        r += 1e-3f;
        float depth = .5f / r, z = depth + tZt, aa = a * (1 / TAU) + .05f * z * sinf(T * .3f) + T * .05f;
        float tu = fr(aa * 12), tv = fr(z * 1.5f);
        float h = hsh((unsigned)(int)floorf(aa * 12) * 31u + (unsigned)(int)floorf(z * 1.5f) * 977u);
        float e = fminf(fminf(tu, 1 - tu), fminf(tv, 1 - tv)), *c = tPal[(int)(h * 255)];
        float lit = .18f + (h > .85f ? .3f + .5f * tKe : 0), line = sm(.07f, 0, e) * .55f;
        float ring = expf(-fabsf(fr(z * .25f - tLt * .5f) - .5f) * 30) * (.15f + tKe);
        float fog = expf(-depth * .33f), cl = .02f / (r * r + .02f) * (.6f + .6f * tKe);
        P[0] = (c[0] * lit + line * .9f + ring) * fog + cl + tSe * .25f;
        P[1] = (c[1] * lit + line * .3f + ring * .5f) * fog + cl * .9f + tSe * .25f;
        P[2] = (c[2] * lit + line + ring * .8f) * fog + cl * .8f + tSe * .3f;
    }
}
static void s_tunnel(float t) {
    float v, kt = since(p_kick, t, &v), st, amp = sm(30, 33, t), hue = (int)(t / 2) * .13f;
    tKe = v * expf(-kt * 7); st = snare_since(t, &tSe); tSe *= expf(-st * 9);
    tLt = t - 30; tZt = tLt * (2 + tLt * .15f); tCx = .35f * sinf(t * .9f) * amp; tCy = .25f * cosf(t * .6f) * amp;
    tK1 = sm(36.5f, 37.5f, t); tK2 = sm(40, 41, t);
    for (int i = 0; i < 256; i++) { v3 c = pal(i / 256.f * .3f + hue, 0, .1f, .2f); tPal[i][0] = c.x; tPal[i][1] = c.y; tPal[i][2] = c.z; }
    rows(r_tunnel);
}

/* ------------------------------------------------------------------ 44-52s: cube storm (polygon rasterizer) */
typedef struct { float A[4], B[4], C[4], r, g, b, er, eg, eb, ew; int x0, x1, y0, y1; } Face;
typedef struct { float d; int i, j, k; } Cell;
#define MC 6000
static Face *F;
static Cell CL[MC];
static int nf;
static float cBg;
static int cellcmp(const void *a, const void *b) { float d = ((const Cell *)b)->d - ((const Cell *)a)->d; return (d > 0) - (d < 0); }
static void r_cubes(int band) {
    int ya = band * 8, yb = ya + 8;
    for (int y = ya; y < yb; y++) {
        float *P = fb + y * W * 3, w = V[y];
        for (int x = 0; x < W; x++, P += 3) {
            float u = U[x], q = 1 / (1 + (u * u + w * w) * 1.5f);
            P[0] = (.03f + .1f * q) * cBg; P[1] = (.02f + .05f * q) * cBg; P[2] = (.08f + .22f * q) * cBg;
        }
    }
    for (int f = 0; f < nf; f++) {
        Face *o = F + f;
        if (o->y1 < ya || o->y0 >= yb) continue;
        int y0 = o->y0 > ya ? o->y0 : ya, y1 = o->y1 < yb - 1 ? o->y1 : yb - 1;
        for (int y = y0; y <= y1; y++) {
            float py = y + .5f, *P = fb + (y * W + o->x0) * 3;
            float r0 = o->B[0] * py + o->C[0], r1 = o->B[1] * py + o->C[1], r2 = o->B[2] * py + o->C[2], r3 = o->B[3] * py + o->C[3];
            for (int x = o->x0; x <= o->x1; x++, P += 3) {
                float pxx = x + .5f, d = fminf(fminf(o->A[0] * pxx + r0, o->A[1] * pxx + r1), fminf(o->A[2] * pxx + r2, o->A[3] * pxx + r3));
                if (d < -.5f) continue;
                float cov = d > .5f ? 1 : d + .5f, e = 1 - d / o->ew;
                e = e > 0 ? e * e : 0;
                P[0] += (o->r + o->er * e - P[0]) * cov;
                P[1] += (o->g + o->eg * e - P[1]) * cov;
                P[2] += (o->b + o->eb * e - P[2]) * cov;
            }
        }
    }
}
static const unsigned char *CLOSER = (const unsigned char *)"CLOSER";
static void s_cubes(float t) {
    float lt = t - 44, v, kt = since(p_kick, t, &v), ke = v * expf(-kt * 7), se, st = snare_since(t, &se);
    se *= expf(-st * 14) * sm(44, 52, t);
    float zp = lt * 3 + lt * lt * .55f, roll = lt * .35f + .3f * sinf(t * .5f), cr = cosf(roll), sr = sinf(roll);
    float shk = .04f * lt * lt / 64, shx = (hsh(frame * 3) - .5f) * shk, shy = (hsh(frame * 3 + 1) - .5f) * shk;
    int beat = (int)(t * 2), nc = 0, k0 = (int)floorf(zp * .5f) - 1;
    v3 ro = {.25f * sinf(t * .9f), .25f * cosf(t * .7f), zp};
    cBg = 1 + se * 3;
    for (int k = k0; k < k0 + 11; k++)
        for (int j = -14; j < 14; j++)
            for (int i = -18; i < 18; i++) {
                float x = 2 * i + 1 - ro.x, y = 2 * j + 1 - ro.y, z = 2 * k + 1 - ro.z;
                if (z < -.5f || x * x + y * y > (z + 1.2f) * (z + 1.2f) * 2.6f) continue;
                float d = sqrtf(x * x + y * y + z * z);
                if (d < 19 && nc < MC) CL[nc++] = (Cell){d, i, j, k};
            }
    qsort(CL, nc, sizeof(Cell), cellcmp);
    nf = 0;
    for (int c = 0; c < nc; c++) {
        Cell *q = CL + c;
        unsigned id = (unsigned)q->i * 73u + (unsigned)q->j * 1009u + (unsigned)q->k * 7919u;
        float h = hsh(id), an = h * 6.28f + t * (1 + h * 2), ca = cosf(an), sa = sinf(an), s = .32f;
        /* rotation matrix columns: Rx(an) * Rz(an) */
        v3 ax[3] = {{ca, sa * ca, sa * sa}, {-sa, ca * ca, ca * sa}, {0, -sa, ca}};
        v3 cen = {2 * q->i + 1 - ro.x, 2 * q->j + 1 - ro.y, 2 * q->k + 1 - ro.z};
        float fog = expf(-q->d * .1f) * sm(19, 13, q->d), em = hsh(id + beat * 101u) < .2f ? ke * 2.5f : 0;
        v3 col = pal(h * .25f + lt * .04f, .5f, .6f, .7f);
        float sx[8], sy[8];
        int bad = 0;
        for (int vI = 0; vI < 8; vI++) {
            float a = vI & 1 ? s : -s, b = vI & 2 ? s : -s, e = vI & 4 ? s : -s;
            float X = cen.x + ax[0].x * a + ax[1].x * b + ax[2].x * e, Y = cen.y + ax[0].y * a + ax[1].y * b + ax[2].y * e,
                  Z = cen.z + ax[0].z * a + ax[1].z * b + ax[2].z * e;
            if (Z < .12f) { bad = 1; break; }
            float xs = X * 1.3f / Z, ys = Y * 1.3f / Z, uu = xs * cr + ys * sr - shx, ww = -xs * sr + ys * cr - shy;
            sx[vI] = W * .5f + uu * H * .5f; sy[vI] = H * .5f - ww * H * .5f;
        }
        if (bad) continue;
        for (int fI = 0; fI < 6; fI++) {
            int axis = fI >> 1, sg = fI & 1 ? 1 : -1, bit = 1 << axis, b1 = 1 << ((axis + 1) % 3), b2 = 1 << ((axis + 2) % 3);
            v3 n = ax[axis];
            n = (v3){n.x * sg, n.y * sg, n.z * sg};
            v3 fc = {cen.x + n.x * s, cen.y + n.y * s, cen.z + n.z * s};
            if (dot(n, fc) >= 0) continue;
            int base = sg > 0 ? bit : 0, vi[4] = {base, base | b1, base | b1 | b2, base | b2};
            Face *o = F + nf;
            float area = 0, mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
            for (int e = 0; e < 4; e++) {
                int a = vi[e], b = vi[(e + 1) & 3];
                area += sx[a] * sy[b] - sx[b] * sy[a];
                mnx = fminf(mnx, sx[a]); mxx = fmaxf(mxx, sx[a]); mny = fminf(mny, sy[a]); mxy = fmaxf(mxy, sy[a]);
            }
            if (fabsf(area) < .5f || mxx < 0 || mnx >= W || mxy < 0 || mny >= H) continue;
            float sg2 = area > 0 ? 1 : -1;
            for (int e = 0; e < 4; e++) {
                int a = vi[e], b = vi[(e + 1) & 3];
                float A = sy[a] - sy[b], B = sx[b] - sx[a], l = sg2 / (sqrtf(A * A + B * B) + 1e-9f);
                o->A[e] = A * l; o->B[e] = B * l; o->C[e] = -(A * sx[a] + B * sy[a]) * l;
            }
            float dif = fabsf(dot(n, nrm(fc))), lit = (.12f + .88f * dif + em) * fog, eg = (.5f + ke * 1.5f) * fog;
            o->r = col.x * lit + .03f * (1 - fog); o->g = col.y * lit + .02f * (1 - fog); o->b = col.z * lit + .08f * (1 - fog);
            o->er = .5f * eg; o->eg = .25f * eg; o->eb = 1.f * eg;
            o->ew = clampf(sqrtf(fabsf(area)) * .07f, 1, 4);
            o->x0 = mnx < 0 ? 0 : (int)mnx; o->x1 = mxx >= W ? W - 1 : (int)mxx;
            o->y0 = mny < 0 ? 0 : (int)mny; o->y1 = mxy >= H ? H - 1 : (int)mxy;
            nf++;
        }
    }
    par(r_cubes, H / 8);
    for (int y = 0; y < H; y++) /* snare flashes */
        if (se > .01f) for (float *P = fb + y * W * 3, *E = P + W * 3; P < E; P += 3) { P[0] += se * .4f; P[1] += se * .4f; P[2] += se * .5f; }
    if (t > 48 && t < 52 && fr(t * 2) < .3f) {
        int s = 15;
        text((W - 35 * s) / 2, (H - 7 * s) / 2, s, (const char *)CLOSER, 6, 1, .95f - .85f * (t > 50), .95f - .85f * (t > 50));
    }
}

/* ------------------------------------------------------------------ 54-59s: it was watching */
static v3 gazeat(int i) { return nrm((v3){(hsh(i) * 2 - 1) * .5f, (hsh(i + 50) * 2 - 1) * .3f, -1}); }
static float eKe, eSe, eOpen, ePc, eBlood, eRay, eIris[64][3];
static v3 eG, eE1, eE2, eL, eRo;
static void r_eye(int y) {
    float *P = fb + y * W * 3, sc = 1 - .05f * eKe, ww = V[y] * sc, ci = .86f;
    for (int x = 0; x < W; x++, P += 3) {
        float uu = U[x] * sc, rr = sqrtf(uu * uu + ww * ww), a = fatan2(ww, uu);
        float s = .5f + .5f * sinf(a * 14 + T * 3);
        s *= s; s = s * s * s;
        float ray = s * (.12f + .5f * eKe) / (1 + rr) * eRay;
        float r = ray * .8f + .02f, gg = ray * .08f, b = ray * .15f + .02f;
        float hw = fmaxf(0, 1 - uu * uu), hh = eOpen * .62f * powf(hw, .7f);
        if (ww < hh && ww > -hh * .8f) {
            v3 rd = nrm((v3){uu * .45f, ww * .45f, 1});
            float bb = dot(eRo, rd), cc = dot(eRo, eRo) - 1, hd = bb * bb - cc;
            if (hd > 0) {
                float Tt = -bb - sqrtf(hd);
                v3 n = {eRo.x + rd.x * Tt, eRo.y + rd.y * Tt, eRo.z + rd.z * Tt};
                float c = dot(n, eG), an = fatan2(dot(n, eE2), dot(n, eE1));
                if (c > ePc) { r = .01f; gg = .01f; b = .02f; }
                else if (c > ci) {
                    float q = (ePc - c) / (ePc - ci), fib = .5f + .5f * sinf(an * 37 + sinf(an * 5 + T) * 2) * sinf(an * 11 + q * 6);
                    float *col = eIris[(int)(q * 63)], l = (.35f + .65f * fib) * (1.2f - .8f * sm(.7f, 1, q)) * (q < .15f ? 1.5f : 1);
                    r = col[0] * l; gg = col[1] * l; b = col[2] * l;
                } else {
                    float lat = acosf(clampf(c, -1, 1)), vein = sm(.1f, 0, fabsf(sinf(an * 7 + sinf(lat * 9) * 1.5f))) * sm(.4f, 1.3f, lat);
                    float vk = vein * (.25f + .75f * eBlood);
                    r = mixf(.93f, .75f, vk); gg = mixf(.88f, .05f, vk); b = mixf(.84f, .06f, vk);
                }
                float dn = 2 * dot(rd, n), sp = fmaxf(dot((v3){rd.x - dn * n.x, rd.y - dn * n.y, rd.z - dn * n.z}, eL), 0);
                sp *= sp; sp *= sp; sp *= sp; sp *= sp; sp *= sp; sp *= sp;
                float dif = .3f + .7f * fmaxf(dot(n, eL), 0), lid = (.3f + .7f * sm(0, .12f, hh - ww) * sm(0, .1f, ww + hh * .8f));
                r = (r * dif + sp * 1.3f) * lid; gg = (gg * dif + sp * 1.3f) * lid; b = (b * dif + sp * 1.3f) * lid;
            }
        } else if (hw > 0) {
            float e = fminf(fabsf(ww - hh), fabsf(ww + hh * .8f)), rim = expf(-e * 40) * eOpen;
            r += rim * .7f; gg += rim * .12f; b += rim * .15f;
        }
        P[0] = r + eSe * .3f; P[1] = gg + eSe * .25f; P[2] = b + eSe * .25f;
    }
}
static void s_eye(float t) {
    float v, kt = since(p_kick, t, &v), st;
    eKe = v * expf(-kt * 7); st = snare_since(t, &eSe); eSe *= expf(-st * 12);
    eOpen = sm(54.02f, 54.3f, t) * (.03f + .97f * sm(.03f, .14f, fabsf(t - 56.25f))) * (1 - sm(58.55f, 59.1f, t));
    int bi = (int)(t * 2);
    float k = sm(0, .12f, t * 2 - bi), lk = sm(57.4f, 57.7f, t), hue = t * .05f + (int)(t / 2) * .2f;
    v3 g0 = gazeat(bi - 1), g1 = gazeat(bi), g = nrm((v3){mixf(g0.x, g1.x, k), mixf(g0.y, g1.y, k), mixf(g0.z, g1.z, k)});
    eG = nrm((v3){g.x * (1 - lk), g.y * (1 - lk), mixf(g.z, -1, lk)});
    ePc = mixf(mixf(.972f - .012f * eKe, .994f, sm(57.5f, 57.9f, t)), .885f, sm(58, 58.2f, t));
    eBlood = sm(54, 58.5f, t); eRay = 1 - sm(59.1f, 59.4f, t);
    eE1 = nrm(cross(eG, (v3){0, 1, 0})); eE2 = cross(eE1, eG); eL = nrm((v3){-.5f, .6f, -1});
    eRo = (v3){0, 0, -2.4f + .2f * sm(54, 58, t)};
    for (int i = 0; i < 64; i++) { v3 c = pal(hue + i / 63.f * .25f, 0, .15f, .3f); eIris[i][0] = c.x; eIris[i][1] = c.y; eIris[i][2] = c.z; }
    rows(r_eye);
}

/* ------------------------------------------------------------------ post: vignette, flashes, glitch, dither */
static float pFl, pBr, pGl;
static void r_post(int y) {
    int sh = 0, sw = 0;
    if (pGl > 0 && hsh(y / 16 * 17 + frame * 131) < pGl * .35f) { sh = (int)((hsh(y / 16 + frame * 7) - .5f) * W * .4f * pGl); sw = 1; }
    float w = V[y], *row = fb + y * W * 3;
    uint32_t *o = px + y * W;
    for (int x = 0; x < W; x++) {
        float *s = row + ((x - sh + W * 4) % W) * 3, u = U[x], vg = (1 - .18f * (u * u * .25f + w * w)) * pBr;
        float dth = (((x ^ y) & 3) * 4 + (y & 3)) * (1 / 16.f / 255) - .5f / 255;
        float r = s[sw ? 2 : 0] * vg + pFl + dth, g = s[sw ? 0 : 1] * vg + pFl + dth, b = s[sw ? 1 : 2] * vg + pFl + dth;
        o[x] = (uint32_t)(clampf(r, 0, 1) * 255.f) << 16 | (uint32_t)(clampf(g, 0, 1) * 255.f) << 8 | (uint32_t)(clampf(b, 0, 1) * 255.f);
    }
}
static void render(float t) {
    float sp, tw = (float)warp(t, &sp);
    T = t; pBr = 1; pGl = 0;
    if (t < 8) s_void(t);
    else if (t < 18) s_cloud(t);
    else if (t < 30) s_horizon(t);
    else if (t < 44) s_tunnel(t);
    else if (t < 52.75f) { T = tw; s_cubes(tw); pBr = sp; }
    else if (t < 59.3f) s_eye(t);
    else { s_dot(t, 1, 1); typed(t, 59.5f, "> yes."); }
    if (t > 50 && t < 52.75f) pGl = sm(50, 52, t) * .7f + (1 - sp) * .8f;
    static const float FT[6] = {8, 18, 30, 44, 54, 58}, FV[6] = {.9f, .7f, .6f, .6f, 1.2f, .9f};
    pFl = 0;
    for (int i = 0; i < 6; i++) if (t >= FT[i]) pFl += FV[i] * expf(-(t - FT[i]) * 6);
    rows(r_post);
}

/* ------------------------------------------------------------------ window (Xlib via dlopen) */
typedef long (*xf)();
static void *X, *dpy, *xim, *xout;
static unsigned long win;
static int OW = W, OH = H, crash;
static uint32_t *obuf;
static xf xPut, xPending, xNext, xSync, xLook, xCreateImage;
static long xvis, xdep, xgc, delAtom, stAtom, fsAtom, xroot;
#define XS(n) ((xf)dlsym(X, #n))
static void say(const char *s) { if (write(1, s, strlen(s))) {} }
static void bye(int sig) {
    (void)sig;
    if (crash == 1) say("\033[A\r\033[J");
    _exit(0);
}
static void fullscreen(int on) {
    union { long l[24]; int i[48]; } e = {{0}};
    e.i[0] = 33; e.l[4] = win; e.l[5] = stAtom; e.i[12] = 32; e.l[7] = on; e.l[8] = fsAtom;
    XS(XSendEvent)(dpy, xroot, 0, 3L << 19, &e);
}
static int xopen(int fs) {
    if (!(X = dlopen("libX11.so.6", RTLD_NOW)) || !(dpy = (void *)XS(XOpenDisplay)((char *)0))) return 0;
    int scr = (int)XS(XDefaultScreen)(dpy);
    xroot = XS(XRootWindow)(dpy, scr); xvis = XS(XDefaultVisual)(dpy, scr); xdep = XS(XDefaultDepth)(dpy, scr); xgc = XS(XDefaultGC)(dpy, scr);
    xPut = XS(XPutImage); xPending = XS(XPending); xNext = XS(XNextEvent); xSync = XS(XSync); xLook = XS(XLookupKeysym); xCreateImage = XS(XCreateImage);
    win = XS(XCreateSimpleWindow)(dpy, xroot, 0, 0, W, H, 0, 0L, XS(XBlackPixel)(dpy, scr));
    XS(XStoreName)(dpy, win, "WATCHER");
    XS(XSelectInput)(dpy, win, 1L | 1L << 17);
    delAtom = XS(XInternAtom)(dpy, "WM_DELETE_WINDOW", 0);
    XS(XSetWMProtocols)(dpy, win, &delAtom, 1);
    stAtom = XS(XInternAtom)(dpy, "_NET_WM_STATE", 0); fsAtom = XS(XInternAtom)(dpy, "_NET_WM_STATE_FULLSCREEN", 0);
    if (fs) XS(XChangeProperty)(dpy, win, stAtom, 4L, 32, 0, &fsAtom, 1);
    long pm = XS(XCreateBitmapFromData)(dpy, win, "\0\0\0\0\0\0\0\0", 8, 8), col[2] = {0}; /* invisible mouse cursor */
    XS(XDefineCursor)(dpy, win, XS(XCreatePixmapCursor)(dpy, pm, pm, col, col, 0, 0));
    xim = (void *)xCreateImage(dpy, xvis, xdep, 2, 0, px, W, H, 32, 0);
    obuf = malloc(sizeof(uint32_t) * 4096 * 4096);
    XS(XMapRaised)(dpy, win);
    return 1;
}
/* bilinear scale of the 1024x576 frame into the (letterboxed) window */
static float ss;
static int ox, oy, cX[4096], cW[4096];
static void r_scale(int b) {
    for (int Y = b * 8; Y < b * 8 + 8 && Y < OH; Y++) {
        uint32_t *o = obuf + Y * OW;
        float fy = (Y - oy + .5f) / ss - .5f;
        if (fy < -.5f || fy > H - .5f) { memset(o, 0, OW * 4); continue; }
        fy = clampf(fy, 0, H - 1.001f);
        int y0 = (int)fy, wy = (int)((fy - y0) * 256);
        const uint32_t *r0 = px + y0 * W, *r1 = r0 + (y0 + 1 < H ? W : 0);
        for (int X = 0; X < OW; X++) {
            int x0 = cX[X], wx = cW[X];
            if (x0 < 0) { o[X] = 0; continue; }
            uint32_t a = r0[x0], bq = r0[x0 + 1], c = r1[x0], d = r1[x0 + 1];
            uint32_t rb0 = ((a & 0xff00ff) * (256 - wx) + (bq & 0xff00ff) * wx) >> 8 & 0xff00ff, g0 = ((a & 0xff00) * (256 - wx) + (bq & 0xff00) * wx) >> 8 & 0xff00;
            uint32_t rb1 = ((c & 0xff00ff) * (256 - wx) + (d & 0xff00ff) * wx) >> 8 & 0xff00ff, g1 = ((c & 0xff00) * (256 - wx) + (d & 0xff00) * wx) >> 8 & 0xff00;
            o[X] = ((rb0 * (256 - wy) + rb1 * wy) >> 8 & 0xff00ff) | ((g0 * (256 - wy) + g1 * wy) >> 8 & 0xff00);
        }
    }
}
static void scaleinit(void) {
    ss = fminf(OW / (float)W, OH / (float)H); ox = (int)((OW - W * ss) / 2); oy = (int)((OH - H * ss) / 2);
    for (int X = 0; X < OW; X++) {
        float fx = (X - ox + .5f) / ss - .5f;
        cX[X] = -1;
        if (fx >= -.5f && fx <= W - .5f) { fx = clampf(fx, 0, W - 1.001f); cX[X] = (int)fx; cW[X] = (int)((fx - cX[X]) * 256); }
    }
}
static int events(void) {
    union { long l[24]; int i[48]; } e;
    while (xPending(dpy)) {
        xNext(dpy, &e);
        int type = e.i[0];
        if (type == 2) {
            long ks = xLook(&e, 0);
            if (ks == 0xff1b || ks == 'q') return 0;
            if (ks == 'f') fullscreen(2);
        }
        if (type == 33 && e.l[7] == delAtom) return 0;
        if (type == 22) {
            int w = e.i[14], h = e.i[15];
            if (w > 4096) w = 4096;
            if (h > 4096) h = 4096;
            if (w != OW || h != OH) { OW = w; OH = h; xout = 0; scaleinit(); }
        }
    }
    return 1;
}
static void show(void) {
    xSync(dpy, 0); /* wait for the previous frame only now: the server worked while we rendered */
    if (OW == W && OH == H) xPut(dpy, win, xgc, xim, 0, 0, 0, 0, W, H);
    else {
        if (!xout) xout = (void *)xCreateImage(dpy, xvis, xdep, 2, 0, obuf, OW, OH, 32, 0);
        par(r_scale, (OH + 7) / 8);
        xPut(dpy, win, xgc, xout, 0, 0, 0, 0, OW, OH);
    }
    XS(XFlush)(dpy);
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    int nth = (int)sysconf(_SC_NPROCESSORS_ONLN), audio;
    double t0, last = 0;
    pthread_t th;
    fb = malloc(sizeof(float) * W * H * 3); px = malloc(4 * W * H); F = malloc(sizeof(Face) * MC * 3);
    if (!xopen(!(argc > 1 && argv[1][1] == 'w'))) { say("watcher: needs a graphical (X11) display\n"); return 1; }
    for (int x = 0; x < W; x++) U[x] = (x + .5f - W * .5f) / (H * .5f);
    for (int y = 0; y < H; y++) V[y] = (H * .5f - y - .5f) / (H * .5f);
    if (nth < 1) nth = 1;
    signal(SIGINT, bye); signal(SIGTERM, bye); signal(SIGHUP, bye);
    for (double ts = now(); now() - ts < .6;) { events(); usleep(20000); } /* let the window appear */
    audio = audio_open();
    if (audio) pthread_create(&th, 0, audio_thread, 0);
    setpriority(PRIO_PROCESS, 0, 5); /* render threads yield to the audio thread */
    pthread_barrier_init(&bar0, 0, nth); pthread_barrier_init(&bar1, 0, nth);
    for (int i = 1; i < nth; i++) pthread_create(&th, 0, worker, 0);
    t0 = now();
    for (;;) {
        double t, fs = now();
        if (audio) {
            t = a_pos > -.5 ? a_pos + (fs - a_stamp) : 0;
            if (fs - t0 > 3 && a_pos < -.5) audio = 0;
        } else t = fs - t0;
        if (t < last) t = last;
        last = t;
        if (t > END || !events()) break;
        if (t >= 52.75 && t < 54) {
            /* the "crash": the window vanishes and the shell gets a familiar message */
            if (!crash) { crash = 1; XS(XUnmapWindow)(dpy, win); xSync(dpy, 0); say("Segmentation fault (core dumped)\n"); }
            usleep(5000);
            continue;
        }
        if (crash == 1) { crash = 2; say("\033[A\r\033[J"); XS(XMapRaised)(dpy, win); xSync(dpy, 0); }
        render((float)t);
        show();
        frame++;
        double sl = 1 / 60. - (now() - fs);
        if (sl > 0) usleep((useconds_t)(sl * 1e6));
    }
    bye(0);
}

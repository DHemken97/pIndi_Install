/*
 * pindi-player - PiNDI splash screen + NDI receiver, one per display output.
 *
 * Owns the HDMI outputs through DRM/KMS from early boot. For every output:
 *   - shows the splash image with the IP (bottom left) and serial (bottom right)
 *   - finds its NDI stream (output 1: OUT_<serial>, output 2: OUT_<serial>_2, ...)
 *   - displays it full screen through the hardware scaler
 *   - falls back to the splash whenever the stream is lost, and reconnects
 *
 * Threads: one NDI receiver per output; the main thread does all KMS work with
 * non-blocking atomic commits, so outputs never wait on each other's vsync.
 * Works on the Pi 3 (vc4), Pi 4 and Pi 5: the video pixel format is chosen from
 * what each output's plane supports (UYVY, else NV16, else RGB).
 *
 * Settings come from the environment (see /etc/default/pindi).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <jpeglib.h>

#include <alsa/asoundlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <dlfcn.h>
#include <linux/videodev2.h>
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#define LOG(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)

/* Text height cap: 16pt at 96 dpi. */
#define FONT_PT_PX 21
#define MAX_OUT 4
#define NBUF 8 /* per output; outputs showing its source hold some of them (decode once) */
#define RUN_DIR "/run/pindi"     /* status, previews (this program and pindi-sender write here) */
#define CTL_DIR "/run/pindi-ctl" /* live switches, Identify: written by the web interface only (root) */
#define WEB_RUN "/run/pindi-web" /* preview requests from the web front end */

static const NDIlib_v6 *ndi;
static bool hx_decode; /* the SDK can decode NDI|HX, so a sender may use either codec */
static void (*ndi_request_keyframe)(NDIlib_recv_instance_t); /* Advanced SDK only */
static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *cfg(const char *key, const char *def)
{
    const char *v = getenv(key);
    return (v && *v) ? v : def;
}

/* Per-output setting: PINDI_OUT<n>_<KEY>, and for output 1 also the older PINDI_<KEY>. */
static const char *ocfg(int n, const char *key, const char *def)
{
    char k[64];
    snprintf(k, sizeof k, "PINDI_OUT%d_%s", n, key);
    const char *v = getenv(k);
    if (v && *v)
        return v;
    if (n == 1) {
        snprintf(k, sizeof k, "PINDI_%s", key);
        v = getenv(k);
        if (v && *v)
            return v;
    }
    return def;
}

/* ------------------------------------------------------------------ device info */

static char serial[64];

static void read_serial(char *out, size_t n)
{
    FILE *f = fopen("/sys/firmware/devicetree/base/serial-number", "rb");
    size_t len = 0;
    out[0] = 0;
    if (f) {
        len = fread(out, 1, n - 1, f);
        fclose(f);
    }
    out[len] = 0;
    /* Strip NULs / whitespace. */
    size_t j = 0;
    for (size_t i = 0; i < len; i++)
        if (isalnum((unsigned char)out[i]))
            out[j++] = out[i];
    out[j] = 0;

    if (!out[0]) {
        char line[256];
        f = fopen("/proc/cpuinfo", "r");
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, "Serial : %63s", out) == 1 || sscanf(line, "Serial\t: %63s", out) == 1)
                break;
        if (f)
            fclose(f);
    }
    if (!out[0])
        snprintf(out, n, "unknown");
}

static void read_ip(char *out, size_t n)
{
    struct ifaddrs *ifa, *p;
    int best = 0;
    snprintf(out, n, "no network");
    if (getifaddrs(&ifa))
        return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if ((p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP))
            continue;
        int score = strcmp(p->ifa_name, "eth0") == 0 ? 2 : 1;
        if (score > best) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, out, n);
            best = score;
        }
    }
    freeifaddrs(ifa);
}

static void write_file_atomic(const char *path, const char *data)
{
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fputs(data, f);
    fclose(f);
    rename(tmp, path);
}

/* ------------------------------------------------------------------ splash image */

struct image { int w, h; uint32_t *px; }; /* XRGB8888 */

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jpeg_fail(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

static bool load_jpeg(const char *path, struct image *im)
{
    struct jpeg_decompress_struct cinfo;
    struct jerr err;
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("splash: cannot open %s: %s", path, strerror(errno));
        return false;
    }
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_fail;
    if (setjmp(err.jb)) {
        LOG("splash: cannot decode %s", path);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        free(im->px);
        im->px = NULL;
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_BGRX; /* == little-endian XRGB8888 */
    jpeg_start_decompress(&cinfo);
    im->w = cinfo.output_width;
    im->h = cinfo.output_height;
    im->px = im->w <= 16384 && im->h <= 16384 ? malloc((size_t)im->w * im->h * 4) : NULL;
    if (!im->px) {
        LOG("splash: %s is too big (%dx%d)", path, im->w, im->h);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return false;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = (JSAMPROW)(im->px + (size_t)cinfo.output_scanline * im->w);
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return true;
}

/* Background colour: average of 8x8 patches 5% in from each corner (edges can have artefacts). */
static uint32_t image_bg(const struct image *im)
{
    unsigned sum[3] = { 0 }, n = 0;
    int ix = im->w / 20, iy = im->h / 20;
    int xs[2] = { ix, im->w - ix - 8 }, ys[2] = { iy, im->h - iy - 8 };
    for (int c = 0; c < 4; c++)
        for (int y = ys[c / 2]; y < ys[c / 2] + 8; y++)
            for (int x = xs[c % 2]; x < xs[c % 2] + 8; x++) {
                if (x < 0 || y < 0 || x >= im->w || y >= im->h)
                    continue;
                uint32_t p = im->px[(size_t)y * im->w + x];
                sum[0] += p & 255;
                sum[1] += (p >> 8) & 255;
                sum[2] += (p >> 16) & 255;
                n++;
            }
    if (!n)
        return im->px[0];
    return (sum[0] / n) | (sum[1] / n) << 8 | (sum[2] / n) << 16;
}

static bool near_bg(uint32_t p, uint32_t bg)
{
    for (int sh = 0; sh < 24; sh += 8)
        if (abs((int)((p >> sh) & 255) - (int)((bg >> sh) & 255)) > 12)
            return false;
    return true;
}

/* True if most of a row/column (stepping by step) differs from the background. */
static bool edge_is_line(const struct image *im, size_t first, size_t step, int n, uint32_t bg)
{
    int off = 0;
    for (int i = 0; i < n; i++)
        off += !near_bg(im->px[first + i * step], bg);
    return off > n / 2;
}

/* Crop up to 4 px of border lines (e.g. a light frame) that don't match the background. */
static void trim_edges(struct image *im, uint32_t bg)
{
    int top = 0, bot = 0, left = 0, right = 0;
    while (top < 4 && edge_is_line(im, (size_t)top * im->w, 1, im->w, bg)) top++;
    while (bot < 4 && edge_is_line(im, (size_t)(im->h - 1 - bot) * im->w, 1, im->w, bg)) bot++;
    while (left < 4 && edge_is_line(im, left, im->w, im->h, bg)) left++;
    while (right < 4 && edge_is_line(im, im->w - 1 - right, im->w, im->h, bg)) right++;
    if (!(top | bot | left | right))
        return;
    int nw = im->w - left - right, nh = im->h - top - bot;
    for (int y = 0; y < nh; y++)
        memmove(im->px + (size_t)y * nw, im->px + (size_t)(y + top) * im->w + left, (size_t)nw * 4);
    im->w = nw;
    im->h = nh;
}

/* Scale the image to fit (keeping aspect), centred on its own background colour. */
static void render_fit(uint32_t *dst, int W, int H, const struct image *im)
{
    if (!im->px) {
        memset(dst, 0, (size_t)W * H * 4);
        return;
    }
    uint32_t bg = image_bg(im);
    for (size_t i = 0; i < (size_t)W * H; i++)
        dst[i] = bg;

    double s = fmin((double)W / im->w, (double)H / im->h);
    int dw = (int)(im->w * s + 0.5), dh = (int)(im->h * s + 0.5);
    int ox = (W - dw) / 2, oy = (H - dh) / 2;
    int *x0 = malloc(dw * sizeof(int)), *x1 = malloc(dw * sizeof(int)), *wx = malloc(dw * sizeof(int));

    for (int x = 0; x < dw; x++) {
        double fx = (x + 0.5) / s - 0.5;
        if (fx < 0) fx = 0;
        int i0 = (int)fx;
        if (i0 > im->w - 1) i0 = im->w - 1;
        x0[x] = i0;
        x1[x] = i0 + 1 < im->w ? i0 + 1 : i0;
        wx[x] = (int)((fx - i0) * 256);
    }
    for (int y = 0; y < dh; y++) {
        double fy = (y + 0.5) / s - 0.5;
        if (fy < 0) fy = 0;
        int y0 = (int)fy;
        if (y0 > im->h - 1) y0 = im->h - 1;
        int y1 = y0 + 1 < im->h ? y0 + 1 : y0;
        int wy = (int)((fy - y0) * 256);
        const uint32_t *r0 = im->px + (size_t)y0 * im->w, *r1 = im->px + (size_t)y1 * im->w;
        uint32_t *d = dst + (size_t)(oy + y) * W + ox;
        for (int x = 0; x < dw; x++) {
            uint32_t a = r0[x0[x]], b = r0[x1[x]], c = r1[x0[x]], e = r1[x1[x]];
            uint32_t out = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                int top = (((a >> sh) & 255) * (256 - wx[x]) + ((b >> sh) & 255) * wx[x]) >> 8;
                int bot = (((c >> sh) & 255) * (256 - wx[x]) + ((e >> sh) & 255) * wx[x]) >> 8;
                out |= (uint32_t)((top * (256 - wy) + bot * wy) >> 8) << sh;
            }
            d[x] = out;
        }
    }
    free(x0);
    free(x1);
    free(wx);
}

/* ------------------------------------------------------------------ text (main thread only) */

static FT_Library ft;
static FT_Face face;
static bool font_ok;
static bool splash_info = true; /* PINDI_SPLASH_INFO=0: plain splash, no IP/serial/status */

/* Next code point of UTF-8 text (stream and device names can be anything); bad bytes as U+FFFD. */
static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    int extra = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && !extra) {
        *ps = (const char *)s;
        return 0xFFFD;
    }
    c &= extra ? 0x3F >> extra : 0x7F;
    for (int i = 0; i < extra; i++, s++) {
        if ((*s & 0xC0) != 0x80) {
            *ps = (const char *)s;
            return 0xFFFD;
        }
        c = c << 6 | (*s & 0x3F);
    }
    *ps = (const char *)s;
    return c;
}

static int text_width(const char *s)
{
    int w = 0;
    while (*s)
        if (!FT_Load_Char(face, utf8_next(&s), FT_LOAD_DEFAULT))
            w += face->glyph->advance.x >> 6;
    return w;
}

static void draw_text(uint32_t *px, int W, int H, int x, int base, const char *s, uint32_t rgb)
{
    while (*s) {
        if (FT_Load_Char(face, utf8_next(&s), FT_LOAD_RENDER))
            continue;
        FT_GlyphSlot g = face->glyph;
        int gx = x + g->bitmap_left, gy = base - g->bitmap_top;
        for (int r = 0; r < (int)g->bitmap.rows; r++) {
            int y = gy + r;
            if (y < 0 || y >= H)
                continue;
            const uint8_t *row = g->bitmap.buffer + r * g->bitmap.pitch;
            for (int c = 0; c < (int)g->bitmap.width; c++) {
                int xx = gx + c;
                unsigned a = row[c];
                if (!a || xx < 0 || xx >= W)
                    continue;
                uint32_t *d = &px[(size_t)y * W + xx], o = 0;
                for (int sh = 0; sh < 24; sh += 8)
                    o |= ((((*d >> sh) & 255) * (255 - a) + ((rgb >> sh) & 255) * a) / 255) << sh;
                *d = o;
            }
        }
        x += g->advance.x >> 6;
    }
}

/* ------------------------------------------------------------------ buffers */

static int drm_fd = -1;
static int wake_pipe[2] = { -1, -1 };

struct buf {
    uint32_t fb, handle, pitch, fmt;
    int w, h;
    uint64_t size, uv_off; /* uv_off: CbCr plane offset for NV16 / NV12 */
    uint8_t *map;
};

/* XRGB8888 / XBGR8888 / UYVY (one plane) or NV16 / NV12 (Y plane then CbCr plane, one allocation).
 * NV12 is what the hardware H.264 decoder writes: its planes are 16 lines aligned. */
static int nv12_rows(int h) { return (h + 15) & ~15; }

static bool buf_create(int w, int h, uint32_t fmt, struct buf *b)
{
    bool nv16 = fmt == DRM_FORMAT_NV16, nv12 = fmt == DRM_FORMAT_NV12;
    int bpp = nv16 || nv12 ? 8 : fmt == DRM_FORMAT_UYVY ? 16 : 32;
    struct drm_mode_create_dumb c = { .width = w, .height = nv16 ? h * 2 : nv12 ? nv12_rows(h) * 3 / 2 : h,
                                      .bpp = bpp };
    struct drm_mode_map_dumb m = { 0 };
    memset(b, 0, sizeof *b);
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) {
        LOG("display: cannot allocate %dx%d buffer: %s", w, h, strerror(errno));
        return false;
    }
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    b->w = w;
    b->h = h;
    b->fmt = fmt;
    b->uv_off = nv16 ? (uint64_t)c.pitch * h : nv12 ? (uint64_t)c.pitch * nv12_rows(h) : 0;
    uint32_t handles[4] = { c.handle, nv16 || nv12 ? c.handle : 0 };
    uint32_t pitches[4] = { c.pitch, nv16 || nv12 ? c.pitch : 0 };
    uint32_t offsets[4] = { 0, (uint32_t)b->uv_off };
    if (drmModeAddFB2(drm_fd, w, h, fmt, handles, pitches, offsets, &b->fb, 0))
        goto fail;
    m.handle = c.handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
        goto fail;
    b->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, m.offset);
    if (b->map == MAP_FAILED) {
        b->map = NULL;
        goto fail;
    }
    return true;
fail:
    LOG("display: cannot create %dx%d buffer: %s", w, h, strerror(errno));
    if (b->fb)
        drmModeRmFB(drm_fd, b->fb);
    struct drm_mode_destroy_dumb d = { .handle = c.handle };
    drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    memset(b, 0, sizeof *b);
    return false;
}

static void buf_destroy(struct buf *b)
{
    if (!b->handle)
        return;
    if (b->map)
        munmap(b->map, b->size);
    if (b->fb)
        drmModeRmFB(drm_fd, b->fb);
    struct drm_mode_destroy_dumb d = { .handle = b->handle };
    drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    memset(b, 0, sizeof *b);
}

/* The Pi 3/4 HVS has no packed 4:2:2 format, so split UYVY into NV16 (Y plane + CbCr plane). */
static void uyvy_to_nv16(const uint8_t *s, uint8_t *y, uint8_t *uv, int w)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; x + 32 <= w; x += 32) {
        uint8x16x4_t p = vld4q_u8(s + x * 2); /* U Y0 V Y1 */
        uint8x16x2_t luma = { { p.val[1], p.val[3] } };
        uint8x16x2_t chroma = { { p.val[0], p.val[2] } };
        vst2q_u8(y + x, luma);
        vst2q_u8(uv + x, chroma);
    }
#endif
    for (; x + 2 <= w; x += 2) {
        const uint8_t *p = s + x * 2;
        uv[x] = p[0];
        y[x] = p[1];
        uv[x + 1] = p[2];
        y[x + 1] = p[3];
    }
}

/* ------------------------------------------------------------------ outputs */

enum { B_NONE = -1, B_FREE, B_WRITING, B_READY, B_QUEUED, B_SCANOUT };
enum { P_NONE, P_SPLASH, P_VIDEO, P_EXT };

struct output {
    int n; /* 1-based, as shown in the web UI */
    char conn_name[32];
    uint32_t conn_id, crtc_id, plane_id;
    bool has_uyvy, has_nv16, has_nv12;
    struct { uint32_t fb, crtc, sx, sy, sw, sh, cx, cy, cw, ch, rot, enc; } pp;
    uint64_t enc_601, enc_709; /* COLOR_ENCODING values (pp.enc 0: not supported) */
    uint32_t p_mode, p_active, p_conn_crtc;

    /* Main thread only. */
    bool connected, active;
    drmModeModeInfo mode;
    uint32_t mode_blob;
    int W, H;
    struct buf splash;
    uint32_t *base, *comp;
    char shown_ip[64], shown_msg[192];
    bool identify, restore_video; /* web UI "Identify": big text on the splash, video held back */
    char ident[256];              /* its lines, '\n' separated */
    int last_video;               /* buffer last on screen, to put back after identifying */
    int pending; /* P_*: a commit is in flight */
    uint64_t rotation;
    bool rot_logged, commit_logged, mode_failed;
    uint64_t enable_retry_at;
    uint64_t splash_since; /* splash on screen since (0 = video) */
    bool standby;          /* display switched off after standby_min without video */

    /* Shared with the receiver thread (lock). */
    pthread_mutex_t lock;
    bool usable; /* display up: the receiver may post frames */
    struct buf vid[NBUF];
    int vstate[NBUF];
    double vaspect[NBUF];
    int64_t vsent[NBUF];  /* when the frame was captured (or sent), 100 ns UTC, or INT64_MAX: frame_pts() */
    uint64_t vrecv[NBUF]; /* when this Pi got the frame from NDI, monotonic us */
    int pw, ph;
    uint32_t pfmt; /* format of the current buffer pool */
    int ww, wh;
    uint32_t wfmt; /* format the receiver needs */
    bool want_realloc, want_splash, on_splash;
    int ready, ready2, queued, scanout; /* ready, ready2: the next frames to show, oldest first */
    int backlog;          /* refreshes in a row that found two frames waiting (lock) */
    char splash_msg[192]; /* what the receiver is doing, shown on the splash */
    bool thumb_reading;   /* the preview thread is reading a buffer: don't free any */
    bool cant_display;    /* no buffers for the current frame size (pw x ph) */
    bool wake_request;    /* receiver: our stream is there, turn the display back on */
    const char *probe_codec; /* codec_name() the format check found for probe_src (lock) */
    char probe_src[256];
    bool probing;            /* a format check is running (lock) */
    uint64_t probe_ms;       /* when the last one started (lock) */

    /* Configuration. */
    bool disabled; /* PINDI_OUT<n>_ENABLED=0: no NDI, HDMI signal off (e.g. encoder-only units) */
    char target[160], alt[160], source_ip[64];
    char backup[160]; /* PINDI_OUT<n>_BACKUP_STREAM: played while the main stream is unavailable */
    uint64_t source_stale; /* out<n>.source's time stamp at start: a switch from before then is ignored */
    const char *rot_s, *mirror_s;
    const char *audio_s; /* PINDI_OUT<n>_AUDIO: hdmi, analog or off */
    int audio_delay_ms;  /* PINDI_OUT<n>_AUDIO_DELAY_MS: audio buffer = lip-sync delay */
    bool pacing;         /* PINDI_OUT<n>_PACING=smooth (experimental): frames on screen at an even cadence */
    int standby_min;     /* PINDI_OUT<n>_STANDBY_MIN: display off after this long without video (0 = never) */
    bool cec;            /* PINDI_OUT<n>_CEC=1: also put the TV in standby / wake it over HDMI-CEC */
    int brightness;      /* PINDI_OUT<n>_BRIGHTNESS: -50..50, 0 = unchanged (the display's gamma table) */

    /* Audio playback (receiver thread only). */
    struct {
        snd_pcm_t *pcm;
        int rate;
        snd_pcm_uframes_t target, slack;
        uint64_t retry_at, last_frame;
        int16_t *buf;
        size_t cap;
        char info[160];
        uint64_t in, dropped, xruns, frames; /* samples received / not played, underruns, NDI frames */
        uint64_t max_gap, win_start;            /* longest wait between NDI audio frames (ms) */
        int64_t last_sent;                      /* the previous frame's NDI timestamp (100 ns UTC) */
        int extra_ms;                           /* added after underruns (jittery sender), kept per source */
        int logged_rate;
        snd_pcm_uframes_t win_min, pending_drop; /* lowest queue level in this 2 s window */
    } au;

    /* Latency of frames that reached the screen (main thread), published every 5 s. */
    struct { double pi_sum, pi_min, pi_max, tot_sum, tot_min, tot_max; int n, tot_n; uint64_t since; } lat;
    /* Frame pacing (o->lock): every frame goes on screen at its capture time + one delay, on the refresh
     * that time falls before, so a 30 fps source on 60 Hz shows each frame for exactly two refreshes
     * whatever the network and decoder jitter. The delay covers the slowest recent capture-to-ready time
     * and puts the due times mid-refresh, away from the boundary where jitter would flip frames between
     * neighbouring refreshes. It changes only when a frame was too slow (later), when a whole refresh is
     * to spare (one refresh earlier: same position) and when the source's and the display's clocks have
     * drifted the due times to a boundary (back to the middle: one planned 1- or 3-refresh frame). */
    struct {
        uint64_t vbl_us;  /* a recent vblank (monotonic us), from flips and vblank events */
        bool vbl_wait;    /* a vblank event is requested (main thread) */
        bool on;          /* timing known */
        bool center;      /* put the due times mid-refresh at the next chance */
        double lag_peak;  /* us: slowest recent capture -> ready time, decaying */
        double delay;     /* us from capture to on screen */
        double dev;       /* smoothed position of due times within a refresh, -0.5 .. 0.5 (0 = middle) */
        double last_pts;  /* monotonic us */
        uint64_t last_us;
        uint32_t held, late, moves; /* refreshes a frame waited for its time, frames shown late, delay moves */
        uint32_t full;                /* frames dropped because two were already waiting */
        double gap_min, gap_max;      /* us between consecutive capture times */
        double lag_min, lag_max;      /* us capture -> ready */
    } pace;
    /* How many refreshes each video frame stayed on screen (cadence; [4] = 4 or more), main thread. */
    struct { uint64_t shown_us, log_since; uint32_t hist[5], log_hist[5]; } cad;

    /* Decode once (share_mu): outputs showing the same source use one receiver, the leader's, and put
     * its decoded buffers on their own planes - no copy; rotation, mirror, size and brightness stay
     * per output. vuse[b]: how many other outputs have this output's buffer b ready, queued or on
     * screen (it is not written to until 0). ext_*: a leader's buffer this output shows, and whose. */
    struct output *leader;  /* following this output's receiver, NULL = none */
    char share_src[256];    /* leader: the source its receiver is connected to, "" = not leading */
    char share_url[128], share_video[64];
    int share_bw;
    bool share_flush;       /* the owner reallocates its buffers: get its buffers off screen */
    int vuse[NBUF];
    int ext_ready, ext_ready2, ext_queued, ext_scanout;
    struct output *ext_ready_of, *ext_ready2_of, *ext_queued_of, *ext_scanout_of;

    /* Receiver thread. */
    pthread_t thr;
    bool thr_started;
    struct { char state[32], source[256], url[128], video[64], target[160]; } st;
};

static struct output outs[MAX_OUT];
static int nout;
static pthread_mutex_t share_mu = PTHREAD_MUTEX_INITIALIZER; /* taken after an output's lock, never before */
static struct image splash_img;

static void wake(void)
{
    char c = 1;
    if (write(wake_pipe[1], &c, 1) < 0) { /* pipe full: main thread is awake anyway */ }
}

/* share_mu held: o no longer uses buffer idx of owner. */
static void ext_release(struct output *owner, int idx)
{
    if (owner && idx >= 0 && owner->vuse[idx] > 0)
        owner->vuse[idx]--;
}

/* Two frames waiting at this many refreshes in a row is not a burst but a standing backlog (a sender
 * at exactly the display rate never drains it): one frame is skipped, saving a refresh of delay. */
#define BACKLOG_DROP 30

/* o->lock held: frame idx is complete. Up to two frames wait to be shown, so a frame that arrives
 * right after another (decoders work in bursts) waits one refresh instead of replacing it; with
 * both waiting, the oldest is dropped (at most one refresh of delay). */
static void ready_push(struct output *o, int idx)
{
    if (o->ready < 0) {
        o->ready = idx;
    } else if (o->ready2 < 0) {
        o->ready2 = idx;
    } else {
        o->vstate[o->ready] = B_FREE;
        o->ready = o->ready2;
        o->ready2 = idx;
        o->pace.full++;
    }
    o->vstate[idx] = B_READY;
}

/* o->lock held: drop the frames waiting to be shown. */
static void ready_clear(struct output *o)
{
    if (o->ready >= 0)
        o->vstate[o->ready] = B_FREE;
    if (o->ready2 >= 0)
        o->vstate[o->ready2] = B_FREE;
    o->ready = o->ready2 = -1;
}

/* share_mu held: the same for an output showing a leader's frames. */
static void ext_push(struct output *f, struct output *L, int idx)
{
    if (f->ext_ready >= 0 && f->ext_ready2 >= 0) {
        ext_release(f->ext_ready_of, f->ext_ready);
        f->ext_ready = f->ext_ready2;
        f->ext_ready_of = f->ext_ready2_of;
        f->ext_ready2 = -1;
        f->ext_ready2_of = NULL;
    }
    if (f->ext_ready < 0) {
        f->ext_ready = idx;
        f->ext_ready_of = L;
    } else {
        f->ext_ready2 = idx;
        f->ext_ready2_of = L;
    }
    L->vuse[idx]++;
}

static void ext_clear_ready(struct output *f)
{
    ext_release(f->ext_ready_of, f->ext_ready);
    ext_release(f->ext_ready2_of, f->ext_ready2);
    f->ext_ready = f->ext_ready2 = -1;
    f->ext_ready_of = f->ext_ready2_of = NULL;
}

/* Leader thread: buffer idx of L is its newest frame - make it the next one on its followers too. */
static void share_publish(struct output *L, int idx)
{
    bool any = false;
    pthread_mutex_lock(&L->lock);
    pthread_mutex_lock(&share_mu);
    int st = L->vstate[idx];
    if (st == B_READY || st == B_QUEUED || st == B_SCANOUT)
        for (int i = 0; i < nout; i++) {
            struct output *f = &outs[i];
            if (f->leader != L)
                continue;
            ext_push(f, L, idx);
            any = true;
        }
    pthread_mutex_unlock(&share_mu);
    pthread_mutex_unlock(&L->lock);
    if (any)
        wake();
}

/* share_mu held: o stops leading; its followers search on their own. */
static void share_drop_followers(struct output *o)
{
    o->share_src[0] = 0;
    for (int i = 0; i < nout; i++)
        if (outs[i].leader == o) {
            outs[i].leader = NULL;
            ext_clear_ready(&outs[i]);
        }
}

/* Receiver thread: o's receiver is gone. */
static void share_release(struct output *o)
{
    pthread_mutex_lock(&share_mu);
    share_drop_followers(o);
    pthread_mutex_unlock(&share_mu);
}

/* Receiver thread: stop following. */
static void share_leave(struct output *o)
{
    pthread_mutex_lock(&share_mu);
    o->leader = NULL;
    ext_clear_ready(o);
    pthread_mutex_unlock(&share_mu);
}

/* Receiver thread: about to connect to source name: the output already receiving it (then follow
 * it), or NULL after claiming the source for o (then connect). */
static struct output *share_claim(struct output *o, const char *name, const char *url, int bw)
{
    struct output *L = NULL;
    pthread_mutex_lock(&share_mu);
    share_drop_followers(o); /* they followed what o received before */
    for (int i = 0; i < nout && !L; i++)
        if (&outs[i] != o && outs[i].share_bw == bw && !strcmp(outs[i].share_src, name) &&
            outs[i].has_uyvy == o->has_uyvy && outs[i].has_nv16 == o->has_nv16 && outs[i].has_nv12 == o->has_nv12)
            L = &outs[i];
    if (L) {
        o->leader = L;
    } else {
        snprintf(o->share_src, sizeof o->share_src, "%s", name);
        snprintf(o->share_url, sizeof o->share_url, "%s", url);
        o->share_video[0] = 0;
        o->share_bw = bw;
    }
    pthread_mutex_unlock(&share_mu);
    return L;
}

/* Main thread: o's buffers go away (display off, reallocation): forget them wherever they are shown,
 * and give back the other outputs' buffers o itself held. */
static void share_forget(struct output *o)
{
    pthread_mutex_lock(&share_mu);
    ext_clear_ready(o);
    ext_release(o->ext_queued_of, o->ext_queued);
    ext_release(o->ext_scanout_of, o->ext_scanout);
    o->ext_queued = o->ext_scanout = -1;
    o->ext_queued_of = o->ext_scanout_of = NULL;
    for (int i = 0; i < nout; i++) {
        struct output *f = &outs[i];
        if (f->ext_ready_of == o)
            f->ext_ready = -1, f->ext_ready_of = NULL;
        if (f->ext_ready2_of == o)
            f->ext_ready2 = -1, f->ext_ready2_of = NULL;
        if (f->ext_ready < 0 && f->ext_ready2 >= 0) {
            f->ext_ready = f->ext_ready2, f->ext_ready_of = f->ext_ready2_of;
            f->ext_ready2 = -1, f->ext_ready2_of = NULL;
        }
        if (f->ext_queued_of == o)
            f->ext_queued = -1, f->ext_queued_of = NULL;
        if (f->ext_scanout_of == o)
            f->ext_scanout = -1, f->ext_scanout_of = NULL;
    }
    memset(o->vuse, 0, sizeof o->vuse);
    o->share_flush = false;
    pthread_mutex_unlock(&share_mu);
}

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
    uint32_t id = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, obj, type);
    for (uint32_t k = 0; props && k < props->count_props && !id; k++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[k]);
        if (p && !strcmp(p->name, name))
            id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

/* The value of enum property name's entry val_name (0 if there is none). */
static uint64_t prop_enum(uint32_t obj, uint32_t type, const char *name, const char *val_name)
{
    uint64_t val = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, obj, type);
    for (uint32_t k = 0; props && k < props->count_props; k++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[k]);
        if (p && !strcmp(p->name, name))
            for (int e = 0; e < p->count_enums; e++)
                if (!strcmp(p->enums[e].name, val_name))
                    val = p->enums[e].value;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return val;
}

static uint64_t prop_value(uint32_t obj, uint32_t type, const char *name)
{
    uint64_t val = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, obj, type);
    for (uint32_t k = 0; props && k < props->count_props; k++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, props->props[k]);
        if (p && !strcmp(p->name, name))
            val = props->prop_values[k];
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return val;
}

/* Open the KMS device. Only vc4 for the first 10 s, then accept any KMS driver.
 * (Pi 4/5: card0 is the 3D GPU, the display controller is a separate card.) */
static bool display_open(bool any_driver)
{
    for (int i = 0; i < 8; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmVersion *v = drmGetVersion(fd);
        bool want = v && (any_driver || strcmp(v->name, "vc4") == 0);
        drmModeRes *res = want ? drmModeGetResources(fd) : NULL;
        if (res && res->count_connectors > 0 && res->count_crtcs > 0 &&
            !drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) &&
            !drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
            LOG("display: using %s (%s)", path, v->name);
            drmModeFreeResources(res);
            drmFreeVersion(v);
            drm_fd = fd;
            return true;
        }
        if (res)
            drmModeFreeResources(res);
        if (v)
            drmFreeVersion(v);
        close(fd);
    }
    return false;
}

static int conn_cmp(const void *a, const void *b)
{
    return strcmp(((const struct output *)a)->conn_name, ((const struct output *)b)->conn_name);
}

/* Find every display connector and give each its own CRTC and primary plane. */
static void discover_outputs(void)
{
    drmModeRes *res = drmModeGetResources(drm_fd);
    drmModePlaneRes *pr = drmModeGetPlaneResources(drm_fd);
    uint32_t used_crtcs = 0;
    if (!res || !pr)
        goto out;

    for (int i = 0; i < res->count_connectors && nout < MAX_OUT; i++) {
        drmModeConnector *c = drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK || c->connector_type == DRM_MODE_CONNECTOR_VIRTUAL) {
            drmModeFreeConnector(c);
            continue;
        }
        struct output *o = &outs[nout];
        memset(o, 0, sizeof *o);
        const char *tn = drmModeGetConnectorTypeName(c->connector_type);
        snprintf(o->conn_name, sizeof o->conn_name, "%s-%u", tn ? tn : "Unknown", c->connector_type_id);
        o->conn_id = c->connector_id;

        /* CRTC: the one already driving it if any, else the first free compatible one. */
        uint32_t possible = 0;
        for (int e = 0; e < c->count_encoders; e++) {
            drmModeEncoder *enc = drmModeGetEncoder(drm_fd, c->encoders[e]);
            if (enc)
                possible |= enc->possible_crtcs;
            drmModeFreeEncoder(enc);
        }
        int crtc_idx = -1;
        for (int k = 0; k < res->count_crtcs && crtc_idx < 0; k++)
            if ((possible & (1u << k)) && !(used_crtcs & (1u << k)))
                crtc_idx = k;
        drmModeFreeConnector(c);
        if (crtc_idx < 0)
            continue;

        /* Primary plane for that CRTC, and what it can scan out. */
        for (uint32_t p = 0; p < pr->count_planes && !o->plane_id; p++) {
            drmModePlane *pl = drmModeGetPlane(drm_fd, pr->planes[p]);
            if (pl && (pl->possible_crtcs & (1u << crtc_idx)) &&
                prop_value(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type") == DRM_PLANE_TYPE_PRIMARY) {
                o->plane_id = pl->plane_id;
                for (uint32_t f = 0; f < pl->count_formats; f++) {
                    o->has_uyvy |= pl->formats[f] == DRM_FORMAT_UYVY;
                    o->has_nv16 |= pl->formats[f] == DRM_FORMAT_NV16;
                    o->has_nv12 |= pl->formats[f] == DRM_FORMAT_NV12;
                }
            }
            drmModeFreePlane(pl);
        }
        if (!o->plane_id)
            continue;

        used_crtcs |= 1u << crtc_idx;
        o->crtc_id = res->crtcs[crtc_idx];
        uint32_t pl = o->plane_id;
        o->pp.fb = prop_id(pl, DRM_MODE_OBJECT_PLANE, "FB_ID");
        o->pp.crtc = prop_id(pl, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        o->pp.sx = prop_id(pl, DRM_MODE_OBJECT_PLANE, "SRC_X");
        o->pp.sy = prop_id(pl, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        o->pp.sw = prop_id(pl, DRM_MODE_OBJECT_PLANE, "SRC_W");
        o->pp.sh = prop_id(pl, DRM_MODE_OBJECT_PLANE, "SRC_H");
        o->pp.cx = prop_id(pl, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        o->pp.cy = prop_id(pl, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        o->pp.cw = prop_id(pl, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        o->pp.ch = prop_id(pl, DRM_MODE_OBJECT_PLANE, "CRTC_H");
        o->pp.rot = prop_id(pl, DRM_MODE_OBJECT_PLANE, "rotation");
        o->pp.enc = prop_id(pl, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING");
        o->enc_601 = prop_enum(pl, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING", "ITU-R BT.601 YCbCr");
        o->enc_709 = prop_enum(pl, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING", "ITU-R BT.709 YCbCr");
        o->p_mode = prop_id(o->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        o->p_active = prop_id(o->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        o->p_conn_crtc = prop_id(o->conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        nout++;
    }
    qsort(outs, nout, sizeof outs[0], conn_cmp);
    for (int i = 0; i < nout; i++) {
        struct output *o = &outs[i];
        o->n = i + 1;
        o->ext_ready = o->ext_ready2 = o->ext_queued = o->ext_scanout = -1;
        pthread_mutex_init(&o->lock, NULL);
        o->ready = o->ready2 = o->queued = o->scanout = o->last_video = -1;
        for (int b = 0; b < NBUF; b++)
            o->vstate[b] = B_NONE;
        LOG("display: output %d = %s (video as %s)", o->n, o->conn_name,
            o->has_uyvy ? "UYVY" : o->has_nv16 ? "NV16" : "RGB");
    }
out:
    drmModeFreePlaneResources(pr);
    drmModeFreeResources(res);
}

/* PINDI_OUT<n>_ROTATE (0/180) and _MIRROR (none/horizontal/vertical). The HVS can't do 90/270. */
static uint64_t rotation_value(struct output *o)
{
    uint64_t v = !strcmp(o->rot_s, "180") ? DRM_MODE_ROTATE_180 : DRM_MODE_ROTATE_0;
    if (!strcasecmp(o->mirror_s, "horizontal"))
        v |= DRM_MODE_REFLECT_X;
    else if (!strcasecmp(o->mirror_s, "vertical"))
        v |= DRM_MODE_REFLECT_Y;
    return v;
}

/* Brightness through the display's gamma table: free (the HVS applies it to every pixel) and black
 * stays black. +50 lifts the mid-tones like a gamma of 2, -50 darkens them the same amount. Always
 * set, so going back to 0 undoes an earlier setting. */
static void set_brightness(struct output *o)
{
    drmModeCrtc *c = drmModeGetCrtc(drm_fd, o->crtc_id);
    int size = c ? c->gamma_size : 0;
    drmModeFreeCrtc(c);
    if (size < 2) {
        if (o->brightness)
            LOG("display: output %d: no gamma table here, brightness ignored", o->n);
        return;
    }
    uint16_t *lut = malloc(sizeof *lut * size);
    if (!lut)
        return;
    double inv = pow(2.0, -o->brightness / 50.0);
    for (int i = 0; i < size; i++)
        lut[i] = (uint16_t)lround(pow((double)i / (size - 1), inv) * 65535.0);
    if (drmModeCrtcSetGamma(drm_fd, o->crtc_id, size, lut, lut, lut))
        LOG("display: output %d: brightness: %s", o->n, strerror(errno));
    else if (o->brightness)
        LOG("display: output %d: brightness %+d", o->n, o->brightness);
    free(lut);
}

static void add_plane(drmModeAtomicReq *req, struct output *o, struct buf *b, int dx, int dy, int dw, int dh)
{
    uint32_t pl = o->plane_id;
    drmModeAtomicAddProperty(req, pl, o->pp.fb, b ? b->fb : 0);
    drmModeAtomicAddProperty(req, pl, o->pp.crtc, b ? o->crtc_id : 0);
    drmModeAtomicAddProperty(req, pl, o->pp.sx, 0);
    drmModeAtomicAddProperty(req, pl, o->pp.sy, 0);
    drmModeAtomicAddProperty(req, pl, o->pp.sw, b ? (uint64_t)b->w << 16 : 0);
    drmModeAtomicAddProperty(req, pl, o->pp.sh, b ? (uint64_t)b->h << 16 : 0);
    drmModeAtomicAddProperty(req, pl, o->pp.cx, dx);
    drmModeAtomicAddProperty(req, pl, o->pp.cy, dy);
    drmModeAtomicAddProperty(req, pl, o->pp.cw, dw);
    drmModeAtomicAddProperty(req, pl, o->pp.ch, dh);
    if (o->pp.rot && b)
        drmModeAtomicAddProperty(req, pl, o->pp.rot, o->rotation);
    /* YCbCr: HD video (NDI, H.264) uses the BT.709 colours, SD BT.601; the plane's default is 601. */
    if (o->pp.enc && b && (b->fmt == DRM_FORMAT_UYVY || b->fmt == DRM_FORMAT_NV16 || b->fmt == DRM_FORMAT_NV12))
        drmModeAtomicAddProperty(req, pl, o->pp.enc, b->h >= 720 ? o->enc_709 : o->enc_601);
}

/* Show buffer b on output o, letterboxed to aspect ar (0 = fill). Non-blocking, page-flip event. */
static bool commit_buf(struct output *o, struct buf *b, double ar, int kind)
{
    int dw = o->W, dh = o->H;
    if (ar > 0) {
        dh = (int)(o->W / ar + 0.5);
        if (dh > o->H) {
            dh = o->H;
            dw = (int)(o->H * ar + 0.5);
        }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        add_plane(req, o, b, (o->W - dw) / 2, (o->H - dh) / 2, dw, dh);
        int r = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, o);
        drmModeAtomicFree(req);
        if (!r) {
            o->pending = kind;
            return true;
        }
        if (errno == EINVAL && o->rotation != DRM_MODE_ROTATE_0) {
            if (!o->rot_logged)
                LOG("display: output %d: rotate %s / mirror %s not supported here, ignoring",
                    o->n, o->rot_s, o->mirror_s);
            o->rot_logged = true;
            o->rotation = DRM_MODE_ROTATE_0;
            continue;
        }
        if (errno != EBUSY && !o->commit_logged) {
            LOG("display: output %d: commit failed: %s", o->n, strerror(errno));
            o->commit_logged = true;
        }
        return false;
    }
    return false;
}

/* The monitor's modes for the web UI, one "WxH@R" per line (preferred first, marked "*"), no duplicates. */
static void modes_publish(struct output *o, drmModeConnector *c)
{
    char buf[4096] = "\n", m[32], ms[32], path[64]; /* leading newline: entries are "\n...\n" for lookups */
    size_t len = 1;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < c->count_modes; i++) {
            drmModeModeInfo *mi = &c->modes[i];
            if ((mi->flags & DRM_MODE_FLAG_INTERLACE) || (!(mi->type & DRM_MODE_TYPE_PREFERRED)) != pass)
                continue;
            snprintf(m, sizeof m, "\n%dx%d@%d\n", mi->hdisplay, mi->vdisplay, mi->vrefresh);
            snprintf(ms, sizeof ms, "\n%dx%d@%d*\n", mi->hdisplay, mi->vdisplay, mi->vrefresh);
            if (!strstr(buf, m) && !strstr(buf, ms) && len + strlen(ms) < sizeof buf)
                len += snprintf(buf + len, sizeof buf - len, "%s", (pass ? m : ms) + 1);
        }
    snprintf(path, sizeof path, RUN_DIR "/out%d.modes", o->n);
    write_file_atomic(path, buf + 1);
}

static bool pick_mode(struct output *o, drmModeConnector *c, drmModeModeInfo *out)
{
    int w = 0, h = 0, r = 0;
    const char *want = o->mode_failed ? "" : ocfg(o->n, "MODE", "");
    if (*want && sscanf(want, "%dx%d@%d", &w, &h, &r) >= 2) {
        for (int i = 0; i < c->count_modes; i++) {
            drmModeModeInfo *m = &c->modes[i];
            if (m->hdisplay == w && m->vdisplay == h && !(m->flags & DRM_MODE_FLAG_INTERLACE) &&
                (!r || (int)m->vrefresh == r)) {
                *out = *m;
                return true;
            }
        }
        LOG("display: output %d: mode %s not offered by the monitor", o->n, want);
    }
    /* The monitor's preferred mode, unless it is bigger than 1080p (4K TVs often prefer
     * 4K30, which would halve the frame rate): then the best mode up to 1080p. */
    for (int i = 0; i < c->count_modes; i++)
        if ((c->modes[i].type & DRM_MODE_TYPE_PREFERRED) && c->modes[i].hdisplay <= 1920 &&
            c->modes[i].vdisplay <= 1080) {
            *out = c->modes[i];
            return true;
        }
    int best = -1;
    for (int i = 0; i < c->count_modes; i++) {
        drmModeModeInfo *m = &c->modes[i];
        if ((m->flags & DRM_MODE_FLAG_INTERLACE) || m->hdisplay > 1920 || m->vdisplay > 1080)
            continue;
        if (best < 0 || m->hdisplay * m->vdisplay > c->modes[best].hdisplay * c->modes[best].vdisplay ||
            (m->hdisplay * m->vdisplay == c->modes[best].hdisplay * c->modes[best].vdisplay &&
             m->vrefresh > c->modes[best].vrefresh))
            best = i;
    }
    if (best < 0 && c->count_modes > 0)
        best = 0;
    if (best < 0)
        return false;
    *out = c->modes[best];
    return true;
}

/* Identify: the first line very large, the others large, centred on a darkened band. */
static void draw_identify(struct output *o)
{
    char text[sizeof o->ident], *lines[6];
    int n = 0;
    snprintf(text, sizeof text, "%s", o->ident);
    for (char *p = strtok(text, "\n"); p && n < 6; p = strtok(NULL, "\n"))
        lines[n++] = p;
    if (!n)
        return;
    int big = o->H / 7, small = o->H / 18, gap = small / 2;
    int total = big + (n - 1) * (small + gap) + gap * 2;
    int top = (o->H - total) / 2;
    for (int y = top - gap; y < top + total + gap && y < o->H; y++)
        for (int x = 0; y >= 0 && x < o->W; x++) {
            uint32_t *d = &o->comp[(size_t)y * o->W + x];
            *d = (*d >> 2) & 0x3F3F3F; /* a quarter of the brightness */
        }
    int base = top + gap;
    for (int i = 0; i < n; i++) {
        int px = i ? small : big;
        FT_Set_Pixel_Sizes(face, 0, px);
        base += i ? small + gap : big;
        int w = text_width(lines[i]);
        int x = (o->W - w) / 2;
        draw_text(o->comp, o->W, o->H, x + px / 24 + 1, base + px / 24 + 1, lines[i], 0x000000);
        draw_text(o->comp, o->W, o->H, x, base, lines[i], i ? 0xE0E0E0 : 0x2BB3A3);
    }
}

static void splash_update_text(struct output *o, const char *ip, bool force)
{
    char msg[192];
    pthread_mutex_lock(&o->lock);
    snprintf(msg, sizeof msg, "%s", o->splash_msg);
    pthread_mutex_unlock(&o->lock);
    if (o->identify)
        snprintf(msg, sizeof msg, "%s", "");
    if (!o->active || (!force && !strcmp(ip, o->shown_ip) && !strcmp(msg, o->shown_msg)))
        return;
    snprintf(o->shown_ip, sizeof o->shown_ip, "%s", ip);
    snprintf(o->shown_msg, sizeof o->shown_msg, "%s", msg);
    memcpy(o->comp, o->base, (size_t)o->W * o->H * 4);

    if (font_ok && splash_info) {
        /* Font size: the smaller of 2% of the screen height and 16pt. */
        int px = (int)lround(o->H * 0.02);
        if (px > FONT_PT_PX) px = FONT_PT_PX;
        if (px < 8) px = 8;
        FT_Set_Pixel_Sizes(face, 0, px);
        int margin = px;
        int base = o->H - margin + (int)(face->size->metrics.descender >> 6);
        int shadow = px >= 16 ? 2 : 1;
        char left[96], right[96];
        snprintf(left, sizeof left, "IP: %s", ip);
        snprintf(right, sizeof right, "Serial: %s", serial);
        int rx = o->W - margin - text_width(right);
        draw_text(o->comp, o->W, o->H, margin + shadow, base + shadow, left, 0x000000);
        draw_text(o->comp, o->W, o->H, rx + shadow, base + shadow, right, 0x000000);
        draw_text(o->comp, o->W, o->H, margin, base, left, 0xD0D0D0);
        draw_text(o->comp, o->W, o->H, rx, base, right, 0xD0D0D0);
        if (*msg) {
            /* Status centred one line above, so it never runs into the IP or serial. */
            int line = (int)(face->size->metrics.height >> 6);
            int mx = (o->W - text_width(msg)) / 2;
            if (mx < margin)
                mx = margin;
            draw_text(o->comp, o->W, o->H, mx + shadow, base - line + shadow, msg, 0x000000);
            draw_text(o->comp, o->W, o->H, mx, base - line, msg, 0xD0D0D0);
        }
    }
    if (font_ok && o->identify)
        draw_identify(o);
    for (int y = 0; y < o->H; y++)
        memcpy(o->splash.map + (size_t)y * o->splash.pitch, o->comp + (size_t)y * o->W, (size_t)o->W * 4);
}

/* Free the video buffers. Caller holds o->lock and has made sure none is on screen. */
static void pool_free(struct output *o)
{
    share_forget(o);
    for (int b = 0; b < NBUF; b++) {
        buf_destroy(&o->vid[b]);
        o->vstate[b] = B_NONE;
    }
    o->pw = o->ph = 0;
    o->pfmt = 0;
    o->ready = o->ready2 = o->queued = o->scanout = o->last_video = -1;
}

static void output_disable(struct output *o)
{
    pthread_mutex_lock(&o->lock);
    o->usable = false;
    pthread_mutex_unlock(&o->lock);
    /* Let a receiver (or the preview) finish with a frame before its buffer goes away. */
    for (int tries = 0; tries < 100; tries++) {
        bool writing = o->thumb_reading;
        pthread_mutex_lock(&o->lock);
        for (int b = 0; b < NBUF; b++)
            writing |= o->vstate[b] == B_WRITING;
        pthread_mutex_unlock(&o->lock);
        if (!writing)
            break;
        usleep(10000);
    }
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    add_plane(req, o, NULL, 0, 0, 0, 0);
    drmModeAtomicAddProperty(req, o->conn_id, o->p_conn_crtc, 0);
    drmModeAtomicAddProperty(req, o->crtc_id, o->p_active, 0);
    drmModeAtomicAddProperty(req, o->crtc_id, o->p_mode, 0);
    if (drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        LOG("display: output %d: disable failed: %s", o->n, strerror(errno));
    drmModeAtomicFree(req);
    pthread_mutex_lock(&o->lock);
    pool_free(o);
    o->want_realloc = false;
    pthread_mutex_unlock(&o->lock);
    buf_destroy(&o->splash);
    free(o->base);
    free(o->comp);
    o->base = o->comp = NULL;
    if (o->mode_blob)
        drmModeDestroyPropertyBlob(drm_fd, o->mode_blob);
    o->mode_blob = 0;
    o->active = false;
    o->pending = P_NONE;
    o->shown_ip[0] = 0;
}

/* Turn a disabled output's signal off (the kernel console may have lit it up at boot). */
static void output_off(struct output *o)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    add_plane(req, o, NULL, 0, 0, 0, 0);
    drmModeAtomicAddProperty(req, o->conn_id, o->p_conn_crtc, 0);
    drmModeAtomicAddProperty(req, o->crtc_id, o->p_active, 0);
    drmModeAtomicAddProperty(req, o->crtc_id, o->p_mode, 0);
    if (drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        LOG("display: output %d: cannot switch off: %s", o->n, strerror(errno));
    else
        LOG("display: output %d (%s): disabled", o->n, o->conn_name);
    drmModeAtomicFree(req);
}

/* Mode-set a connected output and put the splash on it (blocking). */
static bool output_enable(struct output *o)
{
    drmModeConnector *c = drmModeGetConnector(drm_fd, o->conn_id); /* probes EDID */
    bool ok = false;
    if (!c || c->connection != DRM_MODE_CONNECTED)
        goto out;
    modes_publish(o, c);
    if (!pick_mode(o, c, &o->mode))
        goto out;
    o->W = o->mode.hdisplay;
    o->H = o->mode.vdisplay;
    if (drmModeCreatePropertyBlob(drm_fd, &o->mode, sizeof o->mode, &o->mode_blob))
        goto out;
    if (!buf_create(o->W, o->H, DRM_FORMAT_XRGB8888, &o->splash))
        goto out;
    o->base = malloc((size_t)o->W * o->H * 4);
    o->comp = malloc((size_t)o->W * o->H * 4);
    render_fit(o->base, o->W, o->H, &splash_img);
    o->active = true;
    char ip[64];
    read_ip(ip, sizeof ip);
    splash_update_text(o, ip, true);
    o->rotation = rotation_value(o);

    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, o->conn_id, o->p_conn_crtc, o->crtc_id);
        drmModeAtomicAddProperty(req, o->crtc_id, o->p_mode, o->mode_blob);
        drmModeAtomicAddProperty(req, o->crtc_id, o->p_active, 1);
        add_plane(req, o, &o->splash, 0, 0, o->W, o->H);
        ok = !drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        drmModeAtomicFree(req);
        if (!ok && o->rotation != DRM_MODE_ROTATE_0) {
            LOG("display: output %d: rotate %s / mirror %s not supported here, ignoring",
                o->n, o->rot_s, o->mirror_s);
            o->rotation = DRM_MODE_ROTATE_0;
        } else if (!ok) {
            break;
        }
    }
    if (!ok) {
        LOG("display: output %d: mode set %dx%d@%d failed: %s", o->n, o->W, o->H, o->mode.vrefresh, strerror(errno));
        if (!o->mode_failed && *ocfg(o->n, "MODE", "")) {
            LOG("display: output %d: falling back to the automatic mode", o->n);
            o->mode_failed = true; /* the next hotplug poll (1 s) retries without it */
        }
        o->active = false;
        buf_destroy(&o->splash);
        free(o->base);
        free(o->comp);
        o->base = o->comp = NULL;
        drmModeDestroyPropertyBlob(drm_fd, o->mode_blob);
        o->mode_blob = 0;
        goto out;
    }
    if (o->rotation != DRM_MODE_ROTATE_0)
        LOG("display: output %d: rotate %s, mirror %s", o->n, o->rot_s, o->mirror_s);
    LOG("display: output %d (%s): %dx%d@%d", o->n, o->conn_name, o->W, o->H, o->mode.vrefresh);
    set_brightness(o);
    pthread_mutex_lock(&o->lock);
    o->on_splash = true;
    o->want_splash = false;
    o->usable = true;
    pthread_mutex_unlock(&o->lock);
    o->splash_since = now_ms();
    o->pending = P_NONE;
    o->commit_logged = false;
out:
    drmModeFreeConnector(c);
    return ok;
}

static uint64_t clock_us(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* The display's refresh period, from the mode's timings (exact, unlike vrefresh). */
static double refresh_us(const struct output *o)
{
    return o->mode.clock && o->mode.htotal && o->mode.vtotal
               ? (double)o->mode.htotal * o->mode.vtotal * 1000.0 / o->mode.clock
               : 1e6 / 60;
}

#define PACE_MARGIN_US 4000.0 /* on top of the slowest recent frame */

/* An NDI frame's capture time: PiNDI senders put it in the timecode (100 ns UTC); other senders'
 * timecodes are not times, so then the time the frame was sent. */
static int64_t frame_pts(const NDIlib_video_frame_v2_t *v)
{
    int64_t d = v->timecode - v->timestamp;
    return v->timecode > 0 && d > -50000000 && d < 50000000 ? v->timecode : v->timestamp;
}

/* 100 ns UTC -> monotonic us (the realtime clock's offset now). */
static double pts_mono_us(int64_t pts)
{
    struct timespec rt, mt;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mt);
    double off = (double)(rt.tv_sec - mt.tv_sec) * 1e6 + (double)(rt.tv_nsec - mt.tv_nsec) / 1e3;
    return (double)pts / 10.0 - off;
}

/* o->lock held: frame idx just became ready to show - update the pacing delay. */
static void pace_note(struct output *o, int idx)
{
    if (!o->pacing || o->vsent[idx] == INT64_MAX)
        return;
    uint64_t now = clock_us(CLOCK_MONOTONIC);
    double pts = pts_mono_us(o->vsent[idx]), lag = (double)now - pts, per = refresh_us(o);
    if (!o->pace.on || fabs(pts - o->pace.last_pts) > 1e6 || lag < -1e6 || lag > 5e6) {
        o->pace.on = lag > -1e6 && lag < 5e6; /* a new stream, or the sender restarted */
        o->pace.lag_peak = lag;
        o->pace.delay = lag + PACE_MARGIN_US;
        o->pace.dev = 0;
        o->pace.center = true;
    } else {
        double gap = pts - o->pace.last_pts;
        if (!o->pace.gap_max || gap < o->pace.gap_min)
            o->pace.gap_min = gap;
        if (gap > o->pace.gap_max)
            o->pace.gap_max = gap;
        if (!o->pace.lag_max || lag < o->pace.lag_min)
            o->pace.lag_min = lag;
        if (lag > o->pace.lag_max)
            o->pace.lag_max = lag;
        /* Slowly forget a slow frame (2 ms/s), at once take a slower one. */
        o->pace.lag_peak -= (double)(now - o->pace.last_us) * 0.002;
        if (lag > o->pace.lag_peak)
            o->pace.lag_peak = lag;
    }
    o->pace.last_pts = pts;
    o->pace.last_us = now;
    double need = o->pace.lag_peak + PACE_MARGIN_US;
    if (need > o->pace.delay) { /* this frame was too slow for the delay */
        o->pace.delay = need;
        o->pace.center = true;
    } else if (need < o->pace.delay - per * 1.5) { /* a refresh to spare: one earlier, same position */
        o->pace.delay -= per;
        o->pace.moves++;
    }
    if (!o->pace.vbl_us)
        return;
    double x = (pts + o->pace.delay - (double)o->pace.vbl_us) / per;
    double d = x - floor(x) - 0.5; /* where this due time falls in its refresh: 0 = the middle */
    if (o->pace.center) {
        o->pace.dev = d;
    } else {
        double step = d - o->pace.dev;
        step -= floor(step + 0.5); /* the short way round (positions wrap at a refresh) */
        o->pace.dev += step * 0.05;
        o->pace.dev -= floor(o->pace.dev + 0.5);
    }
    if (o->pace.center || fabs(o->pace.dev) > 0.3) {
        o->pace.delay -= o->pace.dev * per;
        if (o->pace.delay < need)
            o->pace.delay += per;
        o->pace.dev = 0;
        o->pace.moves += !o->pace.center;
        o->pace.center = false;
    }
}

/* o->lock held: when frame idx should be on screen (monotonic us). */
static double pace_due(struct output *o, int idx)
{
    return pts_mono_us(o->vsent[idx]) + o->pace.delay;
}

/* A vblank without a flip (requested while a frame waits for its time). */
static void on_vblank(int fd, uint64_t seq, uint64_t ns, uint64_t data)
{
    (void)fd; (void)seq;
    struct output *o = (struct output *)(uintptr_t)data;
    pthread_mutex_lock(&o->lock);
    o->pace.vbl_us = ns / 1000;
    pthread_mutex_unlock(&o->lock);
    o->pace.vbl_wait = false;
}

/* A new video frame reached the screen at flip_us: count how long the one before it stayed up. */
static void cadence_add(struct output *o, uint64_t flip_us)
{
    if (o->cad.shown_us) {
        long k = lrint((double)(flip_us - o->cad.shown_us) / refresh_us(o));
        k = k < 1 ? 1 : k > 4 ? 4 : k;
        o->cad.hist[k]++;
        o->cad.log_hist[k]++;
    }
    o->cad.shown_us = flip_us;
}

/* A video frame reached the screen at flip_us (monotonic, from the vblank event). */
static void latency_add(struct output *o, int64_t sent, uint64_t recv_us, uint64_t flip_us)
{
    if (!o->lat.n && !o->lat.tot_n) {
        o->lat.pi_min = o->lat.tot_min = 1e9;
        o->lat.pi_max = o->lat.tot_max = -1e9;
    }
    double pi = (double)(int64_t)(flip_us - recv_us) / 1000.0;
    o->lat.pi_sum += pi;
    o->lat.pi_min = fmin(o->lat.pi_min, pi);
    o->lat.pi_max = fmax(o->lat.pi_max, pi);
    o->lat.n++;
    if (sent != INT64_MAX) {
        /* Sender time is UTC: move the flip time from the monotonic to the realtime clock. */
        int64_t offset_us = (int64_t)clock_us(CLOCK_REALTIME) - (int64_t)clock_us(CLOCK_MONOTONIC);
        double tot = ((double)((int64_t)flip_us + offset_us) * 10.0 - (double)sent) / 10000.0;
        o->lat.tot_sum += tot;
        o->lat.tot_min = fmin(o->lat.tot_min, tot);
        o->lat.tot_max = fmax(o->lat.tot_max, tot);
        o->lat.tot_n++;
    }
}

/* Every 5 s: write the averages for the web UI (empty file when no video is shown). */
static void latency_publish(struct output *o, uint64_t now)
{
    if (now - o->lat.since < 5000)
        return;
    char path[64], buf[384] = "";
    snprintf(path, sizeof path, RUN_DIR "/out%d.latency", o->n);
    if (o->lat.n)
        snprintf(buf, sizeof buf, "pi_avg=%.1f\npi_min=%.1f\npi_max=%.1f\n", o->lat.pi_sum / o->lat.n,
                 o->lat.pi_min, o->lat.pi_max);
    if (o->lat.tot_n)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "total_avg=%.1f\ntotal_min=%.1f\ntotal_max=%.1f\n",
                 o->lat.tot_sum / o->lat.tot_n, o->lat.tot_min, o->lat.tot_max);
    if (o->lat.n)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "frames=%d\n", o->lat.n);
    if (o->lat.n && o->lat.since) /* new video frames that reached the screen per second */
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "fps=%.2f\n",
                 o->lat.n * 1000.0 / (double)(now - o->lat.since));
    uint32_t *h = o->cad.hist;
    if (h[1] + h[2] + h[3] + h[4]) /* frames on screen for 1, 2, 3, 4+ refreshes */
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "cadence=%u %u %u %u\nrefresh=%.3f\n", h[1], h[2],
                 h[3], h[4], 1e6 / refresh_us(o));
    write_file_atomic(path, buf);
    memset(&o->lat, 0, sizeof o->lat);
    memset(o->cad.hist, 0, sizeof o->cad.hist);
    o->lat.since = now;
    h = o->cad.log_hist;
    if (!o->cad.log_since)
        o->cad.log_since = now;
    if (now - o->cad.log_since >= 60000) {
        if (h[1] + h[2] + h[3] + h[4])
            LOG("video: output %d: frames on screen for 1/2/3/4+ refreshes: %u/%u/%u/%u (%.3f Hz)%s", o->n, h[1],
                h[2], h[3], h[4], 1e6 / refresh_us(o), o->pacing ? "" : ", pacing off");
        if (o->pacing && o->pace.on) {
            pthread_mutex_lock(&o->lock);
            LOG("video: output %d: pacing: on screen %.0f ms after capture, %u refreshes waited, %u frames late, "
                "%u delay moves", o->n, o->pace.delay / 1000, o->pace.held, o->pace.late, o->pace.moves);
            LOG("video: output %d: pacing: capture times %.1f-%.1f ms apart, ready %.0f-%.0f ms after capture, "
                "%u dropped (queue full)", o->n, o->pace.gap_min / 1000, o->pace.gap_max / 1000,
                o->pace.lag_min / 1000, o->pace.lag_max / 1000, o->pace.full);
            o->pace.held = o->pace.late = o->pace.moves = o->pace.full = 0;
            o->pace.gap_min = o->pace.gap_max = o->pace.lag_min = o->pace.lag_max = 0;
            pthread_mutex_unlock(&o->lock);
        }
        memset(o->cad.log_hist, 0, sizeof o->cad.log_hist);
        o->cad.log_since = now;
    }
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    (void)fd; (void)seq; (void)crtc;
    struct output *o = data;
    uint64_t flip_us = (uint64_t)sec * 1000000 + usec;
    pthread_mutex_lock(&o->lock);
    o->pace.vbl_us = flip_us;
    /* Whatever is on screen now replaces a leader's buffer shown before. */
    pthread_mutex_lock(&share_mu);
    ext_release(o->ext_scanout_of, o->ext_scanout);
    o->ext_scanout = -1;
    o->ext_scanout_of = NULL;
    if (o->pending == P_EXT) {
        o->ext_scanout = o->ext_queued;
        o->ext_scanout_of = o->ext_queued_of;
        o->ext_queued = -1;
        o->ext_queued_of = NULL;
    } else {
        ext_release(o->ext_queued_of, o->ext_queued); /* a commit that didn't show it */
        o->ext_queued = -1;
        o->ext_queued_of = NULL;
    }
    struct output *L = o->ext_scanout_of;
    int li = o->ext_scanout;
    pthread_mutex_unlock(&share_mu);
    if (o->pending == P_VIDEO || o->pending == P_EXT) {
        if (o->scanout >= 0)
            o->vstate[o->scanout] = B_FREE;
        o->scanout = o->pending == P_VIDEO ? o->queued : -1;
        if (o->scanout >= 0) {
            o->last_video = o->scanout;
            o->vstate[o->scanout] = B_SCANOUT;
            latency_add(o, o->vsent[o->scanout], o->vrecv[o->scanout], flip_us);
            cadence_add(o, flip_us);
        } else if (L) { /* the leader's buffer is held for us (vuse): its times stay put */
            latency_add(o, L->vsent[li], L->vrecv[li], flip_us);
            cadence_add(o, flip_us);
        }
        o->queued = -1;
        o->on_splash = false;
        o->splash_since = 0;
    } else if (o->pending == P_SPLASH) {
        if (!o->on_splash)
            o->splash_since = now_ms();
        if (o->scanout >= 0)
            o->vstate[o->scanout] = B_FREE;
        o->scanout = -1;
        o->on_splash = true;
        o->cad.shown_us = 0;
    }
    pthread_mutex_unlock(&o->lock);
    o->pending = P_NONE;
}

/* Main thread: push the next thing to the screen if the output is idle. */
static void output_service(struct output *o)
{
    if (!o->active || o->pending != P_NONE)
        return;
    pthread_mutex_lock(&o->lock);

    if (o->want_realloc) {
        bool writing = o->thumb_reading;
        for (int b = 0; b < NBUF; b++)
            writing |= o->vstate[b] == B_WRITING;
        pthread_mutex_lock(&share_mu);
        for (int b = 0; b < NBUF; b++)
            if (o->vuse[b]) { /* shown by other outputs: they put their splash up first */
                writing = true;
                o->share_flush = true;
            }
        pthread_mutex_unlock(&share_mu);
        if (!o->on_splash) {
            pthread_mutex_unlock(&o->lock);
            commit_buf(o, &o->splash, 0, P_SPLASH); /* get the old buffers off screen first */
            return;
        }
        if (writing) {
            pthread_mutex_unlock(&o->lock);
            return;
        }
        pool_free(o);
        o->pw = o->ww;
        o->ph = o->wh;
        o->pfmt = o->wfmt;
        o->want_realloc = false;
        int ok = 0;
        for (int b = 0; b < NBUF; b++)
            if (buf_create(o->pw, o->ph, o->pfmt, &o->vid[b])) {
                o->vstate[b] = B_FREE;
                ok++;
            }
        o->cant_display = ok < 2; /* one buffer would freeze on the first frame: treat as failure */
        if (o->cant_display) {
            LOG("video: output %d: cannot display %dx%d (not enough video memory) - try receive quality "
                "\"low-bandwidth preview\"", o->n, o->pw, o->ph);
            int w = o->pw, h = o->ph;
            uint32_t f = o->pfmt;
            pool_free(o);
            o->pw = w; /* remembered, so frames of this size are refused instead of retried */
            o->ph = h;
            o->pfmt = f;
        } else {
            LOG("video: output %d: %dx%d %.4s", o->n, o->pw, o->ph, (const char *)&o->pfmt);
        }
        pthread_mutex_unlock(&o->lock);
        return;
    }

    if (o->identify) {
        ready_clear(o);
        bool show = !o->on_splash;
        pthread_mutex_unlock(&o->lock);
        if (show)
            commit_buf(o, &o->splash, 0, P_SPLASH);
        return;
    }

    /* After identifying: put the last picture back if the source sends nothing new (screen
     * captures only send on change). Its buffer is untouched while it is still free. */
    if (o->restore_video) {
        o->restore_video = false;
        int idx = o->last_video;
        if (o->ready < 0 && idx >= 0 && o->vstate[idx] == B_FREE && !o->want_splash) {
            o->vstate[idx] = B_READY;
            o->ready = idx;
        }
    }

    /* A frame of the receiver this output follows (or a flush: its buffers are needed back). */
    pthread_mutex_lock(&share_mu);
    struct output *L = o->ext_ready_of;
    int li = o->ext_ready;
    bool flush = (o->ext_scanout_of && o->ext_scanout_of->share_flush) ||
                 (o->ext_ready_of && o->ext_ready_of->share_flush);
    if (flush) {
        ext_clear_ready(o);
        L = NULL;
    } else if (L && o->ready < 0 && !o->identify) {
        if (o->ext_ready2 < 0) {
            o->backlog = 0;
        } else if (++o->backlog >= BACKLOG_DROP) { /* a standing backlog: skip a frame to lose it */
            ext_release(L, li);
            L = o->ext_ready2_of;
            li = o->ext_ready2;
            o->ext_ready2 = -1;
            o->ext_ready2_of = NULL;
            o->backlog = 0;
        }
        o->ext_queued = li;
        o->ext_queued_of = L;
        o->ext_ready = o->ext_ready2;
        o->ext_ready_of = o->ext_ready2_of;
        o->ext_ready2 = -1;
        o->ext_ready2_of = NULL;
    } else {
        L = NULL;
    }
    pthread_mutex_unlock(&share_mu);
    if (flush && !o->on_splash) {
        pthread_mutex_unlock(&o->lock);
        commit_buf(o, &o->splash, 0, P_SPLASH);
        return;
    }
    if (L) {
        o->want_splash = false;
        pthread_mutex_unlock(&o->lock);
        pthread_mutex_lock(&L->lock); /* aspect: written before the frame was published */
        double ar = L->vaspect[li];
        pthread_mutex_unlock(&L->lock);
        if (!commit_buf(o, &L->vid[li], ar, P_EXT)) {
            pthread_mutex_lock(&share_mu);
            ext_release(o->ext_queued_of, o->ext_queued);
            o->ext_queued = -1;
            o->ext_queued_of = NULL;
            pthread_mutex_unlock(&share_mu);
        }
        return;
    }

    if (o->ready >= 0 && o->pacing && o->pace.on && o->pace.vbl_us && o->vsent[o->ready] != INT64_MAX) {
        double per = refresh_us(o), now = (double)clock_us(CLOCK_MONOTONIC);
        double next = (double)o->pace.vbl_us + per * (floor((now - (double)o->pace.vbl_us) / per) + 1);
        if (o->ready2 >= 0 && pace_due(o, o->ready2) <= next) { /* both due: the older one is late */
            o->vstate[o->ready] = B_FREE;
            o->ready = o->ready2;
            o->ready2 = -1;
            o->pace.late++;
        }
        if (pace_due(o, o->ready) > next) { /* not yet: look again at the next refresh */
            o->pace.held++;
            pthread_mutex_unlock(&o->lock);
            if (!o->pace.vbl_wait) {
                if (drmCrtcQueueSequence(drm_fd, o->crtc_id,
                                         DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS, 1, NULL,
                                         (uint64_t)(uintptr_t)o) == 0) {
                    o->pace.vbl_wait = true;
                } else {
                    LOG("video: output %d: no vblank events (%s): frame pacing off", o->n, strerror(errno));
                    o->pacing = false;
                }
            }
            return;
        }
    }
    if (o->ready >= 0) {
        if (o->ready2 < 0 || (o->pacing && o->pace.on)) {
            o->backlog = 0;
        } else if (++o->backlog >= BACKLOG_DROP) { /* a standing backlog: skip a frame to lose it */
            o->vstate[o->ready] = B_FREE;
            o->ready = o->ready2;
            o->ready2 = -1;
            o->backlog = 0;
        }
        int idx = o->ready;
        double ar = o->vaspect[idx];
        o->ready = o->ready2;
        o->ready2 = -1;
        o->vstate[idx] = B_QUEUED;
        o->queued = idx;
        pthread_mutex_unlock(&o->lock);
        if (!commit_buf(o, &o->vid[idx], ar, P_VIDEO)) {
            pthread_mutex_lock(&o->lock);
            if (o->vstate[idx] == B_QUEUED)
                o->vstate[idx] = B_FREE;
            o->queued = -1;
            pthread_mutex_unlock(&o->lock);
        }
        return;
    }

    if (o->want_splash && !o->on_splash) {
        pthread_mutex_unlock(&o->lock);
        commit_buf(o, &o->splash, 0, P_SPLASH);
        return;
    }
    pthread_mutex_unlock(&o->lock);
}

/* Receiver thread: a free video buffer of w x h fmt to write a frame into (state B_WRITING), or -1:
 * none free (drop the frame), a new buffer pool was requested, or no display. *too_big: frames
 * of this size can't be shown (not enough video memory). */
static int frame_begin(struct output *o, int w, int h, uint32_t fmt, bool *too_big)
{
    *too_big = false;
    pthread_mutex_lock(&o->lock);
    if (!o->usable) {
        pthread_mutex_unlock(&o->lock);
        return -1;
    }
    if (o->cant_display && o->pw == w && o->ph == h && o->pfmt == fmt) {
        pthread_mutex_unlock(&o->lock);
        *too_big = true;
        return -1;
    }
    if (o->pw != w || o->ph != h || o->pfmt != fmt) {
        if (!o->want_realloc || o->ww != w || o->wh != h || o->wfmt != fmt) {
            o->ww = w;
            o->wh = h;
            o->wfmt = fmt;
            o->want_realloc = true;
            o->want_splash = false;
        }
        pthread_mutex_unlock(&o->lock);
        wake();
        return -1;
    }
    int idx = -1;
    pthread_mutex_lock(&share_mu); /* buffers other outputs show are not written to */
    for (int b = 0; b < NBUF && idx < 0; b++)
        if (o->vstate[b] == B_FREE && !o->vuse[b])
            idx = b;
    if (idx < 0 && o->ready2 >= 0 && !o->vuse[o->ready2]) { /* replace the newest not-yet-shown frame */
        idx = o->ready2;
        o->ready2 = -1;
    } else if (idx < 0 && o->ready >= 0 && o->ready2 < 0 && !o->vuse[o->ready]) {
        idx = o->ready;
        o->ready = -1;
    }
    pthread_mutex_unlock(&share_mu);
    if (idx >= 0) {
        o->vstate[idx] = B_WRITING;
        o->want_splash = false;
    }
    pthread_mutex_unlock(&o->lock);
    return idx;
}

/* The frame in buffer idx is complete: next on screen. sent: NDI timestamp (100 ns UTC) or INT64_MAX. */
static void frame_end(struct output *o, int idx, double aspect, int64_t sent, uint64_t recv_us)
{
    pthread_mutex_lock(&o->lock);
    if (o->vstate[idx] == B_WRITING) {
        ready_push(o, idx);
        o->vaspect[idx] = aspect;
        o->vsent[idx] = sent;
        o->vrecv[idx] = recv_us;
        pace_note(o, idx);
    }
    pthread_mutex_unlock(&o->lock);
    wake();
    share_publish(o, idx);
}

/* Receiver thread: hand a frame to the display. Drops it if no buffer is free. */
static bool post_frame(struct output *o, const NDIlib_video_frame_v2_t *v, uint64_t recv_us)
{
    uint32_t fmt;
    switch (v->FourCC) {
    case NDIlib_FourCC_video_type_UYVY:
    case NDIlib_FourCC_video_type_UYVA: /* alpha plane follows, ignored */
        fmt = o->has_uyvy ? DRM_FORMAT_UYVY : DRM_FORMAT_NV16; break;
    case NDIlib_FourCC_video_type_BGRA:
    case NDIlib_FourCC_video_type_BGRX:
        fmt = DRM_FORMAT_XRGB8888; break;
    case NDIlib_FourCC_video_type_RGBA:
    case NDIlib_FourCC_video_type_RGBX:
        fmt = DRM_FORMAT_XBGR8888; break;
    default:
        return true;
    }
    bool too_big;
    int idx = frame_begin(o, v->xres, v->yres, fmt, &too_big);
    if (idx < 0)
        return !too_big;
    struct buf *b = &o->vid[idx];
    for (int y = 0; y < v->yres; y++) {
        const uint8_t *src = v->p_data + (size_t)y * v->line_stride_in_bytes;
        uint8_t *dst = b->map + (size_t)y * b->pitch;
        if (fmt == DRM_FORMAT_NV16)
            uyvy_to_nv16(src, dst, b->map + b->uv_off + (size_t)y * b->pitch, v->xres);
        else
            memcpy(dst, src, (size_t)v->xres * (fmt == DRM_FORMAT_UYVY ? 2 : 4));
    }
    frame_end(o, idx, v->picture_aspect_ratio > 0 ? v->picture_aspect_ratio : (double)v->xres / v->yres,
              frame_pts(v), recv_us);
    return true;
}

/* U and V rows -> one interleaved CbCr row (NV16 / NV12). n = chroma samples. */
static void uv_interleave(const uint8_t *u, const uint8_t *v, uint8_t *d, int n)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; x + 16 <= n; x += 16) {
        uint8x16x2_t p = { { vld1q_u8(u + x), vld1q_u8(v + x) } };
        vst2q_u8(d + x * 2, p);
    }
#endif
    for (; x < n; x++) {
        d[x * 2] = u[x];
        d[x * 2 + 1] = v[x];
    }
}

/* Planar 4:2:2 row -> UYVY row. */
static void yuv422p_to_uyvy(const uint8_t *y, const uint8_t *u, const uint8_t *v, uint8_t *d, int w)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; x + 32 <= w; x += 32) {
        uint8x16x2_t yy = vld2q_u8(y + x);
        uint8x16x4_t p = { { vld1q_u8(u + x / 2), yy.val[0], vld1q_u8(v + x / 2), yy.val[1] } };
        vst4q_u8(d + x * 2, p);
    }
#endif
    for (; x + 2 <= w; x += 2) {
        d[x * 2] = u[x / 2];
        d[x * 2 + 1] = y[x];
        d[x * 2 + 2] = v[x / 2];
        d[x * 2 + 3] = y[x + 1];
    }
}

/* Receiver thread: a planar 4:2:2 or 4:2:0 picture (this device's own capture input). False if this
 * display can't show it. */
static bool post_planar(struct output *o, const uint8_t *y, const uint8_t *u, const uint8_t *v, int w, int h,
                        bool is420, int64_t sent, uint64_t recv_us)
{
    uint32_t fmt = is420 ? (o->has_nv12 ? DRM_FORMAT_NV12 : 0)
                         : (o->has_nv16 ? DRM_FORMAT_NV16 : o->has_uyvy ? DRM_FORMAT_UYVY : 0);
    if (!fmt)
        return false;
    bool too_big;
    int idx = frame_begin(o, w, h, fmt, &too_big);
    if (idx < 0)
        return !too_big;
    struct buf *b = &o->vid[idx];
    int cw = w / 2;
    for (int r = 0; r < h; r++) {
        const uint8_t *yr = y + (size_t)r * w;
        uint8_t *dst = b->map + (size_t)r * b->pitch;
        if (fmt == DRM_FORMAT_UYVY) {
            yuv422p_to_uyvy(yr, u + (size_t)r * cw, v + (size_t)r * cw, dst, w);
            continue;
        }
        memcpy(dst, yr, (size_t)w);
        if (fmt == DRM_FORMAT_NV16)
            uv_interleave(u + (size_t)r * cw, v + (size_t)r * cw, b->map + b->uv_off + (size_t)r * b->pitch, cw);
        else if (!(r & 1))
            uv_interleave(u + (size_t)(r / 2) * cw, v + (size_t)(r / 2) * cw,
                          b->map + b->uv_off + (size_t)(r / 2) * b->pitch, cw);
    }
    frame_end(o, idx, (double)w / h, sent, recv_us);
    return true;
}

static void request_splash(struct output *o)
{
    pthread_mutex_lock(&o->lock);
    o->want_splash = true;
    ready_clear(o);
    pthread_mutex_unlock(&o->lock);
    wake();
}

/* ------------------------------------------------------------------ hardware H.264 decoding */

/* The NDI library decodes NDI|HX in software on one core: a Pi 3 manages a few frames per second
 * at 1080p. The Pi 3/4 have a hardware H.264 decoder (bcm2835-codec, a V4L2 memory-to-memory
 * device). For H.264 streams the receiver asks NDI for the compressed frames and feeds them to it;
 * the decoder writes the pictures straight into the output's NV12 buffers (DMABUF), which are then
 * shown like any other frame. Buffer states: B_WRITING = queued in the decoder. */

#define HW_IN_BUFS 4
#define HW_IN_SIZE (2 << 20)

/* NDIlib_compressed_packet_t (Processing.NDI.Advanced.h): it precedes each compressed frame. */
struct ndi_packet {
    int32_t version; /* structure size: the frame data starts this many bytes in */
    uint32_t fourcc;
    int64_t pts, dts;
    uint64_t reserved;
    uint32_t flags; /* 1 = keyframe */
    uint32_t data_size, extra_data_size; /* extra data (SPS/PPS) follows the frame data */
};

struct hwdec {
    int fd;
    struct { uint8_t *map; size_t len; bool queued; } in[HW_IN_BUFS];
    bool cap_on;          /* capture queue set up with the output's buffers */
    bool want_cap;        /* the decoder knows the stream format: set the capture queue up */
    int cw, ch, vw, vh;   /* coded and visible size */
    int dmabuf[NBUF];
    bool need_key;        /* skip to the next keyframe (start, or after a dropped frame) */
    uint32_t seq;
    struct { int64_t sent; uint64_t recv; } meta[16];
    double aspect;
    uint64_t decoded, fed;
};

static char hw_dev[32]; /* the decoder's /dev/videoN, "" if there is none */

static void hwdec_find(void)
{
    for (int i = 0; i < 64; i++) {
        char path[64], name[64] = "";
        snprintf(path, sizeof path, "/sys/class/video4linux/video%d/name", i);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof name, f) && !strncmp(name, "bcm2835-codec-decode", 20))
            snprintf(hw_dev, sizeof hw_dev, "/dev/video%d", i);
        fclose(f);
        if (*hw_dev)
            break;
    }
    LOG("video: hardware H.264 decoder %s", *hw_dev ? hw_dev : "not available");
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

/* Give the capture buffers back to the output (stream off returns them all). */
static void hwdec_cap_stop(struct output *o, struct hwdec *d)
{
    if (!d->cap_on)
        return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(d->fd, VIDIOC_STREAMOFF, &type);
    struct v4l2_requestbuffers rb = { .count = 0, .type = type, .memory = V4L2_MEMORY_DMABUF };
    xioctl(d->fd, VIDIOC_REQBUFS, &rb);
    pthread_mutex_lock(&o->lock);
    for (int b = 0; b < NBUF; b++)
        if (o->vstate[b] == B_WRITING)
            o->vstate[b] = B_FREE;
    pthread_mutex_unlock(&o->lock);
    for (int b = 0; b < NBUF; b++) {
        if (d->dmabuf[b] >= 0)
            close(d->dmabuf[b]);
        d->dmabuf[b] = -1;
    }
    d->cap_on = false;
}

static void hwdec_close(struct output *o, struct hwdec *d)
{
    if (d->fd < 0)
        return;
    hwdec_cap_stop(o, d);
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(d->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < HW_IN_BUFS; i++)
        if (d->in[i].map)
            munmap(d->in[i].map, d->in[i].len);
    close(d->fd);
    LOG("video: output %d: hardware decoder closed (%llu of %llu frames decoded)", o->n,
        (unsigned long long)d->decoded, (unsigned long long)d->fed);
    memset(d, 0, sizeof *d);
    d->fd = -1;
}

static bool hwdec_open(struct output *o, struct hwdec *d, int w, int h)
{
    memset(d, 0, sizeof *d);
    for (int b = 0; b < NBUF; b++)
        d->dmabuf[b] = -1;
    d->need_key = true;
    d->fd = open(hw_dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) {
        LOG("video: output %d: cannot open %s: %s", o->n, hw_dev, strerror(errno));
        return false;
    }
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = HW_IN_SIZE;
    struct v4l2_requestbuffers rb = { .count = HW_IN_BUFS, .type = f.type, .memory = V4L2_MEMORY_MMAP };
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    if (xioctl(d->fd, VIDIOC_S_FMT, &f) || xioctl(d->fd, VIDIOC_REQBUFS, &rb) || rb.count < 1 ||
        xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub))
        goto fail;
    for (uint32_t i = 0; i < rb.count && i < HW_IN_BUFS; i++) {
        struct v4l2_plane pl = { 0 };
        struct v4l2_buffer b = { .index = i, .type = f.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &pl,
                                 .length = 1 };
        if (xioctl(d->fd, VIDIOC_QUERYBUF, &b))
            goto fail;
        d->in[i].len = pl.length;
        d->in[i].map = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, pl.m.mem_offset);
        if (d->in[i].map == MAP_FAILED) {
            d->in[i].map = NULL;
            goto fail;
        }
    }
    int type = f.type;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type))
        goto fail;
    /* NDI gives the size: set the capture side up now. The decoder reports a format change
     * (source change event) only if the stream turns out different. */
    d->vw = d->cw = w;
    d->vh = h;
    d->ch = nv12_rows(h);
    d->want_cap = true;
    LOG("video: output %d: hardware H.264 decoding (%s)", o->n, hw_dev);
    return true;
fail:
    LOG("video: output %d: hardware decoder setup failed: %s", o->n, strerror(errno));
    hwdec_close(o, d);
    return false;
}

/* The decoder found the stream's size: get NV12 buffers of that size from the main thread, then
 * hand them to the decoder. Returns false while still waiting for the buffers. */
static bool hwdec_cap_start(struct output *o, struct hwdec *d)
{
    pthread_mutex_lock(&o->lock);
    bool fits = o->usable && !o->want_realloc && o->pfmt == DRM_FORMAT_NV12 && o->pw == d->vw && o->ph == d->vh;
    bool idle = fits;
    for (int b = 0; b < NBUF && fits; b++)
        idle &= o->vstate[b] == B_FREE || o->vstate[b] == B_SCANOUT || o->vstate[b] == B_QUEUED ||
                o->vstate[b] == B_READY;
    if (!fits && o->usable && !o->cant_display &&
        (!o->want_realloc || o->ww != d->vw || o->wh != d->vh || o->wfmt != DRM_FORMAT_NV12)) {
        o->ww = d->vw;
        o->wh = d->vh;
        o->wfmt = DRM_FORMAT_NV12;
        o->want_realloc = true;
        o->want_splash = false;
    }
    uint32_t pitch = o->vid[0].pitch;
    uint64_t size = o->vid[0].size;
    uint32_t handle[NBUF];
    for (int b = 0; b < NBUF; b++)
        handle[b] = o->vid[b].handle;
    pthread_mutex_unlock(&o->lock);
    if (!fits) {
        wake();
        return false;
    }
    if (!idle)
        return false;

    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.width = d->cw;
    f.fmt.pix_mp.height = d->ch;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].bytesperline = pitch;
    if (xioctl(d->fd, VIDIOC_S_FMT, &f)) {
        LOG("video: output %d: decoder capture format: %s", o->n, strerror(errno));
        return false;
    }
    if (f.fmt.pix_mp.plane_fmt[0].bytesperline != pitch || f.fmt.pix_mp.plane_fmt[0].sizeimage > size ||
        f.fmt.pix_mp.height != (uint32_t)nv12_rows(d->vh)) {
        LOG("video: output %d: decoder wants %ux%u, %u bytes per line, %u bytes; display buffers have %u, %llu",
            o->n, f.fmt.pix_mp.width, f.fmt.pix_mp.height, f.fmt.pix_mp.plane_fmt[0].bytesperline,
            f.fmt.pix_mp.plane_fmt[0].sizeimage, pitch, (unsigned long long)size);
        return false;
    }
    struct v4l2_requestbuffers rb = { .count = NBUF, .type = f.type, .memory = V4L2_MEMORY_DMABUF };
    if (xioctl(d->fd, VIDIOC_REQBUFS, &rb) || rb.count < NBUF) {
        LOG("video: output %d: decoder buffers: %s", o->n, strerror(errno));
        return false;
    }
    for (int b = 0; b < NBUF; b++)
        if (drmPrimeHandleToFD(drm_fd, handle[b], DRM_CLOEXEC | DRM_RDWR, &d->dmabuf[b])) {
            LOG("video: output %d: cannot export display buffer: %s", o->n, strerror(errno));
            d->cap_on = true; /* so that stop cleans up */
            hwdec_cap_stop(o, d);
            return false;
        }
    int type = f.type;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type)) {
        LOG("video: output %d: decoder start: %s", o->n, strerror(errno));
        d->cap_on = true;
        hwdec_cap_stop(o, d);
        return false;
    }
    d->cap_on = true;
    d->want_cap = false;
    LOG("video: output %d: decoding %dx%d into the display buffers", o->n, d->vw, d->vh);
    return true;
}

/* Queue every free display buffer to the decoder. */
static void hwdec_refill(struct output *o, struct hwdec *d)
{
    if (!d->cap_on)
        return;
    for (int b = 0; b < NBUF; b++) {
        pthread_mutex_lock(&o->lock);
        pthread_mutex_lock(&share_mu);
        bool take = o->vstate[b] == B_FREE && b != o->last_video && !o->vuse[b];
        pthread_mutex_unlock(&share_mu);
        if (take)
            o->vstate[b] = B_WRITING;
        pthread_mutex_unlock(&o->lock);
        if (!take)
            continue;
        struct v4l2_plane pl = { .m.fd = d->dmabuf[b], .length = (uint32_t)o->vid[b].size };
        struct v4l2_buffer vb = { .index = b, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                  .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
        if (xioctl(d->fd, VIDIOC_QBUF, &vb)) {
            pthread_mutex_lock(&o->lock);
            o->vstate[b] = B_FREE;
            pthread_mutex_unlock(&o->lock);
        }
    }
}

/* Events, finished input buffers and decoded pictures; waits up to wait_ms for a picture. */
static void hwdec_poll(struct output *o, struct hwdec *d, int wait_ms)
{
    uint64_t until = now_ms() + wait_ms;
    for (;;) {
        struct v4l2_event ev;
        while (!xioctl(d->fd, VIDIOC_DQEVENT, &ev))
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
                struct v4l2_selection sel = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .target = V4L2_SEL_TGT_COMPOSE };
                if (xioctl(d->fd, VIDIOC_G_FMT, &f))
                    continue;
                d->cw = f.fmt.pix_mp.width;
                d->ch = f.fmt.pix_mp.height;
                d->vw = d->cw;
                d->vh = d->ch;
                if (!xioctl(d->fd, VIDIOC_G_SELECTION, &sel) && sel.r.width && sel.r.height) {
                    d->vw = sel.r.width;
                    d->vh = sel.r.height;
                }
                LOG("video: output %d: decoder: %dx%d (coded %dx%d)", o->n, d->vw, d->vh, d->cw, d->ch);
                hwdec_cap_stop(o, d);
                d->want_cap = true;
            }
        if (d->want_cap && hwdec_cap_start(o, d)) {
            d->need_key = true; /* the frames since the format change were not decoded */
        }
        hwdec_refill(o, d);

        for (;;) { /* input buffers the decoder is done with */
            struct v4l2_plane pl = { 0 };
            struct v4l2_buffer vb = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                                      .m.planes = &pl, .length = 1 };
            if (xioctl(d->fd, VIDIOC_DQBUF, &vb))
                break;
            d->in[vb.index].queued = false;
        }
        bool got = false;
        while (d->cap_on) { /* decoded pictures */
            struct v4l2_plane pl = { 0 };
            struct v4l2_buffer vb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
                                      .m.planes = &pl, .length = 1 };
            if (xioctl(d->fd, VIDIOC_DQBUF, &vb))
                break;
            int idx = vb.index;
            bool ok = !(vb.flags & V4L2_BUF_FLAG_ERROR) && pl.bytesused > 0 && idx < NBUF;
            uint32_t m = (uint32_t)vb.timestamp.tv_usec % 16;
            pthread_mutex_lock(&o->lock);
            if (idx < NBUF && o->vstate[idx] == B_WRITING) {
                if (ok) {
                    ready_push(o, idx);
                    o->vaspect[idx] = d->aspect > 0 ? d->aspect : (double)d->vw / d->vh;
                    o->vsent[idx] = d->meta[m].sent;
                    o->vrecv[idx] = d->meta[m].recv;
                    pace_note(o, idx);
                    o->want_splash = false;
                } else {
                    o->vstate[idx] = B_FREE;
                }
            }
            pthread_mutex_unlock(&o->lock);
            if (ok) {
                d->decoded++;
                got = true;
                wake();
                share_publish(o, idx);
            }
            hwdec_refill(o, d);
        }
        if (got || now_ms() >= until)
            return;
        struct pollfd p = { .fd = d->fd, .events = POLLIN | POLLPRI };
        int left = (int)(until - now_ms());
        if (poll(&p, 1, left > 0 ? left : 0) <= 0)
            return;
    }
}

/* One compressed H.264 frame from NDI. False: the decoder failed (fall back to the NDI library). */
static bool hwdec_feed(struct output *o, struct hwdec *d, const NDIlib_video_frame_v2_t *v, uint64_t recv_us)
{
    const struct ndi_packet *pk = (const struct ndi_packet *)v->p_data;
    /* version is the header's size: 44 bytes (sizeof here is 48, padded to 8 for the int64s). */
    const int32_t hdr = (int32_t)(offsetof(struct ndi_packet, extra_data_size) + sizeof pk->extra_data_size);
    if (!v->p_data || v->data_size_in_bytes < hdr || pk->version < hdr ||
        (int64_t)pk->version + pk->data_size + pk->extra_data_size > v->data_size_in_bytes)
        return true;
    bool key = pk->flags & 1;
    d->aspect = v->picture_aspect_ratio > 0 ? v->picture_aspect_ratio : (double)v->xres / v->yres;
    pthread_mutex_lock(&o->lock);
    bool usable = o->usable && !o->cant_display;
    pthread_mutex_unlock(&o->lock);
    if (!usable) {
        hwdec_cap_stop(o, d); /* the output is going away or being reallocated */
        if (d->cw)
            d->want_cap = true;
        d->need_key = true;
        return true;
    }
    if (d->need_key && !key)
        return true;
    int i = 0;
    while (i < HW_IN_BUFS && (d->in[i].queued || !d->in[i].map))
        i++;
    size_t len = (size_t)pk->data_size + (key ? pk->extra_data_size : 0);
    if (i == HW_IN_BUFS || len > d->in[i].len) {
        d->need_key = true; /* decoder busy: drop, and restart cleanly at the next keyframe */
        return true;
    }
    const uint8_t *data = (const uint8_t *)pk + pk->version;
    size_t off = 0;
    if (key) { /* parameter sets (SPS/PPS) first */
        memcpy(d->in[i].map, data + pk->data_size, pk->extra_data_size);
        off = pk->extra_data_size;
    }
    memcpy(d->in[i].map + off, data, pk->data_size);
    uint32_t m = d->seq++ % 16;
    d->meta[m].sent = frame_pts(v);
    d->meta[m].recv = recv_us;
    struct v4l2_plane pl = { .bytesused = (uint32_t)len, .length = (uint32_t)d->in[i].len };
    struct v4l2_buffer vb = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                              .m.planes = &pl, .length = 1 };
    vb.timestamp.tv_usec = m;
    if (xioctl(d->fd, VIDIOC_QBUF, &vb)) {
        LOG("video: output %d: decoder input: %s", o->n, strerror(errno));
        return false;
    }
    d->in[i].queued = true;
    d->need_key = false;
    d->fed++;
    return true;
}

/* ------------------------------------------------------------------ status for the web UI */

static void status_set(struct output *o, const char *state, const char *source, const char *url, const char *video)
{
    char buf[1024], path[64];
    if (!strcmp(o->st.state, state) && !strcmp(o->st.source, source) && !strcmp(o->st.url, url) &&
        !strcmp(o->st.video, video) && !strcmp(o->st.target, o->target))
        return;
    snprintf(o->st.target, sizeof o->st.target, "%s", o->target);
    snprintf(o->st.state, sizeof o->st.state, "%s", state);
    snprintf(o->st.source, sizeof o->st.source, "%s", source);
    snprintf(o->st.url, sizeof o->st.url, "%s", url);
    snprintf(o->st.video, sizeof o->st.video, "%s", video);
    snprintf(buf, sizeof buf, "state=%s\ntarget=%s\nsource=%s\nurl=%s\nvideo=%s\n",
             o->st.state, o->target, o->st.source, o->st.url, o->st.video);
    snprintf(path, sizeof path, RUN_DIR "/out%d.status", o->n);
    write_file_atomic(path, buf);

    /* The same, short, on the splash (also shown right after a stream is lost). */
    char msg[sizeof o->splash_msg], from[80] = "";
    if (*o->source_ip)
        snprintf(from, sizeof from, " from %s", o->source_ip);
    if (!strcmp(state, "waiting for network"))
        snprintf(msg, sizeof msg, "Waiting for the network");
    else if (!strcmp(state, "connecting"))
        snprintf(msg, sizeof msg, "Connecting to %s", source);
    else if (!strcmp(state, "connected, no frames"))
        snprintf(msg, sizeof msg, "%s: connected, no video", source);
    else if (!strncmp(state, "cannot display", 14))
        snprintf(msg, sizeof msg, "%s: %s is too big for this device - use the low-bandwidth preview", source,
                 video);
    else if (!strcmp(state, "searching") || !strncmp(state, "playing", 7))
        snprintf(msg, sizeof msg, "Waiting for NDI stream %s%s", o->target, from);
    else
        msg[0] = 0;
    pthread_mutex_lock(&o->lock);
    snprintf(o->splash_msg, sizeof o->splash_msg, "%s", msg);
    pthread_mutex_unlock(&o->lock);
    wake();
}

static void outputs_publish(void)
{
    static char last[1024];
    char buf[1024];
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < nout && len < sizeof buf; i++) {
        struct output *o = &outs[i];
        char mode[48] = "";
        if (o->active)
            snprintf(mode, sizeof mode, "%dx%d@%d", o->W, o->H, o->mode.vrefresh);
        len += snprintf(buf + len, sizeof buf - len, "%d\t%s\t%s\t%s\n", o->n, o->conn_name,
                        o->disabled ? "disabled" : o->active ? "connected" : o->standby ? "standby" : "disconnected",
                        mode);
    }
    if (!strcmp(buf, last))
        return;
    snprintf(last, sizeof last, "%s", buf);
    write_file_atomic(RUN_DIR "/outputs", buf);
}

/* Visible sources, one "name<TAB>url" per line; rewritten only when the list changes. */
static pthread_mutex_t sources_lock = PTHREAD_MUTEX_INITIALIZER;

static void sources_publish(const NDIlib_source_t *srcs, uint32_t count)
{
    static char last[16384];
    char buf[16384];
    size_t len = 0;
    buf[0] = 0;
    for (uint32_t i = 0; i < count && len < sizeof buf - 1; i++)
        len += snprintf(buf + len, sizeof buf - len, "%s\t%s\n",
                        srcs[i].p_ndi_name ? srcs[i].p_ndi_name : "",
                        srcs[i].p_url_address ? srcs[i].p_url_address : "");
    if (len >= sizeof buf)
        len = sizeof buf - 1;
    pthread_mutex_lock(&sources_lock);
    if (strcmp(buf, last)) {
        memcpy(last, buf, len + 1);
        write_file_atomic(RUN_DIR "/sources", buf);
    }
    pthread_mutex_unlock(&sources_lock);
}

/* ------------------------------------------------------------------ audio (receiver thread) */

static void audio_info(struct output *o, const char *fmt, ...)
{
    char buf[sizeof o->au.info], path[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!strcmp(buf, o->au.info))
        return;
    snprintf(o->au.info, sizeof o->au.info, "%s", buf);
    snprintf(path, sizeof path, RUN_DIR "/out%d.audio", o->n);
    char line[sizeof buf + 2];
    snprintf(line, sizeof line, "%s\n", buf);
    write_file_atomic(path, line);
}

static void audio_close(struct output *o)
{
    if (o->au.pcm) {
        snd_pcm_drop(o->au.pcm);
        snd_pcm_close(o->au.pcm);
        o->au.pcm = NULL;
    }
    o->au.rate = 0;
    o->au.win_start = 0;
    o->au.pending_drop = 0;
}

/* ALSA device for this output: its own HDMI port (Pi 4/5: vc4hdmi0/1, Pi 3: vc4hdmi), or the jack. */
static void audio_device(struct output *o, char *dev, size_t n)
{
    if (!strcasecmp(o->audio_s, "analog")) {
        snprintf(dev, n, "plughw:CARD=Headphones,DEV=0");
        return;
    }
    int port = 0;
    sscanf(o->conn_name, "HDMI-A-%d", &port);
    char card[32];
    snprintf(card, sizeof card, "vc4hdmi%d", port > 0 ? port - 1 : 0);
    if (snd_card_get_index(card) < 0)
        snprintf(card, sizeof card, "vc4hdmi");
    /* "hdmi:" wraps the IEC958 framing the vc4 HDMI audio needs. */
    snprintf(dev, n, "hdmi:CARD=%s,DEV=0", card);
}

/* After opening or an underrun: queue the target delay as silence, so frames that arrive
 * with some jitter (or in large chunks) find audio still playing. */
static void audio_prefill(struct output *o)
{
    static const int16_t zero[2 * 480] = { 0 };
    snd_pcm_uframes_t left = o->au.target;
    while (left > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(o->au.pcm, zero, left < 480 ? left : 480);
        if (w <= 0)
            break;
        left -= (snd_pcm_uframes_t)w;
    }
}

static bool audio_open(struct output *o, int rate)
{
    char dev[64];
    audio_device(o, dev, sizeof dev);
    const char *where = !strcasecmp(o->audio_s, "analog") ? "headphone jack" : "HDMI";
    snd_pcm_t *pcm = NULL;
    snd_pcm_uframes_t buffer = 0, period = 0;
    int target_ms = o->audio_delay_ms;
    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (!err)
        /* Room for the target, the slack and a large NDI audio frame (some senders use 100 ms). */
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1,
                                 (unsigned)(target_ms + 300) * 1000);
    if (!err)
        err = snd_pcm_get_params(pcm, &buffer, &period);
    if (!err) {
        /* Keep the level just before each write between target and target + slack (see audio_play). */
        snd_pcm_sw_params_t *sw;
        snd_pcm_sw_params_alloca(&sw);
        o->au.target = (snd_pcm_uframes_t)rate * (target_ms + o->au.extra_ms) / 1000;
        o->au.slack = (snd_pcm_uframes_t)rate * 25 / 1000;
        if (o->au.target + o->au.slack > buffer)
            o->au.target = buffer > o->au.slack ? buffer - o->au.slack : buffer / 2;
        err = snd_pcm_sw_params_current(pcm, sw);
        if (!err)
            err = snd_pcm_sw_params_set_start_threshold(pcm, sw, o->au.target);
        if (!err)
            err = snd_pcm_sw_params(pcm, sw);
    }
    if (err) {
        if (pcm)
            snd_pcm_close(pcm);
        LOG("audio: output %d: cannot play on %s (%s): %s", o->n, where, dev, snd_strerror(err));
        audio_info(o, "cannot play on the %s: %s", where, snd_strerror(err));
        o->au.retry_at = now_ms() + 10000;
        return false;
    }
    o->au.pcm = pcm;
    o->au.rate = rate;
    audio_prefill(o);
    if (rate != o->au.logged_rate) /* senders that pause their audio reopen often: log once */
        LOG("audio: output %d: %d Hz on %s (%s), %d ms delay", o->n, rate, where, dev, target_ms + o->au.extra_ms);
    o->au.logged_rate = rate;
    return true;
}

/* An underrun: the sender's audio arrives with more jitter than the delay covers. Add 20 ms
 * (up to 300 ms extra) for this source; the web UI shows the delay actually used. */
static void audio_grow(struct output *o)
{
    if (o->au.extra_ms >= 300)
        return;
    o->au.extra_ms += 20;
    snd_pcm_uframes_t add = (snd_pcm_uframes_t)o->au.rate * 20 / 1000, buffer, period;
    if (!snd_pcm_get_params(o->au.pcm, &buffer, &period) && o->au.target + add + o->au.slack <= buffer)
        o->au.target += add;
    LOG("audio: output %d: underrun, delay now %d ms", o->n, o->audio_delay_ms + o->au.extra_ms);
}

/* Play an NDI audio frame (planar float, any channel count) as 16 bit stereo. */
static void audio_play(struct output *o, const NDIlib_audio_frame_v3_t *a)
{
    uint64_t tnow = now_ms();
    if (o->au.last_frame && tnow - o->au.last_frame > o->au.max_gap && tnow - o->au.last_frame < 5000)
        o->au.max_gap = tnow - o->au.last_frame;
    /* A long wait for audio: say whether the sender sent late too (its timestamps have the same gap)
     * or sent on time and the audio was held up on the way (network, NDI). */
    if (o->au.last_frame && tnow - o->au.last_frame > 150 && tnow - o->au.last_frame < 60000 && o->au.last_sent)
        LOG("audio: output %d: no audio for %llu ms; the sender sent it %.0f ms apart", o->n,
            (unsigned long long)(tnow - o->au.last_frame), (double)(a->timestamp - o->au.last_sent) / 1e4);
    o->au.last_sent = a->timestamp;
    o->au.last_frame = tnow;
    o->au.frames++;
    o->au.in += a->no_samples > 0 ? a->no_samples : 0;
    if (a->FourCC != NDIlib_FourCC_audio_type_FLTP || a->no_samples <= 0 || a->no_channels <= 0 ||
        a->sample_rate <= 0)
        return;
    const char *where = !strcasecmp(o->audio_s, "analog") ? "headphone jack" : "HDMI";
    if (o->au.pcm && o->au.rate != a->sample_rate)
        audio_close(o);
    if (!o->au.pcm && (now_ms() < o->au.retry_at || !audio_open(o, a->sample_rate)))
        return;
    audio_info(o, "%g kHz, %d channel%s%s → %s, %d ms delay%s", a->sample_rate / 1000.0, a->no_channels,
               a->no_channels == 1 ? "" : "s", a->no_channels > 2 ? " (first 2 played)" : "", where,
               o->audio_delay_ms + o->au.extra_ms, o->au.extra_ms ? " (raised: the audio arrives unevenly)" : "");

    size_t n = (size_t)a->no_samples;
    if (o->au.cap < n * 2) {
        free(o->au.buf);
        o->au.cap = n * 2;
        o->au.buf = malloc(o->au.cap * sizeof(int16_t));
        if (!o->au.buf) {
            o->au.cap = 0;
            return;
        }
    }
    const float *l = (const float *)a->p_data;
    const float *r = a->no_channels > 1 ? (const float *)(a->p_data + a->channel_stride_in_bytes) : l;
    for (size_t i = 0; i < n; i++) {
        float lv = l[i] * 32767.0f, rv = r[i] * 32767.0f;
        o->au.buf[i * 2] = (int16_t)(lv > 32767.0f ? 32767 : lv < -32768.0f ? -32768 : lv);
        o->au.buf[i * 2 + 1] = (int16_t)(rv > 32767.0f ? 32767 : rv < -32768.0f ? -32768 : rv);
    }

    /* Keep the queue just before a write near the target: if it has grown beyond target + slack
     * (sender clock faster than ours), drop the oldest part of this frame. An underrun restarts
     * with the target delay of silence. */
    const int16_t *p = o->au.buf;
    snd_pcm_sframes_t avail = snd_pcm_avail_update(o->au.pcm);
    snd_pcm_uframes_t buffer, period;
    if (avail == -EPIPE || avail == -ESTRPIPE) {
        o->au.xruns++;
        audio_grow(o);
        snd_pcm_prepare(o->au.pcm);
        audio_prefill(o);
        avail = snd_pcm_avail_update(o->au.pcm);
    }
    if (avail < 0 || snd_pcm_get_params(o->au.pcm, &buffer, &period)) {
        LOG("audio: output %d: device error (%s), reopening", o->n, snd_strerror((int)avail));
        audio_close(o);
        return;
    }
    snd_pcm_uframes_t queued = buffer > (snd_pcm_uframes_t)avail ? buffer - avail : 0;
    /* Senders deliver audio in bursts; only a queue that stays above target + slack for a whole
     * 2 s window means their clock runs faster than ours. Then trim the excess. */
    if (!o->au.win_start || queued < o->au.win_min)
        o->au.win_min = queued;
    if (!o->au.win_start)
        o->au.win_start = tnow;
    if (tnow - o->au.win_start >= 2000) {
        if (o->au.win_min > o->au.target + o->au.slack)
            o->au.pending_drop = o->au.win_min - o->au.target;
        o->au.win_start = tnow;
        o->au.win_min = queued;
    }
    if (o->au.pending_drop) {
        size_t drop = o->au.pending_drop;
        if (drop > n)
            drop = n;
        o->au.pending_drop -= drop;
        p += drop * 2;
        n -= drop;
        o->au.dropped += drop;
    }
    while (n > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(o->au.pcm, p, n);
        if (w == -EAGAIN) {
            o->au.dropped += n; /* full: drop the rest */
            break;
        }
        if (w < 0) {
            if (w == -EPIPE) {
                o->au.xruns++;
                audio_grow(o);
            }
            if (snd_pcm_recover(o->au.pcm, (int)w, 1) < 0) {
                LOG("audio: output %d: %s, reopening", o->n, snd_strerror((int)w));
                audio_close(o);
                return;
            }
            audio_prefill(o);
            continue;
        }
        p += w * 2;
        n -= (size_t)w;
    }
}

/* ------------------------------------------------------------------ NDI receiver (one thread per output) */

/* NDI names look like "HOST (STREAM)". Return a pointer into name for STREAM, length in *len. */
static const char *stream_part(const char *name, size_t *len)
{
    const char *p = strstr(name, " (");
    size_t n = strlen(name);
    if (!p || n < 2 || name[n - 1] != ')') {
        *len = n;
        return name;
    }
    p += 2;
    *len = (size_t)(name + n - 1 - p);
    return p;
}

static bool name_matches(const char *ndi_name, const char *target, const char *alt)
{
    size_t len;
    const char *s = stream_part(ndi_name, &len);
    return (strlen(target) == len && !strncasecmp(s, target, len)) ||
           (alt[0] && strlen(alt) == len && !strncasecmp(s, alt, len));
}

/* "192.168.1.5:5961" -> true if the host part equals ip. */
static bool url_has_ip(const char *url, const char *ip)
{
    size_t n = strlen(ip);
    return url && !strncmp(url, ip, n) && (url[n] == ':' || url[n] == 0);
}

static uint64_t start_ms;
static void set_target(struct output *o, const char *stream, const char *source_ip, const char *backup);

/* Live source switching (web UI / API): out<n>.source holds "stream=", "source_ip=", "backup=" lines.
 * Returns 1 if the target changed, 2 if the source IP changed too (discovery must restart). */
static int switch_poll(struct output *o, uint64_t *seen)
{
    char path[64], buf[1024], stream[160] = "", ip[64] = "", backup[160] = "";
    struct stat st;
    snprintf(path, sizeof path, CTL_DIR "/out%d.source", o->n);
    if (stat(path, &st))
        return 0;
    uint64_t stamp = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    if (stamp == *seen || stamp == o->source_stale)
        return 0;
    *seen = stamp;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        if (!strncmp(buf, "stream=", 7))
            snprintf(stream, sizeof stream, "%.159s", buf + 7);
        else if (!strncmp(buf, "source_ip=", 10))
            snprintf(ip, sizeof ip, "%.63s", buf + 10);
        else if (!strncmp(buf, "backup=", 7))
            snprintf(backup, sizeof backup, "%.159s", buf + 7);
    }
    fclose(f);
    char old_target[160], old_ip[64], old_backup[160];
    snprintf(old_target, sizeof old_target, "%s", o->target);
    snprintf(old_ip, sizeof old_ip, "%s", o->source_ip);
    snprintf(old_backup, sizeof old_backup, "%s", o->backup);
    set_target(o, stream, ip, backup);
    if (!strcmp(old_target, o->target) && !strcmp(old_ip, o->source_ip) && !strcmp(old_backup, o->backup))
        return 0;
    LOG("ndi: output %d: switching to %s from %s%s%s", o->n, o->target, *o->source_ip ? o->source_ip : "any host",
        *o->backup ? ", backup " : "", o->backup);
    return strcmp(old_ip, o->source_ip) ? 2 : 1;
}

/* The output's main stream among the visible sources (preferring one that has not just failed). */
static const NDIlib_source_t *find_primary(struct output *o, const NDIlib_source_t *srcs, uint32_t count,
                                           const char *failed_src)
{
    const NDIlib_source_t *pick = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (!srcs[i].p_ndi_name || !name_matches(srcs[i].p_ndi_name, o->target, o->alt))
            continue;
        if (*o->source_ip && !url_has_ip(srcs[i].p_url_address, o->source_ip))
            continue;
        if (!pick || !strcmp(pick->p_ndi_name, failed_src))
            pick = &srcs[i];
    }
    return pick;
}

/* Which codec a sender uses (SpeedHQ or NDI|HX) is hidden by the decoding receiver. With the
 * Advanced SDK, a second receiver asking for compressed frames gets them undecoded, and the
 * FourCC names the codec. It connects once per stream, reads one frame and goes away. */
struct codec_probe {
    struct output *o;
    NDIlib_recv_bandwidth_e bandwidth;
    char name[256], url[128];
};

static const char *codec_name(uint32_t cc)
{
    cc &= ~0x80808080u; /* protected variants set the top bit of each byte */
    char c[5] = { (char)toupper(cc & 0xff), (char)toupper(cc >> 8 & 0xff), (char)toupper(cc >> 16 & 0xff),
                  (char)toupper(cc >> 24 & 0xff), 0 };
    if (!strcmp(c, "SHQ0")) return "Full NDI (SpeedHQ 4:2:0)";
    if (!strcmp(c, "SHQ2")) return "Full NDI (SpeedHQ 4:2:2)";
    if (!strcmp(c, "SHQ7")) return "Full NDI (SpeedHQ 4:2:2:4)";
    if (!strcmp(c, "H264") || !strcmp(c, "A264")) return "NDI|HX (H.264)";
    if (!strcmp(c, "HEVC") || !strcmp(c, "AEVC")) return "NDI|HX (HEVC)";
    return NULL; /* uncompressed video (e.g. from the SDK's own decoder) tells nothing */
}

static bool codec_is_h264(uint32_t cc)
{
    const char *c = codec_name(cc);
    return c && !strcmp(c, "NDI|HX (H.264)");
}

/* ------------------------------------------------------------------ this device's own capture inputs */

/* pindi-sender publishes each input's pictures and audio in /run/pindi/in<n>.local. An output showing
 * one of them reads it from there: no NDI encoding and decoding on the same Pi (a Pi 3 can't do both
 * at 1080p60). Keep in sync with pindi-sender.c. */
#define LOCAL_MAGIC 0x4c444e50 /* "PNDL" */
#define LOCAL_SLOTS 4
#define LOCAL_AUDIO 32768
#define LOCAL_DATA 262144
enum { LOCAL_422P = 1, LOCAL_420P = 2 };

struct local_feed {
    uint32_t magic, version;
    char name[128];
    int32_t w, h, fps_n, fps_d;
    uint32_t fmt, slot_size;
    uint64_t seq;
    uint64_t slot_seq[LOCAL_SLOTS];
    int64_t slot_time[LOCAL_SLOTS];
    uint64_t writer_ms, reader_ms;
    uint64_t audio_wr;
    int16_t audio[LOCAL_AUDIO * 2];
};

#define LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* Empty, 127.0.0.1 or one of this device's addresses. */
static bool ip_is_local(const char *ip)
{
    if (!*ip || !strcmp(ip, "127.0.0.1"))
        return true;
    struct ifaddrs *ifa, *p;
    bool local = false;
    if (getifaddrs(&ifa))
        return false;
    for (p = ifa; p && !local; p = p->ifa_next) {
        char a[INET_ADDRSTRLEN];
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET &&
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, a, sizeof a))
            local = !strcmp(a, ip);
    }
    freeifaddrs(ifa);
    return local;
}

/* The live local feed the output's stream names: its path in path, else false. */
static bool local_find(struct output *o, char *path, size_t n)
{
    if (!*o->target || !ip_is_local(o->source_ip))
        return false;
    for (int i = 1; i <= 8; i++) {
        char p[64], full[160];
        struct local_feed hdr;
        snprintf(p, sizeof p, RUN_DIR "/in%d.local", i);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t r = pread(fd, &hdr, offsetof(struct local_feed, audio), 0);
        close(fd);
        if (r != (ssize_t)offsetof(struct local_feed, audio) || hdr.magic != LOCAL_MAGIC || !hdr.writer_ms ||
            now_ms() - hdr.writer_ms > 3000)
            continue;
        hdr.name[sizeof hdr.name - 1] = 0;
        snprintf(full, sizeof full, "LOCAL (%s)", hdr.name);
        if (name_matches(full, o->target, o->alt)) {
            snprintf(path, n, "%s", p);
            return true;
        }
    }
    return false;
}

/* Show a local feed until the output's stream is switched (returns switch_poll's result), the
 * sender stops or the display goes away (0). */
static int local_play(struct output *o, const char *path, uint64_t *switch_seen, NDIlib_find_instance_t find,
                      bool audio_on, int stats_ms)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    uint8_t *map = NULL;
    size_t len = 0;
    struct local_feed *lf = NULL;
    char src[200] = "";
    uint64_t start = now_ms(), last_seq = 0, last_switch = 0, last_sources = 0, last_stats = start, audio_rd = 0;
    unsigned long shown = 0, torn = 0;
    bool got = false;
    float *abuf = malloc(960 * 2 * sizeof(float));
    int ret = 0;
    while (running && abuf) {
        uint64_t now = now_ms();
        pthread_mutex_lock(&o->lock);
        bool usable = o->usable;
        pthread_mutex_unlock(&o->lock);
        if (!usable)
            break;
        if (now - last_switch >= 300) {
            last_switch = now;
            if ((ret = switch_poll(o, switch_seen)))
                break;
        }
        if (find && o->n == 1 && now - last_sources >= 2000) {
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            sources_publish(srcs, count);
            last_sources = now;
        }
        struct stat st;
        if (fstat(fd, &st))
            break;
        if ((size_t)st.st_size != len) { /* new capture mode: the sender resized the frame slots */
            if (map)
                munmap(map, len);
            len = (size_t)st.st_size;
            map = len >= LOCAL_DATA ? mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : NULL;
            if (map == MAP_FAILED)
                map = NULL;
            if (!map)
                break;
            lf = (struct local_feed *)map;
            if (!*src) {
                char name[sizeof lf->name];
                memcpy(name, lf->name, sizeof name);
                name[sizeof name - 1] = 0;
                snprintf(src, sizeof src, "this device (%s)", name);
                LOG("local: output %d: showing %s straight from the capture input (no NDI)", o->n, name);
            }
        }
        if (LOAD(lf->magic) != LOCAL_MAGIC)
            break;
        STORE(lf->reader_ms, now); /* the sender decodes only while someone reads */
        uint64_t wms = LOAD(lf->writer_ms);
        if (!wms || now - wms > 3000) {
            LOG("local: output %d: %s stopped", o->n, src);
            break;
        }

        uint64_t s = LOAD(lf->seq);
        if (s && s != last_seq) {
            last_seq = s;
            int slot = (int)(s % LOCAL_SLOTS);
            uint32_t ssz = LOAD(lf->slot_size);
            int w = lf->w, h = lf->h;
            bool is420 = lf->fmt == LOCAL_420P;
            if (LOAD(lf->slot_seq[slot]) == s && w > 0 && h > 0 && w <= 8192 && h <= 8192 &&
                ssz >= (uint32_t)w * h * 2 &&
                LOCAL_DATA + (size_t)ssz * LOCAL_SLOTS <= len) {
                const uint8_t *base = map + LOCAL_DATA + (size_t)slot * ssz;
                size_t ysz = (size_t)w * h, csz = is420 ? ysz / 4 : ysz / 2;
                bool ok = post_planar(o, base, base + ysz, base + ysz + csz, w, h, is420, lf->slot_time[slot],
                                      clock_us(CLOCK_MONOTONIC));
                if (LOAD(lf->slot_seq[slot]) != s)
                    torn++; /* overwritten while copying (we fell 4 frames behind): a mixed picture */
                char vid[64];
                snprintf(vid, sizeof vid, "%dx%d %.2f fps", w, h, lf->fps_d ? (double)lf->fps_n / lf->fps_d : 0.0);
                status_set(o, ok ? "playing" : "cannot display (format)", src, "this device", vid);
                shown++;
                if (!got) {
                    char buf[512], cpath[64];
                    snprintf(buf, sizeof buf, "source=%s\ncodec=Direct from the capture input (no NDI)\n", src);
                    snprintf(cpath, sizeof cpath, RUN_DIR "/out%d.codec", o->n);
                    write_file_atomic(cpath, buf);
                    LOG("local: output %d: first frame after %llu ms (%dx%d)", o->n,
                        (unsigned long long)(now - start), w, h);
                    got = true;
                }
            }
        }

        if (audio_on) { /* the input's audio: 48 kHz stereo, played like an NDI stream's */
            uint64_t wr = LOAD(lf->audio_wr);
            if (!audio_rd || wr < audio_rd || wr - audio_rd > LOCAL_AUDIO / 2)
                audio_rd = wr; /* start (or resync) at the newest samples */
            while (audio_rd < wr) {
                size_t n = (size_t)(wr - audio_rd);
                if (n > 960)
                    n = 960;
                for (size_t i = 0; i < n; i++) {
                    size_t at = (size_t)((audio_rd + i) % LOCAL_AUDIO) * 2;
                    abuf[i] = lf->audio[at] / 32768.0f;
                    abuf[n + i] = lf->audio[at + 1] / 32768.0f;
                }
                NDIlib_audio_frame_v3_t a = {
                    .sample_rate = 48000, .no_channels = 2, .no_samples = (int)n,
                    .FourCC = NDIlib_FourCC_audio_type_FLTP, .p_data = (uint8_t *)abuf,
                    .channel_stride_in_bytes = (int)(n * sizeof(float)),
                };
                audio_play(o, &a);
                audio_rd += n;
            }
            if (got && now - o->au.last_frame > 3000 && now - start > 3000 && o->au.pcm) {
                audio_close(o);
                audio_info(o, "no audio in this stream");
            }
        }

        if (stats_ms > 0 && now - last_stats >= (uint64_t)stats_ms) {
            LOG("local: output %d: %lu frames in %.0f s from %s%s", o->n, shown, (now - last_stats) / 1000.0, src,
                torn ? " (some overwritten while copying)" : "");
            shown = torn = 0;
            last_stats = now;
        }
        usleep(2000);
    }
    audio_close(o);
    free(abuf);
    if (map)
        munmap(map, len);
    close(fd);
    return ret;
}

static void *codec_probe_thread(void *arg)
{
    struct codec_probe *p = arg;
    NDIlib_source_t src = { .p_ndi_name = p->name, .p_url_address = *p->url ? p->url : NULL };
    NDIlib_recv_create_v3_t rc = {
        .source_to_connect_to = src,
        .color_format = (NDIlib_recv_color_format_e)307, /* NDIlib_recv_color_format_compressed_v5 */
        .bandwidth = p->bandwidth,
        .allow_video_fields = false,
        .p_ndi_recv_name = "PiNDI format check",
    };
    NDIlib_recv_instance_t r = ndi->recv_create_v3(&rc);
    const char *codec = NULL;
    uint64_t until = now_ms() + 5000;
    while (r && running && !codec && now_ms() < until) {
        NDIlib_video_frame_v2_t v;
        if (ndi->recv_capture_v3(r, &v, NULL, NULL, 500) == NDIlib_frame_type_video) {
            codec = codec_name((uint32_t)v.FourCC);
            if (!codec)
                codec = "";
            ndi->recv_free_video_v2(r, &v);
        }
    }
    if (r)
        ndi->recv_destroy(r);
    if (codec && *codec) {
        char buf[512], path[64];
        snprintf(buf, sizeof buf, "source=%s\ncodec=%s\n", p->name, codec);
        snprintf(path, sizeof path, RUN_DIR "/out%d.codec", p->o->n);
        write_file_atomic(path, buf);
        LOG("ndi: output %d: %s sends %s", p->o->n, p->name, codec);
    }
    pthread_mutex_lock(&p->o->lock);
    if (codec && *codec) {
        p->o->probe_codec = codec;
        snprintf(p->o->probe_src, sizeof p->o->probe_src, "%s", p->name);
    }
    p->o->probing = false;
    pthread_mutex_unlock(&p->o->lock);
    free(p);
    return NULL;
}

/* One check at a time. min_gap_ms: skip it if the last one started more recently than that - the
 * check's own connection to the sender briefly takes the receiver's down when it ends (the NDI library
 * shares it), which would otherwise look like "the sender came back" and start the next one, endlessly,
 * cutting the audio each time. False: skipped for that reason (ask again later). */
static bool codec_probe_start(struct output *o, NDIlib_recv_bandwidth_e bandwidth, const char *name,
                              const char *url, uint64_t min_gap_ms)
{
    uint64_t now = now_ms();
    pthread_mutex_lock(&o->lock);
    bool running_now = o->probing, too_soon = !running_now && o->probe_ms && now - o->probe_ms < min_gap_ms;
    if (!running_now && !too_soon) {
        o->probing = true;
        o->probe_ms = now;
        o->probe_codec = NULL;
    }
    pthread_mutex_unlock(&o->lock);
    if (running_now || too_soon)
        return !too_soon;
    struct codec_probe *p = calloc(1, sizeof *p);
    pthread_t t;
    if (!p) {
        pthread_mutex_lock(&o->lock);
        o->probing = false;
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    p->o = o;
    p->bandwidth = bandwidth;
    snprintf(p->name, sizeof p->name, "%s", name);
    snprintf(p->url, sizeof p->url, "%s", url);
    if (pthread_create(&t, NULL, codec_probe_thread, p) == 0) {
        pthread_detach(t);
    } else {
        free(p);
        pthread_mutex_lock(&o->lock);
        o->probing = false;
        pthread_mutex_unlock(&o->lock);
    }
    return true;
}

/* Leader thread: an audio frame for the outputs following o (each plays it with its own settings). */
static void share_audio(struct output *o, const NDIlib_audio_frame_v3_t *a)
{
    pthread_mutex_lock(&share_mu);
    for (int i = 0; i < nout; i++)
        if (outs[i].leader == o && strcasecmp(outs[i].audio_s, "off"))
            audio_play(&outs[i], a);
    pthread_mutex_unlock(&share_mu);
}

static void *receiver(void *arg)
{
    struct output *o = arg;
    if (o->disabled) {
        status_set(o, "disabled", "", "", "");
        return NULL;
    }
    const char *bw_s = ocfg(o->n, "BANDWIDTH", cfg("PINDI_BANDWIDTH", "highest"));
    NDIlib_recv_bandwidth_e bandwidth =
        !strcasecmp(bw_s, "lowest") ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
    /* Many senders (screen capture, static graphics) only send frames on change. */
    int frame_timeout = atoi(cfg("PINDI_FRAME_TIMEOUT_MS", "0"));
    int stats_ms = atoi(cfg("PINDI_STATS_SEC", "60")) * 1000;
    /* A fixed source IP is also given to discovery directly, so it works across subnets. */
    char extra_ips[512];
    const char *extra = cfg("PINDI_EXTRA_IPS", "");
    snprintf(extra_ips, sizeof extra_ips, "%s%s%s", extra, *o->source_ip && *extra ? "," : "", o->source_ip);
    uint64_t switch_seen = 0;
    uint64_t switch_check = 0;
    char recv_name[128];
    snprintf(recv_name, sizeof recv_name, nout > 1 ? "PiNDI %s output %d" : "PiNDI %s", serial, o->n);
    /* The plane decides the pixel format: YUV straight through if it can, else RGB from the SDK. */
    NDIlib_recv_color_format_e color = (o->has_uyvy || o->has_nv16) ? NDIlib_recv_color_format_UYVY_BGRA
                                                                      : NDIlib_recv_color_format_BGRX_BGRA;
    bool audio_on = strcasecmp(o->audio_s, "off") != 0;
    audio_info(o, audio_on ? "" : "off");

    NDIlib_find_instance_t find = NULL;
    NDIlib_recv_instance_t recv = NULL;
    uint64_t find_created = 0, last_match = 0, recv_start = 0, last_alive = 0, last_frame = 0;
    bool find_no_ip = false; /* the finder was made while the device had no IPv4 address */
    uint64_t ip_check = 0;
    uint64_t last_stats = 0, last_sources = 0;
    uint32_t last_count = UINT32_MAX;
    char cur_src[256] = "", cur_url[128] = "", failed_src[256] = "", last_src[256] = "";
    bool got_frame = false, on_backup = false;
    uint64_t search_since = 0, primary_check = 0, splash_at = 0;
    /* NDI|HX H.264 through the hardware decoder (see hwdec_*): the receiver then gets compressed frames. */
    struct hwdec hw = { .fd = -1 };
    bool hx_mode = false;
    char hw_failed[256] = ""; /* the source the hardware decoder failed on: the NDI library decodes it */
    int last_w = 0, last_h = 0;
    uint64_t key_asked = 0;
    int prev_conns = -1; /* connections to the sender at the last check; -1: none yet */
    bool recheck = false; /* the sender reconnected: check its codec once the last check is a minute old */
    uint64_t drop_check = 0;  /* when the dropped-frame count was last looked at (0: not yet) */
    int64_t drops_seen = 0;   /* the receiver's dropped video frames then */
    bool following = false; /* showing another output's receiver (decode once) */
    uint64_t codec_copy_at = 0;
    char codec_copied[512] = ""; /* the leader's out<n>.codec as last copied to ours */

    while (running) {
        uint64_t now = now_ms();
        if (!recv && hx_mode) {
            hwdec_close(o, &hw);
            hx_mode = false;
        }
        if (!recv && *o->share_src)
            share_release(o); /* our receiver is gone: the outputs following it search on their own */

        /* Following another output's receiver: its frames and audio come to this output by themselves.
         * Stop when it stops receiving, this display goes away or this output gets another stream. */
        if (following) {
            char src[256] = "", url[128] = "", vid[64] = "";
            int lead_n = 0;
            pthread_mutex_lock(&share_mu);
            struct output *L = o->leader;
            if (L) {
                lead_n = L->n;
                snprintf(src, sizeof src, "%s", L->share_src);
                snprintf(url, sizeof url, "%s", L->share_url);
                snprintf(vid, sizeof vid, "%s", L->share_video);
            }
            pthread_mutex_unlock(&share_mu);
            pthread_mutex_lock(&o->lock);
            bool use = o->usable;
            pthread_mutex_unlock(&o->lock);
            int sw = 0;
            if (L && use && now - switch_check >= 300) {
                switch_check = now;
                sw = switch_poll(o, &switch_seen);
            }
            if (L && use && !sw) {
                status_set(o, on_backup ? "playing (backup)" : "playing", src, url, vid);
                if (now - codec_copy_at >= 1000) { /* the leader checks the codec: show its answer here too */
                    codec_copy_at = now;
                    char path[64], buf[512] = "";
                    snprintf(path, sizeof path, RUN_DIR "/out%d.codec", lead_n);
                    FILE *f = fopen(path, "r");
                    if (f) {
                        buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
                        fclose(f);
                    }
                    if (*buf && strcmp(buf, codec_copied)) {
                        snprintf(path, sizeof path, RUN_DIR "/out%d.codec", o->n);
                        write_file_atomic(path, buf);
                        snprintf(codec_copied, sizeof codec_copied, "%s", buf);
                    }
                }
                usleep(50000);
                continue;
            }
            share_leave(o);
            following = false;
            codec_copied[0] = 0;
            audio_close(o);
            if (audio_on)
                audio_info(o, "");
            search_since = now;
            failed_src[0] = 0;
            if (!L) {
                LOG("ndi: output %d: the receiver it showed stopped", o->n);
                if (*o->backup) {
                    splash_at = now + 6000; /* hold the picture while the other stream takes over */
                    if (!on_backup)
                        search_since = now - 3000;
                } else {
                    request_splash(o);
                }
            } else if (sw) {
                splash_at = now + 6000; /* keep the picture until the new stream's first frame */
                if (sw == 2 && find) {
                    snprintf(extra_ips, sizeof extra_ips, "%s%s%s", extra, *o->source_ip && *extra ? "," : "",
                             o->source_ip);
                    ndi->find_destroy(find);
                    find = NULL;
                }
            }
            on_backup = false;
            status_set(o, "searching", "", "", "");
            continue;
        }
        bool usable;
        pthread_mutex_lock(&o->lock);
        usable = o->usable;
        pthread_mutex_unlock(&o->lock);

        /* Display in standby: keep looking for our stream, and wake the display when it is there. */
        bool standby;
        pthread_mutex_lock(&o->lock);
        standby = !usable && o->standby;
        pthread_mutex_unlock(&o->lock);
        if (standby) {
            if (!find) { /* discovery may have been restarted just before the display went off */
                NDIlib_find_create_t fc = { .show_local_sources = true, .p_groups = NULL,
                                            .p_extra_ips = *extra_ips ? extra_ips : NULL };
                find = ndi->find_create_v2(&fc);
                find_created = last_match = now;
                if (!find) {
                    usleep(1000000);
                    continue;
                }
            }
            if (recv) {
                ndi->recv_destroy(recv);
                recv = NULL;
                audio_close(o);
            }
            status_set(o, "standby (display off)", "", "", "");
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            if (o->n == 1)
                sources_publish(srcs, count);
            bool found = find_primary(o, srcs, count, "") != NULL;
            for (uint32_t i = 0; i < count && !found && *o->backup; i++)
                found = srcs[i].p_ndi_name && name_matches(srcs[i].p_ndi_name, o->backup, "");
            if (found) {
                pthread_mutex_lock(&o->lock);
                o->wake_request = true;
                pthread_mutex_unlock(&o->lock);
                wake();
            }
            usleep(500000);
            continue;
        }

        /* No monitor on this output: don't receive anything. */
        if (!usable) {
            if (recv) {
                LOG("ndi: output %d: monitor gone, disconnecting %s", o->n, cur_src);
                ndi->recv_destroy(recv);
                recv = NULL;
                audio_close(o);
                if (audio_on)
                    audio_info(o, "");
            }
            status_set(o, "no monitor", "", "", "");
            usleep(300000);
            continue;
        }

        /* Live switch to another stream: keep the picture until the new one's first frame (max 6 s). */
        if (now - switch_check >= 300) {
            switch_check = now;
            int sw = switch_poll(o, &switch_seen);
            if (sw) {
                if (recv) {
                    ndi->recv_destroy(recv);
                    recv = NULL;
                    audio_close(o);
                    if (got_frame)
                        splash_at = now + 6000;
                    else
                        request_splash(o);
                }
                on_backup = false;
                search_since = now;
                failed_src[0] = 0;
                status_set(o, "searching", "", "", "");
                if (sw == 2 && find) {
                    snprintf(extra_ips, sizeof extra_ips, "%s%s%s", extra, *o->source_ip && *extra ? "," : "",
                             o->source_ip);
                    ndi->find_destroy(find);
                    find = NULL;
                }
            }
        }

        /* A finder made before the device had an address (at boot, before the link is up) asks the
         * network again only after several seconds: make a new one as soon as there is an address. */
        if (find && find_no_ip && now - ip_check >= 250) {
            char ip[64];
            ip_check = now;
            read_ip(ip, sizeof ip);
            if (strcmp(ip, "no network")) {
                LOG("ndi: output %d: network up (%s), restarting discovery", o->n, ip);
                ndi->find_destroy(find);
                find = NULL;
            }
        }

        /* Discovery needs avahi; give it up to 20 s to appear after boot. */
        if (!find) {
            if (access("/run/avahi-daemon/socket", F_OK) && now - start_ms < 20000) {
                status_set(o, "waiting for network", "", "", "");
                usleep(100000);
                continue;
            }
            NDIlib_find_create_t fc = { .show_local_sources = true, .p_groups = NULL,
                                        .p_extra_ips = *extra_ips ? extra_ips : NULL };
            find = ndi->find_create_v2(&fc);
            find_created = last_match = now;
            last_count = UINT32_MAX;
            char ip[64];
            read_ip(ip, sizeof ip);
            find_no_ip = !strcmp(ip, "no network");
            if (!find) {
                LOG("ndi: output %d: cannot create finder", o->n);
                usleep(1000000);
                continue;
            }
        }

        /* Keep the visible-sources list fresh for the web UI while playing too. */
        if (recv && now - last_sources >= 2000) {
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            if (o->n == 1)
                sources_publish(srcs, count);
            last_sources = now;
            /* The sender restarted and our stream now has another address (its inputs can come back on
             * each other's ports): the receiver would reconnect to the old one, i.e. to another stream. */
            for (uint32_t i = 0; i < count; i++)
                if (srcs[i].p_ndi_name && !strcmp(srcs[i].p_ndi_name, cur_src) && srcs[i].p_url_address &&
                    *cur_url && strcmp(srcs[i].p_url_address, cur_url)) {
                    LOG("ndi: output %d: %s moved from %s to %s, reconnecting", o->n, cur_src, cur_url,
                        srcs[i].p_url_address);
                    ndi->recv_destroy(recv);
                    recv = NULL;
                    break;
                }
            if (!recv) {
                audio_close(o);
                splash_at = now + 6000; /* keep the picture until the stream's first frame */
                search_since = now;
                continue;
            }
            /* Our stream is one of this device's inputs again (pindi-sender restarted): take it from there. */
            char lpath[64];
            if (!on_backup && local_find(o, lpath, sizeof lpath)) {
                LOG("ndi: output %d: %s is this device's own input, showing it directly", o->n, cur_src);
                ndi->recv_destroy(recv);
                recv = NULL;
                audio_close(o);
                continue;
            }
        }

        if (!recv) {
            /* One of this device's own capture inputs: straight from pindi-sender, not over NDI. */
            char lpath[64];
            if (local_find(o, lpath, sizeof lpath)) {
                int sw = local_play(o, lpath, &switch_seen, find, audio_on, stats_ms);
                now = now_ms();
                if (audio_on)
                    audio_info(o, "");
                if (sw) {
                    splash_at = now + 6000; /* keep the picture until the new stream's first frame */
                    search_since = now;
                    failed_src[0] = 0;
                    status_set(o, "searching", "", "", "");
                    if (sw == 2 && find) {
                        snprintf(extra_ips, sizeof extra_ips, "%s%s%s", extra, *o->source_ip && *extra ? "," : "",
                                 o->source_ip);
                        ndi->find_destroy(find);
                        find = NULL;
                    }
                } else {
                    request_splash(o);
                }
                continue;
            }
            if (!search_since)
                search_since = now;
            /* With a backup, the last picture stays up while it takes over (at most 6 s). */
            if (splash_at && now >= splash_at) {
                request_splash(o);
                splash_at = 0;
            }
            status_set(o, "searching", "", "", "");
            ndi->find_wait_for_sources(find, 250);
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            sources_publish(srcs, count);
            if (count != last_count && o->n == 1) {
                LOG("ndi: %u source(s) visible", count);
                for (uint32_t i = 0; i < count && i < 20; i++)
                    LOG("ndi:   %s", srcs[i].p_ndi_name);
                if (count > 20)
                    LOG("ndi:   ... and %u more", count - 20);
            }
            last_count = count;
            const NDIlib_source_t *pick = find_primary(o, srcs, count, failed_src);
            bool backup = false;
            /* The main stream gets 3 s to appear before the backup is used. */
            if (!pick && *o->backup && now - search_since >= 3000) {
                for (uint32_t i = 0; i < count && !pick; i++)
                    if (srcs[i].p_ndi_name && name_matches(srcs[i].p_ndi_name, o->backup, "") &&
                        strcmp(srcs[i].p_ndi_name, failed_src))
                        pick = &srcs[i];
                backup = pick != NULL;
            }
            if (pick) {
                /* Another output already receives it: show its frames instead of decoding it again. */
                struct output *L = share_claim(o, pick->p_ndi_name, pick->p_url_address ? pick->p_url_address : "",
                                               (int)bandwidth);
                if (L) {
                    LOG("ndi: output %d: %s is already received by output %d, showing it from there (decoded once)",
                        o->n, pick->p_ndi_name, L->n);
                    following = true;
                    on_backup = backup;
                    search_since = 0;
                    splash_at = 0;
                    last_match = now;
                    continue;
                }
                NDIlib_recv_create_v3_t rc = {
                    .source_to_connect_to = *pick,
                    .color_format = color,
                    .bandwidth = bandwidth,
                    .allow_video_fields = false,
                    .p_ndi_recv_name = recv_name,
                };
                snprintf(cur_src, sizeof cur_src, "%s", pick->p_ndi_name);
                snprintf(cur_url, sizeof cur_url, "%s", pick->p_url_address ? pick->p_url_address : "");
                recv = ndi->recv_create_v3(&rc);
                if (recv && hx_mode) { /* a new connection starts with the NDI library's decoding */
                    hwdec_close(o, &hw);
                    hx_mode = false;
                }
                if (recv) {
                    LOG("ndi: output %d: connecting to %s (%s)%s", o->n, cur_src, cur_url,
                        backup ? " - main stream unavailable, using the backup" : "");
                    on_backup = backup;
                    search_since = 0;
                    primary_check = now;
                    if (strcmp(cur_src, last_src))
                        o->au.extra_ms = o->au.logged_rate = 0; /* another sender: start afresh */
                    snprintf(last_src, sizeof last_src, "%s", cur_src);
                    status_set(o, "connecting", cur_src, cur_url, "");
                    recv_start = now;
                    last_alive = now + 5000; /* grace period for the first connection */
                    got_frame = false;
                } else {
                    LOG("ndi: output %d: cannot create receiver for %s", o->n, cur_src);
                    snprintf(failed_src, sizeof failed_src, "%s", cur_src);
                    share_release(o);
                }
                last_match = now;
            } else if (now - last_match > 30000 && now - find_created > 30000) {
                /* Nothing for a while: restart discovery in case the network changed. */
                LOG("ndi: output %d: no match for 30 s, restarting discovery", o->n);
                ndi->find_destroy(find);
                find = NULL;
            }
            continue;
        }

        /* On the backup: switch back as soon as the main stream is there again. The backup's last
         * picture stays up until the main stream's first frame. */
        if (on_backup && now - primary_check >= 2000) {
            primary_check = now;
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            if (find_primary(o, srcs, count, "")) {
                LOG("ndi: output %d: main stream %s is back, leaving the backup", o->n, o->target);
                ndi->recv_destroy(recv);
                recv = NULL;
                audio_close(o);
                on_backup = false;
                splash_at = now + 6000;
                continue;
            }
        }

        NDIlib_video_frame_v2_t v;
        NDIlib_audio_frame_v3_t a;
        /* HX: never wait on the decoder (NDI drops frames it can't hand over, which breaks the H.264
         * references); while pictures are in flight, come back every few ms to collect them. */
        int wait = !hx_mode ? 100 : hw.fed > hw.decoded ? 4 : 20;
        NDIlib_frame_type_e t = ndi->recv_capture_v3(recv, &v, audio_on ? &a : NULL, NULL, wait);
        uint64_t recv_us = clock_us(CLOCK_MONOTONIC);
        now = now_ms();
        bool hx_leave = false;
        if (t == NDIlib_frame_type_video) {
            if (!got_frame) {
                LOG("ndi: output %d: first frame after %llu ms (%dx%d)", o->n,
                    (unsigned long long)(now - recv_start), v.xres, v.yres);
                if (hx_decode && !hx_mode)
                    codec_probe_start(o, bandwidth, cur_src, cur_url, 0);
            }
            bool shown = true;
            if (!hx_mode) {
                shown = post_frame(o, &v, recv_us);
            } else if (!codec_is_h264((uint32_t)v.FourCC)) {
                LOG("video: output %d: %s no longer sends H.264", o->n, cur_src);
                hx_leave = true;
            } else if (!hwdec_feed(o, &hw, &v, recv_us)) {
                snprintf(hw_failed, sizeof hw_failed, "%s", cur_src);
                hx_leave = true;
            }
            last_w = v.xres;
            last_h = v.yres;
            char vid[64];
            snprintf(vid, sizeof vid, "%dx%d %.2f fps", v.xres, v.yres,
                     v.frame_rate_D ? (double)v.frame_rate_N / v.frame_rate_D : 0.0);
            status_set(o, !shown ? "cannot display (too big)" : on_backup ? "playing (backup)" : "playing", cur_src,
                       cur_url, vid);
            if (strcmp(vid, o->share_video)) {
                pthread_mutex_lock(&share_mu);
                snprintf(o->share_video, sizeof o->share_video, "%s", vid);
                pthread_mutex_unlock(&share_mu);
            }
            splash_at = 0;
            ndi->recv_free_video_v2(recv, &v);
            got_frame = true;
            last_frame = last_alive = now;
            failed_src[0] = 0;
        }
        if (t == NDIlib_frame_type_audio) {
            audio_play(o, &a);
            share_audio(o, &a);
            ndi->recv_free_audio_v3(recv, &a);
        }

        if (hx_mode && !hx_leave) {
            hwdec_poll(o, &hw, 0);
            if (hw.need_key && ndi_request_keyframe && now - key_asked > 1000) {
                ndi_request_keyframe(recv); /* don't wait for the sender's next one */
                key_asked = now;
            }
        }
        /* H.264 from the sender (the format check says so): switch to the hardware decoder; back to the
         * NDI library's decoding when the sender changes codec or the hardware decoder fails. */
        bool to_hx = false;
        if (!hx_mode && got_frame && *hw_dev && o->has_nv12 && strcmp(hw_failed, cur_src)) {
            pthread_mutex_lock(&o->lock);
            to_hx = o->probe_codec && !strcmp(o->probe_codec, "NDI|HX (H.264)") && !strcmp(o->probe_src, cur_src);
            o->probe_codec = to_hx ? NULL : o->probe_codec;
            pthread_mutex_unlock(&o->lock);
        }
        if (to_hx || hx_leave) {
            NDIlib_recv_create_v3_t rc = {
                .source_to_connect_to = { .p_ndi_name = cur_src, .p_url_address = *cur_url ? cur_url : NULL },
                .color_format = to_hx ? (NDIlib_recv_color_format_e)302 /* compressed_v3: SpeedHQ, H.264 */ : color,
                .bandwidth = bandwidth,
                .allow_video_fields = false,
                .p_ndi_recv_name = recv_name,
            };
            if (hx_leave)
                hwdec_close(o, &hw);
            NDIlib_recv_instance_t r2 = NULL;
            if (to_hx && !hwdec_open(o, &hw, last_w, last_h))
                snprintf(hw_failed, sizeof hw_failed, "%s", cur_src);
            else
                r2 = ndi->recv_create_v3(&rc);
            if (r2) {
                ndi->recv_destroy(recv);
                recv = r2;
                hx_mode = to_hx;
                key_asked = 0;
                if (to_hx) {
                    char buf[512], path[64];
                    snprintf(buf, sizeof buf, "source=%s\ncodec=NDI|HX (H.264), hardware decoded\n", cur_src);
                    snprintf(path, sizeof path, RUN_DIR "/out%d.codec", o->n);
                    write_file_atomic(path, buf);
                }
                last_alive = now + 5000;
                if (!to_hx && hx_decode)
                    codec_probe_start(o, bandwidth, cur_src, cur_url, 0); /* which codec now? */
            } else if (to_hx) {
                hwdec_close(o, &hw);
            } else { /* no receiver for the NDI library's decoding: reconnect from scratch */
                ndi->recv_destroy(recv);
                recv = NULL;
                hx_mode = false;
            }
            if (!recv)
                continue;
        }
        if (audio_on && got_frame && now - o->au.last_frame > 3000 && now - recv_start > 3000) {
            audio_close(o); /* stream without audio, or it stopped */
            audio_info(o, "no audio in this stream");
        }
        int conns = ndi->recv_get_no_connections(recv);
        if (conns > 0 && now > last_alive)
            last_alive = now;
        /* The sender came back (e.g. restarted with another codec): the NDI library reconnects by itself,
         * so check the codec again, or HX would stay decoded in software. A reconnect within a minute of
         * the last check (a flaky link drops often) is checked when that minute is up, not forgotten. */
        if (conns > 0 && prev_conns == 0 && got_frame && !hx_mode && hx_decode)
            recheck = true;
        /* A reconnect too quick to see (conns never 0) is caught by its symptom: frames dropped while the
         * NDI library decodes, which is what H.264 decoded in software does on a Pi 3/4. */
        if (!hx_mode && hx_decode && *hw_dev && got_frame && now - drop_check >= 2000) {
            NDIlib_recv_performance_t total, dropped;
            ndi->recv_get_performance(recv, &total, &dropped);
            if (drop_check && dropped.video_frames > drops_seen)
                recheck = true;
            drops_seen = dropped.video_frames;
            drop_check = now;
        }
        if (recheck && (hx_mode || (conns > 0 && codec_probe_start(o, bandwidth, cur_src, cur_url, 60000))))
            recheck = false;
        prev_conns = conns;

        if (stats_ms > 0 && now - last_stats >= (uint64_t)stats_ms) {
            NDIlib_recv_performance_t total, dropped;
            ndi->recv_get_performance(recv, &total, &dropped);
            LOG("ndi: output %d stats %s: connections %d, video frames %lld received, %lld dropped", o->n,
                cur_src, conns, (long long)total.video_frames, (long long)dropped.video_frames);
            if (hx_mode)
                LOG("video: output %d: hardware decoder: %llu frames in, %llu decoded%s", o->n,
                    (unsigned long long)hw.fed, (unsigned long long)hw.decoded,
                    hw.need_key ? ", waiting for a keyframe" : "");
            if (o->au.frames)
                LOG("audio: output %d: %llu frames, %llu samples in (%.0f/s), %llu not played, %llu underruns, "
                    "longest gap %llu ms", o->n, (unsigned long long)o->au.frames, (unsigned long long)o->au.in,
                    o->au.in * 1000.0 / (now - last_stats + 1), (unsigned long long)o->au.dropped,
                    (unsigned long long)o->au.xruns, (unsigned long long)o->au.max_gap);
            o->au.frames = o->au.in = o->au.dropped = o->au.xruns = o->au.max_gap = 0;
            last_stats = now;
        }

        if (got_frame && frame_timeout > 0 && now - last_frame > (uint64_t)frame_timeout &&
            strcmp(o->st.state, "connected, no frames")) {
            LOG("ndi: output %d: no frames for %d ms, showing splash", o->n, frame_timeout);
            request_splash(o);
            status_set(o, "connected, no frames", cur_src, cur_url, "");
        }
        if (t == NDIlib_frame_type_error || (now > last_alive && now - last_alive > 3000)) {
            LOG("ndi: output %d: lost %s", o->n, cur_src);
            if (!got_frame)
                snprintf(failed_src, sizeof failed_src, "%s", cur_src);
            ndi->recv_destroy(recv);
            recv = NULL;
            if (*o->backup && got_frame) {
                splash_at = now + 6000; /* hold the picture while the other stream takes over */
                if (!on_backup)
                    search_since = now - 3000; /* the main stream just went: no grace period */
            } else {
                request_splash(o);
            }
            on_backup = false;
            audio_close(o);
            if (audio_on)
                audio_info(o, "");
        }
    }
    share_release(o);
    share_leave(o);
    hwdec_close(o, &hw);
    if (recv)
        ndi->recv_destroy(recv);
    audio_close(o);
    if (find)
        ndi->find_destroy(find);
    return NULL;
}

/* Main stream (empty = OUT_<serial>[_n]), source IP and backup of an output. */
static void set_target(struct output *o, const char *stream, const char *source_ip, const char *backup)
{
    char def[160];
    if (o->n == 1)
        snprintf(def, sizeof def, "OUT_%s", serial);
    else
        snprintf(def, sizeof def, "OUT_%s_%d", serial, o->n);
    snprintf(o->target, sizeof o->target, "%s", *stream ? stream : def);
    o->alt[0] = 0;
    if (!strcmp(o->target, def)) {
        /* Also accept the serial without leading zeros. */
        const char *s = serial;
        while (*s == '0' && s[1])
            s++;
        if (s != serial) {
            if (o->n == 1)
                snprintf(o->alt, sizeof o->alt, "OUT_%s", s);
            else
                snprintf(o->alt, sizeof o->alt, "OUT_%s_%d", s, o->n);
        }
    }
    snprintf(o->source_ip, sizeof o->source_ip, "%s", source_ip);
    snprintf(o->backup, sizeof o->backup, "%s", backup);
}

static void configure_output(struct output *o)
{
    set_target(o, ocfg(o->n, "STREAM", ""), ocfg(o->n, "SOURCE_IP", ""), ocfg(o->n, "BACKUP_STREAM", ""));
    /* A live switch from before a restart: the saved settings win. (The file is the web interface's, in a
     * directory only it can write to: noted, not removed.) */
    char path[64];
    struct stat st;
    snprintf(path, sizeof path, CTL_DIR "/out%d.source", o->n);
    o->source_stale = stat(path, &st) ? 0 : (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    o->rot_s = ocfg(o->n, "ROTATE", "0");
    o->mirror_s = ocfg(o->n, "MIRROR", "none");
    o->audio_s = ocfg(o->n, "AUDIO", "hdmi");
    o->standby_min = atoi(ocfg(o->n, "STANDBY_MIN", "0"));
    o->cec = !strcmp(ocfg(o->n, "CEC", "0"), "1");
    o->brightness = atoi(ocfg(o->n, "BRIGHTNESS", "0"));
    if (o->brightness < -50)
        o->brightness = -50;
    if (o->brightness > 50)
        o->brightness = 50;
    o->audio_delay_ms = atoi(ocfg(o->n, "AUDIO_DELAY_MS", "80"));
    if (o->audio_delay_ms < 20)
        o->audio_delay_ms = 20;
    if (o->audio_delay_ms > 1000)
        o->audio_delay_ms = 1000;
    o->pacing = !strcmp(ocfg(o->n, "PACING", "fast"), "smooth"); /* experimental: off unless chosen */
    o->disabled = !strcmp(ocfg(o->n, "ENABLED", "1"), "0");
    if (o->disabled)
        LOG("output %d: disabled", o->n);
    else
        LOG("output %d: looking for NDI stream %s from %s%s%s", o->n, o->target,
            *o->source_ip ? o->source_ip : "any host", *o->backup ? ", backup " : "", o->backup);
}

/* ------------------------------------------------------------------ preview for the web UI */

#define THUMB_W 384

/* Pixel (x, y) of buffer b as 0xRRGGBB. Dumb buffers are write-combined: only sample what we need. */
static uint32_t buf_pixel(const struct buf *b, int x, int y)
{
    const uint8_t *row = b->map + (size_t)y * b->pitch;
    int Y, U, V;
    switch (b->fmt) {
    case DRM_FORMAT_XRGB8888: { const uint8_t *p = row + x * 4; return p[2] << 16 | p[1] << 8 | p[0]; }
    case DRM_FORMAT_XBGR8888: { const uint8_t *p = row + x * 4; return p[0] << 16 | p[1] << 8 | p[2]; }
    case DRM_FORMAT_UYVY: { const uint8_t *p = row + (x & ~1) * 2; U = p[0]; V = p[2]; Y = p[1 + (x & 1) * 2]; break; }
    case DRM_FORMAT_NV16: {
        const uint8_t *uv = b->map + b->uv_off + (size_t)y * b->pitch + (x & ~1);
        Y = row[x]; U = uv[0]; V = uv[1]; break;
    }
    case DRM_FORMAT_NV12: {
        const uint8_t *uv = b->map + b->uv_off + (size_t)(y / 2) * b->pitch + (x & ~1);
        Y = row[x]; U = uv[0]; V = uv[1]; break;
    }
    default: return 0;
    }
    /* BT.709 limited range (NDI's HD colour space). */
    int c = Y - 16, d = U - 128, e = V - 128;
    int r = (298 * c + 459 * e + 128) >> 8, g = (298 * c - 55 * d - 136 * e + 128) >> 8, bl = (298 * c + 541 * d + 128) >> 8;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
    return (uint32_t)r << 16 | g << 8 | bl;
}

/* While the web UI asks for it (touches out<n>.thumbreq), write out<n>.jpg: what is on screen now. */
static void thumb_publish(struct output *o)
{
    char path[64], tmp[72];
    struct stat st;
    snprintf(path, sizeof path, WEB_RUN "/out%d.thumbreq", o->n);
    if (stat(path, &st) || time(NULL) - st.st_mtime > 15)
        return;

    /* Preview thread: pin the buffers (output_disable / reallocation wait for thumb_reading). A buffer
     * may get a newer frame written into it while we sample it - harmless for a preview. */
    pthread_mutex_lock(&o->lock);
    if (!o->usable) {
        pthread_mutex_unlock(&o->lock);
        return;
    }
    o->thumb_reading = true;
    struct buf *b = (!o->on_splash && o->scanout >= 0) ? &o->vid[o->scanout] : NULL;
    double ar = b ? o->vaspect[o->scanout] : 0;
    const uint32_t *comp = o->comp;
    int W = o->W, sw = b ? b->w : o->W, sh = b ? b->h : o->H;
    struct output *L = NULL; /* showing another output's buffer (decode once): pin that one instead */
    int li = -1;
    if (!b && !o->on_splash) {
        pthread_mutex_lock(&share_mu);
        L = o->ext_scanout_of;
        li = o->ext_scanout;
        pthread_mutex_unlock(&share_mu);
    }
    pthread_mutex_unlock(&o->lock);
    if (L) {
        struct output *lo = L;
        pthread_mutex_lock(&lo->lock);
        if (lo->usable && lo->vstate[li] != B_NONE) {
            lo->thumb_reading = true; /* this thread does one output at a time: never L's own preview now */
            b = &lo->vid[li];
            ar = lo->vaspect[li];
            sw = b->w;
            sh = b->h;
        } else {
            L = NULL;
        }
        pthread_mutex_unlock(&lo->lock);
    }
    if (!ar)
        ar = (double)sw / sh;
    int tw = THUMB_W, th = (int)(THUMB_W / ar + 0.5);
    uint8_t *rgb = th >= 16 && th <= THUMB_W * 2 ? malloc((size_t)tw * th * 3) : NULL;
    if (!rgb) {
        pthread_mutex_lock(&o->lock);
        o->thumb_reading = false;
        pthread_mutex_unlock(&o->lock);
        return;
    }
    uint64_t t0 = clock_us(CLOCK_MONOTONIC);
    for (int y = 0; y < th; y++) {
        int sy = (int)(((long)y * sh + sh / 2) / th);
        for (int x = 0; x < tw; x++) {
            int sx = (int)(((long)x * sw + sw / 2) / tw);
            uint32_t p = b ? buf_pixel(b, sx, sy) : comp[(size_t)sy * W + sx];
            uint8_t *d = rgb + ((size_t)y * tw + x) * 3;
            d[0] = p >> 16;
            d[1] = p >> 8;
            d[2] = p;
        }
    }
    pthread_mutex_lock(&o->lock);
    o->thumb_reading = false;
    pthread_mutex_unlock(&o->lock);
    if (L) {
        pthread_mutex_lock(&L->lock);
        L->thumb_reading = false;
        pthread_mutex_unlock(&L->lock);
    }

    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr je;
    unsigned char *out = NULL;
    unsigned long out_len = 0;
    ci.err = jpeg_std_error(&je);
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &out, &out_len);
    ci.image_width = tw;
    ci.image_height = th;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, 75, TRUE);
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = rgb + (size_t)ci.next_scanline * tw * 3;
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    free(rgb);
    static bool timed;
    if (!timed) {
        LOG("preview: output %d: %dx%d in %.1f ms", o->n, tw, th, (clock_us(CLOCK_MONOTONIC) - t0) / 1000.0);
        timed = true;
    }

    snprintf(path, sizeof path, RUN_DIR "/out%d.jpg", o->n);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(out, 1, out_len, f);
        fclose(f);
        rename(tmp, path);
    }
    free(out);
}

/* /run/pindi-ctl/identify (written by the web UI): first line the output number (0 = all), then the
 * text to show. Shown for 15 s after the file was written. */
static void identify_poll(const char *ip)
{
    struct stat st;
    char buf[sizeof outs[0].ident + 16] = "";
    int which = -1;
    if (!stat(CTL_DIR "/identify", &st) && time(NULL) - st.st_mtime < 15) {
        FILE *f = fopen(CTL_DIR "/identify", "r");
        if (f) {
            size_t len = fread(buf, 1, sizeof buf - 1, f);
            buf[len] = 0;
            fclose(f);
            which = atoi(buf);
        }
    }
    const char *text = strchr(buf, '\n');
    for (int i = 0; i < nout; i++) {
        struct output *o = &outs[i];
        bool on = which == 0 || which == o->n;
        char want[sizeof o->ident];
        snprintf(want, sizeof want, "%s", on && text ? text + 1 : "");
        if (on == o->identify && !strcmp(want, o->ident))
            continue;
        pthread_mutex_lock(&o->lock);
        o->identify = on;
        if (!on)
            o->restore_video = true;
        pthread_mutex_unlock(&o->lock);
        snprintf(o->ident, sizeof o->ident, "%s", want);
        LOG("display: output %d: identify %s", o->n, on ? "on" : "off");
        splash_update_text(o, ip, true);
        wake();
    }
}

/* Low priority: sampling write-combined buffers is slow and must not delay page flips. */
static void *thumb_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)gettid(), 15);
    while (running) {
        for (int i = 0; i < 20 && running; i++)
            usleep(100000);
        for (int i = 0; i < nout && running; i++)
            thumb_publish(&outs[i]);
    }
    return NULL;
}

/* ------------------------------------------------------------------ main */

static bool load_ndi(void)
{
    /* libndi is large; it is loaded only after the splash so it doesn't delay it. */
    /* The NDI Advanced SDK's library is libndi_advanced.so.6 (its soname). */
    void *lib = dlopen(NDILIB_LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libndi_advanced.so.6", RTLD_NOW | RTLD_LOCAL);
    const NDIlib_v6 *(*ndi_load)(void) = lib ? (const NDIlib_v6 *(*)(void))dlsym(lib, "NDIlib_v6_load") : NULL;
    ndi = ndi_load ? ndi_load() : NULL;
    if (!ndi) {
        LOG("ndi: cannot load %s: %s - splash only", NDILIB_LIBRARY_NAME, lib ? "no NDIlib_v6_load" : dlerror());
        return false;
    }
    if (!ndi->initialize()) {
        LOG("ndi: NDIlib_initialize failed (unsupported CPU?) - splash only");
        return false;
    }
    /* The standard SDK decodes only Full NDI (SpeedHQ); NDI|HX needs libndihx.so, which it does not
       ship. Without it, software senders fall back to SpeedHQ and HX-only cameras show no picture.
       The Advanced SDK decodes HX with FFmpeg's libavcodec.so.61 instead. */
    void *hx = dlopen("libndihx.so", RTLD_LAZY | RTLD_LOCAL);
    void *adv = hx ? NULL : dlopen("libndi_advanced.so.6", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    if (adv) {
        dlclose(adv);
        hx = dlopen("libavcodec.so.61", RTLD_LAZY | RTLD_LOCAL);
    }
    if (hx)
        dlclose(hx);
    hx_decode = hx != NULL;
    if (hx_decode) {
        ndi_request_keyframe = (void (*)(NDIlib_recv_instance_t))dlsym(lib, "NDIlib_recv_request_keyframe");
        hwdec_find();
    }
    char buf[256];
    snprintf(buf, sizeof buf, "version=%s\nhx_receive=%d\n", ndi->version ? ndi->version() : "", hx != NULL);
    write_file_atomic(RUN_DIR "/ndi", buf);
    LOG("ndi: %s, NDI|HX decoding %s", ndi->version ? ndi->version() : "?", hx ? "available" : "not available");
    return true;
}

static void start_receivers(void)
{
    for (int i = 0; i < nout; i++)
        if (!outs[i].thr_started && !pthread_create(&outs[i].thr, NULL, receiver, &outs[i]))
            outs[i].thr_started = true;
}

/* HDMI-CEC to the TV on this output's port (cec0 = HDMI-A-1, cec1 = HDMI-A-2), in the background:
 * cec-ctl first claims a playback-device address, then sends the command. */
static void cec_send(struct output *o, const char *cmd)
{
    if (!o->cec)
        return;
    int port = 1;
    sscanf(o->conn_name, "HDMI-A-%d", &port);
    char dev[32], line[256];
    snprintf(dev, sizeof dev, "/dev/cec%d", port > 0 ? port - 1 : 0);
    if (access(dev, F_OK)) {
        LOG("cec: output %d: %s not present", o->n, dev);
        return;
    }
    snprintf(line, sizeof line, "cec-ctl -d %s --playback -o PiNDI >/dev/null 2>&1; cec-ctl -d %s --to 0 %s "
             ">/dev/null 2>&1", dev, dev, cmd);
    LOG("cec: output %d: %s", o->n, cmd);
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c", line, (char *)NULL);
        _exit(1);
    }
}

/* Display standby: signal off after standby_min minutes of splash; back on when the receiver
 * sees our stream (wake_request) or for Identify. */
static void standby_service(struct output *o)
{
    pthread_mutex_lock(&o->lock);
    bool wake_req = o->wake_request || o->identify;
    o->wake_request = false;
    pthread_mutex_unlock(&o->lock);
    if (o->standby) {
        if (wake_req) {
            LOG("display: output %d: waking up from standby", o->n);
            cec_send(o, "--image-view-on");
            o->standby = false;
            o->enable_retry_at = 0;
            if (!output_enable(o))
                o->enable_retry_at = now_ms() + 5000;
        }
        return;
    }
    if (o->active && o->standby_min > 0 && o->splash_since && !o->identify && o->pending == P_NONE &&
        now_ms() - o->splash_since > (uint64_t)o->standby_min * 60000) {
        LOG("display: output %d: no video for %d min, display off (standby)", o->n, o->standby_min);
        cec_send(o, "--standby");
        output_disable(o);
        o->standby = true;
        pthread_mutex_lock(&o->lock);
        o->wake_request = false;
        pthread_mutex_unlock(&o->lock);
    }
}

/* Called about once a second: follows HDMI hotplug on every output. */
static void hotplug_poll(void)
{
    for (int i = 0; i < nout; i++) {
        struct output *o = &outs[i];
        if (o->pending != P_NONE || o->disabled)
            continue;
        drmModeConnector *c = drmModeGetConnectorCurrent(drm_fd, o->conn_id);
        bool connected = c && c->connection == DRM_MODE_CONNECTED;
        drmModeFreeConnector(c);
        if (connected && !o->active && !o->standby) {
            /* A monitor that can't be set up (bad EDID, unsupported mode): retry every 5 s, not every second. */
            if (now_ms() >= o->enable_retry_at && !output_enable(o))
                o->enable_retry_at = now_ms() + 5000;
        } else if (!connected && o->active) {
            LOG("display: output %d: monitor disconnected", o->n);
            output_disable(o);
        } else if (!connected && o->standby) {
            o->standby = false; /* unplugged while asleep: a new monitor starts normally */
        }
        standby_service(o);
    }
    outputs_publish();
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); /* auto-reap modprobe */

    start_ms = now_ms();
    read_serial(serial, sizeof serial);
    mkdir(RUN_DIR, 0755);
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK)) {
        LOG("pipe: %s", strerror(errno));
        return 1;
    }

    /* Splash first - everything else can wait. (pindi.service loads the display drivers early,
     * as root: the player itself runs unprivileged.) */
    if (load_jpeg(cfg("PINDI_SPLASH", "/usr/share/pindi/splash.jpg"), &splash_img))
        trim_edges(&splash_img, image_bg(&splash_img));
    splash_info = strcmp(cfg("PINDI_SPLASH_INFO", "1"), "0") != 0;
    font_ok = !FT_Init_FreeType(&ft) &&
              !FT_New_Face(ft, cfg("PINDI_FONT", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"), 0, &face);
    if (!font_ok)
        LOG("text: font not available, splash will have no text");

    while (running && drm_fd < 0 && now_ms() - start_ms < 30000) {
        if (!display_open(now_ms() - start_ms > 10000))
            usleep(50000);
    }
    if (drm_fd >= 0) {
        discover_outputs();
        for (int i = 0; i < nout; i++) {
            configure_output(&outs[i]);
            if (outs[i].disabled) {
                output_off(&outs[i]);
                continue;
            }
            drmModeConnector *c = drmModeGetConnectorCurrent(drm_fd, outs[i].conn_id);
            if (c && c->connection == DRM_MODE_CONNECTED)
                output_enable(&outs[i]);
            drmModeFreeConnector(c);
        }
        outputs_publish();
        LOG("splash up after %llu ms (%d output%s)", (unsigned long long)(now_ms() - start_ms), nout,
            nout == 1 ? "" : "s");
    } else {
        LOG("display: no KMS device found");
    }

    pthread_t thumb_thr;
    bool thumb_ok = !pthread_create(&thumb_thr, NULL, thumb_thread, NULL);
    bool ndi_ok = load_ndi();
    if (ndi_ok)
        start_receivers();

    drmEventContext ev = { .version = 4, .page_flip_handler2 = on_flip, .sequence_handler = on_vblank };
    uint64_t last_poll = now_ms(), last_ip = 0;
    char ip[64] = "";

    while (running) {
        struct pollfd pfd[2] = { { .fd = wake_pipe[0], .events = POLLIN }, { .fd = drm_fd, .events = POLLIN } };
        int n = poll(pfd, drm_fd >= 0 ? 2 : 1, 200);
        if (n > 0 && (pfd[0].revents & POLLIN)) {
            char drain[64];
            while (read(wake_pipe[0], drain, sizeof drain) > 0) { }
        }
        if (n > 0 && drm_fd >= 0 && (pfd[1].revents & POLLIN))
            drmHandleEvent(drm_fd, &ev);

        for (int i = 0; i < nout; i++)
            output_service(&outs[i]);

        uint64_t now = now_ms();
        for (int i = 0; i < nout; i++)
            latency_publish(&outs[i], now);
        if (drm_fd < 0 && now - last_poll >= 1000) {
            /* Display driver appeared late: set up now. */
            last_poll = now;
            if (display_open(true)) {
                discover_outputs();
                for (int i = 0; i < nout; i++)
                    configure_output(&outs[i]);
                if (ndi_ok)
                    start_receivers();
            }
        }
        if (drm_fd >= 0 && now - last_poll >= 1000) {
            last_poll = now;
            hotplug_poll();
        }
        if (now - last_ip >= 1000) {
            last_ip = now;
            read_ip(ip, sizeof ip);
            identify_poll(ip);
        }
        for (int i = 0; i < nout; i++)
            splash_update_text(&outs[i], ip, false); /* only redraws on a change */
    }

    LOG("shutting down");
    for (int i = 0; i < nout; i++)
        if (outs[i].thr_started)
            pthread_join(outs[i].thr, NULL);
    if (thumb_ok)
        pthread_join(thumb_thr, NULL);
    if (ndi_ok)
        ndi->destroy();
    for (int i = 0; i < nout; i++)
        if (outs[i].active)
            output_disable(&outs[i]);
    if (drm_fd >= 0)
        close(drm_fd);
    return 0;
}

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
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#define LOG(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)

/* Text height cap: 16pt at 96 dpi. */
#define FONT_PT_PX 21
#define MAX_OUT 4
#define NBUF 4
#define RUN_DIR "/run/pindi"

static const NDIlib_v6 *ndi;
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
    im->px = malloc((size_t)im->w * im->h * 4);
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
    uint64_t size, uv_off; /* uv_off: CbCr plane offset for NV16 */
    uint8_t *map;
};

/* XRGB8888 / XBGR8888 / UYVY (one plane) or NV16 (Y plane then CbCr plane, one allocation). */
static bool buf_create(int w, int h, uint32_t fmt, struct buf *b)
{
    bool nv16 = fmt == DRM_FORMAT_NV16;
    int bpp = nv16 ? 8 : fmt == DRM_FORMAT_UYVY ? 16 : 32;
    struct drm_mode_create_dumb c = { .width = w, .height = nv16 ? h * 2 : h, .bpp = bpp };
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
    b->uv_off = nv16 ? (uint64_t)c.pitch * h : 0;
    uint32_t handles[4] = { c.handle, nv16 ? c.handle : 0 };
    uint32_t pitches[4] = { c.pitch, nv16 ? c.pitch : 0 };
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
enum { P_NONE, P_SPLASH, P_VIDEO };

struct output {
    int n; /* 1-based, as shown in the web UI */
    char conn_name[32];
    uint32_t conn_id, crtc_id, plane_id;
    bool has_uyvy, has_nv16;
    struct { uint32_t fb, crtc, sx, sy, sw, sh, cx, cy, cw, ch, rot; } pp;
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

    /* Shared with the receiver thread (lock). */
    pthread_mutex_t lock;
    bool usable; /* display up: the receiver may post frames */
    struct buf vid[NBUF];
    int vstate[NBUF];
    double vaspect[NBUF];
    int64_t vsent[NBUF];  /* sender's submit time, 100 ns UTC (NDI timestamp), or INT64_MAX */
    uint64_t vrecv[NBUF]; /* when this Pi got the frame from NDI, monotonic us */
    int pw, ph;
    uint32_t pfmt; /* format of the current buffer pool */
    int ww, wh;
    uint32_t wfmt; /* format the receiver needs */
    bool want_realloc, want_splash, on_splash;
    int ready, queued, scanout;
    char splash_msg[192]; /* what the receiver is doing, shown on the splash */
    bool thumb_reading;   /* the preview thread is reading a buffer: don't free any */
    bool cant_display;    /* no buffers for the current frame size (pw x ph) */

    /* Configuration. */
    bool disabled; /* PINDI_OUT<n>_ENABLED=0: no NDI, HDMI signal off (e.g. encoder-only units) */
    char target[160], alt[160], source_ip[64];
    char backup[160]; /* PINDI_OUT<n>_BACKUP_STREAM: played while the main stream is unavailable */
    const char *rot_s, *mirror_s;
    const char *audio_s; /* PINDI_OUT<n>_AUDIO: hdmi, analog or off */
    int audio_delay_ms;  /* PINDI_OUT<n>_AUDIO_DELAY_MS: audio buffer = lip-sync delay */

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
        int extra_ms;                           /* added after underruns (jittery sender), kept per source */
        int logged_rate;
        snd_pcm_uframes_t win_min, pending_drop; /* lowest queue level in this 2 s window */
    } au;

    /* Latency of frames that reached the screen (main thread), published every 5 s. */
    struct { double pi_sum, pi_min, pi_max, tot_sum, tot_min, tot_max; int n, tot_n; uint64_t since; } lat;

    /* Receiver thread. */
    pthread_t thr;
    bool thr_started;
    struct { char state[32], source[256], url[128], video[64], target[160]; } st;
};

static struct output outs[MAX_OUT];
static int nout;
static struct image splash_img;

static void wake(void)
{
    char c = 1;
    if (write(wake_pipe[1], &c, 1) < 0) { /* pipe full: main thread is awake anyway */ }
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
        o->p_mode = prop_id(o->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        o->p_active = prop_id(o->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        o->p_conn_crtc = prop_id(o->conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        nout++;
    }
    qsort(outs, nout, sizeof outs[0], conn_cmp);
    for (int i = 0; i < nout; i++) {
        struct output *o = &outs[i];
        o->n = i + 1;
        pthread_mutex_init(&o->lock, NULL);
        o->ready = o->queued = o->scanout = o->last_video = -1;
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
    for (int b = 0; b < NBUF; b++) {
        buf_destroy(&o->vid[b]);
        o->vstate[b] = B_NONE;
    }
    o->pw = o->ph = 0;
    o->pfmt = 0;
    o->ready = o->queued = o->scanout = o->last_video = -1;
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
    pthread_mutex_lock(&o->lock);
    o->on_splash = true;
    o->want_splash = false;
    o->usable = true;
    pthread_mutex_unlock(&o->lock);
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

/* A video frame reached the screen at flip_us (monotonic, from the vblank event). */
static void latency_add(struct output *o, int idx, uint64_t flip_us)
{
    if (!o->lat.n && !o->lat.tot_n) {
        o->lat.pi_min = o->lat.tot_min = 1e9;
        o->lat.pi_max = o->lat.tot_max = -1e9;
    }
    double pi = (double)(int64_t)(flip_us - o->vrecv[idx]) / 1000.0;
    o->lat.pi_sum += pi;
    o->lat.pi_min = fmin(o->lat.pi_min, pi);
    o->lat.pi_max = fmax(o->lat.pi_max, pi);
    o->lat.n++;
    if (o->vsent[idx] != INT64_MAX) {
        /* Sender time is UTC: move the flip time from the monotonic to the realtime clock. */
        int64_t offset_us = (int64_t)clock_us(CLOCK_REALTIME) - (int64_t)clock_us(CLOCK_MONOTONIC);
        double tot = ((double)((int64_t)flip_us + offset_us) * 10.0 - (double)o->vsent[idx]) / 10000.0;
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
    char path[64], buf[256] = "";
    snprintf(path, sizeof path, RUN_DIR "/out%d.latency", o->n);
    if (o->lat.n)
        snprintf(buf, sizeof buf, "pi_avg=%.1f\npi_min=%.1f\npi_max=%.1f\n", o->lat.pi_sum / o->lat.n,
                 o->lat.pi_min, o->lat.pi_max);
    if (o->lat.tot_n)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "total_avg=%.1f\ntotal_min=%.1f\ntotal_max=%.1f\n",
                 o->lat.tot_sum / o->lat.tot_n, o->lat.tot_min, o->lat.tot_max);
    if (o->lat.n)
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "frames=%d\n", o->lat.n);
    write_file_atomic(path, buf);
    memset(&o->lat, 0, sizeof o->lat);
    o->lat.since = now;
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *data)
{
    (void)fd; (void)seq; (void)crtc;
    struct output *o = data;
    pthread_mutex_lock(&o->lock);
    if (o->pending == P_VIDEO) {
        if (o->scanout >= 0)
            o->vstate[o->scanout] = B_FREE;
        o->scanout = o->queued;
        if (o->scanout >= 0) {
            o->last_video = o->scanout;
            o->vstate[o->scanout] = B_SCANOUT;
            latency_add(o, o->scanout, (uint64_t)sec * 1000000 + usec);
        }
        o->queued = -1;
        o->on_splash = false;
    } else if (o->pending == P_SPLASH) {
        if (o->scanout >= 0)
            o->vstate[o->scanout] = B_FREE;
        o->scanout = -1;
        o->on_splash = true;
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
        if (o->ready >= 0) {
            o->vstate[o->ready] = B_FREE;
            o->ready = -1;
        }
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

    if (o->ready >= 0) {
        int idx = o->ready;
        double ar = o->vaspect[idx];
        o->ready = -1;
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

    pthread_mutex_lock(&o->lock);
    if (!o->usable) {
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    if (o->cant_display && o->pw == v->xres && o->ph == v->yres && o->pfmt == fmt) {
        pthread_mutex_unlock(&o->lock);
        return false;
    }
    if (o->pw != v->xres || o->ph != v->yres || o->pfmt != fmt) {
        if (!o->want_realloc || o->ww != v->xres || o->wh != v->yres || o->wfmt != fmt) {
            o->ww = v->xres;
            o->wh = v->yres;
            o->wfmt = fmt;
            o->want_realloc = true;
            o->want_splash = false;
        }
        pthread_mutex_unlock(&o->lock);
        wake();
        return true;
    }
    int idx = -1;
    for (int b = 0; b < NBUF && idx < 0; b++)
        if (o->vstate[b] == B_FREE)
            idx = b;
    if (idx < 0 && o->ready >= 0) { /* replace the not-yet-shown frame with this newer one */
        idx = o->ready;
        o->ready = -1;
    }
    if (idx < 0) {
        pthread_mutex_unlock(&o->lock);
        return true;
    }
    o->vstate[idx] = B_WRITING;
    o->want_splash = false;
    struct buf *b = &o->vid[idx];
    pthread_mutex_unlock(&o->lock);

    for (int y = 0; y < v->yres; y++) {
        const uint8_t *src = v->p_data + (size_t)y * v->line_stride_in_bytes;
        uint8_t *dst = b->map + (size_t)y * b->pitch;
        if (fmt == DRM_FORMAT_NV16)
            uyvy_to_nv16(src, dst, b->map + b->uv_off + (size_t)y * b->pitch, v->xres);
        else
            memcpy(dst, src, (size_t)v->xres * (fmt == DRM_FORMAT_UYVY ? 2 : 4));
    }

    pthread_mutex_lock(&o->lock);
    if (o->vstate[idx] == B_WRITING) {
        if (o->ready >= 0)
            o->vstate[o->ready] = B_FREE;
        o->ready = idx;
        o->vstate[idx] = B_READY;
        o->vaspect[idx] = v->picture_aspect_ratio > 0 ? v->picture_aspect_ratio : (double)v->xres / v->yres;
        o->vsent[idx] = v->timestamp;
        o->vrecv[idx] = recv_us;
    }
    pthread_mutex_unlock(&o->lock);
    wake();
    return true;
}

static void request_splash(struct output *o)
{
    pthread_mutex_lock(&o->lock);
    o->want_splash = true;
    if (o->ready >= 0) {
        o->vstate[o->ready] = B_FREE;
        o->ready = -1;
    }
    pthread_mutex_unlock(&o->lock);
    wake();
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
                        o->disabled ? "disabled" : o->active ? "connected" : "disconnected", mode);
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
    snprintf(path, sizeof path, RUN_DIR "/out%d.source", o->n);
    if (stat(path, &st))
        return 0;
    uint64_t stamp = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    if (stamp == *seen)
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
    uint64_t last_stats = 0, last_sources = 0;
    uint32_t last_count = UINT32_MAX;
    char cur_src[256] = "", cur_url[128] = "", failed_src[256] = "", last_src[256] = "";
    bool got_frame = false, on_backup = false;
    uint64_t search_since = 0, primary_check = 0, splash_at = 0;

    while (running) {
        uint64_t now = now_ms();
        bool usable;
        pthread_mutex_lock(&o->lock);
        usable = o->usable;
        pthread_mutex_unlock(&o->lock);

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
            if (!find) {
                LOG("ndi: output %d: cannot create finder", o->n);
                usleep(1000000);
                continue;
            }
        }

        /* Keep the visible-sources list fresh for the web UI while playing too. */
        if (recv && o->n == 1 && now - last_sources >= 2000) {
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            sources_publish(srcs, count);
            last_sources = now;
        }

        if (!recv) {
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
        NDIlib_frame_type_e t = ndi->recv_capture_v3(recv, &v, audio_on ? &a : NULL, NULL, 100);
        uint64_t recv_us = clock_us(CLOCK_MONOTONIC);
        now = now_ms();
        if (t == NDIlib_frame_type_video) {
            if (!got_frame)
                LOG("ndi: output %d: first frame after %llu ms (%dx%d)", o->n,
                    (unsigned long long)(now - recv_start), v.xres, v.yres);
            bool shown = post_frame(o, &v, recv_us);
            char vid[64];
            snprintf(vid, sizeof vid, "%dx%d %.2f fps", v.xres, v.yres,
                     v.frame_rate_D ? (double)v.frame_rate_N / v.frame_rate_D : 0.0);
            status_set(o, !shown ? "cannot display (too big)" : on_backup ? "playing (backup)" : "playing", cur_src,
                       cur_url, vid);
            splash_at = 0;
            ndi->recv_free_video_v2(recv, &v);
            got_frame = true;
            last_frame = last_alive = now;
            failed_src[0] = 0;
        }
        if (t == NDIlib_frame_type_audio) {
            audio_play(o, &a);
            ndi->recv_free_audio_v3(recv, &a);
        }
        if (audio_on && got_frame && now - o->au.last_frame > 3000 && now - recv_start > 3000) {
            audio_close(o); /* stream without audio, or it stopped */
            audio_info(o, "no audio in this stream");
        }
        int conns = ndi->recv_get_no_connections(recv);
        if (conns > 0 && now > last_alive)
            last_alive = now;

        if (stats_ms > 0 && now - last_stats >= (uint64_t)stats_ms) {
            NDIlib_recv_performance_t total, dropped;
            ndi->recv_get_performance(recv, &total, &dropped);
            LOG("ndi: output %d stats %s: connections %d, video frames %lld received, %lld dropped", o->n,
                cur_src, conns, (long long)total.video_frames, (long long)dropped.video_frames);
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
    char path[64];
    snprintf(path, sizeof path, RUN_DIR "/out%d.source", o->n);
    unlink(path); /* a live switch from before a restart: the saved settings win */
    o->rot_s = ocfg(o->n, "ROTATE", "0");
    o->mirror_s = ocfg(o->n, "MIRROR", "none");
    o->audio_s = ocfg(o->n, "AUDIO", "hdmi");
    o->audio_delay_ms = atoi(ocfg(o->n, "AUDIO_DELAY_MS", "80"));
    if (o->audio_delay_ms < 20)
        o->audio_delay_ms = 20;
    if (o->audio_delay_ms > 1000)
        o->audio_delay_ms = 1000;
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
    snprintf(path, sizeof path, RUN_DIR "/out%d.thumbreq", o->n);
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
    pthread_mutex_unlock(&o->lock);
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

/* /run/pindi/identify (written by the web UI): first line the output number (0 = all), then the
 * text to show. Shown for 15 s after the file was written. */
static void identify_poll(const char *ip)
{
    struct stat st;
    char buf[sizeof outs[0].ident + 16] = "";
    int which = -1;
    if (!stat(RUN_DIR "/identify", &st) && time(NULL) - st.st_mtime < 15) {
        FILE *f = fopen(RUN_DIR "/identify", "r");
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
    void *lib = dlopen(NDILIB_LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL);
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
       ship. Without it, software senders fall back to SpeedHQ and HX-only cameras show no picture. */
    void *hx = dlopen("libndihx.so", RTLD_LAZY | RTLD_LOCAL);
    if (hx)
        dlclose(hx);
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
        if (connected && !o->active) {
            /* A monitor that can't be set up (bad EDID, unsupported mode): retry every 5 s, not every second. */
            if (now_ms() >= o->enable_retry_at && !output_enable(o))
                o->enable_retry_at = now_ms() + 5000;
        } else if (!connected && o->active) {
            LOG("display: output %d: monitor disconnected", o->n);
            output_disable(o);
        }
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

    /* Splash first - everything else can wait. Load the display drivers (and the I2C
     * drivers HDMI needs for EDID: Pi 3 i2c_bcm2835, Pi 4/5 i2c_brcmstb) now instead of
     * waiting for udev to get round to it. */
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c",
              "modprobe -q i2c_bcm2835; modprobe -q i2c_brcmstb; modprobe -q vc4", (char *)NULL);
        _exit(1);
    }
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

    drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
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

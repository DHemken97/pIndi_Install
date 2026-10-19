/*
 * pindi-player - PiNDI splash screen + NDI receiver.
 *
 * Owns the HDMI output through DRM/KMS from early boot:
 *   - shows the splash image with the IP (bottom left) and serial (bottom right)
 *   - finds the NDI source whose stream name is OUT_<serial> (any host)
 *   - displays it full screen using the hardware scaler (UYVY scanned out directly)
 *   - falls back to the splash whenever the stream is lost, and reconnects
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
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <jpeglib.h>

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

/* ------------------------------------------------------------------ device info */

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

/* ------------------------------------------------------------------ text */

static FT_Library ft;
static FT_Face face;
static bool font_ok;

static int text_width(const char *s)
{
    int w = 0;
    for (; *s; s++)
        if (!FT_Load_Char(face, (unsigned char)*s, FT_LOAD_DEFAULT))
            w += face->glyph->advance.x >> 6;
    return w;
}

static void draw_text(uint32_t *px, int W, int H, int x, int base, const char *s, uint32_t rgb)
{
    for (; *s; s++) {
        if (FT_Load_Char(face, (unsigned char)*s, FT_LOAD_RENDER))
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

/* ------------------------------------------------------------------ display */

struct buf {
    uint32_t fb, handle, pitch, fmt;
    int w, h;
    uint64_t size, uv_off; /* uv_off: CbCr plane offset for NV16 */
    uint8_t *map;
};

static struct {
    int fd;
    bool ok;
    uint64_t opened_at, last_poll;
    uint32_t conn, crtc, plane;
    drmModeModeInfo mode;
    int W, H;
    struct buf splash, vid[2];
    int vnext;
    bool video_on;
    uint32_t *base, *comp; /* splash without / with text */
    char shown_ip[64];
    bool plane_err_logged;
} D = { .fd = -1 };

static struct image splash_img;
static char serial[64];

/* fmt is XRGB8888 / XBGR8888 (one plane) or NV16 (Y plane then CbCr plane, one allocation). */
static bool buf_create(int w, int h, uint32_t fmt, struct buf *b)
{
    bool nv16 = fmt == DRM_FORMAT_NV16;
    struct drm_mode_create_dumb c = { .width = w, .height = nv16 ? h * 2 : h, .bpp = nv16 ? 8 : 32 };
    struct drm_mode_map_dumb m = { 0 };
    memset(b, 0, sizeof *b);
    if (drmIoctl(D.fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) {
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
    if (drmModeAddFB2(D.fd, w, h, fmt, handles, pitches, offsets, &b->fb, 0))
        goto fail;
    m.handle = c.handle;
    if (drmIoctl(D.fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
        goto fail;
    b->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, m.offset);
    if (b->map == MAP_FAILED) {
        b->map = NULL;
        goto fail;
    }
    return true;
fail:
    LOG("display: cannot create %dx%d buffer: %s", w, h, strerror(errno));
    if (b->fb)
        drmModeRmFB(D.fd, b->fb);
    struct drm_mode_destroy_dumb d = { .handle = c.handle };
    drmIoctl(D.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
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
        drmModeRmFB(D.fd, b->fb);
    struct drm_mode_destroy_dumb d = { .handle = b->handle };
    drmIoctl(D.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    memset(b, 0, sizeof *b);
}

static void display_teardown(void)
{
    buf_destroy(&D.vid[0]);
    buf_destroy(&D.vid[1]);
    buf_destroy(&D.splash);
    free(D.base);
    free(D.comp);
    D.base = D.comp = NULL;
    D.ok = D.video_on = false;
    D.shown_ip[0] = 0;
}

static void display_close(void)
{
    display_teardown();
    if (D.fd >= 0)
        close(D.fd);
    D.fd = -1;
}

/* Open the KMS device. Only vc4 for the first 10 s, then accept any KMS driver. */
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
        if (res && res->count_connectors > 0 && res->count_crtcs > 0) {
            LOG("display: using %s (%s)", path, v->name);
            drmModeFreeResources(res);
            drmFreeVersion(v);
            drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
            D.fd = fd;
            D.opened_at = now_ms();
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

static bool pick_mode(drmModeConnector *c, drmModeModeInfo *out)
{
    int w = 0, h = 0, r = 0;
    const char *want = getenv("PINDI_MODE");
    if (want && *want && sscanf(want, "%dx%d@%d", &w, &h, &r) >= 2) {
        for (int i = 0; i < c->count_modes; i++) {
            drmModeModeInfo *m = &c->modes[i];
            if (m->hdisplay == w && m->vdisplay == h && !(m->flags & DRM_MODE_FLAG_INTERLACE) &&
                (!r || (int)m->vrefresh == r)) {
                *out = *m;
                return true;
            }
        }
        LOG("display: PINDI_MODE=%s not offered by the monitor, using its preferred mode", want);
    }
    for (int i = 0; i < c->count_modes; i++)
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            *out = c->modes[i];
            return true;
        }
    for (int i = 0; i < c->count_modes; i++)
        if (!(c->modes[i].flags & DRM_MODE_FLAG_INTERLACE)) {
            *out = c->modes[i];
            return true;
        }
    return false;
}

static uint32_t primary_plane(int crtc_idx)
{
    uint32_t found = 0;
    drmModePlaneRes *pr = drmModeGetPlaneResources(D.fd);
    for (uint32_t i = 0; pr && i < pr->count_planes && !found; i++) {
        drmModePlane *p = drmModeGetPlane(D.fd, pr->planes[i]);
        if (p && (p->possible_crtcs & (1u << crtc_idx))) {
            drmModeObjectProperties *props =
                drmModeObjectGetProperties(D.fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            for (uint32_t k = 0; props && k < props->count_props; k++) {
                drmModePropertyRes *pp = drmModeGetProperty(D.fd, props->props[k]);
                if (pp && !strcmp(pp->name, "type") && props->prop_values[k] == DRM_PLANE_TYPE_PRIMARY)
                    found = p->plane_id;
                drmModeFreeProperty(pp);
            }
            drmModeFreeObjectProperties(props);
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    return found;
}

static void splash_update_text(const char *ip, bool force)
{
    if (!D.ok || (!force && !strcmp(ip, D.shown_ip)))
        return;
    snprintf(D.shown_ip, sizeof D.shown_ip, "%s", ip);
    memcpy(D.comp, D.base, (size_t)D.W * D.H * 4);

    if (font_ok) {
        /* Font size: the smaller of 2% of the screen height and 16pt. */
        int px = (int)lround(D.H * 0.02);
        if (px > FONT_PT_PX) px = FONT_PT_PX;
        if (px < 8) px = 8;
        FT_Set_Pixel_Sizes(face, 0, px);
        int margin = px;
        int base = D.H - margin + (int)(face->size->metrics.descender >> 6);
        int shadow = px >= 16 ? 2 : 1;
        char left[96], right[96];
        snprintf(left, sizeof left, "IP: %s", ip);
        snprintf(right, sizeof right, "Serial: %s", serial);
        int rx = D.W - margin - text_width(right);
        draw_text(D.comp, D.W, D.H, margin + shadow, base + shadow, left, 0x000000);
        draw_text(D.comp, D.W, D.H, rx + shadow, base + shadow, right, 0x000000);
        draw_text(D.comp, D.W, D.H, margin, base, left, 0xD0D0D0);
        draw_text(D.comp, D.W, D.H, rx, base, right, 0xD0D0D0);
    }
    for (int y = 0; y < D.H; y++)
        memcpy(D.splash.map + (size_t)y * D.splash.pitch, D.comp + (size_t)y * D.W, (size_t)D.W * 4);
}

static bool display_setup(void)
{
    drmModeRes *res = drmModeGetResources(D.fd);
    drmModeConnector *conn = NULL;
    bool ok = false;
    if (!res)
        return false;

    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(D.fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
            conn = c;
        else
            drmModeFreeConnector(c);
    }
    if (!conn || !pick_mode(conn, &D.mode))
        goto out;

    /* CRTC: keep the current one if the encoder has one, else first compatible. */
    D.crtc = 0;
    drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(D.fd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id)
        D.crtc = enc->crtc_id;
    drmModeFreeEncoder(enc);
    for (int e = 0; e < conn->count_encoders && !D.crtc; e++) {
        enc = drmModeGetEncoder(D.fd, conn->encoders[e]);
        for (int i = 0; enc && i < res->count_crtcs; i++)
            if (enc->possible_crtcs & (1u << i)) {
                D.crtc = res->crtcs[i];
                break;
            }
        drmModeFreeEncoder(enc);
    }
    int crtc_idx = -1;
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == D.crtc)
            crtc_idx = i;
    if (crtc_idx < 0)
        goto out;

    D.conn = conn->connector_id;
    D.plane = primary_plane(crtc_idx);
    D.W = D.mode.hdisplay;
    D.H = D.mode.vdisplay;
    if (!buf_create(D.W, D.H, DRM_FORMAT_XRGB8888, &D.splash))
        goto out;
    D.base = malloc((size_t)D.W * D.H * 4);
    D.comp = malloc((size_t)D.W * D.H * 4);
    render_fit(D.base, D.W, D.H, &splash_img);
    D.ok = true;

    char ip[64];
    read_ip(ip, sizeof ip);
    splash_update_text(ip, true);
    if (drmModeSetCrtc(D.fd, D.crtc, D.splash.fb, 0, 0, &D.conn, 1, &D.mode)) {
        LOG("display: set mode failed: %s", strerror(errno));
        display_teardown();
        goto out;
    }
    D.video_on = false;
    D.plane_err_logged = false;
    LOG("display: %dx%d@%d on %s", D.W, D.H, D.mode.vrefresh, D.mode.name);
    ok = true;
out:
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return ok;
}

static void show_splash(void)
{
    if (!D.ok || !D.video_on)
        return;
    if (drmModeSetCrtc(D.fd, D.crtc, D.splash.fb, 0, 0, &D.conn, 1, &D.mode))
        LOG("display: restore splash failed: %s", strerror(errno));
    D.video_on = false;
    buf_destroy(&D.vid[0]);
    buf_destroy(&D.vid[1]);
}

/* Called about once a second: opens the device, follows HDMI hotplug. */
static void display_poll(uint64_t now, uint64_t start)
{
    if (now - D.last_poll < 1000)
        return;
    D.last_poll = now;

    if (D.fd < 0 && !display_open(now - start > 10000))
        return;
    if (D.ok) {
        drmModeConnector *c = drmModeGetConnectorCurrent(D.fd, D.conn);
        bool connected = c && c->connection == DRM_MODE_CONNECTED;
        drmModeFreeConnector(c);
        if (connected)
            return;
        LOG("display: monitor disconnected");
        display_teardown();
    }
    display_setup();
}

/* The HVS has no packed 4:2:2 format, so split UYVY into NV16 (Y plane + CbCr plane). */
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

static void show_frame(const NDIlib_video_frame_v2_t *v)
{
    static int bad_w, bad_h;
    static uint32_t bad_fmt;
    uint32_t fmt;

    if (!D.ok)
        return;
    switch (v->FourCC) {
    case NDIlib_FourCC_video_type_UYVY:
    case NDIlib_FourCC_video_type_UYVA: /* alpha plane follows, ignored */
        fmt = DRM_FORMAT_NV16; break;
    case NDIlib_FourCC_video_type_BGRA:
    case NDIlib_FourCC_video_type_BGRX:
        fmt = DRM_FORMAT_XRGB8888; break;
    case NDIlib_FourCC_video_type_RGBA:
    case NDIlib_FourCC_video_type_RGBX:
        fmt = DRM_FORMAT_XBGR8888; break;
    default:
        if (!D.plane_err_logged)
            LOG("video: unsupported pixel format 0x%08x", (unsigned)v->FourCC);
        D.plane_err_logged = true;
        return;
    }

    /* New format/size: build a fresh buffer pair, swap after it is on screen. */
    struct buf old[2] = { D.vid[0], D.vid[1] };
    bool fresh = false;
    if (!D.vid[0].handle || D.vid[0].w != v->xres || D.vid[0].h != v->yres || D.vid[0].fmt != fmt) {
        struct buf nb[2];
        if (bad_w == v->xres && bad_h == v->yres && bad_fmt == fmt)
            return; /* already failed for this format, don't retry every frame */
        if (!buf_create(v->xres, v->yres, fmt, &nb[0]) ||
            !buf_create(v->xres, v->yres, fmt, &nb[1])) {
            buf_destroy(&nb[0]);
            bad_w = v->xres;
            bad_h = v->yres;
            bad_fmt = fmt;
            LOG("video: cannot display %dx%d - try PINDI_BANDWIDTH=lowest", v->xres, v->yres);
            return;
        }
        bad_w = bad_h = 0;
        D.vid[0] = nb[0];
        D.vid[1] = nb[1];
        D.vnext = 0;
        fresh = true;
        LOG("video: %dx%d %.4s %.2f fps", v->xres, v->yres, (const char *)&v->FourCC,
            v->frame_rate_D ? (double)v->frame_rate_N / v->frame_rate_D : 0.0);
    }

    struct buf *b = &D.vid[D.vnext];
    for (int y = 0; y < v->yres; y++) {
        const uint8_t *src = v->p_data + (size_t)y * v->line_stride_in_bytes;
        uint8_t *dst = b->map + (size_t)y * b->pitch;
        if (fmt == DRM_FORMAT_NV16)
            uyvy_to_nv16(src, dst, b->map + b->uv_off + (size_t)y * b->pitch, v->xres);
        else
            memcpy(dst, src, (size_t)v->xres * 4);
    }

    /* Fit to screen keeping the picture aspect ratio. */
    double ar = v->picture_aspect_ratio > 0 ? v->picture_aspect_ratio : (double)v->xres / v->yres;
    int dw = D.W, dh = (int)(D.W / ar + 0.5);
    if (dh > D.H) {
        dh = D.H;
        dw = (int)(D.H * ar + 0.5);
    }
    int dx = (D.W - dw) / 2, dy = (D.H - dh) / 2;

    /* Blocks until the flip is on screen, so the other buffer is free afterwards. */
    if (drmModeSetPlane(D.fd, D.plane, D.crtc, b->fb, 0, dx, dy, dw, dh, 0, 0,
                        (uint32_t)v->xres << 16, (uint32_t)v->yres << 16)) {
        if (!D.plane_err_logged)
            LOG("video: set plane failed: %s", strerror(errno));
        D.plane_err_logged = true;
    } else {
        D.video_on = true;
        D.vnext ^= 1;
    }
    if (fresh) {
        buf_destroy(&old[0]);
        buf_destroy(&old[1]);
    }
}

/* ------------------------------------------------------------------ NDI */

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

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); /* auto-reap modprobe */

    uint64_t start = now_ms();
    read_serial(serial, sizeof serial);

    char target[160], alt[160] = "";
    const char *custom = getenv("PINDI_STREAM");
    if (custom && *custom) {
        snprintf(target, sizeof target, "%s", custom);
    } else {
        snprintf(target, sizeof target, "OUT_%s", serial);
        const char *s = serial;
        while (*s == '0' && s[1])
            s++;
        if (s != serial)
            snprintf(alt, sizeof alt, "OUT_%s", s); /* also accept without leading zeros */
    }
    LOG("serial %s, looking for NDI stream %s", serial, target);

    /* Splash first - everything else can wait. Load the display driver (and the
     * I2C driver HDMI needs for EDID) now instead of waiting for udev. */
    if (access("/dev/dri/card0", F_OK) && fork() == 0) {
        execl("/sbin/modprobe", "modprobe", "-q", "-a", "i2c_bcm2835", "vc4", (char *)NULL);
        _exit(1);
    }
    if (load_jpeg(cfg("PINDI_SPLASH", "/usr/share/pindi/splash.jpg"), &splash_img))
        trim_edges(&splash_img, image_bg(&splash_img));
    font_ok = !FT_Init_FreeType(&ft) &&
              !FT_New_Face(ft, cfg("PINDI_FONT", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"), 0, &face);
    if (!font_ok)
        LOG("text: font not available, splash will have no text");
    while (running && !D.ok && now_ms() - start < 30000) {
        D.last_poll = 0;
        display_poll(now_ms(), start);
        if (!D.ok) {
            if (D.fd >= 0)
                break; /* device is there but no monitor yet: carry on, poll later */
            usleep(50000);
        }
    }
    LOG("splash up after %llu ms", (unsigned long long)(now_ms() - start));

    const char *bw_s = cfg("PINDI_BANDWIDTH", "highest");
    NDIlib_recv_bandwidth_e bandwidth =
        !strcasecmp(bw_s, "lowest") ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
    /* Many senders (screen capture, static graphics) only send frames on change. */
    int frame_timeout = atoi(cfg("PINDI_FRAME_TIMEOUT_MS", "0"));
    const char *extra_ips = getenv("PINDI_EXTRA_IPS");
    char recv_name[128];
    snprintf(recv_name, sizeof recv_name, "PiNDI %s", serial);

    /* libndi is large; it is loaded only now so it doesn't delay the splash. */
    void *lib = dlopen(NDILIB_LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL);
    const NDIlib_v6 *(*ndi_load)(void) = lib ? (const NDIlib_v6 *(*)(void))dlsym(lib, "NDIlib_v6_load") : NULL;
    ndi = ndi_load ? ndi_load() : NULL;
    bool ndi_ok = ndi && ndi->initialize();
    if (!ndi)
        LOG("ndi: cannot load %s: %s - splash only", NDILIB_LIBRARY_NAME, lib ? "no NDIlib_v6_load" : dlerror());
    else if (!ndi_ok)
        LOG("ndi: NDIlib_initialize failed (unsupported CPU?) - splash only");

    NDIlib_find_instance_t find = NULL;
    NDIlib_recv_instance_t recv = NULL;
    uint64_t find_created = 0, last_match = 0, last_ip_check = 0;
    uint64_t recv_start = 0, last_alive = 0, last_frame = 0, last_stats = 0;
    int stats_ms = atoi(cfg("PINDI_STATS_SEC", "60")) * 1000;
    uint32_t last_count = UINT32_MAX;
    char cur_src[256] = "", failed_src[256] = "", ip[64] = "";
    bool got_frame = false;

    while (running) {
        uint64_t now = now_ms();
        display_poll(now, start);

        if (now - last_ip_check >= 1000) {
            last_ip_check = now;
            read_ip(ip, sizeof ip);
            splash_update_text(ip, false);
        }

        if (!ndi_ok) {
            usleep(250000);
            continue;
        }

        /* Discovery needs avahi; give it up to 20 s to appear after boot. */
        if (!find) {
            if (access("/run/avahi-daemon/socket", F_OK) && now - start < 20000) {
                usleep(100000);
                continue;
            }
            NDIlib_find_create_t fc = { .show_local_sources = true, .p_groups = NULL,
                                        .p_extra_ips = (extra_ips && *extra_ips) ? extra_ips : NULL };
            find = ndi->find_create_v2(&fc);
            find_created = last_match = now;
            last_count = UINT32_MAX;
            if (!find) {
                LOG("ndi: cannot create finder");
                usleep(1000000);
                continue;
            }
            LOG("ndi: discovery started");
        }

        if (!recv) {
            ndi->find_wait_for_sources(find, 250);
            uint32_t count = 0;
            const NDIlib_source_t *srcs = ndi->find_get_current_sources(find, &count);
            if (count != last_count) {
                LOG("ndi: %u source(s) visible", count);
                for (uint32_t i = 0; i < count; i++)
                    LOG("ndi:   %s", srcs[i].p_ndi_name);
                last_count = count;
            }
            const NDIlib_source_t *pick = NULL;
            for (uint32_t i = 0; i < count; i++) {
                if (!srcs[i].p_ndi_name || !name_matches(srcs[i].p_ndi_name, target, alt))
                    continue;
                /* Prefer a match that has not just failed (e.g. stale entry on another host). */
                if (!pick || !strcmp(pick->p_ndi_name, failed_src))
                    pick = &srcs[i];
            }
            if (pick) {
                NDIlib_recv_create_v3_t rc = {
                    .source_to_connect_to = *pick,
                    .color_format = NDIlib_recv_color_format_UYVY_BGRA,
                    .bandwidth = bandwidth,
                    .allow_video_fields = false,
                    .p_ndi_recv_name = recv_name,
                };
                snprintf(cur_src, sizeof cur_src, "%s", pick->p_ndi_name);
                recv = ndi->recv_create_v3(&rc);
                if (recv) {
                    LOG("ndi: connecting to %s", cur_src);
                    recv_start = now;
                    last_alive = now + 5000; /* grace period for the first connection */
                    got_frame = false;
                } else {
                    LOG("ndi: cannot create receiver for %s", cur_src);
                    snprintf(failed_src, sizeof failed_src, "%s", cur_src);
                }
                last_match = now;
            } else if (now - last_match > 30000 && now - find_created > 30000) {
                /* Nothing for a while: restart discovery in case the network changed. */
                LOG("ndi: no match for 30 s, restarting discovery");
                ndi->find_destroy(find);
                find = NULL;
            }
            continue;
        }

        NDIlib_video_frame_v2_t v;
        NDIlib_frame_type_e t = ndi->recv_capture_v3(recv, &v, NULL, NULL, 100);
        now = now_ms();
        if (t == NDIlib_frame_type_video) {
            if (!got_frame)
                LOG("ndi: first frame after %llu ms", (unsigned long long)(now - recv_start));
            show_frame(&v);
            ndi->recv_free_video_v2(recv, &v);
            got_frame = true;
            last_frame = last_alive = now;
            failed_src[0] = 0;
        }
        int conns = ndi->recv_get_no_connections(recv);
        if (conns > 0 && now > last_alive)
            last_alive = now;

        if (now - last_stats >= (uint64_t)stats_ms && stats_ms > 0) {
            NDIlib_recv_performance_t total, dropped;
            ndi->recv_get_performance(recv, &total, &dropped);
            LOG("ndi: stats %s: connections %d, video frames %lld received, %lld dropped",
                cur_src, conns, (long long)total.video_frames, (long long)dropped.video_frames);
            last_stats = now;
        }

        if (D.video_on && frame_timeout > 0 && now - last_frame > (uint64_t)frame_timeout) {
            LOG("ndi: no frames for %d ms, showing splash", frame_timeout);
            show_splash();
        }
        if (t == NDIlib_frame_type_error || (now > last_alive && now - last_alive > 3000)) {
            LOG("ndi: lost %s", cur_src);
            if (!got_frame)
                snprintf(failed_src, sizeof failed_src, "%s", cur_src);
            ndi->recv_destroy(recv);
            recv = NULL;
            show_splash();
        }
    }

    LOG("shutting down");
    if (recv)
        ndi->recv_destroy(recv);
    if (find)
        ndi->find_destroy(find);
    if (ndi_ok)
        ndi->destroy();
    display_close();
    return 0;
}

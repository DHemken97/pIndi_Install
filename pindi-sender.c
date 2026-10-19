/*
 * pindi-sender - PiNDI outgoing NDI streams from USB (UVC) HDMI capture devices.
 *
 * Nothing is sent unless configured (web UI "Configure Outgoing Streams"):
 *   PINDI_IN<n>_ENABLED=1
 *   PINDI_IN<n>_DEVICE=/dev/v4l/by-id/...      stable device path
 *   PINDI_IN<n>_NAME=IN_<serial>               NDI stream name (default IN_<serial>[_n])
 *   PINDI_IN<n>_MODE="MJPG 1920x1080@30"       optional, else the best mode up to 1080p30
 *
 * One thread per input: V4L2 capture -> (MJPEG decode straight to YUV) -> NDI send.
 * Status for the web UI in /run/pindi/in<n>.status.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <turbojpeg.h>

#include <dlfcn.h>
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#define LOG(...) do { printf(__VA_ARGS__); putchar('\n'); } while (0)
#define MAX_IN 8
#define NCAP 4
#define RUN_DIR "/run/pindi"

static const NDIlib_v6 *ndi;
static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }
static char serial[64];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *icfg(int n, const char *key, const char *def)
{
    char k[64];
    snprintf(k, sizeof k, "PINDI_IN%d_%s", n, key);
    const char *v = getenv(k);
    return (v && *v) ? v : def;
}

static void read_serial(void)
{
    FILE *f = fopen("/sys/firmware/devicetree/base/serial-number", "rb");
    size_t len = f ? fread(serial, 1, sizeof serial - 1, f) : 0, j = 0;
    if (f)
        fclose(f);
    for (size_t i = 0; i < len; i++)
        if (isalnum((unsigned char)serial[i]))
            serial[j++] = serial[i];
    serial[j] = 0;
    if (!serial[0])
        snprintf(serial, sizeof serial, "unknown");
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

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

/* ------------------------------------------------------------------ pixel conversion */

/* YUYV -> UYVY: swap each byte pair. */
static void yuyv_to_uyvy(const uint8_t *s, uint8_t *d, size_t bytes)
{
    size_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 16 <= bytes; i += 16)
        vst1q_u8(d + i, vrev16q_u8(vld1q_u8(s + i)));
#endif
    for (; i + 2 <= bytes; i += 2) {
        d[i] = s[i + 1];
        d[i + 1] = s[i];
    }
}

/* Planar 4:2:2 (Y, U, V rows) -> UYVY row. */
static void yuv422p_to_uyvy(const uint8_t *y, const uint8_t *u, const uint8_t *v, uint8_t *d, int w)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; x + 32 <= w; x += 32) {
        uint8x16x2_t yy = vld2q_u8(y + x); /* even, odd luma */
        uint8x16x4_t o = { { vld1q_u8(u + x / 2), yy.val[0], vld1q_u8(v + x / 2), yy.val[1] } };
        vst4q_u8(d + x * 2, o);
    }
#endif
    for (; x + 2 <= w; x += 2) {
        d[x * 2] = u[x / 2];
        d[x * 2 + 1] = y[x];
        d[x * 2 + 2] = v[x / 2];
        d[x * 2 + 3] = y[x + 1];
    }
}

/* ------------------------------------------------------------------ capture modes */

struct mode { uint32_t fourcc; int w, h; double fps; uint32_t num, den; };

static bool fourcc_ok(uint32_t f)
{
    return f == V4L2_PIX_FMT_MJPEG || f == V4L2_PIX_FMT_YUYV || f == V4L2_PIX_FMT_UYVY;
}

/* Enumerate the device's modes and pick the configured one, else the best up to 1080p30. */
static bool choose_mode(int fd, const char *want, struct mode *out)
{
    char wf[8] = "";
    int ww = 0, wh = 0;
    double wfps = 0;
    if (want && *want && sscanf(want, "%4s %dx%d@%lf", wf, &ww, &wh, &wfps) < 3)
        wf[0] = 0;

    bool found = false;
    struct mode best = { 0 };
    double best_score = -1;
    struct v4l2_fmtdesc fd_desc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    for (fd_desc.index = 0; !xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc); fd_desc.index++) {
        uint32_t f = fd_desc.pixelformat;
        if (!fourcc_ok(f))
            continue;
        struct v4l2_frmsizeenum fs = { .pixel_format = f };
        for (fs.index = 0; !xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs); fs.index++) {
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                break;
            int w = fs.discrete.width, h = fs.discrete.height;
            struct v4l2_frmivalenum fi = { .pixel_format = f, .width = w, .height = h };
            for (fi.index = 0; !xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi); fi.index++) {
                if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE || !fi.discrete.numerator)
                    break;
                double fps = (double)fi.discrete.denominator / fi.discrete.numerator;
                struct mode m = { f, w, h, fps, fi.discrete.numerator, fi.discrete.denominator };
                if (wf[0]) {
                    char fc[5];
                    memcpy(fc, &f, 4);
                    fc[4] = 0;
                    if (!strcasecmp(fc, wf) && w == ww && h == wh && (wfps <= 0 || fabs(fps - wfps) < 0.5)) {
                        *out = m;
                        return true;
                    }
                    continue;
                }
                if (w > 1920 || h > 1080)
                    continue;
                /* Prefer >= 24 fps, then resolution, then MJPEG (USB 2 can't carry raw 1080p),
                 * then frame rate up to 30. */
                double score = (fps >= 23.9 ? 1e9 : 0) + (double)w * h * 10 + (f == V4L2_PIX_FMT_MJPEG ? 5 : 0) +
                               (fps > 30.5 ? 0 : fps / 100);
                if (score > best_score) {
                    best_score = score;
                    best = m;
                    found = true;
                }
            }
        }
    }
    if (wf[0])
        LOG("capture: configured mode \"%s\" not offered by the device, using the default", want);
    if (wf[0])
        return choose_mode(fd, NULL, out);
    *out = best;
    return found;
}

/* ------------------------------------------------------------------ input thread */

struct input {
    int n;
    char device[256], name[128], mode_s[64];
    pthread_t thr;
};

static void status(struct input *in, const char *state, const char *mode, double fps)
{
    char buf[768], path[64];
    snprintf(buf, sizeof buf, "state=%s\ndevice=%s\nname=%s\nmode=%s\nfps=%.1f\n", state, in->device, in->name,
             mode ? mode : "", fps);
    snprintf(path, sizeof path, RUN_DIR "/in%d.status", in->n);
    write_file_atomic(path, buf);
}

struct capbuf { void *start; size_t len; };

static void run_device(struct input *in, NDIlib_send_instance_t send)
{
    int fd = open(in->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        status(in, "waiting for device", NULL, 0);
        return;
    }
    struct capbuf cap[NCAP] = { 0 };
    int ncap = 0;
    uint8_t *frame[2] = { NULL, NULL }, *planes = NULL;
    tjhandle tj = NULL;
    struct mode m;
    char mode_desc[64] = "";

    if (!choose_mode(fd, in->mode_s, &m)) {
        status(in, "no usable video format", NULL, 0);
        goto out;
    }
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = m.w;
    fmt.fmt.pix.height = m.h;
    fmt.fmt.pix.pixelformat = m.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt)) {
        status(in, "cannot set format", NULL, 0);
        goto out;
    }
    m.w = fmt.fmt.pix.width;
    m.h = fmt.fmt.pix.height;
    struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    parm.parm.capture.timeperframe.numerator = m.num;
    parm.parm.capture.timeperframe.denominator = m.den;
    if (!xioctl(fd, VIDIOC_S_PARM, &parm) && parm.parm.capture.timeperframe.numerator) {
        m.num = parm.parm.capture.timeperframe.numerator;
        m.den = parm.parm.capture.timeperframe.denominator;
    }
    snprintf(mode_desc, sizeof mode_desc, "%.4s %dx%d@%.4g", (const char *)&m.fourcc, m.w, m.h,
             (double)m.den / m.num);

    struct v4l2_requestbuffers req = { .count = NCAP, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(fd, VIDIOC_REQBUFS, &req) || req.count < 2) {
        status(in, "cannot allocate capture buffers", mode_desc, 0);
        goto out;
    }
    for (ncap = 0; ncap < (int)req.count && ncap < NCAP; ncap++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = ncap };
        if (xioctl(fd, VIDIOC_QUERYBUF, &b))
            break;
        cap[ncap].len = b.length;
        cap[ncap].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (cap[ncap].start == MAP_FAILED) {
            cap[ncap].start = NULL;
            break;
        }
        xioctl(fd, VIDIOC_QBUF, &b);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ncap < 2 || xioctl(fd, VIDIOC_STREAMON, &type)) {
        status(in, "cannot start capture", mode_desc, 0);
        goto out;
    }

    /* Two output frames: NDI's async send keeps using the previous one while we fill the next. */
    size_t frame_bytes = (size_t)m.w * m.h * 4;
    frame[0] = malloc(frame_bytes);
    frame[1] = malloc(frame_bytes);
    planes = malloc((size_t)m.w * m.h * 2);
    if (m.fourcc == V4L2_PIX_FMT_MJPEG)
        tj = tjInitDecompress();
    LOG("input %d: %s from %s as NDI \"%s\"", in->n, mode_desc, in->device, in->name);
    status(in, "starting", mode_desc, 0);

    int cur = 0, timeouts = 0, frames = 0;
    uint64_t t0 = now_ms();
    bool warned = false;
    while (running) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 1000);
        if (r == 0) {
            status(in, "no signal", mode_desc, 0);
            if (++timeouts >= 5)
                break; /* re-open the device */
            continue;
        }
        if (r < 0 || (p.revents & (POLLERR | POLLHUP)))
            break;
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (xioctl(fd, VIDIOC_DQBUF, &b)) {
            if (errno == EAGAIN)
                continue;
            break;
        }
        timeouts = 0;
        const uint8_t *src = cap[b.index].start;
        NDIlib_video_frame_v2_t f = {
            .xres = m.w, .yres = m.h,
            .frame_rate_N = (int)m.den, .frame_rate_D = (int)m.num,
            .picture_aspect_ratio = 0,
            .frame_format_type = NDIlib_frame_format_type_progressive,
            .timecode = NDIlib_send_timecode_synthesize,
            .p_data = frame[cur],
        };
        bool ok = true;
        if (m.fourcc == V4L2_PIX_FMT_MJPEG) {
            int jw, jh, sub, cs;
            if (!b.bytesused || tjDecompressHeader3(tj, src, b.bytesused, &jw, &jh, &sub, &cs) || jw != m.w ||
                jh != m.h) {
                ok = false;
            } else if (sub == TJSAMP_420) {
                /* Decode straight into NDI's I420 layout. */
                unsigned char *dst[3] = { frame[cur], frame[cur] + (size_t)m.w * m.h,
                                          frame[cur] + (size_t)m.w * m.h * 5 / 4 };
                int strides[3] = { m.w, m.w / 2, m.w / 2 };
                ok = !tjDecompressToYUVPlanes(tj, src, b.bytesused, dst, m.w, strides, m.h, TJFLAG_FASTDCT) ||
                     tjGetErrorCode(tj) == TJERR_WARNING;
                f.FourCC = NDIlib_FourCC_video_type_I420;
                f.line_stride_in_bytes = m.w;
            } else if (sub == TJSAMP_422) {
                /* Decode to planar 4:2:2, then interleave to UYVY (NDI's native format). */
                unsigned char *dst[3] = { planes, planes + (size_t)m.w * m.h, planes + (size_t)m.w * m.h * 3 / 2 };
                int strides[3] = { m.w, m.w / 2, m.w / 2 };
                ok = !tjDecompressToYUVPlanes(tj, src, b.bytesused, dst, m.w, strides, m.h, TJFLAG_FASTDCT) ||
                     tjGetErrorCode(tj) == TJERR_WARNING;
                for (int y = 0; ok && y < m.h; y++)
                    yuv422p_to_uyvy(dst[0] + (size_t)y * m.w, dst[1] + (size_t)y * m.w / 2,
                                    dst[2] + (size_t)y * m.w / 2, frame[cur] + (size_t)y * m.w * 2, m.w);
                f.FourCC = NDIlib_FourCC_video_type_UYVY;
                f.line_stride_in_bytes = m.w * 2;
            } else {
                ok = !tjDecompress2(tj, src, b.bytesused, frame[cur], m.w, m.w * 4, m.h, TJPF_BGRX, TJFLAG_FASTDCT) ||
                     tjGetErrorCode(tj) == TJERR_WARNING;
                f.FourCC = NDIlib_FourCC_video_type_BGRX;
                f.line_stride_in_bytes = m.w * 4;
            }
            if (!ok && !warned) {
                LOG("input %d: skipping undecodable MJPEG frame(s)", in->n);
                warned = true;
            }
        } else if (m.fourcc == V4L2_PIX_FMT_YUYV) {
            yuyv_to_uyvy(src, frame[cur], (size_t)m.w * m.h * 2);
            f.FourCC = NDIlib_FourCC_video_type_UYVY;
            f.line_stride_in_bytes = m.w * 2;
        } else {
            memcpy(frame[cur], src, (size_t)m.w * m.h * 2);
            f.FourCC = NDIlib_FourCC_video_type_UYVY;
            f.line_stride_in_bytes = m.w * 2;
        }
        xioctl(fd, VIDIOC_QBUF, &b);
        if (!ok)
            continue;

        ndi->send_send_video_async_v2(send, &f);
        cur ^= 1;
        frames++;
        uint64_t now = now_ms();
        if (now - t0 >= 2000) {
            int conns = ndi->send_get_no_connections(send, 0);
            char st[64];
            snprintf(st, sizeof st, "sending (%d receiver%s)", conns, conns == 1 ? "" : "s");
            status(in, st, mode_desc, frames * 1000.0 / (now - t0));
            frames = 0;
            t0 = now;
        }
    }
    ndi->send_send_video_async_v2(send, NULL); /* NDI is done with our buffers after this */
    xioctl(fd, VIDIOC_STREAMOFF, &type);
out:
    for (int i = 0; i < ncap; i++)
        if (cap[i].start)
            munmap(cap[i].start, cap[i].len);
    if (tj)
        tjDestroy(tj);
    free(frame[0]);
    free(frame[1]);
    free(planes);
    close(fd);
    if (running)
        status(in, "device lost, retrying", mode_desc, 0);
}

static void *input_thread(void *arg)
{
    struct input *in = arg;
    NDIlib_send_create_t sc = { .p_ndi_name = in->name, .p_groups = NULL, .clock_video = false, .clock_audio = false };
    NDIlib_send_instance_t send = ndi->send_create(&sc);
    if (!send) {
        LOG("input %d: cannot create NDI sender \"%s\"", in->n, in->name);
        status(in, "cannot create NDI sender", NULL, 0);
        return NULL;
    }
    while (running) {
        run_device(in, send);
        for (int i = 0; i < 20 && running; i++)
            usleep(100000);
    }
    ndi->send_destroy(send);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    read_serial();
    mkdir(RUN_DIR, 0755);

    struct input ins[MAX_IN];
    int nin = 0;
    for (int n = 1; n <= MAX_IN; n++) {
        char path[64];
        snprintf(path, sizeof path, RUN_DIR "/in%d.status", n);
        unlink(path);
        if (strcmp(icfg(n, "ENABLED", "0"), "1") || !*icfg(n, "DEVICE", ""))
            continue;
        struct input *in = &ins[nin++];
        memset(in, 0, sizeof *in);
        in->n = n;
        snprintf(in->device, sizeof in->device, "%s", icfg(n, "DEVICE", ""));
        char def[96];
        if (n == 1)
            snprintf(def, sizeof def, "IN_%s", serial);
        else
            snprintf(def, sizeof def, "IN_%s_%d", serial, n);
        snprintf(in->name, sizeof in->name, "%s", icfg(n, "NAME", def));
        snprintf(in->mode_s, sizeof in->mode_s, "%s", icfg(n, "MODE", ""));
    }
    if (!nin) {
        LOG("no outgoing streams configured");
        return 0;
    }

    void *lib = dlopen(NDILIB_LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL);
    const NDIlib_v6 *(*load)(void) = lib ? (const NDIlib_v6 *(*)(void))dlsym(lib, "NDIlib_v6_load") : NULL;
    ndi = load ? load() : NULL;
    if (!ndi || !ndi->initialize()) {
        LOG("cannot load/initialise NDI");
        return 1;
    }
    for (int i = 0; i < nin; i++)
        pthread_create(&ins[i].thr, NULL, input_thread, &ins[i]);
    for (int i = 0; i < nin; i++)
        pthread_join(ins[i].thr, NULL);
    ndi->destroy();
    return 0;
}

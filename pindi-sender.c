/*
 * pindi-sender - PiNDI outgoing NDI streams from USB (UVC) HDMI capture devices.
 *
 * Nothing is sent unless configured (web UI "Configure Outgoing Streams"):
 *   PINDI_IN<n>_ENABLED=1
 *   PINDI_IN<n>_DEVICE=/dev/v4l/by-id/...      stable device path
 *   PINDI_IN<n>_NAME=IN_<serial>               NDI stream name (default IN_<serial>[_n])
 *   PINDI_IN<n>_MODE="MJPG 1920x1080@30"       optional, else the best mode up to 1080p30
 *   PINDI_IN<n>_AUDIO=auto                     the device's own USB audio (default), or off
 *   PINDI_IN<n>_AUDIO_SYNC=off                 send the audio as the device's clock delivers it (default:
 *                                              kept in step with this Pi's clock)
 *
 * One thread per input: V4L2 capture -> decode workers (MJPEG decode straight to YUV, two threads
 * at HD) -> NDI send, and one for its audio: ALSA capture (48 kHz stereo) -> NDI send.
 * Status for the web UI in /run/pindi/in<n>.status.
 *
 * The decoded pictures and the audio are also published in shared memory (/run/pindi/in<n>.local,
 * struct local_feed). pindi-player shows an input of this device from there instead of receiving it
 * over NDI: no NDI encoding and decoding on the same Pi, which a Pi 3 cannot do at 1080p60.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <turbojpeg.h>
#include <alsa/asoundlib.h>

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
/* The NDI Advanced SDK's evaluation build said its 30 minutes are up: NDI|HX video stops (audio and
 * Full NDI go on). Only the library's own message tells, so its output is watched (watch_output). */
static volatile sig_atomic_t ndi_eval_over;
static void on_signal(int sig) { (void)sig; running = 0; }
static char serial[64];

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
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

/* YUYV / UYVY -> planar 4:2:2. */
static void packed_to_422p(const uint8_t *s, uint8_t *y, uint8_t *u, uint8_t *v, int w, bool uyvy)
{
    int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; x + 32 <= w; x += 32) {
        uint8x16x4_t p = vld4q_u8(s + x * 2); /* YUYV: Y0 U Y1 V; UYVY: U Y0 V Y1 */
        uint8x16x2_t yy = uyvy ? (uint8x16x2_t){ { p.val[1], p.val[3] } } : (uint8x16x2_t){ { p.val[0], p.val[2] } };
        vst2q_u8(y + x, yy);
        vst1q_u8(u + x / 2, uyvy ? p.val[0] : p.val[1]);
        vst1q_u8(v + x / 2, uyvy ? p.val[2] : p.val[3]);
    }
#endif
    for (; x + 2 <= w; x += 2) {
        const uint8_t *p = s + x * 2;
        y[x] = p[uyvy ? 1 : 0];
        y[x + 1] = p[uyvy ? 3 : 2];
        u[x / 2] = p[uyvy ? 0 : 1];
        v[x / 2] = p[uyvy ? 2 : 3];
    }
}

/* JPEG (capture cards, webcams) is full range, 0-255; NDI, the H.264 encoder and the display expect
 * video range: black 16, white 235, colour 16-240. Without this, everything below 16 is shown black
 * (in a dark scene from a capture card that was over half the picture). y' = 16 + y * 219/255,
 * c' = 128 + (c - 128) * 224/255, in place, in 8.8 fixed point. */
static void full_to_video(uint8_t *p, size_t n, bool chroma)
{
    const int mul = chroma ? 225 : 220, add = chroma ? 128 * 256 - 128 * 225 + 128 : 16 * 256 + 128;
    size_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint8x8_t vm = vdup_n_u8((uint8_t)mul);
    const uint16x8_t va = vdupq_n_u16((uint16_t)add);
    for (; i + 16 <= n; i += 16) {
        uint8x16_t v = vld1q_u8(p + i);
        uint16x8_t lo = vmlal_u8(va, vget_low_u8(v), vm), hi = vmlal_u8(va, vget_high_u8(v), vm);
        vst1q_u8(p + i, vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)));
    }
#endif
    for (; i < n; i++)
        p[i] = (uint8_t)((p[i] * mul + add) >> 8);
}

/* ------------------------------------------------------------------ shared memory for pindi-player */

/* Keep in sync with pindi-player.c. Frames: LOCAL_SLOTS slots of slot_size bytes from LOCAL_DATA,
 * planar Y, U, V. A slot's slot_seq is 0 while it is being written. */
#define LOCAL_MAGIC 0x4c444e50 /* "PNDL" */
#define LOCAL_SLOTS 4
#define LOCAL_AUDIO 32768      /* audio ring: frames of 48 kHz stereo */
#define LOCAL_DATA 262144
enum { LOCAL_422P = 1, LOCAL_420P = 2 };

struct local_feed {
    uint32_t magic, version;
    char name[128];
    int32_t w, h, fps_n, fps_d;
    uint32_t fmt, slot_size;
    uint64_t seq; /* newest frame, 0 = none yet */
    uint64_t slot_seq[LOCAL_SLOTS];
    int64_t slot_time[LOCAL_SLOTS];  /* capture time, 100 ns since 1970 (as NDI timestamps) */
    uint64_t writer_ms, reader_ms;   /* CLOCK_MONOTONIC heartbeats */
    uint64_t audio_wr;               /* audio frames written so far */
    int16_t audio[LOCAL_AUDIO * 2];
};

#define LOAD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* Somebody (pindi-player) has read the feed within the last 1.5 s. */
static bool local_wanted(struct local_feed *lf)
{
    if (!lf)
        return false;
    uint64_t r = LOAD(lf->reader_ms);
    return r && now_ms() - r < 1500;
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
    bool audio_on;
    bool audio_sync; /* keep the audio in step with this Pi's clock (PINDI_IN<n>_AUDIO_SYNC) */
    bool hx; /* PINDI_IN<n>_CODEC=hx: NDI|HX through the hardware H.264 encoder */
    bool video_range; /* PINDI_IN<n>_RANGE=video: the device's JPEG is already 16-235 (else converted) */
    pthread_t thr, athr;
    NDIlib_send_instance_t send;
    int w, h, fps_n, fps_d; /* last capture mode, for the black "no signal" frames */
    uint8_t *black;
    pthread_mutex_t lock;
    char audio[256]; /* audio status line (lock) */
    struct local_feed *lf; /* header of in<n>.local (fixed mapping, the audio thread uses it too) */
    int lf_fd;
    uint8_t *lf_frames;    /* its frame slots (remapped when the capture mode changes) */
    size_t lf_frames_len;
};

/* Create in<n>.local, or resize its frame slots for a new capture mode. */
static bool local_setup(struct input *in, int w, int h, uint32_t fmt, int fps_n, int fps_d)
{
    if (!in->lf) {
        char path[64];
        snprintf(path, sizeof path, RUN_DIR "/in%d.local", in->n);
        in->lf_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (in->lf_fd < 0 || ftruncate(in->lf_fd, LOCAL_DATA))
            return false;
        in->lf = mmap(NULL, LOCAL_DATA, PROT_READ | PROT_WRITE, MAP_SHARED, in->lf_fd, 0);
        if (in->lf == MAP_FAILED) {
            in->lf = NULL;
            return false;
        }
        snprintf(in->lf->name, sizeof in->lf->name, "%s", in->name);
        in->lf->version = 1;
        STORE(in->lf->magic, LOCAL_MAGIC);
    }
    struct local_feed *lf = in->lf;
    size_t slot = (size_t)w * h * 2, len = slot * LOCAL_SLOTS;
    if (in->lf_frames && in->lf_frames_len != len) {
        munmap(in->lf_frames, in->lf_frames_len);
        in->lf_frames = NULL;
    }
    STORE(lf->seq, 0); /* readers wait for the first frame of the new layout */
    for (int i = 0; i < LOCAL_SLOTS; i++)
        STORE(lf->slot_seq[i], 0);
    if (!in->lf_frames) {
        if (ftruncate(in->lf_fd, LOCAL_DATA + len))
            return false;
        in->lf_frames = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, in->lf_fd, LOCAL_DATA);
        if (in->lf_frames == MAP_FAILED) {
            in->lf_frames = NULL;
            return false;
        }
        in->lf_frames_len = len;
    }
    lf->w = w;
    lf->h = h;
    lf->fps_n = fps_n;
    lf->fps_d = fps_d;
    lf->fmt = fmt;
    STORE(lf->slot_size, (uint32_t)slot);
    return true;
}

/* NDI timestamp (100 ns since 1970) of a V4L2 buffer's capture time (monotonic). */
static int64_t capture_time(const struct timeval *tv)
{
    struct timespec rt, mt;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mt);
    int64_t age_us = ((int64_t)mt.tv_sec * 1000000 + mt.tv_nsec / 1000) - ((int64_t)tv->tv_sec * 1000000 + tv->tv_usec);
    if (!tv->tv_sec || age_us < 0 || age_us > 1000000)
        age_us = 0;
    return ((int64_t)rt.tv_sec * 1000000 + rt.tv_nsec / 1000 - age_us) * 10;
}

static void set_audio_status(struct input *in, const char *s)
{
    pthread_mutex_lock(&in->lock);
    snprintf(in->audio, sizeof in->audio, "%s", s);
    pthread_mutex_unlock(&in->lock);
}

/* fps = frames sent, cam_fps = frames from the device, damaged = frames skipped (last 2 s). */
static void status(struct input *in, const char *state, const char *mode, double fps, double cam_fps, int damaged)
{
    char buf[1024], path[64], audio[sizeof in->audio];
    pthread_mutex_lock(&in->lock);
    snprintf(audio, sizeof audio, "%s", in->audio);
    pthread_mutex_unlock(&in->lock);
    snprintf(buf, sizeof buf, "state=%s\ndevice=%s\nname=%s\nmode=%s\nfps=%.1f\ncam_fps=%.1f\ndamaged=%d\naudio=%s\n",
             state, in->device, in->name, mode ? mode : "", fps, cam_fps, damaged, audio);
    snprintf(path, sizeof path, RUN_DIR "/in%d.status", in->n);
    write_file_atomic(path, buf);
}

struct capbuf { void *start; size_t len; };

/* No picture from the device (HDMI source unplugged, device lost): send black once a second, so
 * receivers don't sit on a frozen last frame. Only while someone is connected. */
static void send_black(struct input *in, NDIlib_send_instance_t send)
{
    if (in->w <= 0 || in->h <= 0 || !ndi->send_get_no_connections(send, 0))
        return;
    if (!in->black) {
        size_t n = (size_t)in->w * in->h * 2;
        in->black = malloc(n);
        if (!in->black)
            return;
        for (size_t i = 0; i < n; i += 2) {
            in->black[i] = 0x80; /* U/V */
            in->black[i + 1] = 0x10; /* Y: black */
        }
    }
    NDIlib_video_frame_v2_t f = {
        .xres = in->w, .yres = in->h, .FourCC = NDIlib_FourCC_video_type_UYVY,
        .frame_rate_N = in->fps_n, .frame_rate_D = in->fps_d,
        .frame_format_type = NDIlib_frame_format_type_progressive,
        .timecode = NDIlib_send_timecode_synthesize, .p_data = in->black, .line_stride_in_bytes = in->w * 2,
    };
    ndi->send_send_video_v2(send, &f); /* synchronous: never mixed with the async capture frames */
}

/* Decoding runs in worker threads: one MJPEG decoder thread manages about 30 fps at 1080p on a
 * Pi 3, two take turns for 60 (three with NDI|HX, whose encoding needs no CPU: decoding plus the
 * conversion for the encoder take ~50 ms a frame). Frame s goes to worker s % nw; results are published (shared memory,
 * NDI) strictly in order. NDI encoding has its own thread: SpeedHQ at 1080p takes ~50 ms a frame on
 * a Pi 3, so it sends the newest finished frame whenever it is free and the others skip NDI. That
 * way the encoder never holds up this device's own display. NDI frames: NOUT buffers, each free,
 * being filled by a worker, pending (the newest, waiting for the encoder) or being sent. */
#define MAX_WORKERS 3
#define NOUT 4
enum { O_FREE, O_FILLING, O_PENDING, O_SENDING };

/* ------------------------------------------------------------------ NDI|HX: hardware H.264 encoder */

/* With PINDI_IN<n>_CODEC=hx the stream is sent as NDI|HX: the Pi 3/4 hardware encoder
 * (bcm2835-codec, a V4L2 memory-to-memory device) makes H.264 from the UYVY pictures and the NDI
 * Advanced SDK sends it as is. No SpeedHQ encoding on the CPU, a tenth of the bandwidth. The
 * pipeline's NDI frames are then the encoder's input buffers: the workers convert into them
 * directly; O_SENDING = queued in the encoder. */
#define ENC_OUT_BUFS 4

struct hwenc {
    int fd;
    uint8_t *in_map[NOUT];
    size_t in_len[NOUT];
    uint8_t *out_map[ENC_OUT_BUFS];
    size_t out_len[ENC_OUT_BUFS];
    int bpl;            /* bytes per UYVY input line */
    uint8_t *pkt;       /* NDIlib_compressed_packet_t + frame + SPS/PPS */
    size_t pkt_cap;
    int64_t frame_100ns;
    uint64_t sent;
    int keys, forced; /* keyframes sent / forced for new receivers, since the last log */
    int conns;        /* receivers at the last frame: a new one gets a keyframe right away */
};

static char enc_dev[32]; /* "" = no hardware H.264 encoder */
/* Advanced SDK only (NULL with the standard SDK, which cannot send compressed video). */
static int (*ndi_target_bit_rate)(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t *);
static bool (*ndi_keyframe_required)(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t *);

static void hwenc_find(void)
{
    for (int i = 0; i < 64 && !*enc_dev; i++) {
        char path[64], name[64] = "";
        snprintf(path, sizeof path, "/sys/class/video4linux/video%d/name", i);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof name, f) && !strncmp(name, "bcm2835-codec-encode", 20) && !strstr(name, "image"))
            snprintf(enc_dev, sizeof enc_dev, "/dev/video%d", i);
        fclose(f);
    }
}

static void hwenc_close(struct hwenc *e)
{
    if (e->fd < 0)
        return;
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(e->fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(e->fd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NOUT; i++)
        if (e->in_map[i])
            munmap(e->in_map[i], e->in_len[i]);
    for (int i = 0; i < ENC_OUT_BUFS; i++)
        if (e->out_map[i])
            munmap(e->out_map[i], e->out_len[i]);
    close(e->fd);
    free(e->pkt);
    memset(e, 0, sizeof *e);
    e->fd = -1;
}

static void enc_ctrl(int fd, uint32_t id, int value)
{
    struct v4l2_control c = { .id = id, .value = value };
    xioctl(fd, VIDIOC_S_CTRL, &c);
}

static bool hwenc_open(struct hwenc *e, const struct mode *m, NDIlib_send_instance_t send, int n)
{
    memset(e, 0, sizeof *e);
    e->fd = open(enc_dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (e->fd < 0)
        goto fail;
    struct v4l2_format in = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    in.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_UYVY;
    in.fmt.pix_mp.width = m->w;
    in.fmt.pix_mp.height = m->h;
    in.fmt.pix_mp.num_planes = 1;
    struct v4l2_format out = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    out.fmt.pix_mp.width = m->w;
    out.fmt.pix_mp.height = m->h;
    out.fmt.pix_mp.num_planes = 1;
    out.fmt.pix_mp.plane_fmt[0].sizeimage = 2 << 20;
    if (xioctl(e->fd, VIDIOC_S_FMT, &in) || xioctl(e->fd, VIDIOC_S_FMT, &out))
        goto fail;
    e->bpl = (int)in.fmt.pix_mp.plane_fmt[0].bytesperline;
    struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    sp.parm.output.timeperframe.numerator = m->num;
    sp.parm.output.timeperframe.denominator = m->den;
    xioctl(e->fd, VIDIOC_S_PARM, &sp);
    /* The bit rate NDI wants for HX at this size and rate; a keyframe every second; parameter sets
     * with every keyframe, so receivers can join any time. */
    NDIlib_video_frame_v2_t f = { .xres = m->w, .yres = m->h, .FourCC = (NDIlib_FourCC_video_type_e)NDI_LIB_FOURCC('H', '2', '6', '4'),
                                  .frame_rate_N = (int)m->den, .frame_rate_D = (int)m->num,
                                  .frame_format_type = NDIlib_frame_format_type_progressive };
    int bps = ndi_target_bit_rate ? ndi_target_bit_rate(send, &f) : 0;
    if (bps <= 0)
        bps = (int)((double)m->w * m->h * m->den / m->num * 0.1); /* ~12 Mbit/s at 1080p60 */
    if (bps > 25000000)
        bps = 25000000;
    enc_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE, bps);
    /* A keyframe at least every 30 frames: the encoder manages about 1080p30, so that's about a second
     * even from a 60 fps card (a period of 60 gave one every 2 s). */
    int period = (int)(m->den / m->num + 0.5);
    enc_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, period > 30 ? 30 : period);
    enc_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1);
    enc_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_2);

    struct v4l2_requestbuffers rb = { .count = NOUT, .type = in.type, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(e->fd, VIDIOC_REQBUFS, &rb) || rb.count < NOUT)
        goto fail;
    for (int i = 0; i < NOUT; i++) {
        struct v4l2_plane pl = { 0 };
        struct v4l2_buffer b = { .index = i, .type = in.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &pl, .length = 1 };
        if (xioctl(e->fd, VIDIOC_QUERYBUF, &b))
            goto fail;
        e->in_len[i] = pl.length;
        e->in_map[i] = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, e->fd, pl.m.mem_offset);
        if (e->in_map[i] == MAP_FAILED) {
            e->in_map[i] = NULL;
            goto fail;
        }
    }
    struct v4l2_requestbuffers ro = { .count = ENC_OUT_BUFS, .type = out.type, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(e->fd, VIDIOC_REQBUFS, &ro) || ro.count < 1)
        goto fail;
    for (int i = 0; i < (int)ro.count && i < ENC_OUT_BUFS; i++) {
        struct v4l2_plane pl = { 0 };
        struct v4l2_buffer b = { .index = i, .type = out.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &pl, .length = 1 };
        if (xioctl(e->fd, VIDIOC_QUERYBUF, &b))
            goto fail;
        e->out_len[i] = pl.length;
        e->out_map[i] = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, e->fd, pl.m.mem_offset);
        if (e->out_map[i] == MAP_FAILED) {
            e->out_map[i] = NULL;
            goto fail;
        }
        if (xioctl(e->fd, VIDIOC_QBUF, &b))
            goto fail;
    }
    int t = in.type;
    if (xioctl(e->fd, VIDIOC_STREAMON, &t))
        goto fail;
    t = out.type;
    if (xioctl(e->fd, VIDIOC_STREAMON, &t))
        goto fail;
    e->frame_100ns = (int64_t)(10000000.0 * m->num / m->den);
    LOG("input %d: NDI|HX: hardware H.264 encoder %s, %.1f Mbit/s", n, enc_dev, bps / 1e6);
    return true;
fail:
    LOG("input %d: hardware H.264 encoder: %s - sending Full NDI", n, strerror(errno));
    hwenc_close(e);
    return false;
}

/* NDIlib_compressed_packet_t (Processing.NDI.Advanced.h): 44 bytes before the frame data. */
#define PKT_HDR 44

/* One encoded frame -> NDI: H.264 Annex B, with SPS/PPS moved to the packet's extra data. */
static void hwenc_send(struct hwenc *e, NDIlib_send_instance_t send, const struct mode *m, const uint8_t *data,
                       size_t len, bool key, int64_t timecode)
{
    if (e->pkt_cap < PKT_HDR + len * 2) {
        free(e->pkt);
        e->pkt_cap = PKT_HDR + len * 2 + 65536;
        e->pkt = malloc(e->pkt_cap);
        if (!e->pkt) {
            e->pkt_cap = 0;
            return;
        }
    }
    /* Split into NAL units (start codes 00 00 01 / 00 00 00 01). */
    uint8_t *frame = e->pkt + PKT_HDR, *extra = e->pkt + PKT_HDR + len;
    size_t flen = 0, xlen = 0, i = 0;
    while (i + 3 < len) {
        size_t sc = i;
        while (sc + 3 < len && !(data[sc] == 0 && data[sc + 1] == 0 && data[sc + 2] == 1))
            sc++;
        if (sc + 3 >= len)
            break;
        size_t start = sc > 0 && data[sc - 1] == 0 ? sc - 1 : sc; /* include a 4-byte start code's zero */
        size_t nxt = sc + 3;
        while (nxt + 3 < len && !(data[nxt] == 0 && data[nxt + 1] == 0 && data[nxt + 2] == 1))
            nxt++;
        size_t end = nxt + 3 < len ? (data[nxt - 1] == 0 ? nxt - 1 : nxt) : len;
        int type = data[sc + 3] & 0x1f;
        if (type == 7 || type == 8) {
            memcpy(extra + xlen, data + start, end - start);
            xlen += end - start;
        } else {
            memcpy(frame + flen, data + start, end - start);
            flen += end - start;
        }
        i = end;
    }
    memmove(frame + flen, extra, xlen); /* extra data right after the frame data */
    int64_t pts = (int64_t)e->sent * e->frame_100ns;
    uint8_t *h = e->pkt;
    int32_t ver = PKT_HDR;
    uint32_t cc = NDI_LIB_FOURCC('H', '2', '6', '4'), flags = key ? 1 : 0, ds = (uint32_t)flen,
             es = key ? (uint32_t)xlen : 0;
    uint64_t zero = 0;
    memcpy(h, &ver, 4);
    memcpy(h + 4, &cc, 4);
    memcpy(h + 8, &pts, 8);
    memcpy(h + 16, &pts, 8); /* dts: no B-frames */
    memcpy(h + 24, &zero, 8);
    memcpy(h + 32, &flags, 4);
    memcpy(h + 36, &ds, 4);
    memcpy(h + 40, &es, 4);
    NDIlib_video_frame_v2_t f = {
        .xres = m->w, .yres = m->h,
        .FourCC = (NDIlib_FourCC_video_type_e)NDI_LIB_FOURCC('H', '2', '6', '4'),
        .frame_rate_N = (int)m->den, .frame_rate_D = (int)m->num,
        .frame_format_type = NDIlib_frame_format_type_progressive,
        .timecode = timecode,
        .p_data = e->pkt,
        .data_size_in_bytes = (int)(PKT_HDR + flen + es),
    };
    ndi->send_send_video_v2(send, &f);
    e->sent++;
    e->keys += key;
}

struct pipeline;
struct worker {
    struct pipeline *pl;
    pthread_t thr;
    bool busy, ndi, local;
    uint64_t seq;
    uint8_t *src;
    size_t len, cap;
    int64_t time;
    bool dark;  /* the decoded picture is black */
    tjhandle tj;
    double warn_rate; /* share of recent frames the JPEG decoder warned about */
    uint8_t *planes;  /* planar picture when there is no shared memory */
};

struct pipeline {
    struct input *in;
    NDIlib_send_instance_t send;
    struct mode m;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool quit;
    uint64_t next; /* next frame number to publish */
    int nw;
    struct worker w[MAX_WORKERS];
    uint8_t *out[NOUT];
    int ostate[NOUT];
    int pending; /* out[] index waiting for the encoder, -1 = none */
    int64_t out_time[NOUT]; /* capture time of the picture in out[] (100 ns UTC: NDI timecode) */
    int64_t black_since;    /* capture time the picture went black (mu), 0 = it isn't */
    bool flush;  /* ndi_wait_idle: make NDI let go of the last frame */
    NDIlib_video_frame_v2_t pframe;
    pthread_t ndi_thr;
    int frames, damaged, ndi_sent; /* published / skipped / sent over NDI since the last status (mu) */
    bool hx;          /* NDI|HX through enc; out[] are its input buffers */
    struct hwenc enc;
    int out_stride;   /* bytes per UYVY line in out[] */
};

/* NDI|HX: queue frame i (if any) to the encoder, then send what it has finished. Up to NOUT
 * frames are in the encoder at once; with nothing new to queue, wait up to wait_ms for output. */
static void hx_step(struct pipeline *pl, int i, int wait_ms)
{
    struct hwenc *e = &pl->enc;
    if (i >= 0) {
        /* A new receiver can't start before a keyframe: send one now (NDI|HX: within 100 ms). The
         * SDK's own request is checked too, but it never fired with these compressed frames. */
        int conns = ndi->send_get_no_connections(pl->send, 0);
        if (conns > e->conns || (ndi_keyframe_required && ndi_keyframe_required(pl->send, &pl->pframe))) {
            enc_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
            e->forced++;
        }
        e->conns = conns;
        struct v4l2_plane p = { .bytesused = (uint32_t)e->in_len[i], .length = (uint32_t)e->in_len[i] };
        struct v4l2_buffer b = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                                 .m.planes = &p, .length = 1 };
        pthread_mutex_lock(&pl->mu);
        int64_t t = pl->out_time[i];
        pthread_mutex_unlock(&pl->mu);
        b.timestamp.tv_sec = t / 10000000;
        b.timestamp.tv_usec = t % 10000000 / 10;
        if (xioctl(e->fd, VIDIOC_QBUF, &b)) {
            pthread_mutex_lock(&pl->mu);
            pl->ostate[i] = O_FREE;
            pthread_cond_broadcast(&pl->cv);
            pthread_mutex_unlock(&pl->mu);
        }
    }
    uint64_t until = now_ms() + (uint64_t)wait_ms;
    bool got = false;
    for (;;) {
        for (;;) { /* pictures the encoder is done with */
            struct v4l2_plane p = { 0 };
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                                     .m.planes = &p, .length = 1 };
            if (xioctl(e->fd, VIDIOC_DQBUF, &b))
                break;
            pthread_mutex_lock(&pl->mu);
            pl->ostate[b.index] = O_FREE;
            pthread_cond_broadcast(&pl->cv);
            pthread_mutex_unlock(&pl->mu);
        }
        for (;;) {
            struct v4l2_plane p = { 0 };
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
                                     .m.planes = &p, .length = 1 };
            if (xioctl(e->fd, VIDIOC_DQBUF, &b))
                break;
            if (p.bytesused && b.index < ENC_OUT_BUFS) {
                int64_t tc = ((int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec) * 10;
                hwenc_send(e, pl->send, &pl->m, e->out_map[b.index] + p.data_offset, p.bytesused - p.data_offset,
                           b.flags & V4L2_BUF_FLAG_KEYFRAME, tc ? tc : NDIlib_send_timecode_synthesize);
                pthread_mutex_lock(&pl->mu);
                pl->ndi_sent++;
                pthread_mutex_unlock(&pl->mu);
                got = true;
            }
            xioctl(e->fd, VIDIOC_QBUF, &b);
        }
        int left = (int)(until - now_ms());
        if (got || left <= 0)
            return;
        struct pollfd pf = { .fd = e->fd, .events = POLLIN };
        poll(&pf, 1, left);
    }
}

static void *ndi_thread(void *arg)
{
    struct pipeline *pl = arg;
    /* Below the decoders: this device's own display comes first. The SDK's encoder threads are
     * started by the first send and inherit this. */
    setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    int prev = -1; /* NDI keeps using an async frame until the next send (or a NULL flush) returns */
    pthread_mutex_lock(&pl->mu);
    for (;;) {
        if (pl->hx) {
            bool queued = false;
            for (int k = 0; k < NOUT; k++)
                queued |= pl->ostate[k] == O_SENDING;
            while (!pl->quit && pl->pending < 0 && !queued)
                pthread_cond_wait(&pl->cv, &pl->mu);
            if (pl->quit)
                break;
            int i = pl->pending;
            pl->pending = -1;
            if (i >= 0)
                pl->ostate[i] = O_SENDING;
            pthread_mutex_unlock(&pl->mu);
            hx_step(pl, i, i >= 0 ? 0 : 3);
            pthread_mutex_lock(&pl->mu);
            pthread_cond_broadcast(&pl->cv);
            continue;
        }
        while (!pl->quit && pl->pending < 0 && !(pl->flush && prev >= 0))
            pthread_cond_wait(&pl->cv, &pl->mu);
        if (pl->quit)
            break;
        int i = pl->pending;
        NDIlib_video_frame_v2_t f = pl->pframe;
        pl->pending = -1;
        if (i >= 0)
            pl->ostate[i] = O_SENDING;
        pthread_mutex_unlock(&pl->mu);
        uint64_t t = now_ms();
        ndi->send_send_video_async_v2(pl->send, i >= 0 ? &f : NULL); /* waits for the previous frame */
        if (now_ms() - t > 200)
            LOG("NDI took %llu ms to take a video frame", (unsigned long long)(now_ms() - t));
        pthread_mutex_lock(&pl->mu);
        if (prev >= 0)
            pl->ostate[prev] = O_FREE;
        prev = i;
        if (i >= 0)
            pl->ndi_sent++;
        pthread_cond_broadcast(&pl->cv);
    }
    pthread_mutex_unlock(&pl->mu);
    return NULL;
}

/* Wait until NDI has let go of every frame (before sending to it directly, or stopping). */
static void ndi_wait_idle(struct pipeline *pl)
{
    pthread_mutex_lock(&pl->mu);
    pl->flush = true;
    pthread_cond_broadcast(&pl->cv);
    for (;;) {
        bool busy = pl->pending >= 0;
        for (int i = 0; i < NOUT; i++)
            busy |= pl->ostate[i] == O_SENDING;
        if (!busy)
            break;
        pthread_cond_wait(&pl->cv, &pl->mu);
    }
    pl->flush = false;
    pthread_mutex_unlock(&pl->mu);
}

static void worker_frame(struct worker *wk)
{
    struct pipeline *pl = wk->pl;
    struct input *in = pl->in;
    const struct mode *m = &pl->m;
    struct local_feed *lf = in->lf;
    uint64_t s = wk->seq;
    int slot = (int)(s % LOCAL_SLOTS);
    size_t ysz = (size_t)m->w * m->h;
    bool local = wk->local && lf && in->lf_frames;
    uint8_t *base = local ? in->lf_frames + (size_t)slot * lf->slot_size : wk->planes;
    wk->dark = false;
    if (local)
        STORE(lf->slot_seq[slot], 0);
    int ob = -1; /* NDI buffer for this frame: only if one is free (else the encoder is busy: skip NDI) */
    if (wk->ndi) {
        pthread_mutex_lock(&pl->mu);
        for (int i = 0; i < NOUT && ob < 0; i++)
            if (pl->ostate[i] == O_FREE) {
                ob = i;
                pl->ostate[i] = O_FILLING;
            }
        pthread_mutex_unlock(&pl->mu);
    }
    bool to_ndi = ob >= 0;
    uint8_t *out = to_ndi ? pl->out[ob] : NULL;
    NDIlib_video_frame_v2_t f = {
        .xres = m->w, .yres = m->h,
        .frame_rate_N = (int)m->den, .frame_rate_D = (int)m->num,
        .picture_aspect_ratio = 0,
        .frame_format_type = NDIlib_frame_format_type_progressive,
        /* When the picture was captured: receivers pace frames by it (PINDI_OUT<n>_PACING). */
        .timecode = wk->time ? wk->time : NDIlib_send_timecode_synthesize,
        .p_data = out,
    };
    bool ok = true, planar_ok = true;
    uint32_t fmt = LOCAL_422P;
    bool skip = !to_ndi && !local;
    if (skip) {
        /* Nobody takes this frame: the encoder is still busy (the Pi 3/4 H.264 encoder manages about
         * 1080p30, so half of a 60 fps card's frames) and there is no local display. Don't decode it. */
    } else if (m->fourcc == V4L2_PIX_FMT_MJPEG) {
        int jw, jh, sub, cs, rc = -1;
        if (!wk->len || tjDecompressHeader3(wk->tj, wk->src, wk->len, &jw, &jh, &sub, &cs) || jw != m->w ||
            jh != m->h) {
            ok = false;
        } else if (sub == TJSAMP_420 || sub == TJSAMP_422) {
            int ch = sub == TJSAMP_420 ? m->h / 2 : m->h;
            unsigned char *dst[3] = { base, base + ysz, base + ysz + (size_t)m->w / 2 * ch };
            int strides[3] = { m->w, m->w / 2, m->w / 2 };
            rc = tjDecompressToYUVPlanes(wk->tj, wk->src, wk->len, dst, m->w, strides, m->h, TJFLAG_FASTDCT);
            fmt = sub == TJSAMP_420 ? LOCAL_420P : LOCAL_422P;
            if (!in->video_range) {
                full_to_video(dst[0], ysz, false);
                full_to_video(dst[1], (size_t)m->w / 2 * ch * 2, true); /* U and V are adjacent */
            }
        } else if (to_ndi && !pl->hx) {
            /* Other subsampling: RGB for NDI only (not for the H.264 encoder). */
            planar_ok = false;
            rc = tjDecompress2(wk->tj, wk->src, wk->len, out, m->w, m->w * 4, m->h, TJPF_BGRX, TJFLAG_FASTDCT);
            f.FourCC = NDIlib_FourCC_video_type_BGRX;
            f.line_stride_in_bytes = m->w * 4;
        } else {
            planar_ok = false;
            rc = 0; /* nothing to do with this one */
        }
        if (ok) {
            /* Warnings usually mean a truncated frame (grey/garbage band = visible flicker), so those
             * are dropped - unless the device warns on nearly every frame: then it's a harmless quirk. */
            bool warn = rc && tjGetErrorCode(wk->tj) == TJERR_WARNING;
            ok = !rc || (warn && wk->warn_rate > 0.8);
            wk->warn_rate = wk->warn_rate * 0.95 + (warn ? 0.05 : 0);
        }
        if (ok && planar_ok) {
            /* How dark the picture is (a sparse sample of the luma): a black picture together with silence
             * is a break in the programme (e.g. before an ad), not an audio fault - logged at publishing. */
            unsigned sum = 0, mx = 0, n = 0;
            for (int row = m->h / 32; row < m->h; row += m->h / 16)
                for (int x = 16; x < m->w; x += 64, n++) {
                    unsigned l = base[(size_t)row * m->w + x];
                    sum += l;
                    mx = l > mx ? l : mx;
                }
            wk->dark = n && sum / n < 24 && mx < 48;
        }
        if (ok && planar_ok && to_ndi) {
            if (fmt == LOCAL_420P && !pl->hx) { /* NDI's I420 is this very layout */
                memcpy(out, base, ysz * 3 / 2);
                f.FourCC = NDIlib_FourCC_video_type_I420;
                f.line_stride_in_bytes = m->w;
            } else { /* UYVY (for SpeedHQ, or the H.264 encoder's input) */
                bool c420 = fmt == LOCAL_420P;
                const uint8_t *y = base, *u = base + ysz, *v = base + ysz + ysz / (c420 ? 4 : 2);
                for (int row = 0; row < m->h; row++) {
                    size_t cr = (size_t)(c420 ? row / 2 : row) * m->w / 2;
                    yuv422p_to_uyvy(y + (size_t)row * m->w, u + cr, v + cr, out + (size_t)row * pl->out_stride, m->w);
                }
                f.FourCC = NDIlib_FourCC_video_type_UYVY;
                f.line_stride_in_bytes = pl->out_stride;
            }
        }
    } else if (wk->len < (size_t)m->w * m->h * 2) {
        ok = false; /* a short (damaged) raw frame: its rows would be read past the end of the data */
    } else {
        bool uyvy = m->fourcc == V4L2_PIX_FMT_UYVY;
        if (local)
            for (int row = 0; row < m->h; row++)
                packed_to_422p(wk->src + (size_t)row * m->w * 2, base + (size_t)row * m->w,
                               base + ysz + (size_t)row * m->w / 2, base + ysz * 3 / 2 + (size_t)row * m->w / 2,
                               m->w, uyvy);
        if (to_ndi)
            for (int row = 0; row < m->h; row++) {
                const uint8_t *src = wk->src + (size_t)row * m->w * 2;
                uint8_t *dst = out + (size_t)row * pl->out_stride;
                if (uyvy)
                    memcpy(dst, src, (size_t)m->w * 2);
                else
                    yuyv_to_uyvy(src, dst, (size_t)m->w * 2);
            }
        f.FourCC = NDIlib_FourCC_video_type_UYVY;
        f.line_stride_in_bytes = pl->out_stride;
    }

    /* Publish in order. */
    pthread_mutex_lock(&pl->mu);
    while (pl->next != s && !pl->quit)
        pthread_cond_wait(&pl->cv, &pl->mu);
    if (ok && planar_ok) {
        if (wk->dark && !pl->black_since) {
            pl->black_since = wk->time;
        } else if (!wk->dark && pl->black_since) {
            double secs = (wk->time - pl->black_since) / 1e7;
            if (secs >= 0.5)
                LOG("input %d: video: picture black for %.2f s", in->n, secs);
            pl->black_since = 0;
        }
    }
    if (ok) {
        if (local && planar_ok) {
            lf->slot_time[slot] = wk->time;
            lf->fmt = fmt;
            STORE(lf->slot_seq[slot], s);
            STORE(lf->seq, s);
        }
        if (to_ndi) { /* newest for the encoder; an older one still waiting is dropped */
            if (pl->pending >= 0)
                pl->ostate[pl->pending] = O_FREE;
            pl->pending = ob;
            pl->ostate[ob] = O_PENDING;
            pl->out_time[ob] = wk->time;
            pl->pframe = f;
            ob = -1;
        }
        pl->frames += !skip;
    } else {
        pl->damaged++;
    }
    if (ob >= 0)
        pl->ostate[ob] = O_FREE;
    pl->next = s + 1;
    wk->busy = false;
    pthread_cond_broadcast(&pl->cv);
    pthread_mutex_unlock(&pl->mu);
}

static void *worker_thread(void *arg)
{
    struct worker *wk = arg;
    struct pipeline *pl = wk->pl;
    for (;;) {
        pthread_mutex_lock(&pl->mu);
        while (!pl->quit && !wk->busy)
            pthread_cond_wait(&pl->cv, &pl->mu);
        bool quit = pl->quit;
        pthread_mutex_unlock(&pl->mu);
        if (quit)
            break;
        worker_frame(wk);
    }
    return NULL;
}

/* Wait until every frame handed out so far is published (NDI may then be flushed or sent to directly). */
static void pipeline_drain(struct pipeline *pl, uint64_t next_seq)
{
    pthread_mutex_lock(&pl->mu);
    while (pl->next < next_seq)
        pthread_cond_wait(&pl->cv, &pl->mu);
    pthread_mutex_unlock(&pl->mu);
}

/* No picture: black on this device's display too (planar 4:2:2), published as frame seq. */
static void local_black(struct input *in, struct pipeline *pl, uint64_t seq)
{
    struct local_feed *lf = in->lf;
    int slot = (int)(seq % LOCAL_SLOTS);
    size_t ysz = (size_t)in->w * in->h;
    uint8_t *base = in->lf_frames + (size_t)slot * lf->slot_size;
    STORE(lf->slot_seq[slot], 0);
    memset(base, 16, ysz);
    memset(base + ysz, 128, ysz);
    lf->fmt = LOCAL_422P;
    lf->slot_time[slot] = capture_time(&(struct timeval){ 0 });
    STORE(lf->slot_seq[slot], seq);
    STORE(lf->seq, seq);
    pthread_mutex_lock(&pl->mu);
    pl->next = seq + 1;
    pthread_mutex_unlock(&pl->mu);
}

static void run_device(struct input *in, NDIlib_send_instance_t send)
{
    int fd = open(in->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        status(in, "waiting for device", NULL, 0, 0, 0);
        return;
    }
    struct capbuf cap[NCAP] = { 0 };
    int ncap = 0, started = 0;
    struct pipeline pl = { .in = in, .send = send, .next = 1, .pending = -1, .enc = { .fd = -1 } };
    bool ndi_started = false;
    pthread_mutex_init(&pl.mu, NULL);
    pthread_cond_init(&pl.cv, NULL);
    struct mode m;
    char mode_desc[64] = "";
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool streaming = false;
    uint64_t seq = 1; /* next frame number to hand out */

    if (!choose_mode(fd, in->mode_s, &m)) {
        status(in, "no usable video format", NULL, 0, 0, 0);
        goto out;
    }
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = m.w;
    fmt.fmt.pix.height = m.h;
    fmt.fmt.pix.pixelformat = m.fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt)) {
        status(in, "cannot set format", NULL, 0, 0, 0);
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
    if (in->w != m.w || in->h != m.h) {
        free(in->black); /* the black frame is made at the new size when needed */
        in->black = NULL;
    }
    in->w = m.w;
    in->h = m.h;
    in->fps_n = (int)m.den;
    in->fps_d = (int)m.num;
    if (!local_setup(in, m.w, m.h, LOCAL_422P, (int)m.den, (int)m.num))
        LOG("input %d: no shared memory for this device's own display: %s", in->n, strerror(errno));

    struct v4l2_requestbuffers req = { .count = NCAP, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(fd, VIDIOC_REQBUFS, &req) || req.count < 2) {
        status(in, "cannot allocate capture buffers", mode_desc, 0, 0, 0);
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
    if (ncap < 2 || xioctl(fd, VIDIOC_STREAMON, &type)) {
        status(in, "cannot start capture", mode_desc, 0, 0, 0);
        goto out;
    }
    streaming = true;

    pl.m = m;
    if (in->hx && !*enc_dev)
        LOG("input %d: NDI|HX needs the Pi's hardware H.264 encoder (none here) - sending Full NDI", in->n);
    else if (in->hx && !ndi_target_bit_rate)
        LOG("input %d: NDI|HX needs the NDI Advanced SDK - sending Full NDI", in->n);
    else if (in->hx)
        pl.hx = hwenc_open(&pl.enc, &m, send, in->n);
    pl.out_stride = pl.hx ? pl.enc.bpl : m.w * 2;
    pl.nw = m.fourcc == V4L2_PIX_FMT_MJPEG && m.w * m.h >= 1280 * 720 ? (pl.hx ? 3 : 2) : 1;
    for (int i = 0; i < NOUT; i++)
        if (!(pl.out[i] = pl.hx ? pl.enc.in_map[i] : malloc((size_t)m.w * m.h * 4)))
            goto out;
    if (pthread_create(&pl.ndi_thr, NULL, ndi_thread, &pl))
        goto out;
    ndi_started = true;
    for (int i = 0; i < pl.nw; i++) {
        struct worker *wk = &pl.w[i];
        wk->pl = &pl;
        wk->planes = malloc((size_t)m.w * m.h * 2);
        if (m.fourcc == V4L2_PIX_FMT_MJPEG)
            wk->tj = tjInitDecompress();
        if (!wk->planes || (m.fourcc == V4L2_PIX_FMT_MJPEG && !wk->tj) ||
            pthread_create(&wk->thr, NULL, worker_thread, wk))
            goto out;
        started++;
    }
    LOG("input %d: %s from %s as NDI%s \"%s\" (%d decoding thread%s)", in->n, mode_desc, in->device,
        pl.hx ? "|HX" : "", in->name, pl.nw, pl.nw == 1 ? "" : "s");
    status(in, "starting", mode_desc, 0, 0, 0);

    int timeouts = 0, cam = 0, log_cam = 0, log_sent = 0, log_ndi = 0, log_damaged = 0, conns = 0;
    bool idle = false, local = false;
    uint64_t t0 = now_ms(), t_log = t0, conns_checked = 0;
    while (running) {
        if (in->lf)
            STORE(in->lf->writer_ms, now_ms());
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, 1000);
        if (r == 0) {
            status(in, "no signal", mode_desc, 0, 0, 0);
            pipeline_drain(&pl, seq);
            ndi_wait_idle(&pl); /* send_black sends directly */
            send_black(in, send);
            if (local_wanted(in->lf) && in->lf_frames)
                local_black(in, &pl, seq++);
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
        cam++;
        /* The SDK encodes (full and preview stream) even with nobody connected: with no receivers,
         * skip decoding and sending altogether. That's most of the CPU (and heat) on a Pi 3. */
        uint64_t tnow = now_ms();
        if (tnow - conns_checked >= 250) {
            conns = ndi->send_get_no_connections(send, 0);
            conns_checked = tnow;
        }
        local = local_wanted(in->lf);
        if (!conns && !local) {
            xioctl(fd, VIDIOC_QBUF, &b);
            idle = true;
        } else {
            if (idle)
                LOG("input %d: %s, sending", in->n, conns ? "receiver connected" : "shown on this device");
            idle = false;
            struct worker *wk = &pl.w[seq % pl.nw];
            pthread_mutex_lock(&pl.mu);
            while (wk->busy)
                pthread_cond_wait(&pl.cv, &pl.mu);
            pthread_mutex_unlock(&pl.mu);
            if (wk->cap < b.bytesused) {
                free(wk->src);
                wk->cap = b.bytesused;
                wk->src = malloc(wk->cap);
                if (!wk->src)
                    wk->cap = 0;
            }
            wk->len = wk->src ? b.bytesused : 0;
            if (wk->len)
                memcpy(wk->src, cap[b.index].start, wk->len);
            xioctl(fd, VIDIOC_QBUF, &b);
            wk->time = capture_time(&b.timestamp);
            wk->ndi = conns > 0;
            wk->local = local;
            pthread_mutex_lock(&pl.mu);
            wk->seq = seq++;
            wk->busy = true;
            pthread_cond_broadcast(&pl.cv);
            pthread_mutex_unlock(&pl.mu);
        }

        uint64_t now = now_ms();
        if (now - t0 >= 2000) {
            char st[160];
            NDIlib_tally_t tally = { 0 };
            pthread_mutex_lock(&pl.mu);
            int frames = pl.frames, damaged = pl.damaged, sent = pl.ndi_sent;
            pl.frames = pl.damaged = pl.ndi_sent = 0;
            pthread_mutex_unlock(&pl.mu);
            double secs = (now - t0) / 1000.0;
            if (conns)
                ndi->send_get_tally(send, &tally, 0); /* a switcher (vMix, TriCaster...) has it on program/preview */
            if (in->hx && ndi_eval_over)
                snprintf(st, sizeof st, "NDI|HX video stopped: 30-minute limit of the NDI Advanced SDK evaluation "
                                        "(sound goes on)");
            else if (conns)
                snprintf(st, sizeof st, "sending (%d receiver%s, %.0f fps over NDI)%s%s", conns, conns == 1 ? "" : "s",
                         sent / secs, local ? ", also shown on this device" : "",
                         tally.on_program ? " - ON AIR" : tally.on_preview ? " - on preview" : "");
            else if (local)
                snprintf(st, sizeof st, "shown on this device (no NDI receivers)");
            else
                snprintf(st, sizeof st, "ready (no receivers)");
            status(in, st, mode_desc, frames / secs, cam / secs, damaged);
            log_cam += cam;
            log_sent += frames;
            log_ndi += sent;
            log_damaged += damaged;
            cam = 0;
            t0 = now;
        }
        if (now - t_log >= 60000) {
            double secs = (now - t_log) / 1000.0;
            LOG("input %d: last %.0f s: camera %.1f fps, decoded %.1f fps, NDI %.1f fps, %d damaged frame%s skipped",
                in->n, secs, log_cam / secs, log_sent / secs, log_ndi / secs, log_damaged,
                log_damaged == 1 ? "" : "s");
            if (pl.hx) {
                LOG("input %d: NDI|HX: %d keyframes, %d forced for new receivers", in->n, pl.enc.keys, pl.enc.forced);
                pl.enc.keys = pl.enc.forced = 0;
            }
            log_cam = log_sent = log_ndi = log_damaged = 0;
            t_log = now;
        }
    }
out:
    if (started)
        pipeline_drain(&pl, seq);
    if (ndi_started)
        ndi_wait_idle(&pl); /* NDI is done with our buffers after this */
    pthread_mutex_lock(&pl.mu);
    pl.quit = true;
    pthread_cond_broadcast(&pl.cv);
    pthread_mutex_unlock(&pl.mu);
    for (int i = 0; i < started; i++)
        pthread_join(pl.w[i].thr, NULL);
    if (ndi_started)
        pthread_join(pl.ndi_thr, NULL);
    if (pl.hx)
        memset(pl.out, 0, sizeof pl.out); /* the encoder's buffers */
    hwenc_close(&pl.enc);
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (pl.w[i].tj)
            tjDestroy(pl.w[i].tj);
        free(pl.w[i].src);
        free(pl.w[i].planes);
    }
    for (int i = 0; i < NOUT; i++)
        free(pl.out[i]);
    if (streaming)
        xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < ncap; i++)
        if (cap[i].start)
            munmap(cap[i].start, cap[i].len);
    close(fd);
    if (running)
        status(in, "device lost, retrying", mode_desc, 0, 0, 0);
}

/* ------------------------------------------------------------------ audio thread */

/* The ALSA card on the same USB device as the video device, -1 if it has none, -2 if the device is gone. */
static int audio_card_for(const char *video_dev)
{
    char real[PATH_MAX], sys[PATH_MAX + 64], usb[PATH_MAX];
    if (!realpath(video_dev, real) || strncmp(real, "/dev/", 5))
        return -2;
    snprintf(sys, sizeof sys, "/sys/class/video4linux/%s/device", real + 5);
    if (!realpath(sys, usb))
        return -1;
    char *slash = strrchr(usb, '/'); /* .../1-1.4/1-1.4:1.0 -> .../1-1.4 (the USB device) */
    if (!slash)
        return -1;
    *slash = 0;
    for (int card = 0; card < 32; card++) {
        char p[64], dev[PATH_MAX];
        snprintf(p, sizeof p, "/sys/class/sound/card%d/device", card);
        if (!realpath(p, dev))
            continue;
        size_t n = strlen(usb);
        if (!strncmp(dev, usb, n) && dev[n] == '/')
            return card;
    }
    return -1;
}

#define AUDIO_RATE 48000
#define AUDIO_PERIOD 960 /* 20 ms */

/* 48 kHz stereo, 20 ms periods, 500 ms buffer. Set explicitly: with snd_pcm_set_params()'s choice
 * (25 ms periods) the MS2109 capture cards fail every read with an I/O error. The long buffer rides
 * out the thread being held off by video decoding instead of overrunning (lost audio). */
static int audio_params(snd_pcm_t *pcm)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned rate = AUDIO_RATE;
    snd_pcm_uframes_t period = AUDIO_PERIOD, buffer = AUDIO_PERIOD * 25;
    int err = snd_pcm_hw_params_any(pcm, hw);
    if (!err)
        err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (!err)
        err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (!err)
        err = snd_pcm_hw_params_set_channels(pcm, hw, 2);
    if (!err)
        err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    if (!err)
        err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    if (!err)
        err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if (!err)
        err = snd_pcm_hw_params(pcm, hw);
    return err;
}

/* One line per second of audio: wall-clock time, peak and RMS in dBFS. Kept to about 10000 lines. */
static void levels_append(struct input *in, int peak, double rms)
{
    char path[64];
    snprintf(path, sizeof path, RUN_DIR "/in%d.levels", in->n);
    struct stat sb;
    if (!stat(path, &sb) && sb.st_size > 512 * 1024) {
        char old[72];
        snprintf(old, sizeof old, "%s.old", path);
        rename(path, old);
    }
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d.%03ld peak %6.1f rms %6.1f\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000, peak ? 20 * log10(peak / 32768.0) : -99.0,
            rms > 0 ? 20 * log10(rms / 32768.0) : -99.0);
    fclose(f);
}

/* Hand n frames to this device's own display and to NDI. */
static void audio_out(struct input *in, const int16_t *s, int n)
{
    struct local_feed *lf = in->lf;
    if (local_wanted(lf)) { /* this device's own display */
        uint64_t wr = lf->audio_wr;
        for (int i = 0; i < n; i++) {
            size_t at = (size_t)((wr + (uint64_t)i) % LOCAL_AUDIO) * 2;
            lf->audio[at] = s[i * 2];
            lf->audio[at + 1] = s[i * 2 + 1];
        }
        STORE(lf->audio_wr, wr + (uint64_t)n);
    }
    /* Nobody listening: drop, as for the video. */
    if (!ndi->send_get_no_connections(in->send, 0))
        return;
    NDIlib_audio_frame_interleaved_16s_t a = {
        .sample_rate = AUDIO_RATE, .no_channels = 2, .no_samples = n,
        .timecode = NDIlib_send_timecode_synthesize, .reference_level = 0, .p_data = (int16_t *)s,
    };
    uint64_t t = now_us();
    ndi->util_send_send_audio_interleaved_16s(in->send, &a);
    t = now_us() - t;
    if (t > 50000)
        LOG("input %d: audio: NDI took %llu ms to take 20 ms of audio", in->n, (unsigned long long)t / 1000);
}

/* Stereo frames s[0..r) resampled to d[0..n) (linear; n is r +- 1 for drift). */
static void audio_stretch(const int16_t *s, int r, int16_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        double pos = (double)i * (r - 1) / (n - 1);
        int j = (int)pos, k = j + 1 < r ? j + 1 : j;
        double f = pos - j;
        for (int c = 0; c < 2; c++)
            d[i * 2 + c] = (int16_t)lrint(s[j * 2 + c] * (1 - f) + s[k * 2 + c] * f);
    }
}

static void *audio_thread(void *arg)
{
    struct input *in = arg;
    int16_t *buf = malloc(AUDIO_PERIOD * 2 * sizeof(int16_t));
    int16_t stretched[(AUDIO_PERIOD + 1) * 2];
    static const int16_t silence[AUDIO_PERIOD * 2];
    /* Ahead of the video decoding: a busy CPU then delays a picture rather than losing audio. */
    struct sched_param sp = { .sched_priority = 10 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
        LOG("input %d: audio runs without real-time priority", in->n);
    bool logged_none = false;
    while (running && buf) {
        int card = audio_card_for(in->device);
        if (card == -2) {
            set_audio_status(in, "waiting for the device");
            for (int i = 0; i < 20 && running; i++)
                usleep(100000);
            continue;
        }
        if (card < 0) {
            if (!logged_none)
                LOG("input %d: no USB audio on this device, sending video only", in->n);
            logged_none = true;
            set_audio_status(in, "none (the device has no audio input)");
            for (int i = 0; i < 50 && running; i++)
                usleep(100000);
            continue;
        }
        logged_none = false;
        char dev[32], *name = NULL;
        snprintf(dev, sizeof dev, "plughw:%d,0", card);
        snd_card_get_name(card, &name);
        snd_pcm_t *pcm = NULL;
        int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
        if (!err)
            err = audio_params(pcm);
        if (err) {
            LOG("input %d: cannot capture audio from %s: %s", in->n, dev, snd_strerror(err));
            char st[128];
            snprintf(st, sizeof st, "cannot capture: %s", snd_strerror(err));
            set_audio_status(in, st);
            if (pcm)
                snd_pcm_close(pcm);
            free(name);
            for (int i = 0; i < 50 && running; i++)
                usleep(100000);
            continue;
        }
        LOG("input %d: audio from %s (%s), 48 kHz stereo", in->n, name ? name : "?", dev);
        char src[64];
        snprintf(src, sizeof src, "%s", name ? name : dev);
        char st[256];
        snprintf(st, sizeof st, "48 kHz stereo from %s", src);
        set_audio_status(in, st);
        free(name);

        /* NDI's synthesized timecodes count samples, but the card's audio clock is not the Pi's: a
         * card running a little fast or slow, or audio lost to an overrun, leaves the audio drifting
         * from real time (and the video) until receivers drop it for a few seconds to resync. So the
         * samples sent are kept in step with the Pi's clock: a read stretched by one sample either
         * way for drift, silence for audio that was lost. */
        int warm = 0, xruns = 0, log_xruns = 0;
        uint64_t t_start = 0, t_stat = now_ms(), t_log = t_stat, sent = 0;
        int64_t adjusted = 0, log_lost = 0; /* samples added (+) or removed (-) for drift; filled with silence */
        double base = 0, drift = 0;
        uint64_t quiet = 0; /* frames of silence from the card in a row (logged from 500 ms) */
        int lv_peak = 0, lv_n = 0;
        double lv_sum = 0;
        while (running) {
            snd_pcm_sframes_t r = snd_pcm_readi(pcm, buf, AUDIO_PERIOD);
            if (r == -EPIPE || r == -ESTRPIPE || r == -EINTR) {
                if (r == -EPIPE) {
                    xruns++;
                    log_xruns++;
                }
                if (snd_pcm_recover(pcm, (int)r, 1) < 0)
                    break;
                continue;
            }
            if (r < 0) {
                LOG("input %d: audio read: %s", in->n, snd_strerror((int)r));
                break; /* device gone: look for it again */
            }
            /* The card itself going silent (its HDMI audio dropping) looks like a network dropout at the
             * receivers, so say when it happens (under -60 dBFS for half a second), and keep each second's
             * level in in<n>.levels to look back at. */
            for (snd_pcm_sframes_t i = 0; i < r; i++) {
                int a = abs(buf[i * 2]), b = abs(buf[i * 2 + 1]), m = a > b ? a : b;
                if (m > lv_peak)
                    lv_peak = m;
                lv_sum += (double)buf[i * 2] * buf[i * 2] + (double)buf[i * 2 + 1] * buf[i * 2 + 1];
                if (++lv_n == AUDIO_RATE) {
                    levels_append(in, lv_peak, sqrt(lv_sum / (2.0 * lv_n)));
                    lv_peak = lv_n = 0;
                    lv_sum = 0;
                }
                if (m <= 32) {
                    if (++quiet == AUDIO_RATE / 2)
                        LOG("input %d: audio: the card sends silence", in->n);
                } else {
                    if (quiet >= AUDIO_RATE / 2)
                        LOG("input %d: audio: sound again after %.2f s of silence from the card", in->n,
                            quiet / (double)AUDIO_RATE);
                    quiet = 0;
                }
            }
            uint64_t now = now_us();
            snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
            if (avail < 0)
                avail = 0;
            /* Samples captured (this read and those still waiting) ahead of the Pi's clock. */
            double ahead = (double)(sent + (uint64_t)r + (uint64_t)avail) - base -
                           (double)(now - t_start) * AUDIO_RATE / 1e6;
            if (warm < 50) { /* the first second: let the capture settle, then start the clock */
                warm++;
                t_start = now;
                base = (double)(sent + (uint64_t)r + (uint64_t)avail);
                ahead = 0;
            }
            drift += (ahead - drift) * 0.01;
            const int16_t *out = buf;
            int n = (int)r;
            if (!in->audio_sync) {
                /* sent as the card delivers it */
            } else if (ahead < -AUDIO_RATE / 10) { /* over 100 ms lost (an overrun): silence in its place */
                int gap = (int)-ahead;
                log_lost += gap;
                while (gap > 0) {
                    int k = gap < AUDIO_PERIOD ? gap : AUDIO_PERIOD;
                    audio_out(in, silence, k);
                    sent += (uint64_t)k;
                    gap -= k;
                }
                drift = 0;
            } else if (ahead > AUDIO_RATE / 10) { /* card far faster than one sample per read can fix */
                base += ahead;
                drift = 0;
            } else if (n > 2 && (drift > AUDIO_RATE / 200 || drift < -AUDIO_RATE / 200)) { /* 5 ms */
                int m = drift > 0 ? n - 1 : n + 1;
                audio_stretch(buf, n, stretched, m);
                adjusted += m - n;
                out = stretched;
                n = m;
            }
            audio_out(in, out, n);
            sent += (uint64_t)n;

            uint64_t ms = now_ms();
            double secs = warm < 50 ? 0 : (now - t_start) / 1e6;
            double ppm = secs > 10 ? -(double)adjusted / (secs * AUDIO_RATE) * 1e6 : 0;
            if (ms - t_stat >= 2000) {
                if (in->audio_sync)
                    snprintf(st, sizeof st, "48 kHz stereo from %s (clock %+d ppm, %d overrun%s)", src,
                             (int)lrint(ppm), xruns, xruns == 1 ? "" : "s");
                else
                    snprintf(st, sizeof st, "48 kHz stereo from %s (device timing, %d overrun%s)", src, xruns,
                             xruns == 1 ? "" : "s");
                set_audio_status(in, st);
                t_stat = ms;
            }
            if (ms - t_log >= 60000) {
                if (in->audio_sync)
                    LOG("input %d: audio: card clock %+.0f ppm, %d overrun%s, %.2f s of lost audio filled", in->n,
                        ppm, log_xruns, log_xruns == 1 ? "" : "s", log_lost / (double)AUDIO_RATE);
                else
                    LOG("input %d: audio: device timing, %d overrun%s", in->n, log_xruns, log_xruns == 1 ? "" : "s");
                log_xruns = 0;
                log_lost = 0;
                t_log = ms;
            }
        }
        snd_pcm_close(pcm);
        if (running) {
            LOG("input %d: audio device lost", in->n);
            set_audio_status(in, "device lost, retrying");
            usleep(1000000);
        }
    }
    free(buf);
    return NULL;
}

static void *input_thread(void *arg)
{
    struct input *in = arg;
    NDIlib_send_instance_t send = in->send; /* made by main, in input order */
    if (!send) {
        LOG("input %d: cannot create NDI sender \"%s\"", in->n, in->name);
        status(in, "cannot create NDI sender", NULL, 0, 0, 0);
        return NULL;
    }
    bool athr = in->audio_on && !pthread_create(&in->athr, NULL, audio_thread, in);
    if (!in->audio_on)
        set_audio_status(in, "off");
    while (running) {
        run_device(in, send);
        for (int i = 0; i < 20 && running; i++) {
            if (i % 10 == 0)
                send_black(in, send);
            usleep(100000);
        }
    }
    free(in->black);
    if (athr)
        pthread_join(in->athr, NULL);
    ndi->send_destroy(send);
    return NULL;
}

#define NDI_EVAL_MSG "designed for development use and will run on a stream for 30 minutes"

static int output_fd = -1; /* the original stderr (the journal) */

static void *output_thread(void *arg)
{
    int fd = (int)(intptr_t)arg, out = output_fd;
    char line[4096];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, line + n, sizeof line - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        line[n] = 0;
        if (!ndi_eval_over && strstr(line, NDI_EVAL_MSG))
            ndi_eval_over = 1;
        char *nl = strrchr(line, '\n');
        size_t keep = nl ? (size_t)(line + n - (nl + 1)) : (n == sizeof line - 1 ? 0 : n);
        if (write(out, line, n - keep) < 0) { }
        memmove(line, line + n - keep, keep);
        n = keep;
    }
    return NULL;
}

/* Our stdout and stderr (the journal) through a pipe, to notice the NDI library's evaluation message. */
static void watch_output(void)
{
    int p[2];
    pthread_t t;
    int saved = dup(STDERR_FILENO);
    if (saved < 0 || pipe(p))
        return;
    fflush(stdout);
    output_fd = saved; /* output_thread writes the lines on to the original stderr */
    if (dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0 ||
        pthread_create(&t, NULL, output_thread, (void *)(intptr_t)p[0])) {
        dup2(saved, STDOUT_FILENO);
        dup2(saved, STDERR_FILENO);
        return;
    }
    pthread_detach(t);
    close(p[1]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    watch_output();
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
        in->audio_on = strcasecmp(icfg(n, "AUDIO", "auto"), "off") != 0;
        in->audio_sync = strcasecmp(icfg(n, "AUDIO_SYNC", "on"), "off") != 0;
        in->hx = !strcasecmp(icfg(n, "CODEC", "speedhq"), "hx");
        in->video_range = !strcasecmp(icfg(n, "RANGE", "full"), "video");
        pthread_mutex_init(&in->lock, NULL);
    }
    if (!nin) {
        LOG("no outgoing streams configured");
        return 0;
    }

    /* The NDI Advanced SDK's library is libndi_advanced.so.6 (its soname). */
    void *lib = dlopen(NDILIB_LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libndi_advanced.so.6", RTLD_NOW | RTLD_LOCAL);
    const NDIlib_v6 *(*load)(void) = lib ? (const NDIlib_v6 *(*)(void))dlsym(lib, "NDIlib_v6_load") : NULL;
    ndi = load ? load() : NULL;
    if (!ndi || !ndi->initialize()) {
        LOG("cannot load/initialise NDI");
        return 1;
    }
    ndi_target_bit_rate = (int (*)(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t *))dlsym(
        lib, "NDIlib_send_get_target_bit_rate");
    ndi_keyframe_required = (bool (*)(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t *))dlsym(
        lib, "NDIlib_send_is_keyframe_required");
    hwenc_find();
    /* The senders are made one after another in input order, not by the input threads as they start:
     * each takes the next free port, so input 1 gets the lower port every time (receivers that remember
     * a stream's address are less likely to find another input there after a restart). */
    for (int i = 0; i < nin; i++) {
        NDIlib_send_create_t sc = { .p_ndi_name = ins[i].name, .p_groups = NULL, .clock_video = false,
                                    .clock_audio = false };
        ins[i].send = ndi->send_create(&sc);
    }
    for (int i = 0; i < nin; i++)
        pthread_create(&ins[i].thr, NULL, input_thread, &ins[i]);
    for (int i = 0; i < nin; i++)
        pthread_join(ins[i].thr, NULL);
    ndi->destroy();
    return 0;
}

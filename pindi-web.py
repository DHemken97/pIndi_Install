#!/usr/bin/python3
"""PiNDI web administration.

Small standard-library HTTP server (runs as root, port 80):
  - login with the Pi's own user accounts (PAM, members of the sudo group)
  - per display output (detected by the player): enable/disable, NDI stream
    (default OUT_<serial>[_n]), source IP (default any), rotation, mirror, status
  - "Configure Outgoing Streams": detected USB capture devices, off until enabled
  - network: DHCP or static IP, with automatic rollback unless confirmed
  - change the login (SSH) password, reboot
"""
import ctypes
import ctypes.util
import grp
import html
import http.cookies
import http.server
import ipaddress
import json
import os
import pwd
import re
import secrets
import socket
import subprocess
import threading
import time
import urllib.parse

PORT = int(os.environ.get("PINDI_WEB_PORT", "80"))
ENV_FILE = "/etc/default/pindi"
ENV_DEFAULTS = "/usr/share/pindi/pindi.conf"
VERSION_FILE = "/usr/share/pindi/version"
NDI_CONFIG = "/etc/pindi/ndi-config.v1.json"  # NDI_CONFIG_DIR=/etc/pindi in the player/sender units
NETWORK_FILE = "/etc/systemd/network/20-pindi-wired.network"
RUN_DIR = "/run/pindi"
ADMIN_GROUP = "sudo"
SESSION_TTL = 8 * 3600
ROLLBACK_SECONDS = 120


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------- PAM (via ctypes, no extra packages)

class _PamMessage(ctypes.Structure):
    _fields_ = [("msg_style", ctypes.c_int), ("msg", ctypes.c_char_p)]


class _PamResponse(ctypes.Structure):
    _fields_ = [("resp", ctypes.c_void_p), ("resp_retcode", ctypes.c_int)]


_CONV_FUNC = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_int,
    ctypes.POINTER(ctypes.POINTER(_PamMessage)),
    ctypes.POINTER(ctypes.POINTER(_PamResponse)),
    ctypes.c_void_p)


class _PamConv(ctypes.Structure):
    _fields_ = [("conv", _CONV_FUNC), ("appdata_ptr", ctypes.c_void_p)]


_libpam = ctypes.CDLL(ctypes.util.find_library("pam"))
_libc = ctypes.CDLL(ctypes.util.find_library("c"))
_libc.calloc.restype = ctypes.c_void_p
_libc.calloc.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
_libc.strdup.restype = ctypes.c_void_p
_libc.strdup.argtypes = [ctypes.c_char_p]
_libpam.pam_start.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                              ctypes.POINTER(_PamConv), ctypes.POINTER(ctypes.c_void_p)]
_libpam.pam_authenticate.argtypes = [ctypes.c_void_p, ctypes.c_int]
_libpam.pam_acct_mgmt.argtypes = [ctypes.c_void_p, ctypes.c_int]
_libpam.pam_end.argtypes = [ctypes.c_void_p, ctypes.c_int]
_pam_lock = threading.Lock()
PAM_PROMPT_ECHO_OFF = 1


def check_password(user, password):
    """True if user/password is a valid login (same check as SSH)."""
    secret = password.encode()

    @_CONV_FUNC
    def conv(n, msgs, resp, _data):
        addr = _libc.calloc(n, ctypes.sizeof(_PamResponse))
        if not addr:
            return 5  # PAM_BUF_ERR
        replies = ctypes.cast(addr, ctypes.POINTER(_PamResponse))
        for i in range(n):
            if msgs[i].contents.msg_style == PAM_PROMPT_ECHO_OFF:
                replies[i].resp = _libc.strdup(secret)  # freed by PAM
        resp[0] = replies
        return 0

    handle = ctypes.c_void_p()
    conversation = _PamConv(conv, None)
    with _pam_lock:
        if _libpam.pam_start(b"pindi-web", user.encode(), ctypes.byref(conversation),
                             ctypes.byref(handle)) != 0:
            return False
        rc = _libpam.pam_authenticate(handle, 0)
        if rc == 0:
            rc = _libpam.pam_acct_mgmt(handle, 0)
        _libpam.pam_end(handle, rc)
    return rc == 0


def is_admin(user):
    try:
        entry = pwd.getpwnam(user)
        group = grp.getgrnam(ADMIN_GROUP)
    except KeyError:
        return False
    return user in group.gr_mem or entry.pw_gid == group.gr_gid


# ---------------------------------------------------------------- helpers

def run(*cmd, input=None):
    return subprocess.run(cmd, input=input, capture_output=True, text=True, timeout=30)


def write_atomic(path, text, mode=0o644):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def read_text(path, default=""):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def later(seconds, fn, *args):
    """Run fn after the HTTP response has gone out."""
    t = threading.Timer(seconds, fn, args)
    t.daemon = True
    t.start()
    return t


def serial_number():
    s = read_text("/sys/firmware/devicetree/base/serial-number").strip("\x00\r\n ")
    return s or "unknown"


SERIAL = serial_number()
MODEL = read_text("/proc/device-tree/model").strip("\x00\r\n ") or "unknown"


# ---------------------------------------------------------------- /etc/default/pindi

_ENV_LINE = re.compile(r"^([A-Z_][A-Z0-9_]*)=(.*)$")


def read_env():
    values = {}
    for line in read_text(ENV_FILE).splitlines():
        m = _ENV_LINE.match(line.strip())
        if not m:
            continue
        key, val = m.groups()
        if len(val) >= 2 and val[0] == val[-1] == '"':
            val = re.sub(r'\\(.)', r'\1', val[1:-1])
        values[key] = val
    return values


def write_env(updates):
    """Set (or remove, when empty) keys in the env file, keeping everything else."""
    lines = read_text(ENV_FILE).splitlines()
    keep = [l for l in lines if not any(l.strip().startswith(k + "=") for k in updates)]
    for key, val in updates.items():
        if val:
            keep.append('%s="%s"' % (key, val.replace("\\", "\\\\").replace('"', '\\"')))
    write_atomic(ENV_FILE, "\n".join(keep) + "\n")


# ---------------------------------------------------------------- network

def ether_interfaces():
    out = []
    for name in sorted(os.listdir("/sys/class/net")):
        base = "/sys/class/net/" + name
        if name != "lo" and read_text(base + "/type").strip() == "1" and not os.path.exists(base + "/wireless"):
            out.append(name)
    return out


def live_network():
    """What the interface is actually using right now."""
    info = {"iface": "", "address": "", "prefix": "", "gateway": "", "dns": []}
    ifaces = ether_interfaces()
    for line in run("ip", "-4", "-o", "addr", "show", "scope", "global").stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] in ifaces:
            info["iface"] = parts[1]
            info["address"], _, info["prefix"] = parts[3].partition("/")
            break
    m = re.search(r"default via (\S+)", run("ip", "-4", "route", "show", "default").stdout)
    if m:
        info["gateway"] = m.group(1)
    for line in read_text("/run/systemd/resolve/resolv.conf").splitlines():
        if line.startswith("nameserver "):
            info["dns"].append(line.split()[1])
    return info


def read_network_config():
    cfg = {"dhcp": True, "address": "", "prefix": "", "gateway": "", "dns": [], "ntp": []}
    for line in read_text(NETWORK_FILE).splitlines():
        key, _, val = line.strip().partition("=")
        if key == "Address":
            cfg["dhcp"] = False
            cfg["address"], _, cfg["prefix"] = val.partition("/")
        elif key == "Gateway":
            cfg["gateway"] = val
        elif key == "DNS":
            cfg["dns"] += val.split()
        elif key == "NTP":
            cfg["ntp"] += val.split()
    return cfg


def render_network_config(dhcp, iface=None, gateway="", dns=(), ntp=()):
    text = "# Written by the PiNDI web interface.\n[Match]\nType=ether\n\n[Network]\n"
    for server in ntp:
        text += "NTP=%s\n" % server
    if dhcp:
        return text + "DHCP=yes\n\n[DHCPv4]\nClientIdentifier=mac\n"
    text += "Address=%s\n" % iface.with_prefixlen
    if gateway:
        text += "Gateway=%s\n" % gateway
    for d in dns:
        text += "DNS=%s\n" % d
    return text


def apply_network():
    run("networkctl", "reload")
    for name in ether_interfaces():
        run("networkctl", "reconfigure", name)


class NetworkChange:
    """A network change that reverts itself unless confirmed in time."""

    def __init__(self):
        self.lock = threading.Lock()
        self.old_text = None
        self.deadline = 0
        self.timer = None
        self.target = ""

    def pending(self):
        with self.lock:
            return self.old_text is not None

    def seconds_left(self):
        return max(0, int(self.deadline - time.time()))

    def apply(self, new_text, target):
        with self.lock:
            if self.old_text is None:
                self.old_text = read_text(NETWORK_FILE)
            if self.timer:
                self.timer.cancel()
            self.target = target
            write_atomic(NETWORK_FILE, new_text)
            self.deadline = time.time() + ROLLBACK_SECONDS
            self.timer = later(ROLLBACK_SECONDS, self.revert)
        log("network: new settings written, applying (%s)" % target)
        later(1.5, apply_network)

    def confirm(self):
        with self.lock:
            if self.timer:
                self.timer.cancel()
            self.old_text = None
            self.timer = None
        log("network: settings confirmed")

    def revert(self):
        with self.lock:
            if self.old_text is None:
                return
            write_atomic(NETWORK_FILE, self.old_text)
            self.old_text = None
            self.timer = None
        log("network: not confirmed in time, reverting")
        apply_network()


NET = NetworkChange()


# ---------------------------------------------------------------- outputs (displays)

def read_kv(path):
    st = {}
    for line in read_text(path).splitlines():
        k, _, v = line.partition("=")
        st[k] = v
    return st


def default_out_stream(n):
    return "OUT_%s" % SERIAL if n == 1 else "OUT_%s_%d" % (SERIAL, n)


def default_in_stream(n):
    return "IN_%s" % SERIAL if n == 1 else "IN_%s_%d" % (SERIAL, n)


def out_setting(env, n, key, default=""):
    """PINDI_OUT<n>_<KEY>, falling back to the original single-output key for output 1."""
    v = env.get("PINDI_OUT%d_%s" % (n, key))
    if v is None and n == 1:
        v = env.get("PINDI_" + key)
    return v if v else default


_ntp = {"t": 0, "ok": False}


def clock_synced():
    if time.time() - _ntp["t"] > 30:
        _ntp["ok"] = run("timedatectl", "show", "-p", "NTPSynchronized", "--value").stdout.strip() == "yes"
        _ntp["t"] = time.time()
    return _ntp["ok"]


def clock_text():
    out = run("timedatectl", "timesync-status").stdout
    server = re.search(r"Server: (\S+)", out)
    offset = re.search(r"Offset: (\S+)", out)
    if clock_synced():
        return "synchronised%s%s" % (" with %s" % esc(server.group(1)) if server else "",
                                    " (offset %s)" % esc(offset.group(1)) if offset else "")
    return ('<span class="warn">not synchronised</span>%s' %
            (" &ndash; trying %s" % esc(server.group(1)) if server else ""))


class NetTraffic:
    """Received/sent bit rate of the wired interfaces, sampled in the background."""

    def __init__(self, interval=2.0):
        self.interval = interval
        self.rx = self.tx = None

    @staticmethod
    def _counters():
        rx = tx = 0
        for name in ether_interfaces():
            base = "/sys/class/net/%s/statistics/" % name
            rx += int(read_text(base + "rx_bytes", "0").strip() or 0)
            tx += int(read_text(base + "tx_bytes", "0").strip() or 0)
        return rx, tx, time.monotonic()

    def _loop(self):
        last = self._counters()
        while True:
            time.sleep(self.interval)
            cur = self._counters()
            dt = cur[2] - last[2]
            if dt > 0:
                self.rx = max(0, cur[0] - last[0]) * 8 / dt
                self.tx = max(0, cur[1] - last[1]) * 8 / dt
            last = cur

    def start(self):
        threading.Thread(target=self._loop, daemon=True).start()

    def text(self):
        if self.rx is None:
            return "measuring..."
        speeds = [int(read_text("/sys/class/net/%s/speed" % n, "0").strip() or 0) for n in ether_interfaces()]
        link = max([s for s in speeds if s > 0] or [0])
        text = "%s in, %s out" % (mbit(self.rx), mbit(self.tx))
        if link:
            text += " (%d Mbit/s link)" % link
            if max(self.rx, self.tx) > 0.8 * link * 1e6:
                text += " – close to the link limit, expect dropped frames"
        return text


def mbit(bps):
    return "%.1f Mbit/s" % (bps / 1e6) if bps >= 1e5 else "%.0f kbit/s" % (bps / 1e3)


TRAFFIC = NetTraffic()


def ndi_info():
    """What the player found out about the installed NDI SDK (/run/pindi/ndi)."""
    info = read_kv(RUN_DIR + "/ndi")
    version = info.get("version", "").split()  # "NDI SDK LINUX 13:10:53 Apr 13 2026 6.3.2.0"
    return {"version": version[-1] if version else "", "hx_receive": info.get("hx_receive") == "1"}


def hx_send_needs():
    """What sending NDI|HX would take besides the NDI Advanced SDK: a hardware H.264 encoder."""
    base = "/sys/class/video4linux"
    try:
        if any("encode" in read_text(os.path.join(base, d, "name")) for d in os.listdir(base)):
            return ""
    except OSError:
        pass
    if "Raspberry Pi 5" in read_text("/proc/device-tree/model"):
        return " and a hardware H.264 encoder (the Pi 5 has none)"
    return " and the Pi's H.264 encoder driver (bcm2835-codec, not loaded)"


def version():
    return read_text(VERSION_FILE).strip() or "unknown"


# ---------------------------------------------------------------- health

_THROTTLE = [(0, "under-voltage"), (1, "CPU frequency capped"), (2, "throttled"), (3, "soft temperature limit")]
_throttle_cache = {"t": 0, "v": 0}


def throttled():
    """vcgencmd get_throttled bits (Pi firmware). Cached: it is a subprocess."""
    if time.time() - _throttle_cache["t"] > 10:
        m = re.search(r"0x([0-9a-fA-F]+)", run("vcgencmd", "get_throttled").stdout if os.path.exists(
            "/usr/bin/vcgencmd") else "")
        _throttle_cache["v"] = int(m.group(1), 16) if m else 0
        _throttle_cache["t"] = time.time()
    return _throttle_cache["v"]


def health():
    """(summary, power/thermal warning) for the Device card."""
    parts = []
    temp = read_text("/sys/class/thermal/thermal_zone0/temp").strip()
    if temp.isdigit():
        parts.append("CPU %.0f °C" % (int(temp) / 1000))
    load = os.getloadavg()[0]
    parts.append("load %.1f (%d cores)" % (load, os.cpu_count() or 1))
    mem = {}
    for line in read_text("/proc/meminfo").splitlines():
        k, _, v = line.partition(":")
        mem[k] = int(v.split()[0]) if v.split() else 0
    if mem.get("MemTotal"):
        parts.append("memory %.0f%% used" % (100 - 100.0 * mem.get("MemAvailable", 0) / mem["MemTotal"]))
    st = os.statvfs("/")
    parts.append("storage %.0f%% used" % (100 - 100.0 * st.f_bavail / st.f_blocks))
    up = float((read_text("/proc/uptime").split() or ["0"])[0])
    parts.append("up %s" % duration(up))
    t = throttled()
    now = [name for bit, name in _THROTTLE if t & (1 << bit)]
    past = [name for bit, name in _THROTTLE if t & (1 << (bit + 16)) and not t & (1 << bit)]
    warn = ""
    if now:
        warn = "Now: %s." % ", ".join(now)
    if past:
        warn += (" " if warn else "") + "Since boot: %s." % ", ".join(past)
    if t & 0x10001:
        warn += " Use a proper 5 V power supply (Pi 3: 2.5 A, Pi 4: 3 A, Pi 5: 5 A) and a short cable."
    elif t & 0xE000E:
        warn += " Improve cooling (heatsink/fan, ventilated case)."
    return " · ".join(parts), warn.strip() or "OK"


def duration(sec):
    sec = int(sec)
    d, h, m = sec // 86400, sec // 3600 % 24, sec // 60 % 60
    return ("%d d %d h" % (d, h)) if d else ("%d h %d min" % (h, m)) if h else "%d min" % m


# ---------------------------------------------------------------- device name, NDI network settings

def read_ndi_config():
    try:
        cfg = json.loads(read_text(NDI_CONFIG) or "{}")
        return cfg if isinstance(cfg, dict) else {}
    except ValueError:
        return {}


def ndi_settings():
    ndi = read_ndi_config().get("ndi", {})
    nets, groups = ndi.get("networks", {}), ndi.get("groups", {})
    return {"discovery": nets.get("discovery", "") or "", "send_groups": groups.get("send", "") or "",
            "recv_groups": groups.get("recv", "") or "", "machinename": ndi.get("machinename", "") or ""}


def write_ndi_settings(discovery, send_groups, recv_groups, machinename):
    """Update our keys in ndi-config.v1.json, keeping anything else that is in there."""
    cfg = read_ndi_config()
    ndi = cfg.setdefault("ndi", {})
    for section, key, val in (("networks", "discovery", discovery), ("groups", "send", send_groups),
                              ("groups", "recv", recv_groups)):
        sec = ndi.setdefault(section, {})
        if val:
            sec[key] = val
        else:
            sec.pop(key, None)
        if not sec:
            ndi.pop(section)
    if machinename:
        ndi["machinename"] = machinename
    else:
        ndi.pop("machinename", None)
    if not ndi:
        cfg.pop("ndi")
    os.makedirs(os.path.dirname(NDI_CONFIG), exist_ok=True)
    write_atomic(NDI_CONFIG, json.dumps(cfg, indent=2) + "\n")


def device_name():
    return ndi_settings()["machinename"] or socket.gethostname()


def ndi_host_name():
    """How NDI shows this device in source names: "NAME (stream)"."""
    return ndi_settings()["machinename"] or socket.gethostname().upper()


_HOSTNAME = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")


def set_hostname(name):
    old = socket.gethostname()
    if name == old:
        return False
    r = run("hostnamectl", "set-hostname", name)
    if r.returncode:
        raise RuntimeError(r.stderr.strip() or "hostnamectl failed")
    lines = [l for l in read_text("/etc/hosts").splitlines() if not l.startswith("127.0.1.1")]
    lines.append("127.0.1.1\t%s" % name)
    write_atomic("/etc/hosts", "\n".join(lines) + "\n")
    return True


def latency_text(n):
    """Human summary of /run/pindi/out<n>.latency (5 s averages of frames that reached the screen)."""
    lat = read_kv("%s/out%d.latency" % (RUN_DIR, n))
    if "pi_avg" not in lat:
        return ""
    pi = "%.0f ms on this Pi" % float(lat["pi_avg"])
    if "total_avg" not in lat:
        return pi + " (sender gives no timestamp)"
    total = float(lat["total_avg"])
    if not -10000 < total < 10000:
        return pi + " (sender and Pi clocks disagree)"
    if total < float(lat["pi_avg"]):
        # Impossible unless the sender's clock runs ahead of this Pi's.
        return pi + (" – sender → screen not measurable: the sender's clock is at least %.0f ms ahead "
                     "of this Pi's. Sync the sender's clock (Windows: w32tm /resync)." %
                     (float(lat["pi_avg"]) - total))
    text = "%.0f ms sender → screen (%.0f–%.0f), of which %s" % (
        total, float(lat["total_min"]), float(lat["total_max"]), pi)
    if not clock_synced():
        text += " – Pi clock not NTP-synced, sender figure approximate"
    return text


def outputs():
    """Display outputs as detected by the player, with their settings and live status."""
    env = read_env()
    running = run("systemctl", "is-active", "pindi.service").stdout.strip() == "active"
    hx = ndi_info()["hx_receive"]
    out = []
    for line in read_text(RUN_DIR + "/outputs").splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        n = int(parts[0])
        st = read_kv("%s/out%d.status" % (RUN_DIR, n))
        if not running:
            st["state"] = "player not running"
        modes = [m for m in read_text("%s/out%d.modes" % (RUN_DIR, n)).split() if re.match(r"^\d+x\d+@\d+\*?$", m)]
        bandwidth = out_setting(env, n, "BANDWIDTH", env.get("PINDI_BANDWIDTH") or "highest").lower()
        fmt = ""
        if running and st.get("state", "").startswith("playing"):
            # Without the HX decoder the SDK can only have negotiated SpeedHQ with the sender.
            fmt = "Full NDI (SpeedHQ)" if not hx else "Full NDI or NDI|HX (the SDK does not report which)"
            if bandwidth == "lowest":
                fmt += ", low-bandwidth preview"
        out.append({
            "n": n, "connector": parts[1], "link": parts[2], "mode": parts[3] if len(parts) > 3 else "",
            "enabled": out_setting(env, n, "ENABLED", "1") != "0",
            "stream": out_setting(env, n, "STREAM"), "source_ip": out_setting(env, n, "SOURCE_IP"),
            "backup": out_setting(env, n, "BACKUP_STREAM"),
            "rotate": out_setting(env, n, "ROTATE", "0"), "mirror": out_setting(env, n, "MIRROR", "none"),
            "bandwidth": "lowest" if bandwidth == "lowest" else "highest", "format": fmt,
            "mode_setting": out_setting(env, n, "MODE"),
            "audio_out": out_setting(env, n, "AUDIO", "hdmi").lower(),
            "audio_delay": out_setting(env, n, "AUDIO_DELAY_MS", "80"),
            "audio": read_text("%s/out%d.audio" % (RUN_DIR, n)).strip()
            if running and st.get("state", "").startswith("playing") else "",
            "modes": [m.rstrip("*") for m in modes],
            "preferred_mode": next((m.rstrip("*") for m in modes if m.endswith("*")), ""),
            "state": st.get("state", "-"), "target": st.get("target", default_out_stream(n)),
            "source": st.get("source", ""), "video": st.get("video", ""),
            "latency": latency_text(n) if running and st.get("state", "").startswith("playing") else "",
        })
    return out


# ---------------------------------------------------------------- inputs (capture devices)

def _stable_path(dev):
    """/dev/v4l/by-id (or by-path) link for /dev/videoN, so the setting survives re-plugging."""
    for d in ("/dev/v4l/by-id", "/dev/v4l/by-path"):
        try:
            for name in sorted(os.listdir(d)):
                p = os.path.join(d, name)
                if os.path.realpath(p) == dev:
                    return p
        except OSError:
            pass
    return dev


def capture_modes(dev):
    """[(value, label)] of capture modes, e.g. ("MJPG 1920x1080@30", "MJPEG 1920x1080 @ 30 fps")."""
    modes, fourcc, size = [], None, None
    for line in run("v4l2-ctl", "-d", dev, "--list-formats-ext").stdout.splitlines():
        m = re.match(r"\s*\[\d+\]: '(\w+)'", line)
        if m:
            fourcc, size = m.group(1), None
            continue
        m = re.match(r"\s*Size: Discrete (\d+)x(\d+)", line)
        if m:
            size = (int(m.group(1)), int(m.group(2)))
            continue
        m = re.search(r"\(([\d.]+) fps\)", line)
        if m and size and fourcc in ("MJPG", "YUYV", "UYVY"):
            fps = float(m.group(1))
            modes.append((fourcc, size[0], size[1], fps))
    modes.sort(key=lambda x: (x[0] != "MJPG", -x[1] * x[2], -x[3]))
    names = {"MJPG": "MJPEG", "YUYV": "YUYV (raw)", "UYVY": "UYVY (raw)"}
    return [("%s %dx%d@%g" % m, "%s %dx%d @ %g fps" % (names[m[0]], m[1], m[2], m[3])) for m in modes]


def detected_inputs():
    """USB video capture devices currently plugged in."""
    found = []
    base = "/sys/class/video4linux"
    try:
        names = sorted(os.listdir(base), key=lambda s: int(re.sub(r"\D", "", s) or 0))
    except OSError:
        names = []
    for name in names:
        sysdir = os.path.join(base, name)
        driver = os.path.basename(os.path.realpath(os.path.join(sysdir, "device", "driver")))
        if driver != "uvcvideo" or read_text(os.path.join(sysdir, "index")).strip() not in ("", "0"):
            continue  # not a USB capture device, or its metadata node
        dev = "/dev/" + name
        modes = capture_modes(dev)
        if not modes:
            continue
        found.append({"dev": dev, "id": _stable_path(dev), "label": read_text(os.path.join(sysdir, "name")).strip(),
                      "modes": modes})
    return found


def configured_inputs(env):
    """{slot: settings} from PINDI_IN<n>_* keys."""
    slots = {}
    for key, val in env.items():
        m = re.match(r"^PINDI_IN(\d+)_(DEVICE|ENABLED|NAME|MODE|AUDIO)$", key)
        if m:
            slots.setdefault(int(m.group(1)), {})[m.group(2)] = val
    return {n: s for n, s in slots.items() if s.get("DEVICE")}


def inputs():
    """Detected devices merged with configured ones (configured but unplugged ones are listed too)."""
    env = read_env()
    conf = configured_inputs(env)
    by_dev = {s["DEVICE"]: n for n, s in conf.items()}
    out = []
    for d in detected_inputs():
        slot = by_dev.pop(d["id"], None)
        s = conf.get(slot, {})
        out.append(dict(d, slot=slot, present=True, enabled=s.get("ENABLED") == "1",
                        name=s.get("NAME", ""), mode=s.get("MODE", ""), audio_setting=s.get("AUDIO", "auto")))
    for dev, slot in by_dev.items():
        s = conf[slot]
        out.append({"dev": "", "id": dev, "label": os.path.basename(dev), "modes": [], "slot": slot,
                    "present": False, "enabled": s.get("ENABLED") == "1", "name": s.get("NAME", ""),
                    "mode": s.get("MODE", ""), "audio_setting": s.get("AUDIO", "auto")})
    for i in out:
        st = read_kv("%s/in%d.status" % (RUN_DIR, i["slot"])) if i["slot"] else {}
        i["state"] = st.get("state", "") if i["enabled"] else "off"
        i["live_mode"] = st.get("mode", "")
        i["fps"] = st.get("fps", "")
        i["cam_fps"] = st.get("cam_fps", "")
        i["damaged"] = st.get("damaged", "0")
        i["audio"] = st.get("audio", "") if i["enabled"] else ""
    return out


def visible_sources():
    out = []
    for line in read_text(RUN_DIR + "/sources").splitlines():
        name, _, url = line.partition("\t")
        if name:
            host = url.rpartition(":")[0] if ":" in url else url
            m = re.match(r"^(.*) \((.*)\)$", name)
            out.append({"name": name, "stream": m.group(2) if m else name, "ip": host})
    return out


# ---------------------------------------------------------------- sessions

class Sessions:
    def __init__(self):
        self.lock = threading.Lock()
        self.items = {}

    def create(self, user):
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.items[token] = {"user": user, "csrf": secrets.token_urlsafe(24),
                                 "expires": time.time() + SESSION_TTL, "flash": None}
        return token

    def get(self, token):
        with self.lock:
            s = self.items.get(token or "")
            if s and s["expires"] > time.time():
                s["expires"] = time.time() + SESSION_TTL
                return s
            self.items.pop(token or "", None)
            return None

    def drop(self, token):
        with self.lock:
            self.items.pop(token or "", None)


SESSIONS = Sessions()

WELL_KNOWN_PASSWORDS = {"pindi", "raspberry", "password", "admin", "1234", "12345678", "pi"}


class Lockout:
    """After 5 failed sign-ins within 10 minutes an address must wait 10 minutes."""
    LIMIT, WINDOW, PENALTY = 5, 600, 600

    def __init__(self):
        self.lock = threading.Lock()
        self.fails = {}  # ip -> [timestamps]
        self.until = {}  # ip -> locked until

    def blocked(self, ip):
        with self.lock:
            return max(0, int(self.until.get(ip, 0) - time.time()))

    def failure(self, ip):
        now = time.time()
        with self.lock:
            recent = [t for t in self.fails.get(ip, []) if now - t < self.WINDOW] + [now]
            self.fails[ip] = recent
            if len(recent) >= self.LIMIT:
                self.until[ip] = now + self.PENALTY
                self.fails.pop(ip, None)
                log("login: %s locked out for %d s after %d failures" % (ip, self.PENALTY, self.LIMIT))

    def success(self, ip):
        with self.lock:
            self.fails.pop(ip, None)


LOCKOUT = Lockout()


# ---------------------------------------------------------------- pages

CSS = """
:root{--bg:#0b0f14;--card:#141b23;--line:#253140;--text:#e6edf3;--muted:#8b98a5;--accent:#2bb3a3;
--accent2:#1d6f8f;--bad:#e5534b;--ok:#3fb950;--warn:#d29922}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);
font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 20px;
border-bottom:1px solid var(--line)}header h1{margin:0;font-size:20px;letter-spacing:.5px}
header h1 span{color:var(--accent)}header .who{color:var(--muted);font-size:13px}
main{max-width:860px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px 18px;margin:0 0 16px}
.card h2{margin:0 0 12px;font-size:16px}
dl{display:grid;grid-template-columns:max-content 1fr;gap:6px 16px;margin:0}dt{color:var(--muted)}dd{margin:0;word-break:break-all}
label{display:block;margin:10px 0 4px;color:var(--muted);font-size:13px}
input[type=text],input[type=password],input[type=number],select{width:100%;padding:9px 10px;border-radius:7px;border:1px solid var(--line);
background:#0d1319;color:var(--text);font:inherit}
option:disabled{color:var(--muted)}
input:focus{outline:2px solid var(--accent2);border-color:var(--accent2)}
.row{display:grid;grid-template-columns:1fr 1fr;gap:0 14px}
.radio{display:flex;gap:18px;margin:4px 0 4px}.radio label{display:flex;gap:6px;align-items:center;margin:0;color:var(--text);font-size:15px}
button{margin-top:14px;padding:9px 16px;border:0;border-radius:7px;background:var(--accent2);color:#fff;font:inherit;cursor:pointer}
button:hover{filter:brightness(1.15)}button.secondary{background:#2a3644}button.danger{background:#8e2c27}
button.small{margin:0;padding:4px 10px;font-size:13px}
.hint{color:var(--muted);font-size:13px;margin:6px 0 0}
.msg{padding:10px 14px;border-radius:8px;margin:0 0 16px;border:1px solid}
.msg.ok{border-color:var(--ok);background:#12261a}.msg.err{border-color:var(--bad);background:#2a1414}
.msg.warn{border-color:var(--warn);background:#2a2210}
table{width:100%;border-collapse:collapse;font-size:14px}td{padding:6px 4px;border-top:1px solid var(--line)}
td:last-child{text-align:right}.state{font-weight:600}.state.playing{color:var(--ok)}
.login{max-width:360px;margin:12vh auto}
.badge{font-size:12px;font-weight:400;padding:2px 9px;border-radius:10px;background:#2a3644;color:var(--muted);
margin-left:6px;vertical-align:middle}.badge.on{background:#12261a;color:var(--ok)}
label.check{display:flex;gap:8px;align-items:center;color:var(--text);font-size:15px;margin:12px 0 0}
label.check .hint{margin:0}
.sub{border-top:1px solid var(--line);padding:12px 0 4px}.sub:first-of-type{border-top:0}
.sub h3{margin:0 0 2px;font-size:15px}.sub p{margin:4px 0}
.card dl{margin-bottom:6px}.warn{color:var(--warn)}.hint.block{display:block;margin-top:2px}
form.inline{display:inline}
.buttons{display:flex;flex-wrap:wrap;gap:8px;margin:12px 0 0}.buttons button,.buttons .button{margin:0}
a.button{display:inline-block;padding:9px 16px;border-radius:7px;background:var(--accent2);color:#fff;
text-decoration:none}a.button.secondary{background:#2a3644}a.button:hover{filter:brightness(1.15)}
input[type=file]{color:var(--muted);font:inherit;margin:4px 0}button:disabled{opacity:.5;cursor:default}
pre.log{background:#0d1319;border:1px solid var(--line);border-radius:7px;padding:10px;overflow:auto;
max-height:70vh;font:12px/1.4 ui-monospace,Consolas,monospace;white-space:pre-wrap;word-break:break-all}
nav.top a{color:var(--accent);margin-right:14px}
.preview{float:right;margin:0 0 10px 14px;width:240px}.thumb{width:100%;border-radius:6px;border:1px solid var(--line);
background:#000;display:block;min-height:40px}.thumb.none{visibility:hidden}
@media(max-width:600px){.preview{float:none;width:100%;margin:0 0 10px}}
.card:after{content:"";display:table;clear:both}
@media(max-width:600px){.row{grid-template-columns:1fr}main{padding:12px}}
"""

JS = """
function sync(){var s=document.getElementById('mode-static');if(!s)return;
document.getElementById('static-fields').style.display=s.checked?'block':'none';}
document.addEventListener('change',function(e){if(e.target.name==='mode')sync();});
document.addEventListener('click',function(e){var b=e.target.closest('[data-stream]');if(!b)return;
var n=b.dataset.out;document.getElementById('stream-'+n).value=b.dataset.stream;
document.getElementById('source_ip-'+n).value=b.dataset.ip;
document.getElementById('out'+n).scrollIntoView({behavior:'smooth'});});
function poll(){fetch('/api/status',{credentials:'same-origin'}).then(function(r){return r.ok?r.json():null})
.then(function(d){if(!d)return;document.querySelectorAll('[data-k]').forEach(function(el){
var k=el.dataset.k;if(!(k in d))return;var v=d[k];el.textContent=v||'-';
if(/state$/.test(k))el.className='state'+(/^(playing|sending)/.test(v||'')?' playing':'');
if(k==='power')el.className=v==='OK'?'':'warn';});
var c=document.getElementById('countdown');if(c&&d.rollback!=null)c.textContent=d.rollback;}).catch(function(){});}
sync();if(document.querySelector('[data-k]'))setInterval(poll,3000);
setInterval(function(){document.querySelectorAll('img[data-thumb]').forEach(function(i){
if(document.hidden)return;var n=new Image();n.onload=function(){i.src=n.src;i.classList.remove('none');};
n.onerror=function(){i.classList.add('none');};n.src='/thumb/'+i.dataset.thumb+'.jpg?'+Date.now();});},2000);
document.addEventListener('submit',function(e){var m=e.target.dataset.confirm;if(m&&!confirm(m))e.preventDefault();});
var rf=document.getElementById('restore-file');if(rf)rf.addEventListener('change',function(){
var f=rf.files[0],b=document.getElementById('restore-button');b.disabled=true;if(!f)return;
var r=new FileReader();r.onload=function(){document.getElementById('restore-data').value=r.result;b.disabled=false;};
r.readAsText(f);});
"""


def page(title, body, user=None, csrf=None):
    who = ""
    if user:
        who = ('<div class="who">%s &middot; <form class="inline" method="post" action="/logout">'
               '<input type="hidden" name="csrf" value="%s"><button class="small secondary">Log out</button>'
               '</form></div>' % (html.escape(user), csrf))
    return ("<!doctype html><html lang=en><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>%s</title><link rel=stylesheet href=/style.css></head><body>"
            "<header><h1>Pi<span>NDI</span></h1>%s</header><main>%s</main>"
            "<script src=/app.js></script></body></html>") % (html.escape(title), who, body)


def esc(v):
    return html.escape(str(v or ""), quote=True)


def login_page(error=""):
    err = '<div class="msg err">%s</div>' % esc(error) if error else ""
    body = ('<div class="login card"><h2>Sign in</h2>%s<form method="post" action="/login">'
            '<label>User</label><input type="text" name="user" autocomplete="username" autofocus required>'
            '<label>Password</label><input type="password" name="password" autocomplete="current-password" required>'
            '<button>Sign in</button><p class="hint">Use the same login as SSH.</p></form></div>' % err)
    return page("PiNDI - Sign in", body)


def dashboard(session):
    csrf = session["csrf"]
    hidden = '<input type="hidden" name="csrf" value="%s">' % csrf
    parts = []

    flash = session.pop("flash", None)
    session["flash"] = None
    if flash:
        parts.append('<div class="msg %s">%s</div>' % (flash[0], esc(flash[1])))

    if NET.pending():
        parts.append(
            '<div class="msg warn"><b>New network settings are active.</b> They will be undone in '
            '<span id="countdown">%d</span> s unless you keep them.<form method="post" action="/network/confirm">'
            '%s<button>Keep these settings</button> <button class="secondary" formaction="/network/revert">'
            'Undo now</button></form></div>' % (NET.seconds_left(), hidden))

    net = live_network()
    outs = outputs()
    ins = inputs()

    def options(choices, current):
        return "".join('<option value="%s"%s>%s</option>' % (esc(v), " selected" if v == current else "", label)
                       for v, label in choices)

    # Device
    hl, hw = health()
    ndi = ndi_info()
    ndi_text = "SDK %s &middot; Full NDI (SpeedHQ) receive and send" % esc(ndi["version"] or "?")
    if ndi["hx_receive"]:
        ndi_text += " &middot; NDI|HX receive available"
    else:
        ndi_text += ('<span class="hint block">NDI|HX: not available &ndash; the standard NDI SDK has no HX '
                     'decoder or encoder (that needs the NDI Advanced SDK and a vendor licence from NDI). '
                     'Software HX senders automatically send Full NDI to this device instead; cameras that '
                     'only send HX will not show a picture.</span>')
    rows = [
        ("IP address", '<span data-k="ip">%s</span>' % esc(net["address"] or "-")),
        ("Network traffic", '<span data-k="net">%s</span><span class="hint block">Everything this device '
                            'receives and sends on the wired network. A Full NDI 1080p60 stream takes up to about '
                            '130 Mbit/s, depending on the picture (still images far less).</span>' %
                            esc(TRAFFIC.text())),
        ("Health", '<span data-k="health">%s</span>' % esc(hl)),
        ("Power / cooling", '<span data-k="power" class="%s">%s</span>' % ("" if hw == "OK" else "warn", esc(hw))),
        ("NDI", ndi_text),
        ("Device name", "%s <span class=\"hint\">(NDI sources from this device: %s (&hellip;))</span>" %
         (esc(device_name()), esc(ndi_host_name()))),
        ("Serial", esc(SERIAL)),
        ("Model", esc(MODEL)),
        ("Software", "PiNDI %s" % esc(version())),
        ("Clock", clock_text()),
        ("Outputs", "%d display output%s" % (len(outs), "" if len(outs) == 1 else "s")),
        ("Inputs", "%d capture device%s" % (len(ins), "" if len(ins) == 1 else "s")),
    ]
    parts.append('<section class="card"><h2>Device</h2><dl>%s</dl></section>' %
                 "".join("<dt>%s</dt><dd>%s</dd>" % r for r in rows))

    # One card per display output
    if not outs:
        parts.append('<section class="card"><h2>Outputs</h2><p class="hint">No display outputs reported - '
                     'is the player running?</p></section>')
    for o in outs:
        n = o["n"]
        link = "disabled" if not o["enabled"] else (o["link"] + (" " + o["mode"] if o["mode"] else ""))
        badge = '<span class="badge%s">%s</span>' % (" on" if o["link"] == "connected" and o["enabled"] else "",
                                                     esc(link))
        k = "out%d." % n
        status_rows = "" if not o["enabled"] else (
            '<div class="preview"><img class="thumb" data-thumb="%d" alt="What output %d shows now" '
            'src="/thumb/%d.jpg"></div>' % (n, n, n) +
            '<dl><dt>State</dt><dd><span data-k="%sstate" class="state%s">%s</span></dd>'
            '<dt>Looking for</dt><dd data-k="%starget">%s</dd>'
            '<dt>Connected to</dt><dd data-k="%ssource">%s</dd>'
            '<dt>Video</dt><dd data-k="%svideo">%s</dd>'
            '<dt>Format</dt><dd data-k="%sformat">%s</dd>'
            '<dt>Audio</dt><dd data-k="%saudio">%s</dd>'
            '<dt>Latency</dt><dd><span data-k="%slatency">%s</span>'
            '<span class="hint block">Measured per frame: from the moment the sender handed '
            'it to NDI until it was on screen. Not included: the camera/capture before the sender and the '
            'monitor&#39;s own processing.</span></dd></dl>' %
            (k, " playing" if o["state"].startswith("playing") else "", esc(o["state"]), k, esc(o["target"]),
             k, esc(o["source"] or "-"), k, esc(o["video"] or "-"), k, esc(o["format"] or "-"),
             k, esc(o["audio"] or "-"),
             k, esc(o["latency"] or "-")))
        hx_opt = ('<option disabled>NDI|HX (H.264/HEVC) &ndash; %s</option>' %
                  ("chosen by the sender" if ndi["hx_receive"] else "needs the NDI Advanced SDK"))
        parts.append(
            '<section class="card" id="out%d"><h2>Output %d &middot; %s %s</h2>%s'
            '<form method="post" action="/output">%s<input type="hidden" name="n" value="%d">'
            '<label class="check"><input type="checkbox" name="enabled" value="1"%s> Enabled '
            '<span class="hint">(off: no NDI is received and this HDMI port is switched off)</span></label>'
            '<div class="row"><div><label>Stream name</label><input type="text" id="stream-%d" name="stream" '
            'value="%s" placeholder="%s" maxlength="200"></div><div><label>Source IP</label>'
            '<input type="text" id="source_ip-%d" name="source_ip" value="%s" placeholder="Any" maxlength="15">'
            '</div><div><label>Backup stream (optional)</label><input type="text" name="backup" value="%s" '
            'placeholder="None" maxlength="200"><span class="hint block">Played while the main stream is '
            'unavailable; it switches back automatically.</span></div>'
            '<div><label>Rotation</label><select name="rotate">%s</select></div>'
            '<div><label>Mirror</label><select name="mirror">%s</select></div>'
            '<div><label>Receive quality</label><select name="bandwidth">%s</select></div>'
            '<div><label>Display mode</label><select name="display_mode">%s</select></div>'
            '<div><label>Audio output</label><select name="audio">%s</select></div>'
            '<div><label>Audio delay (ms)</label><input type="number" name="audio_delay" min="20" max="1000" '
            'step="10" value="%s"><span class="hint block">The least delay used; it is raised automatically '
            'when a sender&#39;s audio arrives unevenly. The picture appears about 25 ms after it arrives.'
            '</span></div>'
            '<div><label>Codec</label><select name="codec"><option selected>Full NDI (SpeedHQ)</option>%s'
            '</select></div></div>'
            '<p class="hint">Empty stream name = %s, empty IP = any host. A source IP is also searched '
            'directly, so senders on other subnets work. 180&deg; is for ceiling mounting, mirror for rear '
            'projection. The low-bandwidth preview is a small (typically 640&times;360) stream every NDI sender '
            'provides, for slow networks.</p><button>Save output %d</button> '
            '<button class="secondary" formaction="/identify" name="identify" value="1">Identify</button> '
            '<button class="secondary" name="reset" value="1">Reset to default</button></form></section>' %
            (n, n, esc(o["connector"]), badge, status_rows, hidden, n, " checked" if o["enabled"] else "",
             n, esc(o["stream"]), esc(default_out_stream(n)), n, esc(o["source_ip"]), esc(o["backup"]),
             options(ROTATIONS, o["rotate"]), options(MIRRORS, o["mirror"]), options(BANDWIDTHS, o["bandwidth"]),
             options(mode_choices(o), o["mode_setting"]), options(audio_choices(), o["audio_out"]),
             esc(o["audio_delay"]), hx_opt, esc(default_out_stream(n)), n))

    # NDI sources on the network
    srcs = visible_sources()
    enabled_outs = [o["n"] for o in outs if o["enabled"]]
    if srcs and enabled_outs:
        def use_buttons(s):
            return " ".join(
                '<button type="button" class="small secondary" data-out="%d" data-stream="%s" data-ip="%s">'
                'Use%s</button>' % (n, esc(s["stream"]), esc(s["ip"]), " for %d" % n if len(outs) > 1 else "")
                for n in enabled_outs)
        parts.append('<section class="card"><h2>NDI sources on the network</h2><table>%s</table></section>' %
                     "".join('<tr><td>%s</td><td>%s</td><td>%s</td></tr>' %
                             (esc(s["name"]), esc(s["ip"]), use_buttons(s)) for s in srcs))

    # Outgoing streams (capture devices)
    used = {i["slot"] for i in ins if i["slot"]}
    free_slots = [n for n in range(1, 9) if n not in used]
    # Sending HX means encoding H.264 ourselves and passing it through the Advanced SDK.
    hx_send_opt = '<option disabled>NDI|HX (H.264) &ndash; needs the NDI Advanced SDK%s</option>' % esc(
        hx_send_needs())
    cards = []
    for i in ins:
        slot = i["slot"] or (free_slots.pop(0) if free_slots else 0)
        cap_choices = [("", "Automatic (best up to 1080p30)")] + i["modes"]
        if i["mode"] and i["mode"] not in dict(i["modes"]):
            cap_choices.append((i["mode"], i["mode"]))
        state = i["state"] or ("starting" if i["enabled"] else "off")
        live = ""
        if i["enabled"] and i["live_mode"]:
            live = " &middot; %s &middot; %s fps sent" % (esc(i["live_mode"]), esc(i["fps"]))
            if i["cam_fps"]:
                live += " (camera delivers %s fps" % esc(i["cam_fps"])
                if i["damaged"] not in ("", "0"):
                    live += ", %s damaged frame%s skipped in the last 2 s" % (
                        esc(i["damaged"]), "" if i["damaged"] == "1" else "s")
                live += ")"
        if i["audio"]:
            live += " &middot; audio: %s" % esc(i["audio"])
        k = "in%d." % slot
        remove = ('<button class="secondary" name="remove" value="1">Forget this device</button>'
                  if not i["present"] else "")
        cards.append(
            '<div class="sub"><h3>%s <span class="badge%s">%s</span></h3>'
            '<p class="hint">%s</p><p>Status: <span data-k="%sstate" class="state%s">%s</span>%s</p>'
            '<form method="post" action="/outgoing">%s<input type="hidden" name="device" value="%s">'
            '<input type="hidden" name="slot" value="%d">'
            '<label class="check"><input type="checkbox" name="enabled" value="1"%s> Send this input over NDI</label>'
            '<div class="row"><div><label>NDI stream name</label><input type="text" name="name" value="%s" '
            'placeholder="%s" maxlength="100"></div><div><label>Capture mode</label><select name="mode">%s</select>'
            '</div><div><label>Codec</label><select name="codec"><option selected>Full NDI (SpeedHQ)</option>'
            '%s</select></div><div><label>Audio</label><select name="audio">%s</select></div></div>'
            '<button>Save</button> %s</form></div>' %
            (esc(i["label"]), " on" if i["present"] else "", "connected" if i["present"] else "not connected",
             esc(i["id"]), k, " playing" if state.startswith("sending") else "", esc(state), live, hidden,
             esc(i["id"]), slot, " checked" if i["enabled"] else "", esc(i["name"]), esc(default_in_stream(slot)),
             options(cap_choices, i["mode"]), hx_send_opt,
             options([("auto", "From the device (USB audio)"), ("off", "Off")], i["audio_setting"]), remove))
    body = "".join(cards) if cards else (
        '<p class="hint">No capture devices detected. Plug in a USB HDMI capture card and reload this page.</p>')
    parts.append('<section class="card" id="outgoing"><h2>Configure Outgoing Streams</h2>'
                 '<p class="hint">Nothing is sent until you enable it here. The stream appears on the network as '
                 '"%s (name)".</p>%s</section>' % (esc(ndi_host_name()), body))

    # Device settings
    env = read_env()
    ns = ndi_settings()
    parts.append(
        '<section class="card" id="settings"><h2>Device settings</h2><form method="post" action="/device">%s'
        '<div class="row"><div><label>Device name</label><input type="text" name="name" value="%s" maxlength="63" '
        'required><span class="hint block">Shown in NDI source names and used as the network host name '
        '(%s.local).</span></div>'
        '<div><label>NDI Discovery Server (optional)</label><input type="text" name="discovery" value="%s" '
        'placeholder="e.g. 192.168.1.10" maxlength="200"><span class="hint block">Find and announce sources '
        'through a Discovery Server instead of mDNS. Several: comma separated.</span></div>'
        '<div><label>NDI groups to receive from</label><input type="text" name="recv_groups" value="%s" '
        'placeholder="Public" maxlength="200"></div>'
        '<div><label>NDI groups to send to</label><input type="text" name="send_groups" value="%s" '
        'placeholder="Public" maxlength="200"></div></div>'
        '<label class="check"><input type="checkbox" name="splash_info" value="1"%s> Show IP address, serial '
        'and status on the splash screen</label>'
        '<label class="check"><input type="checkbox" name="frame_timeout" value="1"%s> Show the splash when a '
        'connected source stops sending video for 10 s <span class="hint">(off: keep the last picture, '
        'e.g. for screen captures that only send on change)</span></label>'
        '<button>Save device settings</button></form></section>' %
        (hidden, esc(device_name()), esc(socket.gethostname()), esc(ns["discovery"]), esc(ns["recv_groups"]),
         esc(ns["send_groups"]), "" if env.get("PINDI_SPLASH_INFO") == "0" else " checked",
         " checked" if (env.get("PINDI_FRAME_TIMEOUT_MS") or "0") not in ("", "0") else ""))

    # Network
    cfg = read_network_config()
    fill = cfg if not cfg["dhcp"] else {"address": net["address"], "prefix": net["prefix"],
                                         "gateway": net["gateway"], "dns": net["dns"]}
    mask = ""
    if fill["prefix"]:
        try:
            mask = str(ipaddress.IPv4Network("0.0.0.0/%s" % fill["prefix"]).netmask)
        except ValueError:
            pass
    parts.append(
        '<section class="card"><h2>Network</h2><form method="post" action="/network">%s'
        '<div class="radio"><label><input type="radio" name="mode" value="dhcp" %s> Automatic (DHCP)</label>'
        '<label><input type="radio" name="mode" value="static" id="mode-static" %s> Static IP</label></div>'
        '<div id="static-fields"><div class="row"><div><label>IP address</label>'
        '<input type="text" name="address" value="%s" placeholder="192.168.1.50"></div>'
        '<div><label>Subnet mask</label><input type="text" name="netmask" value="%s" placeholder="255.255.255.0"></div>'
        '<div><label>Gateway</label><input type="text" name="gateway" value="%s" placeholder="192.168.1.1"></div>'
        '<div><label>DNS servers</label><input type="text" name="dns" value="%s" placeholder="192.168.1.1, 1.1.1.1">'
        '</div></div></div>'
        '<label>NTP time servers (optional)</label><input type="text" name="ntp" value="%s" '
        'placeholder="e.g. 192.168.1.1 or time.example.com">'
        '<p class="hint">The public pool.ntp.org servers are always tried as well; add a local server for sites '
        'without internet access. An accurate clock is needed for the sender &rarr; screen latency figure.</p>'
        '<p class="hint">After applying, open the device at its new address within %d seconds and press '
        '<b>Keep</b>, otherwise the previous settings come back automatically. The current IP is always '
        'shown on the splash screen.</p><button>Apply network settings</button></form></section>' %
        (hidden, "checked" if cfg["dhcp"] else "", "" if cfg["dhcp"] else "checked",
         esc(fill["address"]), esc(mask), esc(fill["gateway"]), esc(", ".join(fill["dns"])),
         esc(", ".join(cfg["ntp"])), ROLLBACK_SECONDS))

    # Password
    parts.append(
        '<section class="card"><h2>Password</h2><form method="post" action="/password">%s'
        '<label>Current password</label><input type="password" name="current" autocomplete="current-password" required>'
        '<div class="row"><div><label>New password</label><input type="password" name="new" '
        'autocomplete="new-password" required></div><div><label>Repeat new password</label>'
        '<input type="password" name="confirm" autocomplete="new-password" required></div></div>'
        '<p class="hint">Changes the password of <b>%s</b> for both this page and SSH.</p>'
        '<button>Change password</button></form></section>' % (hidden, esc(session["user"])))

    # System
    parts.append(
        '<section class="card" id="system"><h2>System</h2>'
        '<div class="buttons"><a class="button secondary" href="/logs">View logs</a>'
        '<a class="button secondary" href="/diagnostics">Download diagnostics</a>'
        '<a class="button secondary" href="/backup">Download settings backup</a></div>'
        '<form method="post" action="/restore" id="restore-form">%s<label>Restore settings from a backup file'
        '</label><input type="file" id="restore-file" accept=".json,application/json">'
        '<input type="hidden" name="backup" id="restore-data"><p class="hint">Restores outputs, outgoing '
        'streams, device and NDI settings. Network settings are restored too, with the usual automatic '
        'rollback.</p><button class="secondary" id="restore-button" disabled>Restore</button></form>'
        '<div class="buttons"><form class="inline" method="post" action="/identify">%s'
        '<input type="hidden" name="n" value="0"><button class="secondary">Identify all screens</button></form>'
        '<form class="inline" method="post" action="/restart">%s'
        '<button class="secondary">Restart video</button></form>'
        '<form class="inline" method="post" action="/reset" data-confirm="Reset all PiNDI settings (outputs, '
        'outgoing streams, device and NDI settings) to the defaults? Network settings and passwords are kept.">'
        '%s<button class="danger">Reset settings</button></form>'
        '<form class="inline" method="post" action="/reboot" data-confirm="Reboot the device now?">%s'
        '<button class="danger">Reboot</button></form></div></section>' % (hidden, hidden, hidden, hidden, hidden))

    return page("PiNDI", "".join(parts), session["user"], csrf)


# ---------------------------------------------------------------- actions

ROTATIONS = [("0", "0&deg; (normal)"), ("180", "180&deg; (upside down)")]
MIRRORS = [("none", "None"), ("horizontal", "Horizontal (left/right)"), ("vertical", "Vertical (top/bottom)")]
BANDWIDTHS = [("highest", "Full quality"), ("lowest", "Low-bandwidth preview")]
LEGACY_OUT1 = ("STREAM", "SOURCE_IP", "ROTATE", "MIRROR", "MODE")


def audio_choices():
    choices = [("hdmi", "HDMI (this output)")]
    if "Raspberry Pi 5" not in MODEL and "Raspberry Pi 500" not in MODEL:
        choices.append(("analog", "Headphone jack"))
    return choices + [("off", "Off")]


def mode_choices(o):
    """[(value, label)] for an output's display mode: automatic plus what the monitor offers."""
    choices = [("", "Automatic (monitor's preferred, up to 1080p)")]
    for m in o["modes"]:
        w, h, r = map(int, re.split(r"[x@]", m))
        note = " (monitor's preferred)" if m == o["preferred_mode"] else ""
        if w * h > 1920 * 1200 and "Raspberry Pi 3" in MODEL:
            note += " (too big for a Pi 3)"
        choices.append((m, "%d&times;%d @ %d Hz%s" % (w, h, r, note)))
    if o["mode_setting"] and o["mode_setting"] not in o["modes"]:
        choices.append((o["mode_setting"], esc(o["mode_setting"]) + " (not offered by this monitor)"))
    return choices


def valid_name(s):
    return len(s) <= 200 and not any(ord(c) < 32 for c in s)


def act_output(form):
    try:
        n = int(form.get("n", ""))
    except ValueError:
        return ("err", "Unknown output.")
    if n not in [o["n"] for o in outputs()]:
        return ("err", "Unknown output.")
    keys = ("ENABLED", "STREAM", "SOURCE_IP", "ROTATE", "MIRROR", "BANDWIDTH", "MODE", "AUDIO", "AUDIO_DELAY_MS",
            "BACKUP_STREAM")
    updates = {"PINDI_OUT%d_%s" % (n, k): "" for k in keys}
    if n == 1:  # settings from before multi-output support
        updates.update({"PINDI_" + k: "" for k in LEGACY_OUT1})
    if not form.get("reset"):
        enabled = form.get("enabled") == "1"
        stream = form.get("stream", "").strip()
        source_ip = form.get("source_ip", "").strip()
        rotate, mirror = form.get("rotate", "0"), form.get("mirror", "none")
        bandwidth = form.get("bandwidth", "highest")
        audio = form.get("audio", "hdmi")
        if audio not in dict(audio_choices()):
            return ("err", "Invalid audio output.")
        try:
            audio_delay = int(form.get("audio_delay") or 80)
        except ValueError:
            return ("err", "The audio delay must be a number of milliseconds.")
        if not 20 <= audio_delay <= 1000:
            return ("err", "The audio delay must be between 20 and 1000 ms.")
        mode = form.get("display_mode", "")
        if mode and not re.match(r"^\d{3,4}x\d{3,4}@\d{2,3}$", mode):
            return ("err", "Invalid display mode.")
        backup = form.get("backup", "").strip()
        if not valid_name(stream) or not valid_name(backup):
            return ("err", "Invalid stream name.")
        if source_ip:
            try:
                source_ip = str(ipaddress.IPv4Address(source_ip))
            except ValueError:
                return ("err", "Source IP must be an IPv4 address, or empty for any host.")
        if rotate not in dict(ROTATIONS) or mirror not in dict(MIRRORS) or bandwidth not in dict(BANDWIDTHS):
            return ("err", "Invalid display setting.")
        # Explicit per output, so the shared PINDI_BANDWIDTH default of other outputs is left alone.
        default_bw = (read_env().get("PINDI_BANDWIDTH") or "highest").lower()
        updates.update({
            "PINDI_OUT%d_ENABLED" % n: "" if enabled else "0",
            "PINDI_OUT%d_STREAM" % n: "" if stream == default_out_stream(n) else stream,
            "PINDI_OUT%d_SOURCE_IP" % n: source_ip,
            "PINDI_OUT%d_ROTATE" % n: "" if rotate == "0" else rotate,
            "PINDI_OUT%d_MIRROR" % n: "" if mirror == "none" else mirror,
            "PINDI_OUT%d_BANDWIDTH" % n: "" if bandwidth == default_bw else bandwidth,
            "PINDI_OUT%d_MODE" % n: mode,
            "PINDI_OUT%d_BACKUP_STREAM" % n: backup,
            "PINDI_OUT%d_AUDIO" % n: "" if audio == "hdmi" else audio,
            "PINDI_OUT%d_AUDIO_DELAY_MS" % n: "" if audio_delay == 80 else str(audio_delay),
        })
    write_env(updates)
    later(0.5, run, "systemctl", "restart", "pindi.service")
    o = next(o for o in outputs() if o["n"] == n)
    if not o["enabled"]:
        log("output %d: disabled" % n)
        return ("ok", "Output %d disabled. The player is restarting." % n)
    log("output %d: stream=%s ip=%s rotate=%s mirror=%s bandwidth=%s" % (
        n, o["stream"] or default_out_stream(n), o["source_ip"] or "any", o["rotate"], o["mirror"], o["bandwidth"]))
    return ("ok", "Output %d saved: %s from %s, rotation %s°, mirror %s, %s. The player is restarting." % (
        n, o["stream"] or default_out_stream(n), o["source_ip"] or "any host", o["rotate"], o["mirror"],
        dict(BANDWIDTHS)[o["bandwidth"]].lower()))


def act_outgoing(form):
    device = form.get("device", "")
    known = {i["id"]: i for i in inputs()}
    if device not in known:
        return ("err", "Unknown capture device.")
    inp = known[device]
    try:
        slot = int(form.get("slot", "0"))
    except ValueError:
        slot = 0
    if inp["slot"]:
        slot = inp["slot"]
    elif not 1 <= slot <= 8 or slot in configured_inputs(read_env()):
        return ("err", "No free slot for another outgoing stream.")
    keys = ["PINDI_IN%d_%s" % (slot, k) for k in ("ENABLED", "DEVICE", "NAME", "MODE", "AUDIO")]
    if form.get("remove"):
        write_env({k: "" for k in keys})
        msg = "Forgot %s." % inp["label"]
    else:
        name = form.get("name", "").strip()
        mode = form.get("mode", "")
        if not valid_name(name) or len(name) > 100:
            return ("err", "Invalid NDI stream name.")
        if mode and mode not in dict(inp["modes"]) and mode != inp["mode"]:
            return ("err", "That capture mode is not offered by the device.")
        enabled = form.get("enabled") == "1"
        audio = form.get("audio", "auto")
        if audio not in ("auto", "off"):
            return ("err", "Invalid audio setting.")
        write_env(dict(zip(keys, ["1" if enabled else "0", device,
                                  "" if name == default_in_stream(slot) else name, mode,
                                  "" if audio == "auto" else audio])))
        msg = ("%s will be sent as \"%s\"." % (inp["label"], name or default_in_stream(slot))) if enabled \
            else ("%s is not sent." % inp["label"])
    log("outgoing: slot %d %s" % (slot, msg))
    later(0.5, run, "systemctl", "restart", "pindi-sender.service")
    return ("ok", msg)


def act_network(form):
    ntp = [s for s in re.split(r"[\s,;]+", form.get("ntp", "").strip()) if s]
    for s in ntp:
        if not re.match(r"^[A-Za-z0-9]([A-Za-z0-9.-]{0,252})$", s):
            return ("err", "NTP server %s is not a valid host name or IP address." % s)
    if form.get("mode") == "dhcp":
        text = render_network_config(True, ntp=ntp)
        target = "DHCP"
    else:
        try:
            iface = ipaddress.IPv4Interface("%s/%s" % (form.get("address", "").strip(),
                                                       form.get("netmask", "").strip() or "24"))
        except ValueError:
            return ("err", "Enter a valid IP address and subnet mask (e.g. 255.255.255.0 or 24).")
        if iface.network.prefixlen < 31 and iface.ip in (iface.network.network_address,
                                                         iface.network.broadcast_address):
            return ("err", "%s is not a usable host address in that subnet." % iface.ip)
        gateway = form.get("gateway", "").strip()
        if gateway:
            try:
                gw = ipaddress.IPv4Address(gateway)
            except ValueError:
                return ("err", "Gateway must be an IPv4 address.")
            if gw not in iface.network:
                return ("err", "Gateway %s is not inside %s." % (gw, iface.network))
            gateway = str(gw)
        dns = []
        for d in re.split(r"[\s,;]+", form.get("dns", "").strip()):
            if not d:
                continue
            try:
                dns.append(str(ipaddress.IPv4Address(d)))
            except ValueError:
                return ("err", "DNS server %s is not an IPv4 address." % d)
        text = render_network_config(False, iface, gateway, dns, ntp)
        target = str(iface.ip)
    if text == read_text(NETWORK_FILE) and not NET.pending():
        return ("ok", "Network settings unchanged.")
    NET.apply(text, target)
    where = ("http://%s/" % target) if target != "DHCP" else "the address shown on the splash screen"
    return ("warn", "Applying new network settings. Open %s within %d seconds, sign in and press Keep." %
            (where, ROLLBACK_SECONDS))


def act_password(session, form):
    current, new, confirm = form.get("current", ""), form.get("new", ""), form.get("confirm", "")
    if not check_password(session["user"], current):
        time.sleep(1)
        return ("err", "Current password is wrong.")
    if new != confirm:
        return ("err", "The new passwords do not match.")
    if not new:
        return ("err", "The new password cannot be empty.")
    if any(c in new for c in ":\r\n\0"):
        return ("err", "The password cannot contain ':' or line breaks.")
    r = run("chpasswd", input="%s:%s\n" % (session["user"], new))
    if r.returncode:
        return ("err", "Could not change the password: %s" % r.stderr.strip())
    log("password changed for %s" % session["user"])
    return ("ok", "Password changed. Use the new password for this page and SSH.")


def act_identify(form):
    """Big "Output n / device name / IP" on that screen (0 = all) for 15 s."""
    try:
        n = int(form.get("n", "0"))
    except ValueError:
        n = 0
    outs = {o["n"]: o for o in outputs()}
    if n and n not in outs:
        return ("err", "Unknown output.")
    lines = ["Output %d" % n if n else device_name(), device_name() if n else "",
             "IP %s" % (live_network()["address"] or "-"),
             "%s · serial %s" % (outs[n]["connector"], SERIAL) if n else "Serial %s" % SERIAL]
    text = "\n".join(l for l in lines if l)
    write_atomic(RUN_DIR + "/identify", "%d\n%s\n" % (n, text))
    return ("ok", "%s shows its name for 15 seconds." % ("Output %d" % n if n else "Every output"))


def restart_media():
    run("systemctl", "restart", "pindi.service", "pindi-sender.service")


def hostname_for(name):
    """A valid host name from a free-form device name ("Lobby left" -> "Lobby-left")."""
    h = re.sub(r"[^A-Za-z0-9-]+", "-", name).strip("-")[:63].strip("-")
    return h or "PiNDI"


def act_device(form):
    name = form.get("name", "").strip()
    if not name or len(name) > 63 or not valid_name(name) or any(c in name for c in "()\\\""):
        return ("err", "Enter a device name of up to 63 characters, without brackets or quotes.")
    lists = {}
    for key in ("discovery", "recv_groups", "send_groups"):
        items = [s for s in re.split(r"[\s,;]+", form.get(key, "").strip()) if s]
        for s in items:
            if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,252}$", s):
                return ("err", "%s is not a valid %s." % (s, "server address" if key == "discovery" else "group name"))
        lists[key] = ",".join(items)
    host = hostname_for(name)
    try:
        renamed = set_hostname(host)
    except RuntimeError as e:
        return ("err", "Could not set the host name: %s" % e)
    # A name that is a valid host name is used as is (NDI shows it in capitals, as usual); anything
    # else ("Lobby left") also becomes the NDI machine name, so sources show it exactly as typed.
    write_ndi_settings(lists["discovery"], lists["send_groups"], lists["recv_groups"],
                       "" if name == host else name)
    write_env({"PINDI_SPLASH_INFO": "" if form.get("splash_info") == "1" else "0",
               "PINDI_FRAME_TIMEOUT_MS": "10000" if form.get("frame_timeout") == "1" else ""})
    log("device settings: name=%s host=%s discovery=%s recv=%s send=%s" % (
        name, host, lists["discovery"] or "-", lists["recv_groups"] or "-", lists["send_groups"] or "-"))

    def apply():
        if renamed:
            run("systemctl", "restart", "avahi-daemon.service")
        restart_media()
    later(0.5, apply)
    return ("ok", "Device settings saved. Video and outgoing streams are restarting.")


BACKUP_FORMAT = "pindi-backup-1"


def backup_json():
    return json.dumps({"format": BACKUP_FORMAT, "version": version(), "serial": SERIAL,
                       "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "device_name": device_name(),
                       "settings": {k: v for k, v in read_env().items() if k.startswith("PINDI_")},
                       "ndi": read_ndi_config(), "network": read_text(NETWORK_FILE)}, indent=2) + "\n"


def act_restore(form):
    try:
        data = json.loads(form.get("backup", ""))
        assert isinstance(data, dict) and data.get("format") == BACKUP_FORMAT
        settings, ndi, network = data["settings"], data.get("ndi", {}), data.get("network", "")
        assert isinstance(settings, dict) and isinstance(ndi, dict) and isinstance(network, str)
    except (ValueError, KeyError, AssertionError):
        return ("err", "That is not a PiNDI settings backup.")
    for k, v in settings.items():
        if not re.match(r"^PINDI_[A-Z0-9_]+$", str(k)) or not isinstance(v, str) or not valid_name(v):
            return ("err", "The backup contains an invalid setting (%s)." % k)
    if network and not network.startswith("# Written by the PiNDI web interface."):
        return ("err", "The backup contains network settings that were not written by PiNDI.")
    # Replace every PINDI_ setting with the backup's.
    current = {k: "" for k in read_env() if k.startswith("PINDI_")}
    current.update(settings)
    write_env(current)
    os.makedirs(os.path.dirname(NDI_CONFIG), exist_ok=True)
    write_atomic(NDI_CONFIG, json.dumps(ndi, indent=2) + "\n")
    name = str(data.get("device_name", "")).strip()
    if name and valid_name(name):
        try:
            if set_hostname(hostname_for(name)):
                later(0.3, run, "systemctl", "restart", "avahi-daemon.service")
        except RuntimeError:
            pass
    later(0.5, restart_media)
    log("settings restored from backup of %s (%s)" % (data.get("serial", "?"), data.get("created", "?")))
    if network and network != read_text(NETWORK_FILE):
        NET.apply(network, "restored settings")
        return ("warn", "Settings restored. The network settings changed as well: open the device at its "
                        "(new) address within %d seconds and press Keep." % ROLLBACK_SECONDS)
    return ("ok", "Settings restored. Video and outgoing streams are restarting.")


def act_reset():
    defaults = read_text(ENV_DEFAULTS)
    if not defaults:
        return ("err", "The default settings file %s is missing - run install.sh again." % ENV_DEFAULTS)
    write_atomic(ENV_FILE, defaults)
    try:
        os.remove(NDI_CONFIG)
    except OSError:
        pass
    log("settings reset to defaults")
    later(0.5, restart_media)
    return ("ok", "All PiNDI settings are back to the defaults (network settings and passwords kept).")


def journal(lines, units=("pindi", "pindi-sender", "pindi-web")):
    cmd = ["journalctl", "--no-pager", "-o", "short-iso", "-n", str(lines)]
    for u in units:
        cmd += ["-u", u]
    return run(*cmd).stdout


def diagnostics_text():
    out = ["PiNDI diagnostics", "=" * 60]
    hl, hw = health()
    out += ["Created: %s" % time.strftime("%Y-%m-%d %H:%M:%S %z"), "Version: %s" % version(),
            "Model: %s" % MODEL, "Serial: %s" % SERIAL, "Device name: %s" % device_name(),
            "Kernel: %s" % " ".join(os.uname()), "NDI: %s" % (read_text(RUN_DIR + "/ndi").replace("\n", " ")),
            "Health: %s" % hl, "Power/cooling: %s" % hw, "Throttled: 0x%x" % throttled(),
            "Network traffic: %s" % TRAFFIC.text(), ""]
    for title, path in (("Settings (%s)" % ENV_FILE, ENV_FILE), ("NDI config", NDI_CONFIG),
                        ("Network", NETWORK_FILE), ("Outputs", RUN_DIR + "/outputs"),
                        ("Visible NDI sources", RUN_DIR + "/sources")):
        out += ["---- " + title, read_text(path).rstrip() or "(empty)", ""]
    try:
        for name in sorted(os.listdir(RUN_DIR)):
            if name.endswith((".status", ".latency", ".modes")):
                out += ["---- " + name, read_text(os.path.join(RUN_DIR, name)).rstrip(), ""]
    except OSError:
        pass
    out += ["---- ip addr", run("ip", "addr").stdout, "---- services",
            run("systemctl", "status", "--no-pager", "-n", "0", "pindi", "pindi-sender", "pindi-web").stdout,
            "---- log (last 2000 lines)", journal(2000), "---- kernel (last 200 lines)",
            run("journalctl", "-k", "-b", "--no-pager", "-n", "200", "-o", "short-iso").stdout]
    return "\n".join(out)


def logs_page(session):
    body = ('<section class="card"><nav class="top"><a href="/">&larr; Back</a><a href="/diagnostics">Download '
            'diagnostics</a></nav><h2>Log (last 400 lines)</h2><pre class="log">%s</pre></section>' %
            esc(journal(400)))
    return page("PiNDI - Log", body, session["user"], session["csrf"])


# ---------------------------------------------------------------- HTTP

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "PiNDI"
    sys_version = ""

    def log_message(self, fmt, *args):
        pass

    # --- plumbing
    def cookie_token(self):
        c = http.cookies.SimpleCookie(self.headers.get("Cookie", ""))
        return c["pindi_session"].value if "pindi_session" in c else None

    def session(self):
        return SESSIONS.get(self.cookie_token())

    def send(self, code, body, ctype="text/html; charset=utf-8", headers=()):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def redirect(self, where, headers=()):
        self.send_response(303)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()

    def form(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 65536:
            return {}
        raw = self.rfile.read(length).decode("utf-8", "replace")
        return {k: v[0] for k, v in urllib.parse.parse_qs(raw, keep_blank_values=True).items()}

    # --- routes
    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/style.css":
            return self.send(200, CSS, "text/css")
        if path == "/app.js":
            return self.send(200, JS, "application/javascript")
        if path == "/favicon.ico":
            return self.send(404, "")
        s = self.session()
        if path == "/login":
            return self.redirect("/") if s else self.send(200, login_page())
        if not s:
            return self.redirect("/login") if path == "/" else self.send(401, "Unauthorized", "text/plain")
        if path == "/":
            return self.send(200, dashboard(s))
        if path == "/logs":
            return self.send(200, logs_page(s))
        m = re.match(r"^/thumb/(\d)\.jpg$", path)
        if m:
            # Asking for it keeps the player rendering it (for 15 s); the first request may find none yet.
            n = int(m.group(1))
            req = "%s/out%d.thumbreq" % (RUN_DIR, n)
            try:
                with open(req, "a"):
                    os.utime(req)
                with open("%s/out%d.jpg" % (RUN_DIR, n), "rb") as f:
                    return self.send(200, f.read(), "image/jpeg")
            except OSError:
                return self.send(404, "", "text/plain")
        stamp = time.strftime("%Y%m%d-%H%M")
        if path == "/diagnostics":
            return self.send(200, diagnostics_text(), "text/plain; charset=utf-8", [
                ("Content-Disposition", 'attachment; filename="pindi-%s-diagnostics-%s.txt"' % (SERIAL, stamp))])
        if path == "/backup":
            return self.send(200, backup_json(), "application/json", [
                ("Content-Disposition", 'attachment; filename="pindi-%s-settings-%s.json"' % (SERIAL, stamp))])
        if path == "/api/status":
            st = {"ip": live_network()["address"], "rollback": NET.seconds_left() if NET.pending() else None,
                  "net": TRAFFIC.text()}
            st["health"], st["power"] = health()
            for o in outputs():
                for k in ("state", "target", "source", "video", "format", "audio", "latency"):
                    st["out%d.%s" % (o["n"], k)] = o[k]
            # Status files only: cheap enough to poll (no device enumeration).
            for n, conf in configured_inputs(read_env()).items():
                if conf.get("ENABLED") == "1":
                    st["in%d.state" % n] = read_kv("%s/in%d.status" % (RUN_DIR, n)).get("state", "starting")
            return self.send(200, json.dumps(st), "application/json")
        self.send(404, "Not found", "text/plain")

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        form = self.form()
        if path == "/login":
            user = form.get("user", "").strip()
            password = form.get("password", "")
            ip = self.client_address[0]
            wait = LOCKOUT.blocked(ip)
            if wait:
                log("login refused: %s is locked out for %d s" % (ip, wait))
                return self.send(429, login_page("Too many failed sign-ins from this address. Try again in %d "
                                                 "minute%s." % ((wait + 59) // 60, "" if wait <= 60 else "s")))
            if user and is_admin(user) and check_password(user, password):
                LOCKOUT.success(ip)
                token = SESSIONS.create(user)
                if password in WELL_KNOWN_PASSWORDS:
                    SESSIONS.get(token)["flash"] = ("warn", "You signed in with a well-known default password. "
                                                    "Please change it below (it is also the SSH password).")
                log("login: %s from %s" % (user, ip))
                return self.redirect("/", [("Set-Cookie", "pindi_session=%s; Path=/; HttpOnly; SameSite=Strict"
                                            % token)])
            LOCKOUT.failure(ip)
            log("login failed: %r from %s" % (user, ip))
            time.sleep(2)
            return self.send(401, login_page("Wrong user or password, or the user is not an administrator."))

        s = self.session()
        if not s or not secrets.compare_digest(form.get("csrf", ""), s["csrf"]):
            return self.redirect("/login")

        if path == "/logout":
            SESSIONS.drop(self.cookie_token())
            return self.redirect("/login", [("Set-Cookie", "pindi_session=; Path=/; Max-Age=0")])
        if path == "/output":
            s["flash"] = act_output(form)
        elif path == "/outgoing":
            s["flash"] = act_outgoing(form)
        elif path == "/network":
            s["flash"] = act_network(form)
        elif path == "/network/confirm":
            NET.confirm()
            s["flash"] = ("ok", "Network settings kept.")
        elif path == "/network/revert":
            NET.revert()
            s["flash"] = ("ok", "Previous network settings restored.")
        elif path == "/password":
            s["flash"] = act_password(s, form)
        elif path == "/device":
            s["flash"] = act_device(form)
        elif path == "/restore":
            s["flash"] = act_restore(form)
        elif path == "/reset":
            s["flash"] = act_reset()
        elif path == "/identify":
            s["flash"] = act_identify(form)
        elif path == "/restart":
            log("video restart requested by %s" % s["user"])
            later(0.5, restart_media)
            s["flash"] = ("ok", "Video and outgoing streams are restarting.")
        elif path == "/reboot":
            log("reboot requested by %s" % s["user"])
            later(1, run, "systemctl", "reboot")
            return self.send(200, page("PiNDI", '<div class="card"><h2>Rebooting&hellip;</h2>'
                                                 '<p>The device will be back in about 20 seconds.</p></div>'))
        else:
            return self.send(404, "Not found", "text/plain")
        self.redirect("/")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    TRAFFIC.start()
    srv = Server(("", PORT), Handler)
    log("PiNDI web interface on port %d" % PORT)
    srv.serve_forever()


if __name__ == "__main__":
    main()

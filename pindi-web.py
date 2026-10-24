#!/usr/bin/python3
"""PiNDI web administration.

Two processes from this one file:
  - the front end (user pindi-web, ports 443/80): TLS, HTTP, pages, uploads. It reads the device's state
    but can't change it, and holds no secrets beyond the TLS key.
  - the broker (--broker, root, no network): sign-in (PAM), sessions and CSRF, and every change to the
    device, asked for over a local socket. A flaw in the exposed front end is then not a root compromise.

Small standard-library HTTPS server (port 443, self-signed or uploaded certificate; port 80 redirects pages):
  - login with the Pi's own user accounts (PAM, members of the sudo group)
  - per display output (detected by the player): enable/disable, NDI stream
    (default OUT_<serial>[_n]), source IP (default any), rotation, mirror, status
  - "Configure Outgoing Streams": detected USB capture devices, off until enabled
  - network: DHCP or static IP, with automatic rollback unless confirmed
  - Wi-Fi: off by default (hardware switched off in config.txt for a faster boot); on: one network, DHCP
  - change the login password, reboot
"""
import base64
import ctypes
import ctypes.util
import grp
import hashlib
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
import shutil
import ssl
import subprocess
import socketserver
import struct
import sys
import tarfile
import tempfile
import threading
import traceback
import time
import urllib.parse

PORT = int(os.environ.get("PINDI_WEB_PORT", "443"))  # HTTPS
HTTP_PORT = int(os.environ.get("PINDI_WEB_HTTP_PORT", "80"))  # only redirects to HTTPS
TLS_DIR = "/etc/pindi/tls"
MAX_CONNECTIONS = 64  # at once; more are closed straight away
MAX_PER_ADDRESS = 8  # from one address: one misbehaving client can't lock everyone else out
REQUEST_TIMEOUT = 20  # seconds a connection may sit idle (TLS handshake, headers, body)
BODY_MAX = 65536
ENV_FILE = "/etc/default/pindi"
ENV_DEFAULTS = "/usr/share/pindi/pindi.conf"
VERSION_FILE = "/usr/share/pindi/version"
NDI_CONFIG = "/etc/pindi/ndi-config.v1.json"  # NDI_CONFIG_DIR=/etc/pindi in the player/sender units
NETWORK_FILE = "/etc/systemd/network/20-pindi-wired.network"
RUN_DIR = "/run/pindi"  # written by the player and sender (pindi-media): read here, never written by root
CTL_DIR = "/run/pindi-ctl"  # root-only (the broker): live switches and Identify for the player
WEB_RUN = "/run/pindi-web"  # the web front end's own: preview requests for the player
BROKER_DIR = "/run/pindi-webd"
BROKER_SOCK = BROKER_DIR + "/broker.sock"
WEB_USER = "pindi-web"  # the network-facing front end runs as this user
ADMIN_GROUP = "sudo"
MEDIA_GROUP = "pindi-media"  # pindi-player and pindi-sender run as this user
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


def write_atomic(path, text, mode=0o644, group=None):
    """Replace path with text, safely even if the power is cut right after (synced to the SD card).
    mode None: leave it to the file system (the FAT boot partition has no permissions).
    The temporary file gets a fresh random name and is created exclusively (O_EXCL), so a link planted
    under a predictable name can't redirect the write to another file."""
    fd, tmp = tempfile.mkstemp(prefix="." + os.path.basename(path) + ".", dir=os.path.dirname(path) or ".")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        if mode is not None:
            os.chmod(tmp, mode)
        if group:
            try:
                os.chown(tmp, 0, grp.getgrnam(group).gr_gid)
            except KeyError:
                pass  # not installed yet
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    try:
        d = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
        try:
            os.fsync(d)
        finally:
            os.close(d)
    except OSError:
        pass


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


def wifi_interfaces():
    return sorted(n for n in os.listdir("/sys/class/net") if os.path.exists("/sys/class/net/%s/wireless" % n))


def live_network(wifi=True):
    """What the device is actually using right now: the wired address, else (wifi) the Wi-Fi one."""
    info = {"iface": "", "address": "", "prefix": "", "gateway": "", "dns": []}
    found = {}
    for line in run("ip", "-4", "-o", "addr", "show", "scope", "global").stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4:
            found.setdefault(parts[1], parts[3])
    for name in ether_interfaces() + (wifi_interfaces() if wifi else []):
        if name in found:
            info["iface"] = name
            info["address"], _, info["prefix"] = found[name].partition("/")
            break
    m = re.search(r"default via (\S+)", run("ip", "-4", "route", "show", "default", "dev", info["iface"]).stdout
                  if info["iface"] else "")
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


# ---------------------------------------------------------------- Wi-Fi (off by default: faster boot)

WIFI_IFACE = "wlan0"
WIFI_CONF = "/etc/wpa_supplicant/wpa_supplicant-%s.conf" % WIFI_IFACE
WIFI_NETWORK_FILE = "/etc/systemd/network/25-pindi-wifi.network"
WIFI_UNIT = "wpa_supplicant@%s.service" % WIFI_IFACE
# wpa_supplicant starts when the interface appears, so a Pi without Wi-Fi never waits for it at boot.
# install.sh checks this link too, to keep the Wi-Fi hardware switched on in config.txt.
WIFI_WANTS = "/etc/systemd/system/sys-subsystem-net-devices-%s.device.wants/%s" % (WIFI_IFACE, WIFI_UNIT)
WIFI_DROPIN = "/etc/systemd/system/%s.d/pindi.conf" % WIFI_UNIT
CONFIG_TXT = "/boot/firmware/config.txt" if os.path.exists("/boot/firmware/config.txt") else "/boot/config.txt"
_scan = {"t": 0, "ssids": []}


def wifi_enabled():
    return os.path.islink(WIFI_WANTS)


def wifi_present():
    return os.path.exists("/sys/class/net/" + WIFI_IFACE)


def countries():
    """[(code, name)] from tzdata."""
    out = []
    for line in read_text("/usr/share/zoneinfo/iso3166.tab").splitlines():
        code, _, name = line.partition("\t")
        if len(code) == 2 and not line.startswith("#"):
            out.append((code, name))
    return sorted(out, key=lambda c: c[1])


def timezone_country():
    """Country of the configured time zone: a good guess for the Wi-Fi regulatory domain."""
    tz = current_timezone()
    for line in read_text("/usr/share/zoneinfo/zone.tab").splitlines():
        parts = line.split("\t")
        if len(parts) >= 3 and parts[2] == tz:
            return parts[0]
    return ""


def read_wifi_config():
    cfg = {"ssid": "", "psk": "", "country": ""}
    for line in read_text(WIFI_CONF).splitlines():
        key, _, val = line.strip().partition("=")
        if key == "country":
            cfg["country"] = val
        elif key == "ssid":
            try:
                cfg["ssid"] = bytes.fromhex(val).decode("utf-8", "replace")
            except ValueError:
                cfg["ssid"] = val.strip('"')
        elif key == "psk":
            cfg["psk"] = val
    return cfg


def wifi_psk(ssid, passphrase):
    """The 256-bit WPA key (what wpa_passphrase computes), so the passphrase itself is not stored."""
    if re.match(r"^[0-9a-fA-F]{64}$", passphrase):
        return passphrase.lower()
    return hashlib.pbkdf2_hmac("sha1", passphrase.encode(), ssid.encode(), 4096, 32).hex()


def render_wifi_config(ssid, psk, country):
    text = ("# Written by the PiNDI web interface.\nctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev\n"
            "update_config=0\ncountry=%s\n\nnetwork={\n\tssid=%s\n\tscan_ssid=1\n" % (country, ssid.encode().hex()))
    if psk:
        return text + "\tkey_mgmt=WPA-PSK\n\tpsk=%s\n}\n" % psk
    return text + "\tkey_mgmt=NONE\n}\n"


def set_wifi_overlay(on):
    """Comment the disable-wifi overlays in config.txt in or out; takes effect at the next boot."""
    text = read_text(CONFIG_TXT)
    new = re.sub(r"^#?(dtoverlay=disable-wifi(-pi5)?)$", ("#" if on else "") + r"\1", text, flags=re.M)
    if new != text:
        write_atomic(CONFIG_TXT, new, None)
    return new != text


def iw_text(s):
    """iw prints the non-ASCII bytes of a network name as \\xNN."""
    return re.sub(rb"\\x([0-9a-fA-F]{2})", lambda m: bytes([int(m.group(1), 16)]), s.encode()).decode(
        "utf-8", "replace")


def wifi_status():
    """{"ssid", "signal", "address"} of the live connection, or None when not connected."""
    if not wifi_present():
        return None
    link = run("iw", "dev", WIFI_IFACE, "link").stdout if shutil.which("iw") else ""
    ssid = re.search(r"^\s*SSID: (.*)$", link, re.M)
    if not ssid:
        return None
    signal = re.search(r"signal: (-?\d+) dBm", link)
    m = re.search(r"inet (\S+)/", run("ip", "-4", "-o", "addr", "show", "dev", WIFI_IFACE).stdout)
    return {"ssid": iw_text(ssid.group(1)), "signal": int(signal.group(1)) if signal else None,
            "address": m.group(1) if m else ""}


def wifi_needs_reboot():
    """Switched on since the last boot, and the hardware is still off."""
    if not wifi_enabled() or wifi_present():
        return False
    try:
        boot = time.time() - float(read_text("/proc/uptime").split()[0])
        return os.lstat(WIFI_WANTS).st_mtime >= boot
    except (OSError, ValueError, IndexError):
        return True


def wifi_state_text():
    if not wifi_enabled():
        return "Off"
    if wifi_needs_reboot():
        return '<span class="warn">Switches on at the next reboot</span>'
    if not wifi_present():
        return '<span class="warn">No Wi-Fi hardware found on this device</span>'
    st = wifi_status()
    if not st:
        return '<span class="warn">Not connected</span>'
    text = "Connected to %s" % esc(st["ssid"])
    if st["signal"] is not None:
        quality = "good" if st["signal"] >= -60 else "fair" if st["signal"] >= -70 else "weak"
        text += " &middot; signal %d dBm (%s)" % (st["signal"], quality)
    return text + " &middot; IP %s" % esc(st["address"] or "waiting for DHCP")


def wifi_scan():
    """Network names nearby, from the last scan (only while the Wi-Fi hardware is on). A scan takes a few
    seconds, so it runs in the background and the page shows the previous one."""
    if wifi_present() and shutil.which("iw") and time.time() - _scan["t"] > 60:
        _scan["t"] = time.time()
        threading.Thread(target=_wifi_scan, daemon=True).start()
    return _scan["ssids"]


def _wifi_scan():
    run("ip", "link", "set", WIFI_IFACE, "up")
    out = run("iw", "dev", WIFI_IFACE, "scan").stdout or run("iw", "dev", WIFI_IFACE, "scan", "dump").stdout
    found = {}
    for block in re.split(r"(?m)^BSS ", out)[1:]:
        ssid = re.search(r"^\s*SSID: (.+)$", block, re.M)
        signal = re.search(r"signal: (-?[\d.]+) dBm", block)
        if ssid and "\\x00" not in ssid.group(1):  # hidden networks
            name, level = iw_text(ssid.group(1)), float(signal.group(1)) if signal else -100
            found[name] = max(level, found.get(name, -100))
    _scan["ssids"] = sorted(found, key=lambda s: -found[s])


def apply_wifi(on):
    if on:
        run("rfkill", "unblock", "wifi")
        run("systemctl", "daemon-reload")
        run("systemctl", "restart", WIFI_UNIT)
        run("networkctl", "reload")
        run("networkctl", "reconfigure", WIFI_IFACE)
    else:
        run("systemctl", "stop", WIFI_UNIT)
        run("ip", "link", "set", WIFI_IFACE, "down")


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
    """Received/sent bit rate of the wired and Wi-Fi interfaces, sampled in the background."""

    def __init__(self, interval=2.0):
        self.interval = interval
        self.rx = self.tx = None

    @staticmethod
    def _counters():
        rx = tx = 0
        for name in ether_interfaces() + wifi_interfaces():
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
            # NDI arrives in bursts well above its average: from about 70 % the audio, which waits behind the
            # video in the same connection, starts to arrive late and cut out (seen on a Pi 3's 100 Mbit/s port).
            if max(self.rx, self.tx) > 0.7 * link * 1e6:
                text += " – close to the link limit: expect audio dropouts and dropped frames (send a smaller stream)"
        elif any(read_text("/sys/class/net/%s/operstate" % n).strip() == "up" for n in wifi_interfaces()):
            text += " (over Wi-Fi)"
        return text


def mbit(bps):
    return "%.1f Mbit/s" % (bps / 1e6) if bps >= 1e5 else "%.0f kbit/s" % (bps / 1e3)


TRAFFIC = NetTraffic()


def hx_send_available():
    """Sending NDI|HX needs the NDI Advanced SDK and the Pi's hardware H.264 encoder (Pi 3/4)."""
    if not os.path.exists("/usr/local/lib/libndi_advanced.so"):
        return False
    base = "/sys/class/video4linux"
    try:
        return any(read_text(os.path.join(base, d, "name")).strip() == "bcm2835-codec-encode"
                   for d in os.listdir(base))
    except OSError:
        return False


CODECS = [("speedhq", "Full NDI (SpeedHQ)"), ("hx", "NDI|HX (H.264, hardware encoder)")]
AUDIO_SYNCS = [("on", "In step with this Pi's clock (recommended)"), ("off", "As the device delivers it")]
RANGES = [("full", "Normal (most capture cards and webcams)"), ("video", "Already video range (16-235)")]


def ndi_info():
    """What the player found out about the installed NDI SDK (/run/pindi/ndi)."""
    info = read_kv(RUN_DIR + "/ndi")
    version = info.get("version", "").split()  # "NDI SDK LINUX 13:10:53 Apr 13 2026 6.3.2.0"
    return {"version": version[-1] if version else "", "hx_receive": info.get("hx_receive") == "1"}


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

def write_ndi_config(cfg):
    """The NDI config holds the vendor licence ID: readable by the media services' user, not by everyone."""
    os.makedirs(os.path.dirname(NDI_CONFIG), exist_ok=True)
    write_atomic(NDI_CONFIG, json.dumps(cfg, indent=2) + "\n", 0o640, MEDIA_GROUP)


def read_ndi_config():
    try:
        cfg = json.loads(read_text(NDI_CONFIG) or "{}")
        return cfg if isinstance(cfg, dict) else {}
    except ValueError:
        return {}


TRANSPORTS = [("tcp", "TCP (recommended)"), ("udp", "UDP (the NDI default)")]


def ndi_transport(ndi):
    """How this device receives NDI: "tcp" when reliable UDP is switched off in favour of TCP."""
    rudp_off = ndi.get("rudp", {}).get("recv", {}).get("enable") is False
    return "tcp" if rudp_off and ndi.get("tcp", {}).get("recv", {}).get("enable") else "udp"


def set_ndi_transport(ndi, transport):
    # NDI's reliable UDP stalls every receiver for a second or more when packets are lost, then resets
    # the connection; TCP rides it out. Both choices are written out, so install.sh can tell a device
    # that never chose (and gets TCP) from one that chose UDP.
    on = transport == "tcp"
    ndi["rudp"] = {"recv": {"enable": not on}}
    ndi["unicast"] = {"recv": {"enable": not on}}
    ndi["tcp"] = {"recv": {"enable": on}}


def ndi_settings():
    ndi = read_ndi_config().get("ndi", {})
    nets, groups, vendor = ndi.get("networks", {}), ndi.get("groups", {}), ndi.get("vendor", {})
    return {"discovery": nets.get("discovery", "") or "", "send_groups": groups.get("send", "") or "",
            "recv_groups": groups.get("recv", "") or "", "machinename": ndi.get("machinename", "") or "",
            "transport": ndi_transport(ndi), "vendor_name": vendor.get("name", "") or "",
            "vendor_id": vendor.get("id", "") or ""}


def ndi_advanced():
    """The NDI Advanced SDK is installed (needed to send NDI|HX)."""
    return os.path.exists("/usr/local/lib/libndi_advanced.so")


def ndi_evaluation():
    """The Advanced SDK runs as its evaluation build: no vendor licence in the NDI config. It then stops a
    stream's NDI|HX video after 30 minutes (sound and Full NDI go on)."""
    return ndi_advanced() and not ndi_settings()["vendor_id"]


def write_ndi_settings(discovery, send_groups, recv_groups, machinename, transport, vendor_name="", vendor_id=""):
    """Update our keys in ndi-config.v1.json, keeping anything else that is in there."""
    cfg = read_ndi_config()
    ndi = cfg.setdefault("ndi", {})
    set_ndi_transport(ndi, transport)
    if vendor_id:  # an NDI Advanced SDK licence (from licensing@ndi.video)
        ndi["vendor"] = {"name": vendor_name, "id": vendor_id}
    else:
        ndi.pop("vendor", None)
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
    write_ndi_config(cfg)


_tz_cache = []


def timezones():
    if not _tz_cache:
        _tz_cache.extend(run("timedatectl", "list-timezones").stdout.split() or ["UTC"])
    return _tz_cache


def current_timezone():
    return run("timedatectl", "show", "-p", "Timezone", "--value").stdout.strip() or "UTC"


REBOOT_TIMER = "/etc/systemd/system/pindi-reboot.timer"
REBOOT_SERVICE = "/etc/systemd/system/pindi-reboot.service"


def reboot_time():
    """"HH:MM" of the scheduled daily reboot, or ""."""
    m = re.search(r"^OnCalendar=\*-\*-\* (\d\d:\d\d):00$", read_text(REBOOT_TIMER), re.M)
    return m.group(1) if m else ""


def set_reboot_time(hhmm):
    if hhmm == reboot_time():
        return
    if not hhmm:
        run("systemctl", "disable", "--now", "pindi-reboot.timer")
        for f in (REBOOT_TIMER, REBOOT_SERVICE):
            try:
                os.remove(f)
            except OSError:
                pass
        run("systemctl", "daemon-reload")
        log("daily reboot: off")
        return
    write_atomic(REBOOT_SERVICE, "[Unit]\nDescription=PiNDI scheduled daily reboot\n\n[Service]\nType=oneshot\n"
                                 "ExecStart=/usr/bin/systemctl reboot\n")
    write_atomic(REBOOT_TIMER, "[Unit]\nDescription=PiNDI scheduled daily reboot\n\n[Timer]\n"
                               "OnCalendar=*-*-* %s:00\n\n[Install]\nWantedBy=timers.target\n" % hhmm)
    run("systemctl", "daemon-reload")
    run("systemctl", "enable", "--now", "pindi-reboot.timer")
    log("daily reboot: %s" % hhmm)


OS_UPDATE_UNIT = "pindi-os-update"  # the .service is installed by install.sh
OS_UPDATE_TIMER = "/etc/systemd/system/pindi-os-update.timer"
OS_UPDATE_LOG = "/var/log/pindi-os-update.log"


def os_update_time():
    """"HH:MM" of the daily operating-system update, or "" (off, the default: apt never runs on its own)."""
    m = re.search(r"^OnCalendar=\*-\*-\* (\d\d:\d\d):00$", read_text(OS_UPDATE_TIMER), re.M)
    return m.group(1) if m else ""


def set_os_update_time(hhmm):
    if hhmm == os_update_time():
        return
    if not hhmm:
        run("systemctl", "disable", "--now", OS_UPDATE_UNIT + ".timer")
        try:
            os.remove(OS_UPDATE_TIMER)
        except OSError:
            pass
        run("systemctl", "daemon-reload")
        log("os updates: off")
        return
    # Not Persistent: a run missed while switched off must not start in the middle of a show at power-on.
    write_atomic(OS_UPDATE_TIMER, "[Unit]\nDescription=PiNDI daily operating-system updates\n\n[Timer]\n"
                                  "OnCalendar=*-*-* %s:00\n\n[Install]\nWantedBy=timers.target\n" % hhmm)
    run("systemctl", "daemon-reload")
    run("systemctl", "enable", "--now", OS_UPDATE_UNIT + ".timer")
    log("os updates: daily at %s" % hhmm)


def os_update_text():
    """Last operating-system update, for the Device card."""
    if run("systemctl", "is-active", OS_UPDATE_UNIT + ".service").stdout.strip() in ("activating", "active"):
        return "installing now"
    try:
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(OS_UPDATE_LOG)))
        text = read_text(OS_UPDATE_LOG)
        state = "OK" if "PINDI-OS-UPDATE-OK" in text else '<span class="warn">failed (see the log)</span>'
        out = "last run %s, %s" % (when, state)
    except OSError:
        out = "never installed from this device"
    out += " &middot; daily at %s" % os_update_time() if os_update_time() else " &middot; automatic: off"
    if os.path.exists("/run/reboot-required"):
        out += ' &middot; <span class="warn">reboot needed to finish</span>'
    if os_update_overdue():
        days = int((time.time() - last_package_update()) // 86400)
        out += ('<span class="hint block warn">No operating-system updates for %d days: security fixes are '
                'missing. Press <b>Install OS updates now</b> (System) or set a daily time (Device settings).'
                '</span>' % days)
    return out


def act_os_update_now():
    if run("systemctl", "is-active", OS_UPDATE_UNIT + ".service").stdout.strip() in ("activating", "active"):
        return ("err", "Operating-system updates are already being installed.")
    r = run("systemctl", "start", "--no-block", OS_UPDATE_UNIT + ".service")
    if r.returncode:
        return ("err", "Could not start the update: %s" % (r.stderr.strip() or "run install.sh again"))
    log("os updates: started by hand")
    return ("ok", "Installing operating-system updates in the background (needs internet; a few minutes). "
                  "Video keeps running; see the OS update log.")


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


def shown_text(n):
    """Frames per second that actually reached the screen (5 s average, from out<n>.latency)."""
    fps = read_kv("%s/out%d.latency" % (RUN_DIR, n)).get("fps")
    return "%.2f fps on screen" % float(fps) if fps else ""


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


_player = {"t": 0, "ok": False}


def player_running():
    """systemctl is-active pindi, cached for 2 s (the status poll asks for it every 3 s per browser)."""
    if time.time() - _player["t"] > 2:
        _player["ok"] = run("systemctl", "is-active", "pindi.service").stdout.strip() == "active"
        _player["t"] = time.time()
    return _player["ok"]


def outputs():
    """Display outputs as detected by the player, with their settings and live status."""
    env = read_env()
    running = player_running()
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
            # Without the HX decoder the SDK can only have negotiated SpeedHQ with the sender. With it,
            # the player checks the codec once per stream (out<n>.codec).
            codec = read_kv("%s/out%d.codec" % (RUN_DIR, n))
            if not hx:
                fmt = "Full NDI (SpeedHQ)"
            elif codec.get("source") == st.get("source") and codec.get("codec"):
                fmt = codec["codec"]
            else:
                fmt = "Full NDI or NDI|HX (checking...)"
            if bandwidth == "lowest":
                fmt += ", low-bandwidth preview"
        out.append({
            "n": n, "connector": parts[1], "link": parts[2], "mode": parts[3] if len(parts) > 3 else "",
            "enabled": out_setting(env, n, "ENABLED", "1") != "0",
            "stream": out_setting(env, n, "STREAM"), "source_ip": out_setting(env, n, "SOURCE_IP"),
            "backup": out_setting(env, n, "BACKUP_STREAM"),
            "standby": out_setting(env, n, "STANDBY_MIN", "0"),
            "cec": out_setting(env, n, "CEC", "0") == "1",
            "rotate": out_setting(env, n, "ROTATE", "0"), "mirror": out_setting(env, n, "MIRROR", "none"),
            "brightness": out_setting(env, n, "BRIGHTNESS", "0"),
            "pacing": out_setting(env, n, "PACING", "fast"),
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
            "shown": shown_text(n) if running and st.get("state", "").startswith("playing") else "",
        })
    return out


# ---------------------------------------------------------------- inputs (capture devices)

def _stable_path(dev, by_port=False):
    """/dev/v4l/by-id (or by-path) link for /dev/videoN, so the setting survives re-plugging.
    by_port: by-path only. Identical cards without a serial number share one by-id link, which
    then points at either of them; the USB port tells them apart."""
    for d in ("/dev/v4l/by-path",) if by_port else ("/dev/v4l/by-id", "/dev/v4l/by-path"):
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
    try:
        listing = subprocess.run(("v4l2-ctl", "-d", dev, "--list-formats-ext"), capture_output=True, text=True,
                                 timeout=5).stdout
    except subprocess.TimeoutExpired:
        listing = ""  # a hung device (e.g. dropping off USB) must not hang the page
    for line in listing.splitlines():
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
        label = read_text(os.path.join(sysdir, "name")).strip()
        a, sep, b = label.partition(": ")
        if sep and a.strip() == b.strip():
            label = a.strip()  # UVC names are often "NAME: NAME"
        usb = os.path.realpath(os.path.join(sysdir, "device", ".."))
        found.append({"dev": dev, "label": label, "modes": modes,
                      "model": read_text(os.path.join(usb, "idVendor")).strip() + ":" +
                               read_text(os.path.join(usb, "idProduct")).strip()})
    models = [d["model"] for d in found]
    for d in found:
        d["id"] = _stable_path(d["dev"], by_port=models.count(d.pop("model")) > 1)
    return found


def configured_inputs(env):
    """{slot: settings} from PINDI_IN<n>_* keys."""
    slots = {}
    for key, val in env.items():
        m = re.match(r"^PINDI_IN(\d+)_(DEVICE|ENABLED|NAME|MODE|AUDIO|CODEC)$", key)
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
                        name=s.get("NAME", ""), mode=s.get("MODE", ""), audio_setting=s.get("AUDIO", "auto"),
                        audio_sync=s.get("AUDIO_SYNC", "on"), codec=s.get("CODEC", "speedhq"),
                        range=s.get("RANGE", "full")))
    for dev, slot in by_dev.items():
        s = conf[slot]
        out.append({"dev": "", "id": dev, "label": os.path.basename(dev), "modes": [], "slot": slot,
                    "present": False, "enabled": s.get("ENABLED") == "1", "name": s.get("NAME", ""),
                    "mode": s.get("MODE", ""), "audio_setting": s.get("AUDIO", "auto"),
                    "audio_sync": s.get("AUDIO_SYNC", "on"), "codec": s.get("CODEC", "speedhq"),
                    "range": s.get("RANGE", "full")})
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
            now = time.time()
            for t in [t for t, s in self.items.items() if s["expires"] <= now]:
                del self.items[t]
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

    def drop_user(self, user, keep=None):
        """Sign user out everywhere except the session keep (after a password change)."""
        with self.lock:
            for t in [t for t, s in self.items.items() if s["user"] == user and s is not keep]:
                del self.items[t]


SESSIONS = Sessions()

WELL_KNOWN_PASSWORDS = {"pindi", "raspberry", "password", "admin", "1234", "12345678", "pi"}


class Lockout:
    """After limit failures for one key within 10 minutes, that key must wait 10 minutes."""
    WINDOW, PENALTY = 600, 600

    def __init__(self, limit, what):
        self.limit, self.what = limit, what
        self.lock = threading.Lock()
        self.fails = {}  # key -> [timestamps]
        self.until = {}  # key -> locked until

    def blocked(self, key):
        with self.lock:
            return max(0, int(self.until.get(key, 0) - time.time()))

    def failure(self, key):
        now = time.time()
        with self.lock:
            for old in [a for a, t in self.until.items() if t < now]:  # forget served lockouts
                del self.until[old]
            for old in [a for a, f in self.fails.items() if now - f[-1] >= self.WINDOW]:
                del self.fails[old]
            recent = [t for t in self.fails.get(key, []) if now - t < self.WINDOW] + [now]
            self.fails[key] = recent
            if len(recent) >= self.limit:
                self.until[key] = now + self.PENALTY
                self.fails.pop(key, None)
                log("login: %s locked out for %d s after %d failures" % (self.what(key), self.PENALTY, self.limit))

    def success(self, key):
        with self.lock:
            self.fails.pop(key, None)


# Sign-ins are locked per address and user name: someone guessing (or deliberately failing) from an address
# shared with others (NAT) blocks only that name from there, not every user behind it. A wider limit per
# address stops guessing across many names. API tokens are 192-bit random values that can't be guessed, so
# wrong ones are only slowed down, never locked out: a valid token always works.
LOGIN_LOCK = Lockout(5, lambda k: "%s from %s" % (k[1], k[0]))
ADDRESS_LOCK = Lockout(30, lambda k: "every user from %s" % k)


# ---------------------------------------------------------------- pages

CSS = """
:root{--bg:#0b0f14;--card:#141b23;--line:#253140;--text:#e6edf3;--muted:#8b98a5;--accent:#2bb3a3;
--accent2:#1d6f8f;--bad:#e5534b;--ok:#3fb950;--warn:#d29922;color-scheme:dark}
a{color:var(--accent)}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);
font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 20px;
border-bottom:1px solid var(--line)}header h1{margin:0;font-size:20px;letter-spacing:.5px}
header h1 span{color:var(--accent)}header h1 small.device{font-size:14px;font-weight:400;
color:var(--muted);margin-left:8px;letter-spacing:0}header .who{color:var(--muted);font-size:13px}
main{max-width:860px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px 18px;margin:0 0 16px}
.card h2{margin:0 0 12px;font-size:16px}
dl{display:grid;grid-template-columns:max-content 1fr;gap:6px 16px;margin:0}dt{color:var(--muted)}dd{margin:0;word-break:break-all}
label{display:block;margin:10px 0 4px;color:var(--muted);font-size:13px}
input[type=text],input[type=password],input[type=number],input[type=time],select,textarea{width:100%;padding:9px 10px;border-radius:7px;border:1px solid var(--line);
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
td:last-child{text-align:right}.state{font-weight:600}.state.playing{color:var(--ok)}.state.onair{color:#fff;background:var(--bad);padding:0 6px;border-radius:4px}
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
code{font:13px ui-monospace,Consolas,monospace;background:#0d1319;padding:1px 5px;border-radius:4px;
word-break:break-all}
nav.sections{display:flex;flex-wrap:wrap;gap:6px 16px;margin:0 0 14px;font-size:14px}
nav.sections a{color:var(--accent);text-decoration:none}nav.sections a:hover{text-decoration:underline}
h3.group{margin:18px 0 0;font-size:13px;text-transform:uppercase;letter-spacing:.6px;color:var(--accent)}
details.card>summary{cursor:pointer;list-style:none}details.card>summary::-webkit-details-marker{display:none}
details.card>summary h2{display:inline;margin:0}details.card>summary:before{content:"\\25B8  ";color:var(--muted)}
details.card[open]>summary:before{content:"\\25BE  "}details.card[open]>summary{margin-bottom:12px}
html{scroll-behavior:smooth}section.card,details.card{scroll-margin-top:12px}
.preview{float:right;margin:0 0 10px 14px;width:240px}.thumb{width:100%;border-radius:6px;border:1px solid var(--line);
background:#000;display:block;min-height:40px}.thumb.none{visibility:hidden}
@media(max-width:600px){.preview{float:none;width:100%;margin:0 0 10px}}
.card:after{content:"";display:table;clear:both}
.row>div,dd,dt{min-width:0}select,input{max-width:100%}table{table-layout:fixed}td{word-break:break-word}
td:last-child{width:auto;white-space:nowrap}header{flex-wrap:wrap}
@media(max-width:600px){.row{grid-template-columns:1fr}main{padding:12px}dl{grid-template-columns:1fr}
dt{margin-top:6px}dd{margin-bottom:2px}td:nth-child(2){display:none}}
"""

FAVICON = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32"><rect width="32" height="32" rx="7" '
           'fill="#141b23"/><rect x="5" y="8" width="22" height="14" rx="2" fill="none" stroke="#2bb3a3" '
           'stroke-width="2.5"/><path d="M12 26h8" stroke="#2bb3a3" stroke-width="2.5" stroke-linecap="round"/>'
           '<path d="M14 12l5 3-5 3z" fill="#2bb3a3"/></svg>')

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
if(/state$/.test(k))el.className='state'+(/ON AIR/.test(v||'')?' onair':/^(playing|sending)/.test(v||'')?' playing':'');
if(k==='power')el.className=v==='OK'?'':'warn';});
var c=document.getElementById('countdown');if(c&&d.rollback!=null)c.textContent=d.rollback;}).catch(function(){});}
sync();if(document.querySelector('[data-k]'))setInterval(poll,3000);
setInterval(function(){document.querySelectorAll('img[data-thumb]').forEach(function(i){
if(document.hidden)return;var n=new Image();n.onload=function(){i.src=n.src;i.classList.remove('none');};
n.onerror=function(){i.classList.add('none');};n.src='/thumb/'+i.dataset.thumb+'.jpg?'+Date.now();});},2000);
document.addEventListener('submit',function(e){var m=e.target.dataset.confirm;if(m&&!confirm(m))e.preventDefault();});
var uf=document.getElementById('update-file');if(uf)uf.addEventListener('change',function(){
var f=uf.files[0],b=document.getElementById('update-button');b.disabled=true;if(!f)return;
var r=new FileReader();r.onload=function(){document.getElementById('update-data').value=r.result;b.disabled=false;};
r.readAsDataURL(f);});
var ul=document.getElementById('update-log');if(ul)setInterval(function(){fetch('/update/log',{credentials:'same-origin'})
.then(function(r){return r.ok?r.json():null}).then(function(d){if(!d)return;var end=ul.scrollTop+ul.clientHeight>=ul.scrollHeight-20;
ul.textContent=d.log;if(end)ul.scrollTop=ul.scrollHeight;var t={running:'Installing\u2026 do not switch the device off.',
failed:'The update failed; the previous version is still running. See the log.',done:'Installed. The device is rebooting.'};
var st=document.querySelector('[data-k=update_state]');if(st&&t[d.state])st.textContent=t[d.state];}).catch(function(){});},2000);
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
    # The device name in the tab title and header: with many units open, tabs are told apart.
    name = device_name()
    return ("<!doctype html><html lang=en><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>%s</title><link rel=icon href=/favicon.svg type='image/svg+xml'>"
            "<link rel=stylesheet href=/style.css></head><body>"
            "<header><h1>Pi<span>NDI</span> <small class=device>%s</small></h1>%s</header><main>%s</main>"
            "<script src=/app.js></script></body></html>") % (
        html.escape("PiNDI · %s" % title if name.lower() == "pindi" else
                    "%s · %s" % (name, title) if title != "PiNDI" else "%s · PiNDI" % name),
        "" if name.lower() == "pindi" else html.escape(name), who, body)


def esc(v):
    return html.escape(str(v or ""), quote=True)


def login_page(error=""):
    err = '<div class="msg err">%s</div>' % esc(error) if error else ""
    body = ('<div class="login card"><h2>Sign in</h2>%s<form method="post" action="/login">'
            '<label>User</label><input type="text" name="user" autocomplete="username" autofocus required>'
            '<label>Password</label><input type="password" name="password" autocomplete="current-password" required>'
            '<button>Sign in</button></form>'
            '<p class="hint">%s &middot; serial %s</p></div>' % (err, esc(device_name()), esc(SERIAL)))
    return page("Sign in", body)


def certificate_html(hidden):
    """Device settings: the HTTPS certificate, how to check it, and uploading one's own."""
    c = cert_info()
    if not c:
        return ""
    back = ('<form class="inline" method="post" action="/tls/self" data-confirm="Go back to the device\'s own '
            'certificate?">%s<button class="secondary">Use the device\'s own certificate</button></form>' % hidden
            if not c["self"] else "")
    return (
        '<h3 class="group">HTTPS certificate</h3><dl><dt>Certificate</dt><dd>%s</dd><dt>For</dt><dd>%s</dd>'
        '<dt>Valid until</dt><dd>%s</dd><dt>SHA-256 fingerprint</dt><dd><code>%s</code></dd>'
        '<dt>Public-key pin</dt><dd><code>sha256//%s</code></dd></dl>'
        '<p class="hint">%s To make sure a client talks to this device and not an impostor, compare the '
        'fingerprint with what the browser shows, or pin the key: <code>curl --pinnedpubkey sha256//%s -k '
        'https://...</code> (the connection is refused if the key differs, even with <code>-k</code>).</p>'
        '<form method="post" action="/tls">%s<div class="row"><div><label>Your own certificate (PEM, with any '
        'intermediate certificates)</label><textarea name="cert" rows="4" placeholder="-----BEGIN CERTIFICATE-----"'
        '></textarea></div><div><label>Its private key (PEM, not password-protected)</label><textarea name="key" '
        'rows="4" placeholder="-----BEGIN PRIVATE KEY-----"></textarea></div></div><p class="hint">From your '
        'organisation\'s certificate authority, for this device\'s name or address: browsers that trust that '
        'authority then show no warning.</p><button class="secondary">Install certificate</button> %s</form>' %
        ("Uploaded" if not c["self"] else "This device's own (self-signed); made again when its name or address "
         "changes", esc(", ".join(c["names"]) or "-"), esc(c["until"]), esc(c["fingerprint"]), esc(c["pin"]),
         "Browsers warn about a self-signed certificate until it is accepted." if c["self"] else "",
         esc(c["pin"]), hidden, back))


def dashboard(session):
    csrf = session["csrf"]
    hidden = '<input type="hidden" name="csrf" value="%s">' % csrf
    parts = []

    flash = session.pop("flash", None)
    session["flash"] = None
    if flash:
        parts.append('<div class="msg %s">%s</div>' % (flash[0], esc(flash[1])))

    if session.get("net") is not None:  # the broker's pending network change
        parts.append(
            '<div class="msg warn"><b>New network settings are active.</b> They will be undone in '
            '<span id="countdown">%d</span> s unless you keep them.<form method="post" action="/network/confirm">'
            '%s<button>Keep these settings</button> <button class="secondary" formaction="/network/revert">'
            'Undo now</button></form></div>' % (session["net"], hidden))

    net = live_network()
    outs = outputs()
    ins = inputs()

    def options(choices, current):
        return "".join('<option value="%s"%s>%s</option>' % (esc(v), " selected" if v == current else "", label)
                       for v, label in choices)

    # Section menu
    links = [("#out%d" % o["n"], "Output %d" % o["n"]) for o in outs] + [
        ("#sources", "Sources"), ("#outgoing", "Outgoing"), ("#settings", "Settings"), ("#network", "Network"),
        ("#wifi", "Wi-Fi"), ("#system", "System")]
    parts.append('<nav class="sections">%s</nav>' % "".join('<a href="%s">%s</a>' % l for l in links))

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
    ] + ([("Wi-Fi", wifi_state_text())] if wifi_enabled() else []) + [
        ("Network traffic", '<span data-k="net">%s</span><span class="hint block">Everything this device '
                            'receives and sends on the network (wired and Wi-Fi). A Full NDI 1080p60 stream takes up to about '
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
        ("OS updates", os_update_text()),
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
            '<dt>Source</dt><dd data-k="%svideo">%s</dd>'
            '<dt>Output</dt><dd data-k="%sshown">%s</dd>'
            '<dt>Format</dt><dd data-k="%sformat">%s</dd>'
            '<dt>Audio</dt><dd data-k="%saudio">%s</dd>'
            '<dt>Latency</dt><dd><span data-k="%slatency">%s</span>'
            '<span class="hint block">Measured per frame: from the moment the sender handed '
            'it to NDI until it was on screen. Not included: the camera/capture before the sender and the '
            'monitor&#39;s own processing.</span></dd></dl>' %
            (k, " playing" if o["state"].startswith("playing") else "", esc(o["state"]), k, esc(o["target"]),
             k, esc(o["source"] or "-"), k, esc(o["video"] or "-"), k, esc(o["shown"] or "-"),
             k, esc(o["format"] or "-"),
             k, esc(o["audio"] or "-"),
             k, esc(o["latency"] or "-")))
        parts.append(
            '<section class="card" id="out%d"><h2>Output %d &middot; %s %s</h2>%s'
            '<form method="post" action="/output">%s<input type="hidden" name="n" value="%d">'
            '<label class="check"><input type="checkbox" name="enabled" value="1"%s> Enabled '
            '<span class="hint">(off: no NDI is received and this HDMI port is switched off)</span></label>'
            '<h3 class="group">Source</h3><div class="row">'
            '<div><label>Stream name</label><input type="text" id="stream-%d" name="stream" '
            'value="%s" placeholder="%s" maxlength="200"></div><div><label>Source IP</label>'
            '<input type="text" id="source_ip-%d" name="source_ip" value="%s" placeholder="Any" maxlength="15">'
            '</div><div><label>Backup stream (optional)</label><input type="text" name="backup" value="%s" '
            'placeholder="None" maxlength="200"></div>'
            '<div><label>Receive quality</label><select name="bandwidth">%s</select></div></div>'
            '<p class="hint">Empty stream name = %s, empty IP = any host (a source IP is also searched '
            'directly, so senders on other subnets work). The backup plays while the main stream is '
            'unavailable. The low-bandwidth preview is a small (typically 640&times;360) stream every sender '
            'provides, for slow networks.</p>'
            '<h3 class="group">Picture</h3><div class="row">'
            '<div><label>Display mode</label><select name="display_mode">%s</select></div>'
            '<div><label>Rotation</label><select name="rotate">%s</select></div>'
            '<div><label>Mirror</label><select name="mirror">%s</select></div>'
            '<div><label>Brightness</label><select name="brightness">%s</select></div>'
            '<div><label>Frame pacing</label><select name="pacing">%s</select></div>'
            '<div><label>Display off without video</label><select name="standby">%s</select>'
            '<label class="check"><input type="checkbox" name="cec" value="1"%s> Also switch the TV off '
            'and on (HDMI-CEC)</label></div></div>'
            '<p class="hint">180&deg; is for ceiling mounting, mirror for rear projection. With the display '
            'switched off the monitor goes to sleep; it comes back on as soon as the stream appears. Brightness '
            'lightens or darkens the mid-tones; black and white stay as they are. Smooth frame pacing shows '
            'every frame for the same number of refreshes, timed by when the sender captured it, so motion '
            'does not judder (experimental: it can drop frames on a Pi 3).</p>'
            '<h3 class="group">Sound</h3><div class="row">'
            '<div><label>Audio output</label><select name="audio">%s</select></div>'
            '<div><label>Audio delay (ms)</label><input type="number" name="audio_delay" min="20" max="1000" '
            'step="10" value="%s"></div></div>'
            '<p class="hint">The delay is the least used; it is raised automatically when a sender&#39;s audio '
            'arrives unevenly. The picture appears about 25 ms after it arrives.</p>'
            '<button>Save output %d</button> '
            '<button class="secondary" formaction="/identify" name="identify" value="1">Identify</button> '
            '<button class="secondary" name="reset" value="1">Reset to default</button></form></section>' %
            (n, n, esc(o["connector"]), badge, status_rows, hidden, n, " checked" if o["enabled"] else "",
             n, esc(o["stream"]), esc(default_out_stream(n)), n, esc(o["source_ip"]), esc(o["backup"]),
             options(BANDWIDTHS, o["bandwidth"]), esc(default_out_stream(n)),
             options(mode_choices(o), o["mode_setting"]),
             options(ROTATIONS, o["rotate"]), options(MIRRORS, o["mirror"]),
             options(BRIGHTNESSES, o["brightness"]), options(PACINGS, o["pacing"]),
             options(STANDBYS, o["standby"]), " checked" if o["cec"] else "",
             options(audio_choices(), o["audio_out"]), esc(o["audio_delay"]), n))

    # NDI sources on the network
    srcs = visible_sources()
    enabled_outs = [o["n"] for o in outs if o["enabled"]]
    if srcs and enabled_outs:
        def use_buttons(s):
            return " ".join(
                '<button type="button" class="small secondary" data-out="%d" data-stream="%s" data-ip="%s">'
                'Use%s</button>' % (n, esc(s["stream"]), esc(s["ip"]), " for %d" % n if len(outs) > 1 else "")
                for n in enabled_outs)
        parts.append('<details class="card" id="sources"><summary><h2>NDI sources on the network '
                     '<span class="badge">%d</span></h2></summary><table>%s</table></details>' %
                     (len(srcs), "".join('<tr><td>%s</td><td>%s</td><td>%s</td></tr>' %
                                         (esc(s["name"]), esc(s["ip"]), use_buttons(s)) for s in srcs)))

    # Outgoing streams (capture devices)
    used = {i["slot"] for i in ins if i["slot"]}
    free_slots = [n for n in range(1, 9) if n not in used]
    hx_ok = hx_send_available()

    def codec_field(i):
        """Codec choice, only where NDI|HX can be sent (else Full NDI, nothing to choose)."""
        if not hx_ok:
            return ""
        warn = (' <strong>Without an NDI Advanced SDK licence (Device settings) the video of an NDI|HX stream '
                'stops after 30 minutes; the sound goes on.</strong>' if ndi_evaluation() else "")
        return ('<div><label>Codec</label><select name="codec">%s</select><span class="hint block">NDI|HX: '
                'H.264 from the Pi&#39;s hardware encoder &ndash; far less CPU and bandwidth; receivers need NDI|HX '
                'support (NDI Tools, vMix, OBS...).%s</span></div>' % (options(CODECS, i.get("codec", "speedhq")), warn))

    cards = []
    for i in ins:
        slot = i["slot"] or (free_slots.pop(0) if free_slots else 0)
        cap_choices = [("", "Automatic (best up to 1080p30)")] + i["modes"]
        if i["mode"] and i["mode"] not in dict(i["modes"]):
            cap_choices.append((i["mode"], i["mode"]))
        state = i["state"] or ("starting" if i["enabled"] else "off")
        live = ""
        if i["enabled"] and i["live_mode"] and state.startswith("ready"):
            live = " &middot; %s &middot; camera delivers %s fps" % (esc(i["live_mode"]), esc(i["cam_fps"] or "0"))
        elif i["enabled"] and i["live_mode"]:
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
            '</div>%s<div><label>Audio</label><select name="audio">%s</select></div>'
            '<div><label>Audio timing</label><select name="audio_sync">%s</select><span class="hint block">'
            'Capture cards\' audio clocks run a little fast or slow, which receivers eventually catch up on '
            'by dropping audio.</span></div>'
            '<div><label>Picture levels</label><select name="range">%s</select><span class="hint block">Change '
            'only if shadows look grey and washed out.</span></div></div>'
            '<button>Save</button> %s</form></div>' %
            (esc(i["label"]), " on" if i["present"] else "", "connected" if i["present"] else "not connected",
             esc(i["id"]), k, " onair" if "ON AIR" in state else " playing" if state.startswith("sending") else "",
             esc(state), live, hidden,
             esc(i["id"]), slot, " checked" if i["enabled"] else "", esc(i["name"]), esc(default_in_stream(slot)),
             options(cap_choices, i["mode"]), codec_field(i),
             options([("auto", "From the device (USB audio)"), ("off", "Off")], i["audio_setting"]),
             options(AUDIO_SYNCS, i["audio_sync"]),
             options(RANGES, i["range"]), remove))
    body = "".join(cards) if cards else (
        '<p class="hint">No capture devices detected. Plug in a USB HDMI capture card and reload this page.</p>')
    parts.append('<section class="card" id="outgoing"><h2>Configure Outgoing Streams</h2>'
                 '<p class="hint">Nothing is sent until you enable it here. The stream appears on the network as '
                 '"%s (name)".</p>%s</section>' % (esc(ndi_host_name()), body))

    # Device settings
    env = read_env()
    ns = ndi_settings()
    licence_fields = ""
    if ndi_advanced():
        licence_fields = (
            '<div class="row"><div><label>NDI Advanced SDK licence: vendor name</label><input type="text" '
            'name="vendor_name" value="%s" maxlength="200"></div><div><label>Vendor ID</label><input type="text" '
            'name="vendor_id" value="%s" maxlength="200"><span class="hint block">%s</span></div></div>' %
            (esc(ns["vendor_name"]), esc(ns["vendor_id"]),
             "From NDI (licensing@ndi.video). Without it the Advanced SDK is the evaluation: NDI|HX video stops "
             "after 30 minutes." if not ns["vendor_id"] else "Licensed: NDI|HX runs without a time limit."))
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
        '<div class="row"><div><label>NDI receive transport</label><select name="transport">%s</select>'
        '<span class="hint block">TCP rides out lost packets; with UDP a busy network can stall the '
        'audio for a second or more.</span></div></div>'
        '%s'
        '<label class="check"><input type="checkbox" name="net_buffers" value="1"%s> Large network buffers '
        '<span class="hint">(off: the system\'s own 208 KB, which drops video and audio packets when sending '
        '1080p to more than one receiver)</span></label>'
        '<div class="row"><div><label>Time zone</label><select name="timezone">%s</select></div>'
        '<div><label>Reboot every day at (optional)</label><input type="time" name="reboot_at" value="%s">'
        '<span class="hint block">Empty = never. A daily reboot clears any slow build-up on '
        'installations that run for months.</span></div>'
        '<div><label>Install OS updates every day at (optional)</label><input type="time" name="os_update_at" '
        'value="%s"><span class="hint block">Empty = never (the default). Security fixes for Raspberry Pi OS, '
        'from the internet; set it before the daily reboot, outside show times. Needs internet access.</span>'
        '</div></div>'
        '<label class="check"><input type="checkbox" name="splash_info" value="1"%s> Show IP address, serial '
        'and status on the splash screen</label>'
        '<label class="check"><input type="checkbox" name="frame_timeout" value="1"%s> Show the splash when a '
        'connected source stops sending video for 10 s <span class="hint">(off: keep the last picture, '
        'e.g. for screen captures that only send on change)</span></label>'
        '<button>Save device settings</button></form>'
        '<h3 class="group">HTTP API</h3><p class="hint">For control systems (e.g. Bitfocus Companion, Crestron) '
        'and scripts. Send the token as <code>Authorization: Bearer &lt;token&gt;</code>.</p>'
        '<dl><dt>Token</dt><dd><code>%s</code>%s</dd><dt>Status</dt><dd><code>GET /api/v1/status</code></dd>'
        '<dt>Switch</dt><dd><code>POST /api/v1/outputs/1/source</code> with <code>{"stream": "CAM 2", '
        '"source_ip": ""}</code></dd><dt>Identify</dt><dd><code>POST /api/v1/identify</code> with '
        '<code>{"output": 1}</code> (0 = all)</dd><dt>Restart</dt><dd><code>POST /api/v1/restart</code></dd>'
        '</dl><form method="post" action="/api-token" data-confirm="Generate a new API token? The current one '
        'stops working.">%s<button class="secondary">New token</button></form>%s</section>' %
        (hidden, esc(device_name()), esc(socket.gethostname()), esc(ns["discovery"]), esc(ns["recv_groups"]),
         esc(ns["send_groups"]), options(TRANSPORTS, ns["transport"]), licence_fields,
         "" if env.get("PINDI_NET_BUFFERS") == "0" else " checked",
         options([(z, esc(z)) for z in timezones()], current_timezone()),
         esc(reboot_time()), esc(os_update_time()), "" if env.get("PINDI_SPLASH_INFO") == "0" else " checked",
         " checked" if (env.get("PINDI_FRAME_TIMEOUT_MS") or "0") not in ("", "0") else "",
         esc(session["secrets"]["api_token"]),
         '<span class="hint block warn">%s</span>' % esc(session["secrets"]["token_note"])
         if session["secrets"]["token_note"] else "", hidden, certificate_html(hidden)))

    # Network
    cfg = read_network_config()
    wired = net if net["iface"] in ether_interfaces() else live_network(wifi=False)  # this card is the cable's
    fill = cfg if not cfg["dhcp"] else {"address": wired["address"], "prefix": wired["prefix"],
                                         "gateway": wired["gateway"], "dns": wired["dns"]}
    mask = ""
    if fill["prefix"]:
        try:
            mask = str(ipaddress.IPv4Network("0.0.0.0/%s" % fill["prefix"]).netmask)
        except ValueError:
            pass
    parts.append(
        '<section class="card" id="network"><h2>Network</h2><form method="post" action="/network">%s'
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

    # Wi-Fi
    wifi = session["secrets"]["wifi"]  # the configuration file is root's
    wifi_on = wifi_enabled()
    country = wifi["country"] or timezone_country()
    ssids = session["secrets"]["ssids"]
    reboot = ""
    if wifi_needs_reboot():
        reboot = ('<form method="post" action="/reboot" data-confirm="Reboot the device now?">%s'
                  '<button class="secondary">Reboot now</button></form>' % hidden)
    parts.append(
        '<section class="card" id="wifi"><h2>Wi-Fi</h2><dl><dt>Status</dt><dd>%s</dd></dl>'
        '<form method="post" action="/wifi">%s'
        '<label class="check"><input type="checkbox" name="enabled" value="1"%s> Wi-Fi on <span class="hint">'
        '(off: the Wi-Fi hardware is switched off, so the device starts faster)</span></label>'
        '<div class="row"><div><label>Network name (SSID)</label><input type="text" name="ssid" value="%s" '
        'maxlength="32" list="wifi-ssids" autocomplete="off"><datalist id="wifi-ssids">%s</datalist></div>'
        '<div><label>Password</label><input type="password" name="password" autocomplete="new-password" '
        'maxlength="64" placeholder="%s"></div>'
        '<div><label>Country</label><select name="country">%s</select></div></div>'
        '<label class="check"><input type="checkbox" name="open" value="1"%s> Open network (no password)</label>'
        '<p class="hint">The address comes from DHCP. When both are connected the wired network is preferred. '
        'NDI video over Wi-Fi is unreliable: use it for this page, or where no cable reaches. Switching Wi-Fi on '
        'for the first time needs a reboot.%s</p><button>Save Wi-Fi settings</button></form>%s</section>' %
        (wifi_state_text(), hidden, " checked" if wifi_on else "", esc(wifi["ssid"]),
         "".join('<option value="%s">' % esc(s) for s in ssids),
         "Saved &ndash; leave empty to keep it" if wifi["psk"] else "",
         options(([] if country else [("", "Choose...")]) + [(c, esc(n)) for c, n in countries()], country),
         " checked" if wifi_on and wifi["ssid"] and not wifi["psk"] else "",
         " Networks nearby are offered in the name field." if ssids else "", reboot))

    # Password
    parts.append(
        '<section class="card"><h2>Password</h2><form method="post" action="/password">%s'
        '<label>Current password</label><input type="password" name="current" autocomplete="current-password" required>'
        '<div class="row"><div><label>New password</label><input type="password" name="new" '
        'autocomplete="new-password" required></div><div><label>Repeat new password</label>'
        '<input type="password" name="confirm" autocomplete="new-password" required></div></div>'
        '<p class="hint">Changes the password of <b>%s</b>.</p>'
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
        'rollback. A backup from another device is used as a template: this device keeps its own name and '
        'network settings. A backup contains the NDI licence (vendor ID) if one is set: keep it private. '
        'Wi-Fi passwords and the API token are not in it.</p><button class="secondary" id="restore-button" disabled>Restore</button></form>'
        '<form method="post" action="/update" id="update-form" data-confirm="Install this update? Video '
        'stops and the device reboots when it is done.">%s<label>Software update (PiNDI %s)</label>'
        '<input type="file" id="update-file" accept=".tar.gz,.tgz,application/gzip">'
        '<input type="hidden" name="package" id="update-data"><label class="check"><input type="checkbox" '
        'name="downgrade" value="1"> Allow an older version</label><p class="hint">An update package is made '
        'and signed with <code>make-update.sh</code>; packages without a valid signature are refused. Settings '
        'are kept.</p>'
        '<button class="secondary" id="update-button" disabled>Install update</button> '
        '<a href="/update">Last update log</a></form>'
        '<div class="buttons"><form class="inline" method="post" action="/identify">%s'
        '<input type="hidden" name="n" value="0"><button class="secondary">Identify all screens</button></form>'
        '<form class="inline" method="post" action="/restart">%s'
        '<button class="secondary">Restart video</button></form>'
        '<form class="inline" method="post" action="/os-update" data-confirm="Download and install operating-'
        'system updates now? Video keeps running; it needs internet access.">%s'
        '<button class="secondary">Install OS updates now</button></form>'
        '<a class="button secondary" href="/os-update">OS update log</a>'
        '<form class="inline" method="post" action="/reset" data-confirm="Reset all PiNDI settings (outputs, '
        'outgoing streams, device and NDI settings) to the defaults? Network settings and passwords are kept.">'
        '%s<button class="danger">Reset settings</button></form>'
        '<form class="inline" method="post" action="/reboot" data-confirm="Reboot the device now?">%s'
        '<button class="danger">Reboot</button></form></div></section>' %
        (hidden, hidden, esc(version()), hidden, hidden, hidden, hidden, hidden))

    return page("PiNDI", "".join(parts), session["user"], csrf)


# ---------------------------------------------------------------- actions

ROTATIONS = [("0", "0&deg; (normal)"), ("180", "180&deg; (upside down)")]
BRIGHTNESSES = [(str(v), "Normal" if v == 0 else "%+d" % v) for v in range(-50, 51, 10)]
MIRRORS = [("none", "None"), ("horizontal", "Horizontal (left/right)"), ("vertical", "Vertical (top/bottom)")]
BANDWIDTHS = [("highest", "Full quality"), ("lowest", "Low-bandwidth preview")]
PACINGS = [("fast", "Lowest delay"), ("smooth", "Smooth (experimental)")]
STANDBYS = [("0", "Never (always show the splash)"), ("1", "After 1 minute"), ("5", "After 5 minutes"),
            ("15", "After 15 minutes"), ("30", "After 30 minutes"), ("60", "After 1 hour")]
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
            "BACKUP_STREAM", "STANDBY_MIN", "CEC", "BRIGHTNESS", "PACING")
    updates = {"PINDI_OUT%d_%s" % (n, k): "" for k in keys}
    if n == 1:  # settings from before multi-output support
        updates.update({"PINDI_" + k: "" for k in LEGACY_OUT1})
    if not form.get("reset"):
        enabled = form.get("enabled") == "1"
        stream = form.get("stream", "").strip()
        source_ip = form.get("source_ip", "").strip()
        rotate, mirror = form.get("rotate", "0"), form.get("mirror", "none")
        bandwidth = form.get("bandwidth", "highest")
        standby = form.get("standby", "0")
        if standby not in dict(STANDBYS):
            return ("err", "Invalid display-off setting.")
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
        brightness = form.get("brightness", "0")
        pacing = form.get("pacing", "fast")
        if (rotate not in dict(ROTATIONS) or mirror not in dict(MIRRORS) or bandwidth not in dict(BANDWIDTHS) or
                brightness not in dict(BRIGHTNESSES) or pacing not in dict(PACINGS)):
            return ("err", "Invalid display setting.")
        # Explicit per output, so the shared PINDI_BANDWIDTH default of other outputs is left alone.
        default_bw = (read_env().get("PINDI_BANDWIDTH") or "highest").lower()
        updates.update({
            "PINDI_OUT%d_ENABLED" % n: "" if enabled else "0",
            "PINDI_OUT%d_STREAM" % n: "" if stream == default_out_stream(n) else stream,
            "PINDI_OUT%d_SOURCE_IP" % n: source_ip,
            "PINDI_OUT%d_ROTATE" % n: "" if rotate == "0" else rotate,
            "PINDI_OUT%d_MIRROR" % n: "" if mirror == "none" else mirror,
            "PINDI_OUT%d_BRIGHTNESS" % n: "" if brightness == "0" else brightness,
            "PINDI_OUT%d_PACING" % n: "" if pacing == "fast" else pacing,
            "PINDI_OUT%d_BANDWIDTH" % n: "" if bandwidth == default_bw else bandwidth,
            "PINDI_OUT%d_MODE" % n: mode,
            "PINDI_OUT%d_BACKUP_STREAM" % n: backup,
            "PINDI_OUT%d_STANDBY_MIN" % n: "" if standby == "0" else standby,
            "PINDI_OUT%d_CEC" % n: "1" if form.get("cec") == "1" else "",
            "PINDI_OUT%d_AUDIO" % n: "" if audio == "hdmi" else audio,
            "PINDI_OUT%d_AUDIO_DELAY_MS" % n: "" if audio_delay == 80 else str(audio_delay),
        })
    before = read_env()
    write_env(updates)
    after = read_env()
    source_keys = {"PINDI_OUT%d_%s" % (n, k) for k in ("STREAM", "SOURCE_IP", "BACKUP_STREAM")}
    if n == 1:
        source_keys |= {"PINDI_STREAM", "PINDI_SOURCE_IP"}
    changed = {k for k in set(before) | set(after) if before.get(k) != after.get(k)}
    o = next(o for o in outputs() if o["n"] == n)
    if changed and changed <= source_keys and o["enabled"] and o["state"] != "player not running":
        # Only the source changed: switch live, without restarting the player.
        write_switch(n, o["stream"], o["source_ip"], o["backup"])
        log("output %d: switching to %s from %s" % (n, o["stream"] or default_out_stream(n), o["source_ip"] or "any"))
        return ("ok", "Output %d is switching to %s%s." % (
            n, o["stream"] or default_out_stream(n), " from " + o["source_ip"] if o["source_ip"] else ""))
    if not changed:
        return ("ok", "Output %d: nothing changed." % n)
    later(0.5, run, "systemctl", "restart", "pindi.service")
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
    keys = ["PINDI_IN%d_%s" % (slot, k) for k in ("ENABLED", "DEVICE", "NAME", "MODE", "AUDIO", "CODEC", "RANGE",
                                                   "AUDIO_SYNC")]
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
        codec = form.get("codec", "speedhq")
        if codec not in dict(CODECS) or (codec == "hx" and not hx_send_available()):
            return ("err", "Invalid codec.")
        rng = form.get("range", "full")
        if rng not in dict(RANGES):
            return ("err", "Invalid picture levels.")
        audio_sync = form.get("audio_sync", "on")
        if audio_sync not in dict(AUDIO_SYNCS):
            return ("err", "Invalid audio timing.")
        write_env(dict(zip(keys, ["1" if enabled else "0", device,
                                  "" if name == default_in_stream(slot) else name, mode,
                                  "" if audio == "auto" else audio, "" if codec == "speedhq" else codec,
                                  "" if rng == "full" else rng, "" if audio_sync == "on" else audio_sync])))
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


def act_wifi(form):
    on = form.get("enabled") == "1"
    if not on:
        if not wifi_enabled():
            return ("ok", "Wi-Fi is off.")
        os.remove(WIFI_WANTS)
        set_wifi_overlay(False)
        if wifi_present():
            later(0.5, apply_wifi, False)
        log("wifi: off")
        return ("ok", "Wi-Fi is off. The Wi-Fi hardware is switched off completely at the next reboot.")
    ssid, passphrase = form.get("ssid", "").strip(), form.get("password", "")
    country = form.get("country", "")
    if not 1 <= len(ssid.encode()) <= 32 or any(ord(c) < 32 for c in ssid):
        return ("err", "Enter the Wi-Fi network name (at most 32 characters).")
    if country not in dict(countries()):
        return ("err", "Choose the country the device is used in (it sets the allowed Wi-Fi channels).")
    old = read_wifi_config()
    if form.get("open"):
        psk = ""
    elif passphrase:
        if not (8 <= len(passphrase) <= 63 and all(32 <= ord(c) < 127 for c in passphrase)) and \
                not re.match(r"^[0-9a-fA-F]{64}$", passphrase):
            return ("err", "A Wi-Fi password has 8 to 63 characters (letters, digits and symbols).")
        psk = wifi_psk(ssid, passphrase)
    elif ssid == old["ssid"] and old["psk"]:
        psk = old["psk"]  # empty password field: keep the stored key
    else:
        return ("err", "Enter the password of %s (or tick \"Open network\" if it has none)." % ssid)
    write_atomic(WIFI_CONF, render_wifi_config(ssid, psk, country), 0o600)
    write_atomic(WIFI_NETWORK_FILE, "# Written by the PiNDI web interface.\n[Match]\nName=%s\n\n[Network]\n"
                                    "DHCP=yes\n\n[DHCPv4]\nClientIdentifier=mac\n# Wired stays the preferred "
                                    "route.\nRouteMetric=2048\n\n[IPv6AcceptRA]\nRouteMetric=2048\n" % WIFI_IFACE)
    # The Pi OS image keeps Wi-Fi blocked until a country is set; systemd-rfkill restores that block at boot.
    os.makedirs(os.path.dirname(WIFI_DROPIN), exist_ok=True)
    write_atomic(WIFI_DROPIN, "# Written by the PiNDI web interface.\n[Service]\n"
                              "ExecStartPre=-/usr/sbin/rfkill unblock wifi\n")
    for name in os.listdir("/var/lib/systemd/rfkill") if os.path.isdir("/var/lib/systemd/rfkill") else []:
        if name.endswith(":wlan"):
            write_atomic("/var/lib/systemd/rfkill/" + name, "0\n")
    if not wifi_enabled():
        os.makedirs(os.path.dirname(WIFI_WANTS), exist_ok=True)
        os.symlink("/lib/systemd/system/wpa_supplicant@.service", WIFI_WANTS)
    set_wifi_overlay(True)
    log("wifi: on, network %s, country %s%s" % (ssid, country, "" if psk else ", open"))
    if not wifi_present():
        return ("warn", "Wi-Fi settings saved. Reboot the device to switch the Wi-Fi hardware on; it then "
                        "connects to %s." % ssid)
    later(0.5, apply_wifi, True)
    return ("ok", "Connecting to %s. The status below shows the result after a few seconds (reload the page)."
            % ssid)


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
    SESSIONS.drop_user(session["user"], keep=session)
    log("password changed for %s" % session["user"])
    return ("ok", "Password changed. Other sign-ins of %s were signed out." % session["user"])


def write_switch(n, stream, source_ip, backup):
    """Tell the running player to switch output n to another stream (see switch_poll in the player)."""
    write_atomic("%s/out%d.source" % (CTL_DIR, n),
                 "stream=%s\nsource_ip=%s\nbackup=%s\n" % (stream, source_ip, backup))


API_TOKEN_FILE = "/etc/pindi/api-token"


def api_token(create=True):
    token = read_text(API_TOKEN_FILE).strip()
    if not token and create:
        token = secrets.token_urlsafe(24)
        os.makedirs(os.path.dirname(API_TOKEN_FILE), exist_ok=True)
        write_atomic(API_TOKEN_FILE, token + "\n", 0o600)
    return token


def api_status():
    """Machine-readable state, for control systems."""
    hl, hw = health()
    outs = []
    for o in outputs():
        outs.append({k: o[k] for k in ("n", "connector", "enabled", "state", "target", "source", "video",
                                        "format", "audio", "latency", "stream", "source_ip", "backup")})
        outs[-1]["display"] = {"link": o["link"], "mode": o["mode"]}
    # Configured inputs and their status files only: no device enumeration (control systems poll this).
    ins = []
    for slot, conf in sorted(configured_inputs(read_env()).items()):
        enabled = conf.get("ENABLED") == "1"
        st = read_kv("%s/in%d.status" % (RUN_DIR, slot)) if enabled else {}
        ins.append({"slot": slot, "device": conf["DEVICE"], "present": os.path.exists(conf["DEVICE"]),
                    "enabled": enabled, "name": conf.get("NAME") or default_in_stream(slot),
                    "state": st.get("state", "starting") if enabled else "off", "mode": st.get("mode", ""),
                    "fps": st.get("fps", ""), "audio": st.get("audio", ""),
                    "on_air": "ON AIR" in st.get("state", "")})
    return {"device": {"name": device_name(), "ndi_name": ndi_host_name(), "serial": SERIAL, "model": MODEL,
                       "version": version(), "ip": live_network()["address"], "health": hl, "power": hw,
                       "network": TRAFFIC.text()},
            "outputs": outs, "inputs": ins, "sources": visible_sources()}


def api_post(path, data):
    """(code, result) for POST /api/v1/..."""
    m = re.match(r"^/api/v1/outputs/(\d+)/source$", path)
    if m:
        n = int(m.group(1))
        o = next((o for o in outputs() if o["n"] == n), None)
        if not o:
            return 404, {"error": "unknown output"}
        stream = str(data.get("stream", o["stream"])).strip()
        source_ip = str(data.get("source_ip", o["source_ip"])).strip()
        backup = str(data.get("backup", o["backup"])).strip()
        if not valid_name(stream) or not valid_name(backup):
            return 400, {"error": "invalid stream name"}
        if source_ip:
            try:
                source_ip = str(ipaddress.IPv4Address(source_ip))
            except ValueError:
                return 400, {"error": "source_ip must be an IPv4 address or empty"}
        keys = {"PINDI_OUT%d_STREAM" % n: "" if stream == default_out_stream(n) else stream,
                "PINDI_OUT%d_SOURCE_IP" % n: source_ip, "PINDI_OUT%d_BACKUP_STREAM" % n: backup}
        if n == 1:
            keys.update({"PINDI_STREAM": "", "PINDI_SOURCE_IP": ""})
        write_env(keys)
        write_switch(n, keys["PINDI_OUT%d_STREAM" % n], source_ip, backup)
        log("api: output %d -> %s from %s" % (n, stream or default_out_stream(n), source_ip or "any"))
        return 200, {"ok": True, "output": n, "stream": stream or default_out_stream(n), "source_ip": source_ip,
                     "backup": backup}
    if path == "/api/v1/identify":
        try:
            n = int(data.get("output", 0))
        except (TypeError, ValueError):
            return 400, {"error": "output must be a number (0 = all)"}
        kind, msg = act_identify({"n": str(n)})
        return (200 if kind == "ok" else 404), {"ok": kind == "ok", "message": msg}
    if path == "/api/v1/restart":
        later(0.5, restart_media)
        return 200, {"ok": True}
    return 404, {"error": "unknown endpoint"}


def act_api_token():
    for f in (API_TOKEN_FILE, API_TOKEN_NOTE):
        try:
            os.remove(f)
        except OSError:
            pass
    api_token()
    log("api: new token generated")
    return ("ok", "A new API token was generated; the old one no longer works.")


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
    write_atomic(CTL_DIR + "/identify", "%d\n%s\n" % (n, text))
    return ("ok", "%s shows its name for 15 seconds." % ("Output %d" % n if n else "Every output"))


SYSCTL_FILE = "/etc/sysctl.d/90-pindi-ndi.conf"
NET_BUFFERS = ("net.core.wmem_default = 4194304\nnet.core.rmem_default = 4194304\n"
               "net.core.wmem_max = 8388608\nnet.core.rmem_max = 8388608\n")


def set_net_buffers(large):
    """NDI sends each video frame as a burst of UDP packets per receiver; the kernel's 208 KB default
    socket buffer overflows with two receivers at 1080p and the packets are dropped. Large buffers
    (the default, PINDI_NET_BUFFERS unset) avoid that; off restores the system's own sizes."""
    if large:
        write_atomic(SYSCTL_FILE, "# Written by the PiNDI web interface (Device settings).\n" + NET_BUFFERS)
    else:
        try:
            os.remove(SYSCTL_FILE)
        except FileNotFoundError:
            pass
        for k in ("wmem_default", "rmem_default", "wmem_max", "rmem_max"):
            with open("/proc/sys/net/core/" + k, "w") as f:
                f.write("212992\n")
    run("sysctl", "-q", "--system")


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
    tz = form.get("timezone", "")
    if tz and tz not in timezones():
        return ("err", "Unknown time zone.")
    reboot_at = form.get("reboot_at", "")
    if reboot_at and not re.match(r"^([01]\d|2[0-3]):[0-5]\d$", reboot_at):
        return ("err", "The daily reboot time must look like 04:30.")
    os_update_at = form.get("os_update_at", "")
    if os_update_at and not re.match(r"^([01]\d|2[0-3]):[0-5]\d$", os_update_at):
        return ("err", "The OS update time must look like 03:30.")
    transport = form.get("transport", "tcp")
    if transport not in dict(TRANSPORTS):
        return ("err", "Unknown NDI transport.")
    current = ndi_settings()
    vendor_name = form.get("vendor_name", current["vendor_name"]).strip()
    vendor_id = form.get("vendor_id", current["vendor_id"]).strip()
    if len(vendor_name) > 200 or len(vendor_id) > 200 or not valid_name(vendor_name) or not valid_name(vendor_id):
        return ("err", "Invalid NDI licence vendor name or ID.")
    if vendor_id and not vendor_name:
        return ("err", "Enter the vendor name that came with the NDI licence ID.")

    # Everything is valid: apply. Video restarts only if something it uses changed.
    media_keys = ("PINDI_SPLASH_INFO", "PINDI_FRAME_TIMEOUT_MS", "PINDI_NET_BUFFERS")
    env = read_env()
    before = (ndi_settings(), [env.get(k, "") for k in media_keys])
    host = hostname_for(name)
    try:
        renamed = set_hostname(host)
    except RuntimeError as e:
        return ("err", "Could not set the host name: %s" % e)
    # A name that is a valid host name is used as is (NDI shows it in capitals, as usual); anything
    # else ("Lobby left") also becomes the NDI machine name, so sources show it exactly as typed.
    write_ndi_settings(lists["discovery"], lists["send_groups"], lists["recv_groups"],
                       "" if name == host else name, transport, vendor_name, vendor_id)
    if tz and tz != current_timezone():
        run("timedatectl", "set-timezone", tz)
        log("time zone: %s" % tz)
    set_reboot_time(reboot_at)
    set_os_update_time(os_update_at)
    write_env({"PINDI_SPLASH_INFO": "" if form.get("splash_info") == "1" else "0",
               "PINDI_FRAME_TIMEOUT_MS": "10000" if form.get("frame_timeout") == "1" else "",
               "PINDI_NET_BUFFERS": "" if form.get("net_buffers") == "1" else "0"})
    if env.get("PINDI_NET_BUFFERS", "") != read_env().get("PINDI_NET_BUFFERS", ""):
        set_net_buffers(form.get("net_buffers") == "1")
    env = read_env()
    restart = renamed or before != (ndi_settings(), [env.get(k, "") for k in media_keys])
    log("device settings: name=%s host=%s discovery=%s recv=%s send=%s transport=%s buffers=%s" % (
        name, host, lists["discovery"] or "-", lists["recv_groups"] or "-", lists["send_groups"] or "-", transport,
        "large" if form.get("net_buffers") == "1" else "system"))
    if not restart:
        return ("ok", "Device settings saved.")

    def apply():
        if renamed:
            run("systemctl", "restart", "avahi-daemon.service")
            ensure_cert()  # the device's own certificate names the new host name
        restart_media()
    later(0.5, apply)
    return ("ok", "Device settings saved. Video and outgoing streams are restarting.")


BACKUP_FORMAT = "pindi-backup-1"


def backup_json():
    return json.dumps({"format": BACKUP_FORMAT, "version": version(), "serial": SERIAL,
                       "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "device_name": device_name(),
                       "settings": {k: v for k, v in read_env().items() if k.startswith("PINDI_")},
                       "ndi": read_ndi_config(), "network": read_text(NETWORK_FILE),
                       "timezone": current_timezone(), "reboot_at": reboot_time(),
                       "os_update_at": os_update_time()}, indent=2) + "\n"


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
    # A backup from another unit is a template: its name and network settings would clash with that
    # unit (same IP, same NDI names), so this device keeps its own.
    other = str(data.get("serial", "")) != SERIAL
    if other:
        network = ""
        ndi = json.loads(json.dumps(ndi))  # copy
        mine = ndi_settings()["machinename"]
        sec = ndi.setdefault("ndi", {})
        if mine:
            sec["machinename"] = mine
        else:
            sec.pop("machinename", None)
    # Replace every PINDI_ setting with the backup's.
    current = {k: "" for k in read_env() if k.startswith("PINDI_")}
    current.update(settings)
    write_env(current)
    write_ndi_config(ndi)
    name = "" if other else str(data.get("device_name", "")).strip()
    if name and valid_name(name):
        try:
            if set_hostname(hostname_for(name)):
                later(0.3, run, "systemctl", "restart", "avahi-daemon.service")
        except RuntimeError:
            pass
    tz = str(data.get("timezone", ""))
    if tz and tz in timezones() and tz != current_timezone():
        run("timedatectl", "set-timezone", tz)
    reboot_at = str(data.get("reboot_at", ""))
    if re.match(r"^(([01]\d|2[0-3]):[0-5]\d)?$", reboot_at):
        set_reboot_time(reboot_at)
    os_update_at = str(data.get("os_update_at", ""))
    if re.match(r"^(([01]\d|2[0-3]):[0-5]\d)?$", os_update_at):
        set_os_update_time(os_update_at)
    later(0.5, restart_media)
    log("settings restored from backup of %s (%s)" % (data.get("serial", "?"), data.get("created", "?")))
    if network and network != read_text(NETWORK_FILE):
        NET.apply(network, "restored settings")
        return ("warn", "Settings restored. The network settings changed as well: open the device at its "
                        "(new) address within %d seconds and press Keep." % ROLLBACK_SECONDS)
    if other:
        return ("ok", "Settings from device %s applied. This device kept its own name and network settings. "
                      "Video and outgoing streams are restarting." % data.get("serial", "?"))
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


def journal(lines, units=("pindi", "pindi-sender", "pindi-web", "pindi-webd")):
    cmd = ["journalctl", "--no-pager", "-o", "short-iso", "-n", str(lines)]
    for u in units:
        cmd += ["-u", u]
    return run(*cmd).stdout


def diagnostics_text(wifi):
    out = ["PiNDI diagnostics", "=" * 60]
    hl, hw = health()
    out += ["Created: %s" % time.strftime("%Y-%m-%d %H:%M:%S %z"), "Version: %s" % version(),
            "Model: %s" % MODEL, "Serial: %s" % SERIAL, "Device name: %s" % device_name(),
            "Kernel: %s" % " ".join(os.uname()), "NDI: %s" % (read_text(RUN_DIR + "/ndi").replace("\n", " ")),
            "Health: %s" % hl, "Power/cooling: %s" % hw, "Throttled: 0x%x" % throttled(),
            "Network traffic: %s" % TRAFFIC.text(), ""]
    # Diagnostics get sent to others: no NDI licence (vendor ID) in them. The API token and passwords are
    # never in these files.
    ndi_cfg = re.sub(r'("id"\s*:\s*")[^"]*"', r'\1(removed)"', read_text(NDI_CONFIG))
    out += ["---- NDI config", ndi_cfg.rstrip() or "(empty)", ""]
    for title, path in (("Settings (%s)" % ENV_FILE, ENV_FILE),
                        ("Network", NETWORK_FILE), ("Outputs", RUN_DIR + "/outputs"),
                        ("Visible NDI sources", RUN_DIR + "/sources")):
        out += ["---- " + title, read_text(path).rstrip() or "(empty)", ""]
    try:
        for name in sorted(os.listdir(RUN_DIR)):
            if name.endswith((".status", ".latency", ".modes")):
                out += ["---- " + name, read_text(os.path.join(RUN_DIR, name)).rstrip(), ""]
    except OSError:
        pass
    out += ["---- Wi-Fi", "%s; network %s, country %s, %s" % (
        re.sub(r"<[^>]+>", "", html.unescape(wifi_state_text())), wifi["ssid"] or "-", wifi["country"] or "-",
        "password set" if wifi["psk"] else "no password"), ""]
    out += ["---- ip addr", run("ip", "addr").stdout, "---- services",
            run("systemctl", "status", "--no-pager", "-n", "0", "pindi", "pindi-sender", "pindi-web").stdout,
            "---- log (last 2000 lines)", journal(2000), "---- kernel (last 200 lines)",
            run("journalctl", "-k", "-b", "--no-pager", "-n", "200", "-o", "short-iso").stdout]
    return "\n".join(out)


UPDATE_DIR = "/var/lib/pindi/update"
UPDATE_LOG = "/var/log/pindi-update.log"
UPDATE_UNIT = "pindi-update"
UPDATE_MAX = 32 * 1024 * 1024
UPDATE_NEEDS = ("install.sh", "pindi-player.c", "pindi-sender.c", "pindi-web.py", "VERSION")
UPDATE_KEY = "/usr/share/pindi/update-key.pub"  # installed from update-key.pub; make-update.sh signs with its pair
UPDATE_MEMBERS = 200
UPDATE_UNPACKED_MAX = 96 * 1024 * 1024


def unpack_update(pkg, dest):
    """Extract an update archive: regular files and folders only, nothing outside dest, bounded in count and
    size. Done by hand (not extractall) so it is the same on every Python version. Returns the package folder."""
    total = 0
    with tarfile.open(pkg, "r:gz") as t:
        members = []
        for m in t:
            members.append(m)
            if len(members) > UPDATE_MEMBERS:
                raise ValueError("more than %d files" % UPDATE_MEMBERS)
            parts = m.name.replace("\\", "/").split("/")
            if m.name.startswith("/") or ".." in parts or (not m.name.strip("./") and not m.isdir()):
                raise ValueError("unsafe path %r" % m.name)
            if not (m.isfile() or m.isdir()):
                raise ValueError("%s is not a plain file" % m.name)
            total += m.size
            if total > UPDATE_UNPACKED_MAX:
                raise ValueError("more than %d MB unpacked" % (UPDATE_UNPACKED_MAX >> 20))
        for m in members:
            path = os.path.join(dest, *[p for p in m.name.split("/") if p not in ("", ".")])
            if m.isdir():
                os.makedirs(path, mode=0o700, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
            with t.extractfile(m) as f, open(path, "wb") as out:
                shutil.copyfileobj(f, out)
    entries = os.listdir(dest)
    if len(entries) == 1 and os.path.isdir(os.path.join(dest, entries[0])):
        return os.path.join(dest, entries[0])  # packaged inside one folder
    return dest


def verify_update(src):
    """"" if src is a package signed with this device's update key, else why not. SHA256SUMS lists every file
    with its hash; SHA256SUMS.sig is its Ed25519 signature."""
    if not os.path.exists(UPDATE_KEY):
        return "this device has no update key (%s); run install.sh again" % UPDATE_KEY
    sums, sig = os.path.join(src, "SHA256SUMS"), os.path.join(src, "SHA256SUMS.sig")
    if not (os.path.isfile(sums) and os.path.isfile(sig)):
        return "the package is not signed (make it with make-update.sh)"
    r = run("openssl", "pkeyutl", "-verify", "-pubin", "-inkey", UPDATE_KEY, "-rawin", "-in", sums, "-sigfile", sig)
    if r.returncode:
        return "its signature is not valid for this device's update key"
    listed = {}
    for line in read_text(sums).splitlines():
        m = re.match(r"^([0-9a-f]{64}) [ *](.+)$", line)
        if not m:
            return "SHA256SUMS is malformed"
        listed[os.path.normpath(m.group(2))] = m.group(1)
    found = set()
    for root, _dirs, files in os.walk(src):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), src)
            if rel not in ("SHA256SUMS", "SHA256SUMS.sig"):
                found.add(rel)
    if found != set(listed):
        return "its files do not match the signed list (%s)" % ", ".join(sorted(found ^ set(listed))[:5])
    for rel, digest in listed.items():
        h = hashlib.sha256()
        with open(os.path.join(src, rel), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        if h.hexdigest() != digest:
            return "%s was changed after signing" % rel
    return ""


def version_key(v):
    """"1.10.2" > "1.9" (numbers compared as numbers)."""
    return [(0, int(p), "") if p.isdigit() else (1, 0, p) for p in re.split(r"[.+~-]", v)]


def update_state():
    """"running", "failed", "done" (rebooting) or "" (no update since the last boot)."""
    active = run("systemctl", "show", "-p", "ActiveState", "--value", UPDATE_UNIT + ".service").stdout.strip()
    if active in ("activating", "active", "reloading", "deactivating"):
        return "running"
    try:
        boot = time.time() - float(read_text("/proc/uptime").split()[0])
        if os.path.getmtime(UPDATE_LOG) < boot:
            return ""
    except (OSError, ValueError, IndexError):
        return ""
    text = read_text(UPDATE_LOG)
    if "PINDI-UPDATE-FAILED" in text:
        return "failed"
    return "done" if "Update installed" in text else ""


def act_update(form):
    if update_state() == "running":
        return ("err", "An update is already running.")
    data = form.get("package", "")
    if "," in data[:100]:
        data = data.split(",", 1)[1]  # data: URL from the browser
    try:
        blob = base64.b64decode(data, validate=True)
    except ValueError:
        return ("err", "Choose an update package (.tar.gz) first.")
    if not blob or len(blob) > UPDATE_MAX:
        return ("err", "Choose an update package (.tar.gz, at most 32 MB).")
    shutil.rmtree(UPDATE_DIR, ignore_errors=True)
    os.makedirs(UPDATE_DIR, mode=0o700)
    pkg = UPDATE_DIR + ".tar.gz"
    with open(pkg, "wb") as f:
        f.write(blob)
    try:
        src = unpack_update(pkg, UPDATE_DIR)
    except (tarfile.TarError, OSError, ValueError, EOFError) as e:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)
        return ("err", "That is not a valid update package: %s" % e)
    finally:
        os.remove(pkg)
    problem = verify_update(src)
    if not problem:
        missing = [n for n in UPDATE_NEEDS if not os.path.isfile(os.path.join(src, n))]
        problem = "not a PiNDI update package (missing %s)" % ", ".join(missing) if missing else ""
    new_version = read_text(os.path.join(src, "VERSION")).strip()
    if not problem and not re.match(r"^[0-9][0-9A-Za-z.+~-]{0,31}$", new_version):
        problem = "its VERSION is not a version number"
    current = version().split()[0]
    if not problem and current[:1].isdigit() and version_key(new_version) < version_key(current) and \
            form.get("downgrade") != "1":
        problem = ("it is PiNDI %s, older than the installed %s. Tick \"Allow an older version\" to install "
                   "it anyway" % (new_version, version().split()[0]))
    if problem:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)
        log("software update refused: %s" % problem)
        return ("err", "Update refused: %s." % problem)
    # A separate unit: it keeps running while this web interface is replaced, and reboots when done.
    # The versions and folder are arguments, never part of the script text.
    script = ('echo "PiNDI update $1 -> $2, $(date)"; if cd "$3" && bash install.sh --accept-ndi-license '
              '--skip-upgrade --offline --no-reboot; then echo "Update installed, rebooting."; sleep 3; '
              'systemctl reboot; else echo "PINDI-UPDATE-FAILED"; exit 1; fi')
    run("systemctl", "reset-failed", UPDATE_UNIT + ".service")
    try:
        os.remove(UPDATE_LOG)  # file: output doesn't truncate
    except OSError:
        pass
    r = run("systemd-run", "--unit", UPDATE_UNIT, "--collect", "--property", "StandardOutput=file:" + UPDATE_LOG,
            "--property", "StandardError=file:" + UPDATE_LOG, "/bin/bash", "-c", script, "pindi-update",
            version().split()[0], new_version, src)
    if r.returncode:
        return ("err", "Could not start the update: %s" % (r.stderr.strip() or r.stdout.strip()))
    log("software update to %s started" % new_version)
    return ("ok", "Installing PiNDI %s. The device reboots when it is done (about 2 minutes); the log is "
                  "below." % new_version)


def update_page(session):
    state = update_state()
    text = {"running": "Installing&hellip; do not switch the device off.",
            "failed": "The update failed; the previous version is still running. See the log.",
            "done": "Installed. The device is rebooting.", "": "No update has run since the last reboot."}
    body = ('<section class="card"><nav class="top"><a href="/">&larr; Back</a></nav><h2>Software update</h2>'
            '<p><span data-k="update_state" class="state">%s</span></p><pre class="log" id="update-log">%s</pre>'
            '</section>' % (text[state], esc(read_text(UPDATE_LOG)[-20000:])))
    return page("Update", body, session["user"], session["csrf"])


def os_update_page(session):
    body = ('<section class="card"><nav class="top"><a href="/">&larr; Back</a></nav><h2>OS update log</h2>'
            '<p>%s</p><pre class="log">%s</pre></section>' %
            (os_update_text(), esc(read_text(OS_UPDATE_LOG)[-20000:] or "No OS update has run from this device.")))
    return page("OS updates", body, session["user"], session["csrf"])


def logs_page(session):
    body = ('<section class="card"><nav class="top"><a href="/">&larr; Back</a><a href="/diagnostics">Download '
            'diagnostics</a></nav><h2>Log (last 400 lines)</h2><pre class="log">%s</pre></section>' %
            esc(journal(400)))
    return page("Log", body, session["user"], session["csrf"])


# ---------------------------------------------------------------- HTTPS certificate

TLS_CERT, TLS_KEY = TLS_DIR + "/cert.pem", TLS_DIR + "/key.pem"
TLS_SELF = TLS_DIR + "/self-signed"  # while the certificate is the device's own: the names it was made for
TLS_UPLOADED = TLS_DIR + "/uploaded"  # while it is one uploaded in Device settings (install.sh keeps it)
OS_UPDATE_OVERDUE_DAYS = 30


def tls_names():
    """What the device is reached as: host name, <name>.local and every current IPv4 address."""
    host = socket.gethostname()
    ifaces = set(ether_interfaces() + wifi_interfaces())
    ips = set()
    for line in run("ip", "-4", "-o", "addr", "show", "scope", "global").stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[1] in ifaces:
            ips.add(parts[3].split("/")[0])
    return ["DNS:%s" % host, "DNS:%s.local" % host] + ["IP:%s" % a for a in sorted(ips)]


def tls_permissions():
    """Key readable by the front end (it serves TLS), nobody else."""
    try:
        gid = grp.getgrnam(WEB_USER).gr_gid
    except KeyError:
        gid = 0
    os.makedirs(TLS_DIR, mode=0o750, exist_ok=True)
    os.chown(TLS_DIR, 0, gid)
    os.chmod(TLS_DIR, 0o750)
    for path, mode in ((TLS_KEY, 0o640), (TLS_CERT, 0o644)):
        if os.path.exists(path):
            os.chown(path, 0, gid)
            os.chmod(path, mode)


def ensure_cert():
    """Broker: the device's own certificate, made again whenever its host name or addresses change (so it
    keeps matching the address in the browser). An uploaded certificate is left alone."""
    have = os.path.exists(TLS_CERT) and os.path.exists(TLS_KEY)
    if have and not os.path.exists(TLS_SELF):
        tls_permissions()
        return
    names = tls_names()
    if have and read_text(TLS_SELF).split() == names:
        tls_permissions()
        return
    os.makedirs(TLS_DIR, mode=0o750, exist_ok=True)
    fd, key = tempfile.mkstemp(prefix=".key.", dir=TLS_DIR)
    os.close(fd)
    cert = key + ".crt"
    try:
        r = run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                "-days", "3650", "-subj", "/CN=%s" % socket.gethostname(),
                "-addext", "subjectAltName=" + ",".join(names), "-keyout", key, "-out", cert)
        if r.returncode:
            raise RuntimeError("cannot create the TLS certificate: %s" % r.stderr.strip())
        os.replace(key, TLS_KEY)
        os.replace(cert, TLS_CERT)
    finally:
        for f in (key, cert):
            if os.path.exists(f):
                os.remove(f)
    write_atomic(TLS_SELF, " ".join(names) + "\n")
    tls_permissions()
    log("tls: new self-signed certificate for %s" % ", ".join(n.split(":", 1)[1] for n in names))


def act_tls(form):
    """Use an uploaded certificate (PEM, may include the chain) and its unencrypted private key."""
    cert, key = form.get("cert", "").strip() + "\n", form.get("key", "").strip() + "\n"
    if "BEGIN CERTIFICATE" not in cert or "PRIVATE KEY" not in key:
        return ("err", "Paste the certificate (BEGIN CERTIFICATE...) and its private key (BEGIN ... PRIVATE KEY).")
    if "ENCRYPTED" in key:
        return ("err", "The private key is password-protected; export it without a password.")
    files = {}
    try:
        for name, text in (("crt", cert), ("key", key)):
            fd, files[name] = tempfile.mkstemp(prefix=".upload.", dir=TLS_DIR)
            with os.fdopen(fd, "w") as f:
                f.write(text)
        pub_c = run("openssl", "x509", "-in", files["crt"], "-noout", "-pubkey")
        pub_k = run("openssl", "pkey", "-in", files["key"], "-pubout", "-passin", "pass:")
        if pub_c.returncode or pub_k.returncode:
            return ("err", "That is not a valid PEM certificate and private key.")
        if pub_c.stdout.strip() != pub_k.stdout.strip():
            return ("err", "The private key does not belong to that certificate.")
        if run("openssl", "x509", "-in", files["crt"], "-noout", "-checkend", "0").returncode:
            return ("err", "That certificate has expired.")
        try:
            ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER).load_cert_chain(files["crt"], files["key"])
        except (ssl.SSLError, OSError) as e:
            return ("err", "The certificate can't be used: %s" % e)
        os.replace(files.pop("key"), TLS_KEY)
        os.replace(files.pop("crt"), TLS_CERT)
    finally:
        for f in files.values():
            os.remove(f)
    try:
        os.remove(TLS_SELF)
    except FileNotFoundError:
        pass
    write_atomic(TLS_UPLOADED, time.strftime("%Y-%m-%d %H:%M") + "\n")
    tls_permissions()
    log("tls: uploaded certificate in use")
    return ("ok", "Certificate installed. Reload the page; the browser now sees the new certificate.")


def act_tls_self():
    for f in (TLS_CERT, TLS_SELF, TLS_UPLOADED):
        try:
            os.remove(f)
        except FileNotFoundError:
            pass
    ensure_cert()
    return ("ok", "The device's own certificate is in use again (the browser warns about it once more).")


_cert_cache = {"mtime": None, "info": None}


def cert_info():
    """The certificate being served: kind, names, expiry, SHA-256 fingerprint and public-key pin."""
    try:
        mtime = os.path.getmtime(TLS_CERT)
    except OSError:
        return None
    if _cert_cache["mtime"] == mtime:
        return _cert_cache["info"]
    pem = read_text(TLS_CERT)
    try:
        der = ssl.PEM_cert_to_DER_cert(pem[:pem.index("-----END CERTIFICATE-----") + 25])
    except ValueError:
        return None
    fp = hashlib.sha256(der).hexdigest().upper()
    pub = subprocess.run(["openssl", "x509", "-in", TLS_CERT, "-noout", "-pubkey"], capture_output=True).stdout
    spki = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=pub, capture_output=True).stdout
    text = run("openssl", "x509", "-in", TLS_CERT, "-noout", "-enddate", "-ext", "subjectAltName").stdout
    end = re.search(r"notAfter=(.+)", text)
    names = re.findall(r"(?:DNS|IP Address):([^,\s]+)", text)
    info = {"self": os.path.exists(TLS_SELF), "names": names, "until": end.group(1).strip() if end else "?",
            "fingerprint": ":".join(fp[i:i + 2] for i in range(0, len(fp), 2)),
            "pin": base64.b64encode(hashlib.sha256(spki).digest()).decode() if spki else ""}
    _cert_cache.update(mtime=mtime, info=info)
    return info


def tls_context():
    """Front end: serve the broker's certificate, and pick up a new one when it changes."""
    for _ in range(60):  # the broker makes it at its first start
        if os.path.exists(TLS_CERT) and os.path.exists(TLS_KEY):
            break
        time.sleep(0.5)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(TLS_CERT, TLS_KEY)

    def reload():
        seen = (os.path.getmtime(TLS_CERT), os.path.getmtime(TLS_KEY))
        while True:
            time.sleep(10)
            try:
                now = (os.path.getmtime(TLS_CERT), os.path.getmtime(TLS_KEY))
                if now != seen:
                    ctx.load_cert_chain(TLS_CERT, TLS_KEY)  # new connections get it
                    seen = now
                    log("tls: certificate reloaded")
            except (OSError, ssl.SSLError):
                pass  # being replaced: next round
    threading.Thread(target=reload, daemon=True).start()
    return ctx


def last_package_update():
    """When packages last changed (OS update, install.sh's upgrade, apt by hand)."""
    try:
        return os.path.getmtime("/var/lib/dpkg/status")
    except OSError:
        return 0


def os_update_overdue():
    return time.time() - last_package_update() > OS_UPDATE_OVERDUE_DAYS * 86400


# ---------------------------------------------------------------- broker (root): the only part that changes things

BROKER_MAX = UPDATE_MAX * 4 // 3 + 4 * BODY_MAX  # an update package is the largest request
API_TOKEN_NOTE = "/etc/pindi/api-token.note"
_api_fail_logged = {}


def _send_msg(sock, obj):
    data = json.dumps(obj).encode()
    sock.sendall(struct.pack("!I", len(data)) + data)


def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(min(n - len(buf), 1 << 20))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return bytes(buf)


def _recv_msg(sock, limit):
    (n,) = struct.unpack("!I", _recv_exact(sock, 4))
    if n > limit:
        raise ValueError("message too large")
    return json.loads(_recv_exact(sock, n))


def broker(op, **args):
    """Front end: ask the broker. None if it can't be reached."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(300)
            s.connect(BROKER_SOCK)
            _send_msg(s, dict(args, op=op))
            return _recv_msg(s, 16 << 20)
    except (OSError, ValueError, ConnectionError) as e:
        log("broker %s: %s" % (op, e))
        return None


def token_ok(given):
    return isinstance(given, str) and bool(given) and secrets.compare_digest(given.encode(), api_token().encode())


def api_failure(ip):
    """Wrong API token: slowed down (and logged at most once a minute per address), never locked out."""
    now = time.time()
    if now - _api_fail_logged.get(ip, 0) > 60:
        _api_fail_logged[ip] = now
        log("api: wrong or missing token from %s" % ip)
    time.sleep(1)


def op_login(user="", password="", ip=""):
    user, password = str(user).strip(), str(password)
    key = (str(ip), user.lower())
    wait = max(LOGIN_LOCK.blocked(key), ADDRESS_LOCK.blocked(key[0]))
    if wait:
        log("login refused: %s from %s is locked out for %d s" % (user, ip, wait))
        return {"code": 429, "error": "Too many failed sign-ins. Try again in %d minute%s." %
                ((wait + 59) // 60, "" if wait <= 60 else "s")}
    if user and len(user) <= 64 and is_admin(user) and check_password(user, password):
        LOGIN_LOCK.success(key)
        token = SESSIONS.create(user)
        notes = []
        if password in WELL_KNOWN_PASSWORDS:
            notes.append("You signed in with a well-known default password. Please change it below.")
        if os_update_overdue():
            notes.append("No operating-system updates have been installed for over %d days: install them "
                         "(System) or set a daily time (Device settings)." % OS_UPDATE_OVERDUE_DAYS)
        if notes:
            SESSIONS.get(token)["flash"] = ("warn", " ".join(notes))
        log("login: %s from %s" % (user, ip))
        return {"token": token}
    LOGIN_LOCK.failure(key)
    ADDRESS_LOCK.failure(key[0])
    log("login failed: %r from %s" % (user[:64], ip))
    time.sleep(2)
    return {"code": 401, "error": "Wrong user or password, or the user is not an administrator."}


def op_session(token="", pop_flash=False, with_secrets=False):
    s = SESSIONS.get(token)
    if not s:
        return None
    out = {"user": s["user"], "csrf": s["csrf"], "net": NET.seconds_left() if NET.pending() else None}
    if pop_flash:
        out["flash"], s["flash"] = s["flash"], None
    if with_secrets:
        wifi = read_wifi_config()
        out["secrets"] = {"api_token": api_token(), "token_note": read_text(API_TOKEN_NOTE).strip(),
                          "wifi": {"ssid": wifi["ssid"], "psk": bool(wifi["psk"]), "country": wifi["country"]},
                          "ssids": wifi_scan() if wifi_enabled() else []}
    return out


def op_logout(token=""):
    SESSIONS.drop(token)
    return {}


def _restart(s):
    log("video restart requested by %s" % s["user"])
    later(0.5, restart_media)
    return ("ok", "Video and outgoing streams are restarting.")


def _reboot(s):
    log("reboot requested by %s" % s["user"])
    later(1, run, "systemctl", "reboot")
    return {"page": "reboot"}


def _network_confirm(s):
    NET.confirm()
    return ("ok", "Network settings kept.")


def _network_revert(s):
    NET.revert()
    return ("ok", "Previous network settings restored.")


ACTIONS = {
    "/output": lambda s, f: act_output(f), "/outgoing": lambda s, f: act_outgoing(f),
    "/network": lambda s, f: act_network(f), "/wifi": lambda s, f: act_wifi(f),
    "/network/confirm": lambda s, f: _network_confirm(s), "/network/revert": lambda s, f: _network_revert(s),
    "/password": act_password, "/device": lambda s, f: act_device(f), "/restore": lambda s, f: act_restore(f),
    "/reset": lambda s, f: act_reset(), "/identify": lambda s, f: act_identify(f),
    "/update": lambda s, f: act_update(f), "/os-update": lambda s, f: act_os_update_now(),
    "/api-token": lambda s, f: act_api_token(), "/restart": lambda s, f: _restart(s),
    "/reboot": lambda s, f: _reboot(s), "/tls": lambda s, f: act_tls(f), "/tls/self": lambda s, f: act_tls_self(),
}


def op_action(token="", csrf="", path="", form=None):
    """A change from the web interface: only for a valid session with its CSRF token (checked here, not by
    the front end)."""
    s = SESSIONS.get(token)
    if not s or not isinstance(csrf, str) or not secrets.compare_digest(csrf.encode(), s["csrf"].encode()):
        return {"login": True}
    if path == "/logout":
        SESSIONS.drop(token)
        return {"logout": True}
    fn = ACTIONS.get(path)
    if not fn:
        return {"code": 404}
    form = {str(k): str(v) for k, v in (form or {}).items()} if isinstance(form, dict) else {}
    r = fn(s, form)
    if isinstance(r, dict):
        return r
    s["flash"] = r
    return {"redirect": "/update" if path == "/update" and r[0] == "ok" else "/"}


def op_api_auth(token="", ip=""):
    if token_ok(token):
        return True
    api_failure(ip)
    return False


def op_api_post(token="", ip="", path="", data=None):
    if not token_ok(token):
        api_failure(ip)
        return [401, {"error": "missing or wrong API token"}]
    return list(api_post(str(path), data if isinstance(data, dict) else {}))


def op_http_token(token="", ip=""):
    """A token sent over plain HTTP went over the network unencrypted: if it is the valid one, it is replaced."""
    if not token_ok(token):
        return False
    try:
        os.remove(API_TOKEN_FILE)
    except OSError:
        pass
    api_token()
    write_atomic(API_TOKEN_NOTE, "Replaced on %s: the previous token was sent over plain HTTP (not HTTPS) "
                                 "from %s.\n" % (time.strftime("%Y-%m-%d %H:%M"), ip), 0o600)
    log("api: the token was sent over plain HTTP from %s: replaced with a new one" % ip)
    return True


BROKER_OPS = {"login": op_login, "session": op_session, "logout": op_logout, "action": op_action,
              "api_auth": op_api_auth, "api_post": op_api_post, "http_token": op_http_token}


class BrokerHandler(socketserver.BaseRequestHandler):
    def handle(self):
        cred = self.request.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
        _pid, uid, _gid = struct.unpack("3i", cred)
        if uid not in self.server.allowed:
            return
        self.request.settimeout(120)
        try:
            msg = _recv_msg(self.request, BROKER_MAX)
            fn = BROKER_OPS[msg.pop("op")]
        except (OSError, ValueError, KeyError, TypeError, ConnectionError):
            return
        try:
            result = fn(**msg)
        except TypeError:
            result = {"code": 400}
        except Exception:
            log("broker: %s" % traceback.format_exc())
            result = {"code": 500}
        try:
            _send_msg(self.request, result)
        except OSError:
            pass


class BrokerServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True


def broker_main():
    """Root, no network access (pindi-webd.service): serves the front end on BROKER_SOCK."""
    try:
        web = pwd.getpwnam(WEB_USER)
        allowed, gid = {0, web.pw_uid}, web.pw_gid
    except KeyError:
        allowed, gid = {0}, 0
    os.makedirs(BROKER_DIR, exist_ok=True)
    os.chown(BROKER_DIR, 0, gid)
    os.chmod(BROKER_DIR, 0o750)
    os.makedirs(CTL_DIR, exist_ok=True)
    os.chmod(CTL_DIR, 0o755)
    if update_state() != "running":
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # an earlier update's unpacked files
    ensure_cert()

    def watch():  # a new address or host name: a certificate that matches it
        while True:
            time.sleep(60)
            try:
                ensure_cert()
            except Exception as e:
                log("tls: %s" % e)
    threading.Thread(target=watch, daemon=True).start()
    try:
        os.remove(BROKER_SOCK)
    except FileNotFoundError:
        pass
    old = os.umask(0o117)
    srv = BrokerServer(BROKER_SOCK, BrokerHandler)
    os.umask(old)
    os.chown(BROKER_SOCK, 0, gid)
    srv.allowed = allowed
    log("PiNDI web broker on %s" % BROKER_SOCK)
    srv.serve_forever()


# ---------------------------------------------------------------- front end (pindi-web): HTTP

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "PiNDI"
    sys_version = ""
    timeout = REQUEST_TIMEOUT

    def log_message(self, fmt, *args):
        pass

    # --- plumbing
    def cookie_token(self):
        try:
            c = http.cookies.SimpleCookie(self.headers.get("Cookie", ""))
        except http.cookies.CookieError:
            return None
        return c["pindi_session"].value if "pindi_session" in c else None

    def session(self, **opts):
        token = self.cookie_token()
        return broker("session", token=token, **opts) if token else None

    def given_token(self):
        given = self.headers.get("X-API-Key", "")
        auth = self.headers.get("Authorization", "")
        if auth.lower().startswith("bearer "):
            given = auth[7:].strip()
        return given

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

    def unavailable(self):
        return self.send(503, "The web interface's broker (pindi-webd) is not running.", "text/plain")

    def body(self, limit=BODY_MAX):
        """The request body, or None (and an error sent) when its length is missing, invalid or over limit.
        The connection's timeout bounds how long a slow sender can take."""
        length = self.headers.get("Content-Length", "0").strip()
        if not length.isdigit():  # also refuses negative lengths, which would read until the client closes
            self.send(400, "Bad Content-Length", "text/plain")
            return None
        if int(length) > limit:
            self.send(413, "Request too large", "text/plain")
            return None
        try:
            return self.rfile.read(int(length))
        except OSError:  # timed out or dropped
            self.close_connection = True
            return None

    def form(self, limit=BODY_MAX):
        raw = self.body(limit)
        if raw is None:
            return None
        return {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode("utf-8", "replace"),
                                                           keep_blank_values=True).items()}

    # --- routes
    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/style.css":
            return self.send(200, CSS, "text/css")
        if path == "/app.js":
            return self.send(200, JS, "application/javascript")
        if path in ("/favicon.svg", "/favicon.ico"):
            return self.send(200, FAVICON, "image/svg+xml")
        if path.startswith("/api/v1/"):
            if not broker("api_auth", token=self.given_token(), ip=self.client_address[0]):
                return self.send(401, json.dumps({"error": "missing or wrong API token"}), "application/json")
            if path == "/api/v1/status":
                return self.send(200, json.dumps(api_status(), indent=1), "application/json")
            return self.send(404, json.dumps({"error": "unknown endpoint"}), "application/json")
        s = self.session(pop_flash=path == "/", with_secrets=path in ("/", "/diagnostics"))
        if path == "/login":
            return self.redirect("/") if s else self.send(200, login_page())
        if not s:
            return self.redirect("/login") if path == "/" else self.send(401, "Unauthorized", "text/plain")
        if path == "/":
            return self.send(200, dashboard(s))
        if path == "/logs":
            return self.send(200, logs_page(s))
        if path == "/os-update":
            return self.send(200, os_update_page(s))
        if path == "/update":
            return self.send(200, update_page(s))
        if path == "/update/log":
            return self.send(200, json.dumps({"state": update_state(), "log": read_text(UPDATE_LOG)[-20000:]}),
                             "application/json")
        m = re.match(r"^/thumb/(\d)\.jpg$", path)
        if m:
            # Asking for it keeps the player rendering it (for 15 s); the first request may find none yet.
            n = int(m.group(1))
            req = "%s/out%d.thumbreq" % (WEB_RUN, n)
            try:
                with open(req, "a"):
                    os.utime(req)
                with open("%s/out%d.jpg" % (RUN_DIR, n), "rb") as f:
                    return self.send(200, f.read(), "image/jpeg")
            except OSError:
                return self.send(404, "", "text/plain")
        stamp = time.strftime("%Y%m%d-%H%M")
        if path == "/diagnostics":
            return self.send(200, diagnostics_text(s["secrets"]["wifi"]), "text/plain; charset=utf-8", [
                ("Content-Disposition", 'attachment; filename="pindi-%s-diagnostics-%s.txt"' % (SERIAL, stamp))])
        if path == "/backup":
            return self.send(200, backup_json(), "application/json", [
                ("Content-Disposition", 'attachment; filename="pindi-%s-settings-%s.json"' % (SERIAL, stamp))])
        if path == "/api/status":
            st = {"ip": live_network()["address"], "rollback": s["net"], "net": TRAFFIC.text()}
            st["health"], st["power"] = health()
            for o in outputs():
                for k in ("state", "target", "source", "video", "shown", "format", "audio", "latency"):
                    st["out%d.%s" % (o["n"], k)] = o[k]
            # Status files only: cheap enough to poll (no device enumeration).
            for n, conf in configured_inputs(read_env()).items():
                if conf.get("ENABLED") == "1":
                    st["in%d.state" % n] = read_kv("%s/in%d.status" % (RUN_DIR, n)).get("state", "starting")
            return self.send(200, json.dumps(st), "application/json")
        self.send(404, "Not found", "text/plain")

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if path.startswith("/api/v1/"):
            token, ip = self.given_token(), self.client_address[0]
            if not broker("api_auth", token=token, ip=ip):  # before reading the body
                return self.send(401, json.dumps({"error": "missing or wrong API token"}), "application/json")
            raw = self.body()
            if raw is None:
                return
            raw = raw.decode("utf-8", "replace")
            try:
                data = json.loads(raw) if raw.strip().startswith("{") else {
                    k: v[0] for k, v in urllib.parse.parse_qs(raw, keep_blank_values=True).items()}
            except ValueError:
                return self.send(400, json.dumps({"error": "body must be JSON or form data"}), "application/json")
            r = broker("api_post", token=token, ip=ip, path=path, data=data if isinstance(data, dict) else {})
            if not r:
                return self.send(503, json.dumps({"error": "broker not running"}), "application/json")
            return self.send(r[0], json.dumps(r[1]), "application/json")
        if path == "/update" and not self.session():
            return self.redirect("/login")  # the one large body: only read for a signed-in user
        form = self.form(UPDATE_MAX * 4 // 3 + BODY_MAX if path == "/update" else BODY_MAX)
        if form is None:
            return
        if path == "/login":
            r = broker("login", user=form.get("user", ""), password=form.get("password", ""),
                       ip=self.client_address[0])
            if r is None:
                return self.unavailable()
            if "token" in r:
                return self.redirect("/", [("Set-Cookie", "pindi_session=%s; Path=/; Secure; HttpOnly; "
                                                          "SameSite=Strict" % r["token"])])
            return self.send(r.get("code", 401), login_page(r.get("error", "")))
        r = broker("action", token=self.cookie_token() or "", csrf=form.pop("csrf", ""), path=path, form=form)
        if r is None:
            return self.unavailable()
        if r.get("login"):
            return self.redirect("/login")
        if r.get("logout"):
            return self.redirect("/login", [("Set-Cookie", "pindi_session=; Path=/; Secure; Max-Age=0")])
        if r.get("page") == "reboot":
            return self.send(200, page("PiNDI", '<div class="card"><h2>Rebooting&hellip;</h2>'
                                                 '<p>The device will be back in about 20 seconds.</p></div>'))
        if "redirect" in r:
            return self.redirect(r["redirect"])
        self.send(r.get("code", 404), "Not found", "text/plain")


class RedirectHandler(Handler):
    """Plain HTTP (port 80). Browsers asking for a page are sent to HTTPS. Nothing else is accepted: no
    sign-in, no form, no API call, so nothing is redirected with its body. A request carrying the valid API
    token has already sent it unencrypted, so that token is replaced (the client must then be fixed)."""

    def refuse(self):
        token = self.given_token()
        leaked = bool(token) and broker("http_token", token=token, ip=self.client_address[0])
        msg = "Use https:// - plain HTTP is not accepted for sign-in, forms or the API."
        if leaked:
            msg += (" The API token sent here travelled unencrypted and has been replaced: get the new one from "
                    "Device settings and use https:// in the client.")
        self.close_connection = True
        self.send(403, json.dumps({"error": msg}) + "\n", "application/json", [("Connection", "close")])

    def do_GET(self):
        if self.given_token() or self.path.startswith("/api/"):
            return self.refuse()
        host = self.headers.get("Host", "")
        if not re.match(r"^[A-Za-z0-9.-]{1,253}(:\d{1,5})?$", host):
            host = self.connection.getsockname()[0]  # no usable Host header: the address it came in on
        host = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
        if PORT != 443:
            host += ":%d" % PORT
        self.send_response(301)
        self.send_header("Location", "https://%s%s" % (host, self.path if self.path.startswith("/") else "/"))
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    do_HEAD = do_GET
    do_POST = do_PUT = do_DELETE = do_PATCH = refuse


class Server(http.server.ThreadingHTTPServer):
    """At most MAX_CONNECTIONS handled at once (shared by both ports), MAX_PER_ADDRESS from one address. With
    tls, the handshake runs in the connection's own thread under REQUEST_TIMEOUT, so a slow client can't
    hold up the others."""
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64
    conn_lock = threading.Lock()
    conns = {}  # address -> connections being handled

    def __init__(self, addr, handler, tls=None):
        self.tls = tls
        super().__init__(addr, handler)

    def _release(self, ip):
        with self.conn_lock:
            self.conns[ip] -= 1
            if not self.conns[ip]:
                del self.conns[ip]

    def process_request(self, request, client_address):
        ip = client_address[0]
        with self.conn_lock:
            busy = sum(self.conns.values()) >= MAX_CONNECTIONS or self.conns.get(ip, 0) >= MAX_PER_ADDRESS
            if not busy:
                self.conns[ip] = self.conns.get(ip, 0) + 1
        if busy:
            self.shutdown_request(request)  # too many at once: drop this one
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._release(ip)
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._release(client_address[0])

    def finish_request(self, request, client_address):
        request.settimeout(REQUEST_TIMEOUT)
        if not self.tls:
            return super().finish_request(request, client_address)
        try:
            conn = self.tls.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return  # not TLS, a refused certificate, or too slow
        try:
            self.RequestHandlerClass(conn, client_address, self)
        finally:
            conn.close()

    def handle_error(self, request, client_address):
        pass  # broken connections are normal (browsers, port scans)


def main():
    if "--broker" in sys.argv:
        return broker_main()
    if os.geteuid() == 0:
        log("warning: the front end is running as root; it is meant to run as %s (pindi-web.service)" % WEB_USER)
    TRAFFIC.start()
    srv = Server(("", PORT), Handler, tls_context())
    if HTTP_PORT:
        threading.Thread(target=Server(("", HTTP_PORT), RedirectHandler).serve_forever, daemon=True).start()
    log("PiNDI web interface on https port %d%s" % (PORT, " (http %d redirects)" % HTTP_PORT if HTTP_PORT else ""))
    srv.serve_forever()


if __name__ == "__main__":
    main()

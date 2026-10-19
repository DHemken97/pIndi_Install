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
    cfg = {"dhcp": True, "address": "", "prefix": "", "gateway": "", "dns": []}
    for line in read_text(NETWORK_FILE).splitlines():
        key, _, val = line.strip().partition("=")
        if key == "Address":
            cfg["dhcp"] = False
            cfg["address"], _, cfg["prefix"] = val.partition("/")
        elif key == "Gateway":
            cfg["gateway"] = val
        elif key == "DNS":
            cfg["dns"] += val.split()
    return cfg


def render_network_config(dhcp, iface=None, gateway="", dns=()):
    text = "# Written by the PiNDI web interface.\n[Match]\nType=ether\n\n[Network]\n"
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


def outputs():
    """Display outputs as detected by the player, with their settings and live status."""
    env = read_env()
    running = run("systemctl", "is-active", "pindi.service").stdout.strip() == "active"
    out = []
    for line in read_text(RUN_DIR + "/outputs").splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        n = int(parts[0])
        st = read_kv("%s/out%d.status" % (RUN_DIR, n))
        if not running:
            st["state"] = "player not running"
        out.append({
            "n": n, "connector": parts[1], "link": parts[2], "mode": parts[3] if len(parts) > 3 else "",
            "enabled": out_setting(env, n, "ENABLED", "1") != "0",
            "stream": out_setting(env, n, "STREAM"), "source_ip": out_setting(env, n, "SOURCE_IP"),
            "rotate": out_setting(env, n, "ROTATE", "0"), "mirror": out_setting(env, n, "MIRROR", "none"),
            "state": st.get("state", "-"), "target": st.get("target", default_out_stream(n)),
            "source": st.get("source", ""), "video": st.get("video", ""),
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
        m = re.match(r"^PINDI_IN(\d+)_(DEVICE|ENABLED|NAME|MODE)$", key)
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
                        name=s.get("NAME", ""), mode=s.get("MODE", "")))
    for dev, slot in by_dev.items():
        s = conf[slot]
        out.append({"dev": "", "id": dev, "label": os.path.basename(dev), "modes": [], "slot": slot,
                    "present": False, "enabled": s.get("ENABLED") == "1", "name": s.get("NAME", ""),
                    "mode": s.get("MODE", "")})
    for i in out:
        st = read_kv("%s/in%d.status" % (RUN_DIR, i["slot"])) if i["slot"] else {}
        i["state"] = st.get("state", "") if i["enabled"] else "off"
        i["live_mode"] = st.get("mode", "")
        i["fps"] = st.get("fps", "")
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
input[type=text],input[type=password],select{width:100%;padding:9px 10px;border-radius:7px;border:1px solid var(--line);
background:#0d1319;color:var(--text);font:inherit}
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
.card dl{margin-bottom:6px}
form.inline{display:inline}
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
if(/state$/.test(k))el.className='state'+(v==='playing'||/^sending/.test(v||'')?' playing':'');});
var c=document.getElementById('countdown');if(c&&d.rollback!=null)c.textContent=d.rollback;}).catch(function(){});}
sync();if(document.querySelector('[data-k]'))setInterval(poll,3000);
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
    rows = [
        ("IP address", '<span data-k="ip">%s</span>' % esc(net["address"] or "-")),
        ("Serial", esc(SERIAL)),
        ("Hostname", esc(socket.gethostname())),
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
            '<dl><dt>State</dt><dd><span data-k="%sstate" class="state%s">%s</span></dd>'
            '<dt>Looking for</dt><dd>%s</dd>'
            '<dt>Connected to</dt><dd data-k="%ssource">%s</dd>'
            '<dt>Video</dt><dd data-k="%svideo">%s</dd></dl>' %
            (k, " playing" if o["state"] == "playing" else "", esc(o["state"]), esc(o["target"]),
             k, esc(o["source"] or "-"), k, esc(o["video"] or "-")))
        parts.append(
            '<section class="card" id="out%d"><h2>Output %d &middot; %s %s</h2>%s'
            '<form method="post" action="/output">%s<input type="hidden" name="n" value="%d">'
            '<label class="check"><input type="checkbox" name="enabled" value="1"%s> Enabled '
            '<span class="hint">(off: no NDI is received and this HDMI port is switched off)</span></label>'
            '<div class="row"><div><label>Stream name</label><input type="text" id="stream-%d" name="stream" '
            'value="%s" placeholder="%s" maxlength="200"></div><div><label>Source IP</label>'
            '<input type="text" id="source_ip-%d" name="source_ip" value="%s" placeholder="Any" maxlength="15">'
            '</div><div><label>Rotation</label><select name="rotate">%s</select></div>'
            '<div><label>Mirror</label><select name="mirror">%s</select></div></div>'
            '<p class="hint">Empty stream name = %s, empty IP = any host. A source IP is also searched '
            'directly, so senders on other subnets work. 180&deg; is for ceiling mounting, mirror for rear '
            'projection.</p><button>Save output %d</button> '
            '<button class="secondary" name="reset" value="1">Reset to default</button></form></section>' %
            (n, n, esc(o["connector"]), badge, status_rows, hidden, n, " checked" if o["enabled"] else "",
             n, esc(o["stream"]), esc(default_out_stream(n)), n, esc(o["source_ip"]),
             options(ROTATIONS, o["rotate"]), options(MIRRORS, o["mirror"]), esc(default_out_stream(n)), n))

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
    cards = []
    for i in ins:
        slot = i["slot"] or (free_slots.pop(0) if free_slots else 0)
        mode_choices = [("", "Automatic (best up to 1080p30)")] + i["modes"]
        if i["mode"] and i["mode"] not in dict(i["modes"]):
            mode_choices.append((i["mode"], i["mode"]))
        state = i["state"] or ("starting" if i["enabled"] else "off")
        live = (" &middot; %s &middot; %s fps" % (esc(i["live_mode"]), esc(i["fps"]))) \
            if i["enabled"] and i["live_mode"] else ""
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
            '</div></div><button>Save</button> %s</form></div>' %
            (esc(i["label"]), " on" if i["present"] else "", "connected" if i["present"] else "not connected",
             esc(i["id"]), k, " playing" if state.startswith("sending") else "", esc(state), live, hidden,
             esc(i["id"]), slot, " checked" if i["enabled"] else "", esc(i["name"]), esc(default_in_stream(slot)),
             options(mode_choices, i["mode"]), remove))
    body = "".join(cards) if cards else (
        '<p class="hint">No capture devices detected. Plug in a USB HDMI capture card and reload this page.</p>')
    parts.append('<section class="card" id="outgoing"><h2>Configure Outgoing Streams</h2>'
                 '<p class="hint">Nothing is sent until you enable it here. The stream appears on the network as '
                 '"%s (name)".</p>%s</section>' % (esc(socket.gethostname().upper()), body))

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
        '<p class="hint">After applying, open the device at its new address within %d seconds and press '
        '<b>Keep</b>, otherwise the previous settings come back automatically. The current IP is always '
        'shown on the splash screen.</p><button>Apply network settings</button></form></section>' %
        (hidden, "checked" if cfg["dhcp"] else "", "" if cfg["dhcp"] else "checked",
         esc(fill["address"]), esc(mask), esc(fill["gateway"]), esc(", ".join(fill["dns"])), ROLLBACK_SECONDS))

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
        '<section class="card"><h2>System</h2><form method="post" action="/reboot">%s'
        '<button class="danger">Reboot</button></form></section>' % hidden)

    return page("PiNDI", "".join(parts), session["user"], csrf)


# ---------------------------------------------------------------- actions

ROTATIONS = [("0", "0&deg; (normal)"), ("180", "180&deg; (upside down)")]
MIRRORS = [("none", "None"), ("horizontal", "Horizontal (left/right)"), ("vertical", "Vertical (top/bottom)")]
LEGACY_OUT1 = ("STREAM", "SOURCE_IP", "ROTATE", "MIRROR")


def valid_name(s):
    return len(s) <= 200 and not any(ord(c) < 32 for c in s)


def act_output(form):
    try:
        n = int(form.get("n", ""))
    except ValueError:
        return ("err", "Unknown output.")
    if n not in [o["n"] for o in outputs()]:
        return ("err", "Unknown output.")
    keys = ("ENABLED", "STREAM", "SOURCE_IP", "ROTATE", "MIRROR")
    updates = {"PINDI_OUT%d_%s" % (n, k): "" for k in keys}
    if n == 1:  # settings from before multi-output support
        updates.update({"PINDI_" + k: "" for k in LEGACY_OUT1})
    if not form.get("reset"):
        enabled = form.get("enabled") == "1"
        stream = form.get("stream", "").strip()
        source_ip = form.get("source_ip", "").strip()
        rotate, mirror = form.get("rotate", "0"), form.get("mirror", "none")
        if not valid_name(stream):
            return ("err", "Invalid stream name.")
        if source_ip:
            try:
                source_ip = str(ipaddress.IPv4Address(source_ip))
            except ValueError:
                return ("err", "Source IP must be an IPv4 address, or empty for any host.")
        if rotate not in dict(ROTATIONS) or mirror not in dict(MIRRORS):
            return ("err", "Invalid display setting.")
        updates.update({
            "PINDI_OUT%d_ENABLED" % n: "" if enabled else "0",
            "PINDI_OUT%d_STREAM" % n: "" if stream == default_out_stream(n) else stream,
            "PINDI_OUT%d_SOURCE_IP" % n: source_ip,
            "PINDI_OUT%d_ROTATE" % n: "" if rotate == "0" else rotate,
            "PINDI_OUT%d_MIRROR" % n: "" if mirror == "none" else mirror,
        })
    write_env(updates)
    later(0.5, run, "systemctl", "restart", "pindi.service")
    o = next(o for o in outputs() if o["n"] == n)
    if not o["enabled"]:
        log("output %d: disabled" % n)
        return ("ok", "Output %d disabled. The player is restarting." % n)
    log("output %d: stream=%s ip=%s rotate=%s mirror=%s" % (
        n, o["stream"] or default_out_stream(n), o["source_ip"] or "any", o["rotate"], o["mirror"]))
    return ("ok", "Output %d saved: %s from %s, rotation %s°, mirror %s. The player is restarting." % (
        n, o["stream"] or default_out_stream(n), o["source_ip"] or "any host", o["rotate"], o["mirror"]))


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
    keys = ["PINDI_IN%d_%s" % (slot, k) for k in ("ENABLED", "DEVICE", "NAME", "MODE")]
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
        write_env(dict(zip(keys, ["1" if enabled else "0", device,
                                  "" if name == default_in_stream(slot) else name, mode])))
        msg = ("%s will be sent as \"%s\"." % (inp["label"], name or default_in_stream(slot))) if enabled \
            else ("%s is not sent." % inp["label"])
    log("outgoing: slot %d %s" % (slot, msg))
    later(0.5, run, "systemctl", "restart", "pindi-sender.service")
    return ("ok", msg)


def act_network(form):
    if form.get("mode") == "dhcp":
        text = render_network_config(True)
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
        text = render_network_config(False, iface, gateway, dns)
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
        if path == "/api/status":
            st = {"ip": live_network()["address"], "rollback": NET.seconds_left() if NET.pending() else None}
            for o in outputs():
                for k in ("state", "source", "video"):
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
            if user and is_admin(user) and check_password(user, password):
                token = SESSIONS.create(user)
                log("login: %s from %s" % (user, self.client_address[0]))
                return self.redirect("/", [("Set-Cookie", "pindi_session=%s; Path=/; HttpOnly; SameSite=Strict"
                                            % token)])
            log("login failed: %r from %s" % (user, self.client_address[0]))
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
    srv = Server(("", PORT), Handler)
    log("PiNDI web interface on port %d" % PORT)
    srv.serve_forever()


if __name__ == "__main__":
    main()

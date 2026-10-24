# PiNDI

PiNDI turns a Raspberry Pi into an NDI display decoder and encoder:

- **Decoder:** each HDMI output shows an NDI stream full screen, with sound on
  HDMI or the headphone jack.
- **Encoder:** USB HDMI capture devices (and webcams) are sent out as NDI
  streams, with their audio.
- **Web interface:** everything is set up from a browser, with a token HTTP API
  for control systems.

It boots straight to a splash screen (under a second after the kernel) that
shows the device's IP address, serial number and what it is waiting for.

Supported: Raspberry Pi 3, 4 and 5 (and 400/500, CM4/CM5) on Raspberry Pi OS
Lite, 32 or 64 bit. A Pi 3 has one HDMI output and 100 Mbit/s Ethernet. A Pi 4
or 5 has two HDMI outputs and Gigabit Ethernet.

## Install

1. Write Raspberry Pi OS Lite to an SD card with Raspberry Pi Imager. Set a
   user name and password and enable SSH.
2. Download the NDI SDK for Linux from https://ndi.video/sdk/ (it can't be
   included here) and put `Install_NDI_SDK_v6_Linux.tar.gz` in this folder, or
   `Install_NDI_Advanced_SDK_v6_Linux.tar.gz` for NDI|HX receive.
3. Copy this folder to the Pi, for example to `/home/<user>/pindi`.
4. On the Pi, run:

   ```sh
   cd ~/pindi && bash install.sh
   ```

   The script asks you to accept the NDI SDK licence; `--accept-ndi-license`
   skips the question. When it's done, the Pi reboots.

`install.sh` is safe to run again. Options:

- `--skip-upgrade` doesn't run `apt full-upgrade`.
- `--offline` doesn't use apt at all; the packages must already be installed.
- `--no-reboot` skips the reboot at the end. Without a reboot, restart the
  services yourself: `sudo systemctl restart pindi pindi-sender pindi-web`.

The NDI SDK archive is downloaded by hand, and NDI publishes no checksums.
`install.sh` prints the archive's SHA-256. If a checksum file
`Install_NDI_..._Linux.tar.gz.sha256` (from `sha256sum`) is next to the
archive, an archive that doesn't match it is refused, so every device gets
the same SDK.

To remove PiNDI again, run `bash uninstall.sh`. It deletes PiNDI and its
settings, and gives the system back its own boot settings, console, services
and NetworkManager.

## Updating

Run `bash make-update.sh` in this folder. It builds
`pindi-update-<version>.tar.gz`, about 160 KB because the NDI SDK isn't
included; devices keep the SDK they have.

The package is **signed**. Devices only install packages signed with the
private key that matches the `update-key.pub` they were installed with:

- The private key stays on the computer that makes updates, at
  `~/.pindi/update-signing-key.pem` (or the path in `PINDI_SIGNING_KEY`). The
  first run of `make-update.sh` creates it, together with `update-key.pub`.
  Commit `update-key.pub`; never commit or share the private key.
- **Back up the private key.** Without it, devices that have the matching
  `update-key.pub` can only be updated by running `install.sh` over SSH.

Install the package on a device under **System → Software update**:

- The signature and every file's hash are checked first. Unsigned, changed or
  differently signed packages are refused, and so are older versions unless
  **Allow an older version** is ticked.
- It installs offline, keeps all settings, shows the log live, and reboots when
  it's done (about 2 minutes on a Pi 3).
- If the install fails, the files it already replaced are put back and the
  previous version keeps running; the log shows why. The previous version's
  files are also kept in `/var/lib/pindi/previous`.

Running `install.sh` again over SSH also works.

## First steps

Open `https://<IP shown on the splash>/` and sign in with the Pi's own user (a
member of the `sudo` group, e.g. the user from step 1). If you signed in with a
well-known default password, the page reminds you to change it.

The device makes its own certificate, so the browser warns the first time. To
be sure it's this device, compare the browser's certificate fingerprint with the
one under **Device settings → HTTPS certificate**, then accept it. To avoid the warning altogether, upload a
certificate from your organisation's certificate authority there. `http://`
page addresses are redirected to `https://`.

Out of the box, output 1 shows the stream `OUT_<serial>` and output 2 shows
`OUT_<serial>_2`, from any host. Either name your sender like that, or pick a
stream for each output in the web interface. The list of NDI sources on the
network has a **Use** button for each.

Outputs showing the same stream share one receiver: it is received and decoded
once, and each output shows the decoded picture with its own rotation, mirror,
brightness and audio settings.

## Which Pi for which job

Measured with USB capture cards (MJPEG) and 1080p monitors; the details are
under [Outgoing streams](#outgoing-streams) and [NDI|HX](#ndihx).

| Board | One input (send) | One output (show) | Two outputs | Input and output |
| --- | --- | --- | --- | --- |
| Pi 3B / 3B+ | Limited: NDI\|HX 720p60 at 50 fps, 1080p at 19-28 fps; Full NDI 7-16 fps | Yes: Full NDI 1080p60 at 55-59 fps, HX 1080p30 at full rate (about 45 ms on the Pi) | No (one HDMI port) | Only at 720p60 HX (50 fps out, 52-57 ms display delay); at 1080p the display delay reaches 200-900 ms |
| Pi 4B | Yes: 1080p30 HX at the full rate (1080p60 card about 28 fps). Two cards: 24-27 fps each with one at 720p, CPU at its limit | Yes: 60 fps | Yes: 1080p60 on both, no dropped frames, about 40% CPU | Yes: sends about 28 fps HX while showing two streams, 33-40 ms (86% CPU) |
| Pi 5 | Full NDI only (no H.264 encoder, so no HX); not measured yet | Yes: 1080p60 at 60 fps, about 21 ms on the Pi | Best: two different 1080p60 streams at 60 fps, 38% CPU; HX decoded in software at full rate | Not measured yet |

In short: a Pi 3 does one job (shows one stream, or sends one input at 720p
HX); a Pi 4 is the all-rounder and the board for capture; a Pi 5 is the best
display box. Wherever a Pi 3 or 4 shows a stream, have it sent as NDI|HX: a
tenth of the bandwidth, and decoded in hardware with almost no CPU. Give a
Pi 5 a heatsink before heavy use; a Pi 3/4 with under-voltage runs at half
speed (see [Troubleshooting](#troubleshooting)).

## Web interface

| Section | What you can do |
| --- | --- |
| Device | Live network traffic, CPU temperature and load, under-voltage and throttling warnings, NDI SDK version, software version, clock |
| Output *n* | Live preview of the screen, state, latency, format and audio. Settings: stream, source IP, backup stream, receive quality (full or low-bandwidth preview), display mode, rotation (0/180°), mirroring, display standby, audio output and delay. **Identify** shows "Output n" on that screen for 15 s. |
| Outgoing streams | Each USB capture device: send on/off, NDI name, capture mode, audio on/off, audio timing |
| Device settings | Device name (NDI name and host name), NDI Discovery Server, NDI groups, NDI receive transport (TCP or UDP), large network buffers, NDI Advanced SDK licence (vendor name and ID), time zone, daily reboot, daily OS updates (off by default), splash options, HTTP API token |
| Network | DHCP or static IP, NTP servers. A change undoes itself after 2 minutes unless you press **Keep** at the new address. |
| Wi-Fi | Off by default: the Wi-Fi hardware is switched off in `config.txt`, so the device starts faster. On: one network (name, password, country), address from DHCP, wired stays the preferred route. Switching it on the first time needs a reboot. Nearby networks are offered in the name field. |
| System | Log viewer, diagnostics download, settings backup and restore, reset to defaults, restart video, identify all screens, reboot |

### How outputs behave

- **Switching streams** happens live, in about a second, without restarting
  the player. The last picture stays on screen until the new stream's first
  frame arrives.
- **Losing the stream:** the splash comes back about 3 s after the stream is
  lost, and the output reconnects by itself. With a **backup stream** set, the
  backup takes over instead (the last picture stays up meanwhile), and the
  output switches back as soon as the main stream returns.
- **Frame timing:** with **Lowest delay** (the default) up to two decoded
  frames wait for the next screen refresh, so a decoder that delivers two at
  once loses neither. When two frames have been waiting for half a second (a
  sender at exactly the display's rate never catches up), one is skipped,
  saving a refresh of delay: 1080p60 on a Pi 5 went from 33 to 21 ms on the Pi.
  With two different 60 fps streams this shows a frame twice about five times
  a minute instead of once. **Smooth** puts frames on screen at an even
  cadence instead.
- **Display standby** switches the HDMI signal off after the chosen time
  without video, so the monitor sleeps. It comes back on when the stream
  appears. With **HDMI-CEC** ticked, a TV is also sent Standby and Image View
  On.
- **Audio:** the first two channels play at the delay you set (80 ms by
  default). If a sender's audio arrives unevenly, the delay grows automatically
  so the sound doesn't drop out.

### Outgoing streams

Capture devices send nothing until you enable them. An enabled device uses no
encoding CPU while no one is receiving it. If its picture stops (for example
the HDMI source is unplugged), receivers get black instead of a frozen frame.
When a switcher (vMix, TriCaster, ...) has the stream on program, the web
interface shows it **ON AIR** (or "on preview").

Inputs are USB (UVC) capture devices: MJPEG, YUYV or UYVY. HDMI-to-CSI boards
(TC358743) are not supported yet: they need the board's overlay, an EDID and the
source's timings set up, which PiNDI doesn't do. On a Pi 5 (two 4-lane camera
ports) they could take 1080p60 without any MJPEG decoding; a Pi 3/4 camera port
has 2 lanes, enough for about 1080p30.

MJPEG from capture cards is decoded on two CPU cores (three with NDI|HX), so
1080p60 cards deliver their full frame rate. An output showing one of the
device's own inputs takes the picture and sound straight from it, without NDI
encoding and decoding (the status says "Direct from the capture input"); NDI
is then only encoded while another device receives the stream.

**Sending and showing at the same time on a Pi 3B** shares four slow cores,
one memory bus and one video unit. Measured with a 1080p60 capture card:
showing Full NDI 1080p60 leaves the outgoing stream about 10 fps; with NDI|HX
in both directions (hardware decoding and encoding) the display gets about
33 fps and the outgoing stream about 19 fps. A Pi 3B+ (with a fan) is CPU-bound
the same way: Full NDI both ways gives about 39 fps on screen and 7 fps out;
sending HX gives about 19 fps out and 22 fps on screen. Showing Full NDI
1080p60 alone, a 3B+ holds 59 fps. A Pi 3B or 3B+ is a one-input device:
three capture cards on a 3B+ got about 5 fps each. A Pi 4B showing Full NDI 1080p60
on one output and 1080p on the other (no capture) holds 60 fps with no dropped
frames at about 40% CPU and 66 °C, unthrottled. A Pi 4B sends one 1080p30
card as NDI|HX at the full 30 fps; two 1080p30 cards are CPU-bound by the
MJPEG decoding (21 and 14 fps with one of them Full NDI). Both as NDI|HX with
one card at 720p30 gives 24-27 fps each at about 95% CPU. A Pi 5 (1 GB, no
heatsink, room fan) shows two different Full NDI 1080p60 streams at 60 fps
each with no dropped frames, at about 38% CPU and 47-50 °C (10 minutes),
unthrottled, with 20-35 ms spent on the Pi; one stream on both outputs
(decoded once) takes about 16% CPU.

## NDI|HX

The standard NDI SDK sends and receives **Full NDI (SpeedHQ)** only.
Senders that can do both (e.g. NDI Screen Capture HX) automatically send Full
NDI to PiNDI. Cameras that only send HX will not show a picture.

To receive HX, put `Install_NDI_Advanced_SDK_v6_Linux.tar.gz` next to
`install.sh` instead; the installer then also installs FFmpeg's libavcodec,
which that SDK uses to decode H.264/HEVC. The sender chooses the codec; the
web interface shows which one a stream uses and how many frames per second
actually reach the screen. On a Pi 3 or 4, H.264 HX is decoded by the Pi's
hardware decoder (1080p60 on a Pi 3B and 4B). The decoder is shared, so a
Pi 4 with two different HX 1080p60 streams shows each at about 49 fps with
about 250 ms of delay; use Full NDI (or 30 fps HX) for the second one. The same
HX stream on both outputs is decoded only once. HEVC HX, and everything on a Pi 5
(which has no H.264 decoder), is decoded in software by the NDI library. That
manages only a few frames per second at 1080p on a Pi 3, but a Pi 5 shows two
H.264 HX streams (1080p30 and 720p60) at their full rate with 10-24 ms on the
Pi, at about 15% CPU: less delay than a Pi 3's hardware decoder (about 45 ms).
If a sender switches to HX while it is being shown, the next format check (at
most a minute later) moves a Pi 3/4 to its hardware decoder.

With the Advanced SDK on a Pi 3 or 4, an outgoing stream can also be sent as
**NDI|HX** (Codec under Outgoing streams): the Pi's hardware encoder makes
the H.264, so sending costs almost no CPU and a tenth of the bandwidth
(about 16 Mbit/s at 1080p60). Receivers need NDI|HX support (NDI Tools, vMix,
OBS and most NDI software have it).

The Pi 3/4 encoder manages about 1080p30: a 1080p60 card is sent at 25-30 fps
(frames the encoder has no room for are not even decoded). Encoder and HX
decoder share one video unit, so a Pi 3B/3B+ that both sends and shows HX at
1080p gets up to half a second of display delay. Measured with three Pis
sending to and showing each other over HX:

| Pi | Sends | Shows | CPU |
|---|---|---|---|
| 3B+, card at 1080p60 | 23 fps | 26 fps, 200-900 ms delay | 59% |
| 3B+, card at **720p60** | **50 fps** | 28 fps, 57 ms delay | 49% |
| 4B, card at 1080p60 | 28 fps | 2 streams, 0 drops, 33-40 ms | 86% |

So on a Pi 3B/3B+ that shows a stream too, send at 720p.

## HTTP API

The token is under **Device settings → HTTP API**. Send it as
`Authorization: Bearer <token>` or `X-API-Key: <token>`. Tokens in the URL
(`?token=`) are not accepted: URLs end up in logs and browser histories.
Request bodies can be JSON or form data.

The API is served over HTTPS only. A request with a token sent to `http://`
gets a 403. If it was the valid token, it has crossed the network unencrypted,
so it is replaced at once (Device settings shows a note): fix the client's URL
and give it the new token.

With the device's own (self-signed) certificate, don't just switch off
certificate checks (`curl -k` alone): pin the device's key instead, so an
impostor is refused. The pin is under **Device settings → HTTPS certificate**:

```sh
curl -k --pinnedpubkey "sha256//<pin>" -H "Authorization: Bearer $TOKEN" https://192.168.1.50/api/v1/status
```

`--pinnedpubkey` is checked even with `-k`. Clients that can't pin, such as
Bitfocus Companion's generic HTTP module, need either a certificate from a CA
they trust (upload it in Device settings), or a network you trust.

| Request | Body | Does |
| --- | --- | --- |
| `GET /api/v1/status` | | Device, outputs, inputs and visible sources as JSON |
| `POST /api/v1/outputs/<n>/source` | `{"stream": "CAM 2", "source_ip": "", "backup": ""}` (any subset) | Switches output *n* live and saves it |
| `POST /api/v1/identify` | `{"output": 1}` (0 = all) | Shows the name on the screen for 15 s |
| `POST /api/v1/restart` | | Restarts video and outgoing streams |

Example:

```sh
curl -k --pinnedpubkey "sha256//<pin>" -H "Authorization: Bearer $TOKEN" -d stream="CAM 2" \
    https://192.168.1.50/api/v1/outputs/1/source
```

Wrong tokens are answered after a second's delay. They never lock anyone out:
the token is a random 192-bit value that can't be guessed, and a valid token
always works.

## Files

| Path | Contents |
| --- | --- |
| `/etc/default/pindi` | Settings (`PINDI_*`), documented in `pindi.conf`. Normally edited through the web interface. |
| `/etc/pindi/ndi-config.v1.json` | NDI Discovery Server, groups, machine name, receive transport, NDI licence (readable by root and `pindi-media` only) |
| `/etc/pindi/tls/` | The web interface's certificate and key (root and `pindi-web` only). The device's own is made again when its name or addresses change; an uploaded one is kept |
| `/usr/share/pindi/update-key.pub` | Public key that software updates must be signed with |
| `/var/lib/pindi/previous/` | Files the last installation replaced (for a manual rollback) |
| `/var/log/pindi-os-update.log` | Last operating-system update |
| `/etc/sysctl.d/90-pindi-ndi.conf` | Large network buffers (Device settings) |
| `/run/pindi/in<n>.levels` | The capture device's audio level, one line per second |
| `/etc/pindi/api-token` | HTTP API token |
| `/etc/systemd/network/20-pindi-wired.network` | Network settings |
| `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`, `/etc/systemd/network/25-pindi-wifi.network` | Wi-Fi settings (the password is stored only as the derived WPA key) |
| `/run/pindi/` | Live state from the player and sender (status, previews); root never writes here |
| `/run/pindi-ctl/` | Live switches and Identify, from the web interface to the player (root only) |

Services: `pindi` (splash and receiver), `pindi-sender` (outgoing streams),
`pindi-web` (web interface), `pindi-webd` (the web interface's privileged
part), `pindi-os-update` (operating-system updates). Logs:
`journalctl -u pindi -u pindi-sender -u pindi-web -u pindi-webd`, or **View
logs** in the web interface.

## Security

- **Web interface:** HTTPS only (TLS 1.2 or later). Port 80 only redirects
  browsers asking for a page; sign-ins, forms and API calls sent there are
  refused (403), not redirected with their contents. Sign-in uses the Pi's own
  accounts and is limited to the `sudo` group. Session cookies are `Secure`,
  `HttpOnly` and `SameSite=Strict`, and every change needs the page's CSRF
  token.
- **Certificate:** the device's own self-signed certificate names its host
  name, `<name>.local` and current addresses, and is made again when they
  change. Device settings shows its fingerprint and public-key pin so clients
  can check it, or you can upload your own (see the HTTP API section).
- **Sessions:** changing a password signs that user out everywhere else.
- **Lockout:** after 5 failed sign-ins for one user name from one address in
  10 minutes, that name is locked out from that address for 10 minutes. Others
  behind the same address (NAT) can still sign in. After 30 failures from one
  address (any names), the whole address waits 10 minutes. Wrong API tokens
  don't lock anyone out (see the HTTP API section).
- **Request limits:** requests are size-checked before they are read, and idle
  connections are closed after 20 s. At most 8 connections from one address
  are handled at a time (64 in all), so one misbehaving client can't lock
  everyone out.
- **Privileges:** the player and sender run as the unprivileged `pindi-media`
  user (groups `video`, `audio`, `render`), in a systemd sandbox with a
  read-only system.
- **Web interface split in two:**
  - The part on the network, `pindi-web` (TLS, HTTP, pages, uploads), runs as
    the unprivileged `pindi-web` user on a read-only system. It can only read
    the device's state.
  - Every change goes through `pindi-webd`, a small root process with no
    network access, over a local socket only `pindi-web` can use. It checks the
    session and CSRF token itself: sign-in, passwords, settings, network,
    updates.
  - A flaw in the exposed part therefore doesn't give root.
  - Root never writes into a directory the media user can write to, so a
    compromised player can't redirect its writes.
- **Software updates** must be signed (see Updating).
- **Operating-system updates are off by default,** so nothing downloads or
  upgrades during a show. Under **Device settings**, set a daily time for them,
  ideally just before the daily reboot. You can also press **Install OS updates
  now** under System. Either needs internet access. Devices that never get OS
  updates miss security fixes. When no packages have been updated for 30 days,
  the Device card and every sign-in say so. Then turn OS updates on, or re-run
  `install.sh` (which runs `apt full-upgrade`) now and then.
- **Network exposure:** the device listens on 80/443 (web interface), 22 (SSH),
  5353 (mDNS) and the NDI ports (5960 and up). LLMNR is switched off. SSH
  accepts the Pi user's password. Use a strong password, or SSH keys with
  `PasswordAuthentication no`, and keep the devices on a network you trust.
- **Diagnostics** leave out the NDI licence. Settings backups include it, so
  keep them private. Wi-Fi passwords and the API token are in neither.

## Built for unattended use

- The hardware watchdog reboots a hung system.
- The journal is capped at 64 MB, so logs never fill the SD card.
- Settings writes are synced to the SD card, so a power cut right after saving
  can't leave an empty file.
- The Device card warns about under-voltage and overheating.
- An optional daily reboot clears any slow build-up on installations that run
  for months.

## Troubleshooting

- **"Power / cooling" warns about under-voltage:** use the official power
  supply (Pi 3: 2.5 A, Pi 4: 3 A, Pi 5: 5 A) and a short cable.
- **The output says "cannot display (too big)":** the stream is larger than
  this Pi can show (e.g. 4K on a Pi 3). Choose receive quality
  **Low-bandwidth preview** or send a smaller stream.
- **Dropped frames on a Pi 3:** one Full NDI 1080p60 stream can use most of
  the Pi 3's 100 Mbit/s link; the Device card shows the traffic.
- **Audio drops out for a second or two, worst in busy scenes:** Full NDI's
  bitrate follows the picture and can pass 100 Mbit/s per receiver in action
  scenes, more than a Pi 3's network port carries. Send the stream as
  **NDI|HX** (about 10 Mbit/s). Keep **NDI receive transport** on TCP and
  **Large network buffers** on (Device settings).
- **The picture of an NDI|HX stream freezes after 30 minutes, the sound goes on:**
  the NDI Advanced SDK is the evaluation build (no licence in Device settings);
  the outgoing stream's status says so. Send Full NDI, or enter the vendor name
  and ID of an NDI Advanced SDK licence (licensing@ndi.video).
- **Audio drops out on every receiver at once:** check the sender's log
  (`journalctl -u pindi-sender`). "the card sends silence" means the HDMI
  source itself went quiet: set the source's audio to stereo PCM (on a Fire
  TV: Surround Sound *Stereo*, Dolby Digital *Off*).
- **The latency figure says the clocks disagree:** sync the sender's clock (on
  Windows: `w32tm /resync`) and check the Clock line on the Device card.
- **Sending a problem report:** **Download diagnostics** collects the version,
  settings, status and logs in one file.

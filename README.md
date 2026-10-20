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
2. Copy this folder to the Pi, for example to `/home/<user>/pindi`.
3. On the Pi, run:

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

## Updating

Run `bash make-update.sh` in this folder. It builds
`pindi-update-<version>.tar.gz`, about 100 KB because the NDI SDK isn't
included; devices keep the SDK they have.

Install the package on a device under **System → Software update**:

- It installs offline, keeps all settings, shows the log live, and reboots when
  it's done (about 2 minutes on a Pi 3).
- If the install fails, the previous version keeps running and the log shows
  why.

Running `install.sh` again over SSH also works.

## First steps

Open `http://<IP shown on the splash>/` and sign in with the Pi's own user (a
member of the `sudo` group, e.g. the user from step 1). If you signed in with a
well-known default password, the page reminds you to change it.

Out of the box, output 1 shows the stream `OUT_<serial>` and output 2 shows
`OUT_<serial>_2`, from any host. Either name your sender like that, or pick a
stream for each output in the web interface. The list of NDI sources on the
network has a **Use** button for each.

## Web interface

| Section | What you can do |
| --- | --- |
| Device | Live network traffic, CPU temperature and load, under-voltage and throttling warnings, NDI SDK version, software version, clock |
| Output *n* | Live preview of the screen, state, latency, format and audio. Settings: stream, source IP, backup stream, receive quality (full or low-bandwidth preview), display mode, rotation (0/180°), mirroring, display standby, audio output and delay. **Identify** shows "Output n" on that screen for 15 s. |
| Outgoing streams | Each USB capture device: send on/off, NDI name, capture mode, audio on/off |
| Device settings | Device name (NDI name and host name), NDI Discovery Server, NDI groups, time zone, daily reboot, splash options, HTTP API token |
| Network | DHCP or static IP, NTP servers. A change undoes itself after 2 minutes unless you press **Keep** at the new address. |
| System | Log viewer, diagnostics download, settings backup and restore, reset to defaults, restart video, identify all screens, reboot |

### How outputs behave

- **Switching streams** happens live, in about a second, without restarting
  the player. The last picture stays on screen until the new stream's first
  frame arrives.
- **Losing the stream:** the splash comes back about 3 s after the stream is
  lost, and the output reconnects by itself. With a **backup stream** set, the
  backup takes over instead (the last picture stays up meanwhile), and the
  output switches back as soon as the main stream returns.
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

## NDI|HX

The standard NDI SDK used here sends and receives **Full NDI (SpeedHQ)** only.
Senders that can do both (e.g. NDI Screen Capture HX) automatically send Full
NDI to PiNDI. Cameras that only send HX will not show a picture. HX needs the
NDI Advanced SDK and a vendor licence from NDI; the web interface shows the
HX options greyed out for that reason.

## HTTP API

The token is under **Device settings → HTTP API**. Send it as
`Authorization: Bearer <token>`; `X-API-Key: <token>` or `?token=<token>` also
work. Request bodies can be JSON or form data.

| Request | Body | Does |
| --- | --- | --- |
| `GET /api/v1/status` | | Device, outputs, inputs and visible sources as JSON |
| `POST /api/v1/outputs/<n>/source` | `{"stream": "CAM 2", "source_ip": "", "backup": ""}` (any subset) | Switches output *n* live and saves it |
| `POST /api/v1/identify` | `{"output": 1}` (0 = all) | Shows the name on the screen for 15 s |
| `POST /api/v1/restart` | | Restarts video and outgoing streams |

Example:

```sh
curl -H "Authorization: Bearer $TOKEN" -d stream="CAM 2" http://192.168.1.50/api/v1/outputs/1/source
```

Wrong tokens count as failed sign-ins: after 5 failures in 10 minutes, the
address is locked out for 10 minutes.

## Files

| Path | Contents |
| --- | --- |
| `/etc/default/pindi` | Settings (`PINDI_*`), documented in `pindi.conf`. Normally edited through the web interface. |
| `/etc/pindi/ndi-config.v1.json` | NDI Discovery Server, groups, machine name |
| `/etc/pindi/api-token` | HTTP API token |
| `/etc/systemd/network/20-pindi-wired.network` | Network settings |
| `/run/pindi/` | Live state for the web interface (status, previews) |

Services: `pindi` (splash and receiver), `pindi-sender` (outgoing streams),
`pindi-web` (web interface). Logs: `journalctl -u pindi -u pindi-sender -u pindi-web`,
or **View logs** in the web interface.

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
- **The latency figure says the clocks disagree:** sync the sender's clock (on
  Windows: `w32tm /resync`) and check the Clock line on the Device card.
- **Sending a problem report:** **Download diagnostics** collects the version,
  settings, status and logs in one file.

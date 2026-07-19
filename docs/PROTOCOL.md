# Camera control protocol

Control channel between the host UI and `camera-streamer` on the device.
The same protocol rides both transports (`[server] transports`):

- **Carrier, network mode**: TCP, default port **8555**
  (`[server] control-port`, `0` disables), plaintext by default —
  optionally TLS/mTLS, see "Transport security" below. The server binds
  the same address as the RTSP server (`[server] listen`).
  Debuggable by hand: `nc 192.168.55.1 8555`.
- **Carrier, usb mode**: the control channel of the encrypted USB
  session — no TCP listener exists. Requests are dispatched in-process
  on the device; byte-identical payloads.
- **Framing**: newline-delimited UTF-8 JSON — exactly one JSON object per
  line (`\n` terminated) in each direction. No message may contain a raw
  newline.
- **Model**: request/response, plus server-initiated **events**: a line
  with no `"id"` member is an event, not a response (see "Events").
  Poll `get-status` for liveness/state. Multiple concurrent client
  connections are allowed.

This replaces the protobuf channel sketched in DESIGN.md §4 — same wire role,
but no codegen and no extra host dependency (Qt ships JSON; json-glib is in
oe-core).

The machine-readable constants (ports, method names, error codes, update
states) live in `common/proto/Protocol.h` — pure C++17, included by both
the embedded app and the Qt host UI. New methods/constants go there
first; this file is the prose reference.

## Transport security (TLS)

The channels that *command* the device — control (8555) and update
(8557) — can be TLS-wrapped; RTSP and discovery are unaffected. Off by
default. In `[server]` on the device:

- `tls-cert` + `tls-key` — enable TLS on both ports. The device ships a
  self-signed EC P-256 certificate generated on first boot
  (`camera-streamer-gencert.service`,
  `/etc/camera-streamer/tls/server.crt`); clients pin it (or its public
  key) on first pairing.
- `tls-ca` — additionally **require** a client certificate signed by
  this CA (mTLS): only authorized host software can control the camera.
  An unauthorized client fails the handshake on its first read.

Misconfiguration (only one of cert/key set, unreadable files) is a fatal
startup error — the device never silently falls back to plaintext. When
TLS is enabled the wire protocol is unchanged; clients simply wrap the
connection (the handshake happens implicitly on first I/O).

## Envelope

Request:

```json
{"id": 1, "method": "set-exposure", "params": {"camera": 0, "us": 5000}}
```

Response (exactly one per request, matching `id`):

```json
{"id": 1, "result": {}}
{"id": 1, "error": {"code": -32602, "message": "camera must be 0 or 1"}}
```

- `id`: any JSON number or string, echoed verbatim. Requests without an `id`
  get `"id": null` in the response.
- `params` is optional; unknown members are ignored.

Error codes (JSON-RPC-flavored): `-32700` parse error, `-32600` invalid
request, `-32601` unknown method, `-32602` invalid params, `1` operation
failed (message says why, e.g. a V4L2 ioctl error or "not supported for
source 'test'").

## Methods

### `ping`

No params. → `{"pong": true, "version": "<app version>"}`

### `get-status`

No params. → snapshot of the whole service:

```json
{
  "version": "0.2.0",
  "listen": "all",
  "address": "0.0.0.0",
  "port": 8554,
  "control_port": 8555,
  "clients": 1,
  "cameras": [
    {
      "index": 0,
      "mount": "/cam0",
      "enabled": true,
      "source": "argus",
      "width": 1440, "height": 1080, "framerate": 60,
      "codec": "h265", "bitrate": 8000000,
      "exposure": 0, "gain": 0.0, "trigger": -1,
      "streaming": true,
      "frames": 5321
    },
    { "index": 1, "...": "..." }
  ]
}
```

- `clients`: currently connected RTSP clients (all mounts).
- `streaming`: a media pipeline for this mount is currently prepared
  (at least one client has requested it; shared factories mean one
  pipeline per mount).
- `frames`: buffers through the payloader since the pipeline was created —
  the host can watch this advance as a health signal.
- `fps`: frames actually delivered over the last 5 s (0 when not streaming).
  This is the *measured* rate, which can sit below the configured
  `framerate` if Argus AE trades frame rate for exposure in dim light; the
  device also logs a warning below 80 % of the configured rate.
- `exposure_current` (µs) / `gain_current` (VC driver units, milli-dB):
  the values programmed into the sensor *right now*, read from its V4L2
  controls — when the configured `exposure`/`gain` are `0` (auto), these
  are what Argus's AE loop chose this instant. Omitted when the device
  node can't be queried.
- While frames are flowing, each camera also carries a `last_frame` object —
  frame metadata sampled at the payloader (M3):

  ```json
  "last_frame": {"sequence": 5321, "pts": 88683333333, "wallclock": 1783934502123456}
  ```

  `sequence` is the capture sequence number (GstBuffer offset — the v4l2
  frame sequence on the v4l2 path, frame count otherwise), `pts` the buffer
  timestamp in ns (pipeline running-time base), `wallclock` the µs-since-epoch
  system time when the buffer passed the payloader. Comparing `sequence` and
  `wallclock` across /cam0 and /cam1 is the check that hardware-triggered
  capture is actually synchronized.

### `get-config`

No params. → the current in-memory configuration (same camera fields as
`get-status`, without the runtime `streaming`/`frames`).

### `set-exposure`

`{"camera": 0|1, "us": <int µs, 0 = auto>}` → `{}`

- `argus` source: `0` re-enables auto exposure; a value locks
  `exposuretimerange` on the live `nvarguscamerasrc` (and is inherited by
  pipelines created later).
- `v4l2` source: sets the sensor `exposure` V4L2 control (VC IMX296 driver,
  µs) — works whether or not the pipeline is running.
- `test` source: error `1`.

### `set-gain`

`{"camera": 0|1, "gain": <number, 0 = auto/default>}` → `{}`

- `argus`: locks `gainrange` (analog gain multiplier, typically 1–16).
- `v4l2`: sets the sensor `gain` V4L2 control (raw driver units; VC IMX296:
  milli-dB, 0–48000 = 0–48 dB, step 100).

### `set-trigger`

`{"camera": 0|1, "mode": <int>}` → `{}`

VC IMX296 hardware trigger mode, `v4l2` source only (Argus owns the sensor
timing and does not support external trigger):

| mode | meaning |
|---|---|
| 0 | disabled (free running) |
| 1 | external |
| 2 | pulse width |
| 3 | self |
| 4 | single |
| 5 | sync |
| 6 | stream edge |
| 7 | stream level |

The exact set depends on the driver; out-of-range values return the V4L2
error. `-1` in config/`get-status` means "never set, driver default".

### `set-isp`

`{"camera": 0|1, "param": "<name>", "value": <string|number|bool|null>}` → `{}`

Runtime ISP controls, `argus` source only (the v4l2 path bypasses the ISP).
`param` is one of the whitelisted `nvarguscamerasrc` properties:

| param | value | meaning |
|---|---|---|
| `wbmode` | 0–9 | white balance: 0 off, 1 auto, 2 incandescent, 3 fluorescent, 4 warm-fluorescent, 5 daylight, 6 cloudy-daylight, 7 twilight, 8 shade, 9 manual |
| `saturation` | 0.0–2.0 | color saturation (1 = neutral) |
| `tnr-mode` | 0–2 | temporal noise reduction: off / fast / high quality |
| `tnr-strength` | -1.0–1.0 | TNR strength (-1 = auto) |
| `ee-mode` | 0–2 | edge enhancement: off / fast / high quality |
| `ee-strength` | -1.0–1.0 | EE strength (-1 = auto) |
| `aeantibanding` | 0–3 | off / auto / 50 Hz / 60 Hz |
| `exposurecompensation` | -2.0–2.0 | AE compensation in stops |
| `aelock` / `awblock` | bool | lock auto-exposure / auto-white-balance |
| `ispdigitalgainrange` | "min max" | ISP digital gain range, e.g. `"1 4"` |

The value is applied to the live pipeline (if any) and remembered, so
pipelines created later inherit it; `value: null` forgets the override
(takes effect on the next pipeline — a live pipeline keeps the last value).
Current overrides appear as the `isp` object in `get-config`/`get-status`
cameras, and can be preset from the config file with `isp-<param>=` keys
(e.g. `isp-wbmode=1`). Ranges are per L4T r35 `nvarguscamerasrc`; invalid
values are rejected by the element, not by this API.

Note: `set-isp` adjusts Argus's runtime processing. The static tuning
(lens shading, CCM, gamma) lives in `camera_overrides.isp` on the device —
see `yocto/meta-vc-camera/recipes-bsp/isp-tuning/`.

### `set-zoom`

`{"camera": 0|1, "factor": <1.0–8.0>, "x": <0–1>, "y": <0–1>}` → `{}`

Digital zoom: GPU crop + upscale (`nvvidconv`) between the sensor and the
encoder. `factor` 1.0 = full field of view (the converter is dropped from
the pipeline entirely); 2.0 = center half of the frame upscaled to full
resolution, and so on. `x`/`y` place the crop center as a fraction of the
frame — i.e. pan while zoomed; omitted they stay unchanged (initially
0.5/0.5, centered). Detail beyond
the sensor's native pixels is not created: 2× zoom halves the effective
resolution.

Applies to the live pipeline where the converter supports it, and is
re-armed into the mount's launch string — a client that reconnects always
gets the new framing. The UI reconnects its pane on change for exactly
that reason. Zoom state appears as `zoom`/`zoom_x`/`zoom_y` in
`get-status`/`get-config`, and can be preset from the config file
(`zoom=2.0`, `zoom-x=0.5`, `zoom-y=0.5`).

### `set-sync`

`{"enabled": true|false}` → `{}`

Hardware-synchronized dual capture (M3): puts **every enabled camera** into
external trigger mode (`true`, mode 1 — one trigger pulse fanned out to both
sensors exposes them simultaneously) or back to free running (`false`,
mode 0). Errors with code `1` if any enabled camera is not on the `v4l2`
source; nothing is changed in that case.

### `fire-trigger`

`{"camera": 0|1}` → `{}`

Presses the VC driver's software "single trigger" button control — exposes
one frame when the sensor is in a software-triggerable mode (set
`set-trigger` mode 4 "single" first). `v4l2` source only.

### `list-controls`

`{"camera": 0|1}` → every V4L2 control the sensor driver exposes
(`v4l2` and `argus` sources — the device node exists either way):

```json
{"controls": [
  {"id": 9963793, "name": "Exposure", "type": 1,
   "min": 1, "max": 1000000, "step": 1, "default": 10000,
   "value": 5000, "flags": 0},
  {"id": 10092545, "name": "Trigger Mode", "...": "..."}
]}
```

`type` is the raw `V4L2_CTRL_TYPE_*` value.

### `get-control` / `set-control`

Generic escape hatch for anything the driver exposes (black level, ROI,
flash/IO modes, …):

- `get-control` `{"camera": 0, "control": "trigger mode" | "0x009a2000" | 10092545}`
  → the same object shape as one `list-controls` entry.
- `set-control` `{"camera": 0, "control": <name-or-id>, "value": <int>}` → `{}`

Control names match case-insensitively, treating space/`_`/`-` as equal.

### `reload`

No params. → `{}` — then the service re-reads its config file and restarts
the RTSP (and, if its settings changed, control) server, exactly like
`systemctl reload` / SIGHUP. The response is sent **before** the restart;
the control connection may drop if `listen`/`control-port` changed.
Connected RTSP clients are dropped on purpose.

### `reboot`

No params. → `{}` — then the device reboots after a 2-second delay
(so the response can be sent). The control connection will drop when
the device reboots. Useful after a firmware update to activate the
new image.

### `get-update-status`

No params. → current SWUpdate installation status:

```json
{
  "state": "installing",
  "percent": 45,
  "step": 2,
  "total_steps": 7,
  "current": "rootfs.ext4.gz"
}
```

| state | meaning |
|---|---|
| `idle` | no update in progress |
| `uploading` | .swu file is being uploaded to the device |
| `installing` | swupdate is processing the image |
| `success` | installation completed successfully |
| `failure` | installation failed (`error` field has the message) |
| `done` | installation finished, post-update hooks ran |

`percent`/`step`/`total_steps`/`current` are only meaningful while
`state` is `installing`. Poll this every 2 s during an update.

### set-stream

Starts or stops one camera's video stream at runtime, without a reload.

```json
{"id": 7, "method": "set-stream", "params": {"camera": 0, "enabled": false}}
```

Over secure USB the camera's push loop parks and the sensor is released;
re-enabling restarts it within ~200 ms. The flag also updates the runtime
config, so `get-config` reflects it; it is not persisted across restarts.
(Over RTSP, streams already start/stop with client connections; this method
additionally marks the camera disabled for future sessions.)

### `get-metrics`

No params. → resource load and temperatures:

```json
{"cpu_percent": 12.5, "process_cpu_percent": 4.1, "gpu_percent": 26.0,
 "vic_percent": 70.0, "nvenc_percent": 30.0, "cpu_temp_c": 48.5,
 "gpu_temp_c": 47.0, "interval_s": 2.0}
```

Percentages are deltas over `interval_s` (time since the previous call).
The first call after service start only primes the counters and reports
negatives — poll twice and use the second.

### `snapshot`

`{"camera": N, "path": "/tmp/x.ppm"?}` → `{"path": ..., "pending": true}`

Writes the next frame off the detection branch as a PPM on the *device*
(default `/tmp/snapshot-camN.ppm`) — raw ISP output, for judging colour
tuning. The write happens on the next detection frame, after the reply.
Fails if no readable `[detect]` model is installed (the detection branch
is the frame source).

### `set-tuning`

Declared in `Protocol.h` and sent by the host UI's white-balance
calibrator; **not implemented on the device yet** — answers `-32601`
(unknown method).

## Events

Server-initiated lines with no `"id"` member. Clients must skip unknown
events. Currently one:

```json
{"event": "faces", "camera": 0,
 "data": {"camera": 0, "w": 320, "h": 240,
          "faces": [{"x": 12, "y": 30, "w": 40, "h": 52, "score": 0.71}]}}
```

Face-detection boxes, pushed to every control connection in network
mode (in usb mode the same `data` payload arrives on the session's
metadata channel instead). Coordinates are in the detector working frame
— normalise by the payload's `w`/`h`.

## OTA firmware update (TCP, port 8557)

Dedicated binary upload channel for `.swu` (SWUpdate) packages. The host
UI sends the file over this port; the device saves it and triggers
`swupdate` via its IPC API
([docs](https://sbabic.github.io/swupdate/swupdate-ipc-interface.html)).
Install progress is then polled via `get-update-status` on the control
channel.

The upload is on a separate port because the control channel is
newline-delimited JSON — binary data would break framing.

`[server] update-port` configures the port (default 8557, `0` disables).
The server binds the same address as the RTSP server, and is TLS-wrapped
when TLS is configured (see "Transport security" above).

### Upload protocol

One connection = one upload. Data is streamed directly to swupdate via
its IPC socket — no temp file on the device.

1. Host connects to the update port.
2. Host sends a JSON header line (newline-terminated):
   ```json
   {"size": 52428800}
   ```
3. Device connects to swupdate IPC, sends REQ_INSTALL, gets ACK, then
   responds to host with:
   ```json
   {"ok": true}
   ```
   Or an error (connection closes after):
   ```json
   {"ok": false, "error": "swupdate busy or unavailable"}
   ```
4. Host streams exactly `size` raw bytes of the `.swu` file. The device
   forwards each chunk directly to the swupdate IPC fd — swupdate starts
   parsing the CPIO stream as data arrives, no temp file needed.
5. When all `size` bytes are received, the device closes the swupdate
   IPC fd (signals end-of-stream) and starts polling install status.
6. Device sends the final response and closes:
   ```json
   {"ok": true, "state": "installing"}
   ```
   Or on error:
   ```json
   {"ok": false, "error": "upload incomplete"}
   ```
7. Host polls `get-update-status` on the control channel (port 8555)
   for installation progress until `state` is `success` or `failure`.

Max upload size: 2 GB. The device auto-installs during upload — there is
no separate "apply" step. Installation begins as soon as swupdate has
enough data to start parsing.

## Discovery (UDP, port 8556)

So the host UI can find devices instead of assuming 192.168.55.1 (M3).
Datagram request/response, JSON payloads, no framing (one message per
datagram):

- Host broadcasts `{"method": "discover"}` to UDP port 8556
  (255.255.255.255 and/or per-interface broadcast addresses; unicast works
  too). Extra members are ignored.
- Each device answers with a single unicast datagram to the sender:

```json
{"device": "camera-streamer", "version": "0.3.0",
 "rtsp_port": 8554, "control_port": 8555,
 "cameras": [{"index": 0, "mount": "/cam0", "enabled": true},
             {"index": 1, "mount": "/cam1", "enabled": true}]}
```

The device's IP is the reply's source address; stream/control URLs follow
from it. Anything that is not a JSON object with `"method": "discover"` is
ignored (no reply). `[server] discovery-port` configures the port, `0`
disables discovery. The responder binds 0.0.0.0 regardless of `listen=` —
it only reveals what a port scan would.

## Versioning

`ping`/`get-status` carry the app version. Additive changes (new methods,
new response members) don't bump anything; clients must ignore unknown
members. Nothing here is stable API until M3.

# Camera control protocol (M2)

TCP control channel between the host UI and `camera-streamer` on the device.

- **Transport**: plain TCP, default port **8555** (`[server] control-port`,
  `0` disables). The server binds the same address as the RTSP server
  (`[server] listen`), so the control channel is reachable over the same
  network(s) as the video.
- **Framing**: newline-delimited UTF-8 JSON — exactly one JSON object per
  line (`\n` terminated) in each direction. No message may contain a raw
  newline. Debuggable by hand: `nc 192.168.55.1 8555`.
- **Model**: request/response only; the server never sends unsolicited
  messages. Poll `get-status` for liveness/state. Multiple concurrent client
  connections are allowed.

This replaces the protobuf channel sketched in DESIGN.md §4 — same wire role,
but no codegen and no extra host dependency (Qt ships JSON; json-glib is in
oe-core).

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
resolution, and so on. `x`/`y` (optional, default 0.5/0.5) place the crop
center as a fraction of the frame — i.e. pan while zoomed. Detail beyond
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

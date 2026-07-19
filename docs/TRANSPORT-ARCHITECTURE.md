# Transport architecture — two modes

Status: **implemented and verified on hardware** (2026-07-19).

The device serves video over exactly one transport at a time, chosen by
`transports` in `camera-streamer.conf`. There are two, and no others:

| mode | video | detection boxes | encrypted |
|---|---|---|---|
| `usb` | `Channel::Video` | `Channel::Meta` | yes |
| `network` | RTSP / RTP | control `{"event":"faces"}` | **no** |

Anything else — `both`, or an encrypted record protocol over TCP — is
deliberately not supported. See "Rejected" below.

```
IMX296 -> Argus/ISP -> CameraPipeline (one per sensor)
                              |
                        tee --+-------------+
                        |                   |
                   DETECTION             VIDEO
                   YuNet/CUDA           H.265 encode
                        |                   |
        ================+===================+================
          usb mode                     network mode
        ----------------------------   ----------------------------
          SESSION / MUX                  RTSP media (gst-rtsp-server)
          Video|Control|Update|Meta      + detect appsink beside pay0
          ENCRYPT (ChaCha20-Poly1305)    boxes -> ControlServer::broadcast
          ep1/ep2 bulk                   RTP + control TCP
        ================+===================+================
                        |                   |
                    DECRYPT             ControlClient event
                        |                   |
                        +--------+----------+
                                 |
                          applyFaceMeta -> FrameView
                              (one renderer, both modes)
```

## What is shared

Everything above the transport split:

- `CameraPipeline` — one per sensor, owning pipeline lifecycle, PLAYING
  verification, the live source element, relaunch and frame fanout. Argus
  permits a single consumer per camera, so nothing else opens the sensor.
- The detector, and `detect::to_meta_json` — byte-identical payloads on both
  modes.
- `IMetaSink` — the only thing that differs about detection delivery.
  `SessionMetaSink` enqueues on `Channel::Meta`; `ControlMetaSink` broadcasts a
  control event.
- Host: `applyFaceMeta` -> `FrameView`, which paints frame and boxes in one
  pass. Neither mode has its own renderer.

## What differs

Only the carrier, and confidentiality:

- **usb** binds no TCP/UDP socket at all except the NCM recovery listener on
  `192.168.55.1:8557`. Control is dispatched in-process and firmware upload
  rides an anonymous socketpair, so nothing is re-serialised onto loopback.
- **network** binds RTSP, control, discovery and update normally, and video is
  in the clear.

## Accepted trade-off

**Network mode does not encrypt video.** RTSP is kept for interop with VLC,
ffmpeg and NVRs, and that is incompatible with wrapping the stream in the
record protocol. A deployment that needs confidentiality on the wire uses usb
mode. This is a decision, not an oversight: do not "fix" it by adding a second
network path.

Note also that the control connection is no longer strictly
request/response — a line with no `"id"` is a server-initiated event. Any
client that assumes every line is a reply needs updating; the bundled viewer
handles it.

## Rejected

- **`transports=both`.** Argus permits one consumer per camera, so serving RTSP
  and secure USB together meant the USB side re-serving the camera's own RTSP
  mount over loopback: an RTP round trip, and a re-serve string with no detect
  branch, which silently disabled face detection. Removed; the value is
  rejected at config load.
- **An encrypted record protocol over TCP (`TcpTransport`).** It would have
  given network mode confidentiality and a single protocol for both carriers,
  but it duplicates what usb mode already provides and adds a third shape to
  maintain. Two modes, clearly separated, was chosen instead.
- **`set-transport` (switching mode at runtime).** The request arrives on the
  transport being torn down, so its reply can never return; and since usb mode
  binds nothing, a failed switch leaves only the serial console. Mode is a
  config decision applied at startup.

## Per-mode notes

- Detection in **network** mode is per-client: gst-rtsp-server builds the
  pipeline on connect, so no client means no detection. Usb mode has the same
  property per session.
- Detection is paced (`[detect] fps`, default 10) and the detect appsink blocks
  rather than dropping, so `nvvidconv` only converts frames the detector
  actually takes. Measured on target: GR3D 36-41% -> 25-28%. VIC (~70%) is the
  camera/encode path and is unaffected by detection.
- `videoconvert` is NOT in the device image. Only the `nv*` GStreamer plugin
  packages are installed, and `gst_parse_launch` answers a missing element with
  a partial pipeline plus a non-fatal error -- which is how a detect branch was
  silently dropped while video kept working. `nvvidconv` emits system-memory
  BGRx directly; the detector drops the padding byte.

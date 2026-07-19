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
                     |          Camera streamer
  IMX296 --> ARGUS --|
                     |      camera media pipeline
                     |          |                 |
                     |      detection         video stream
                     |      YuNet/CUDA        H.265 encode
                     |          |                 |
                     | ---------+-----------------+-----------------------
                     |            Encrypt / decrypt        (usb mode only)
                     |          ChaCha20-Poly1305, ECDHE-P256
                     |          channels: Video Control Update Meta
                     | ---------+-----------------+-----------------------
                     |         USB                     network
                     |      ep1/ep2 bulk            RTSP/RTP + control TCP
                     | ------------------------------------------------
                     |                  the wire
                     | ------------------------------------------------
                     |            Decrypt          (usb mode only)
                     | ---------+-----------------+-----------------------
                     |          |                 |
                     |     boxes: Meta        boxes: control event
                     |          |                 |
                     |          +--------+--------+
                     |                   |
                     |        applyFaceMeta -> FrameView
                     |            HOST UI (one renderer)
```

The encrypt/decrypt band spans the full width in `usb` mode. In `network` mode
it is absent: RTSP video is in the clear and boxes ride a control event. That
asymmetry is the accepted trade-off recorded below, not an unfinished edge.


## Where the code lives

| Role | Class | File |
|---|---|---|
| Source fragments (both modes) | `ICameraSource` | `pipeline/*Source.{h,cpp}` |
| RTSP graph composition | `PipelineBuilder::rtsp_launch` | `pipeline/PipelineBuilder.{h,cpp}` |
| Typed USB capture/encode | `media::CameraPipeline` | `media/CameraPipeline.{h,cpp}` |
| USB delivery strategy (video) | `media::IFrameTransport` | `media/CameraPipeline.h` |
| Typed usb pipeline (tee/encode/detect) | `media::PipelineSpec`, `Element/Bin/Tee` | `media/` |
| Detector (YuNet/CUDA) | `detect::IFaceDetector` | `detect/FaceDetector.{h,cpp}` |
| Box payload | `detect::to_meta_json` | `detect/FaceDetector.cpp` |
| Delivery strategy (boxes) | `detect::IMetaSink` | `detect/MetaSink.h` |

**usb mode**

| Role | Class | File |
|---|---|---|
| Reusable secure context/session | `SecureUsbContext`, `SecureUsbSession` | `common/secure/SecureUsbContext.{h,cpp}` |
| FunctionFS/application adapter | `SecureUsbServer` | `secure/SecureUsbServer.cpp` |
| Video -> `Channel::Video` | `VideoSink` | same file |
| Raw -> detector | `DetectSink` | same file |
| Boxes -> `Channel::Meta` | `SessionMetaSink` | same file |
| Gadget lifecycle | `FfsGadget` | `secure/FfsGadget.{h,cpp}` |

The reusable device-side API follows the same shape as a TLS server context.
I/O callbacks return the number of bytes transferred (zero means EOF) or an
error; they may wrap FunctionFS, a socket, or any other blocking byte stream:

```cpp
auto context = SecureUsbContext::create({
    .certificate = cert,
    .private_key = key,
});
if (!context)
    return context.error();

auto session = context->accept({
    .read = read_usb,
    .write = write_usb,
});
if (!session)
    return session.error();

session->send(Channel::Control, 0, reply);
auto request = session->receive();
```

`create()` parses the certificate and key, checks that they match, and caches
the identity. `accept()` performs the handshake and preserves bytes read past
the ClientHello for the first encrypted record. `send()` serializes concurrent
writers so the implicit AEAD record counter stays in wire order. None of these
types depends on GLib, GStreamer, libusb, or FunctionFS.

**network mode**

| Role | Class | File |
|---|---|---|
| RTSP server, mounts | `RtspServer` | `rtsp/RtspServer.{h,cpp}` |
| Per-mount media + detection thread | `MountController` | `rtsp/MountController.{h,cpp}` |
| Boxes -> control event | `ControlMetaSink` | `core/Application.cpp` |
| Push to clients | `ControlServer::broadcast` | `control/ControlServer.cpp` |

**host (both modes)**

| Role | Class | File |
|---|---|---|
| USB carrier, session, demux | `SecureUsbBridge` | `host-ui/secureusbbridge.cpp` |
| Control + event reception | `ControlClient` | `host-ui/controlclient.cpp` |
| Boxes -> overlay | `MainWindow::applyFaceMeta` | `host-ui/mainwindow.cpp` |
| Frame + boxes, one paint | `FrameView` | `host-ui/frameview.{h,cpp}` |

## How a box reaches the screen

1. `CameraPipeline::pump_raw()` on a dedicated thread pulls a BGRx frame from
   the `detect` appsink (usb), or `MountController`'s detection thread pulls the
   equivalent appsink in the RTSP media (network).
2. `IFaceDetector::detect()` runs YuNet, and `to_meta_json` formats the boxes.
   Identical on both modes.
3. `IMetaSink::on_meta()` delivers: `SessionMetaSink` enqueues on
   `Channel::Meta`; `ControlMetaSink` wraps it as
   `{"event":"faces","camera":N,"data":...}` and broadcasts.
4. Host: `SecureUsbBridge`'s demux, or `ControlClient::eventReceived` for a
   line with no `"id"`.
5. Both call `MainWindow::applyFaceMeta`, which normalises by the payload's
   `w`/`h` and hands `FrameView` the boxes.

Steps 1, 2 and 5 are shared code. Only step 3-4 differ by mode.

## Threading

- Video and detection pump on SEPARATE threads (`pump()` / `pump_raw()`).
  Inference is ~25 ms against a ~16 ms frame interval, so sharing a thread
  would stall the stream behind the detector.
- `ControlMetaSink::on_meta` runs on the detection thread but `broadcast()`
  touches the main loop's connection set, so it hops via
  `g_main_context_invoke_full` and is fire-and-forget.
- In usb mode, control requests are dispatched in-process and marshalled onto
  the GLib main loop, because handlers mutate config and live GStreamer
  elements.

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

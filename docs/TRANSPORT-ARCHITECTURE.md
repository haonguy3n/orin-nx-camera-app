# Transport architecture — one secure protocol, two carriers

Status: **agreed design, not yet implemented** (2026-07-19). Recorded here so
the refactor has a target; see "Migration" for what exists today.

## The idea

Encryption and multiplexing sit **above** the transport. USB and network carry
the *same* encrypted record protocol, so everything above the split — session,
crypto, channel mux, detection, the host demux and renderer — is written once.
Only the carrier differs.

Network mode is therefore **CSU records over TCP, not RTSP.**

```
IMX296 -> Argus/ISP -> CameraPipeline (one per sensor)
                              |
                        tee --+-------------+
                        |                   |
                   DETECTION             VIDEO
                   YuNet/CUDA           H.265 encode
                        |  boxes (JSON)     |  access units
  ======================+===================+=====================
             SESSION / MUX -- channels: Video | Control | Update | Meta
  ======================+===================+=====================
             ENCRYPT  (ECDHE-P256 handshake, ChaCha20-Poly1305 records)
  ======================+===================+=====================
                        TRANSPORT (strategy)
                  +-----+------+      +-----+------+
                  | USB        |      | NETWORK    |
                  | ep1/ep2    |      | TCP socket |
                  +-----+------+      +-----+------+
  ============================ the wire =========================
                  +-----+------+      +-----+------+
                  | USB libusb |      | TCP client |
                  +-----+------+      +-----+------+
             DECRYPT  (same handshake, same records)
                        DEMUX
              Video    Meta    Control    Update
                |        |        |          |
             decoder     |     control    .swu push
                +---+----+
                    |
                FrameView  (frame + boxes in one paint)   HOST UI
```

## Why this shape

- **One protocol, two carriers.** Only the carrier differs (~100 lines each).
  Everything else is shared by construction rather than by discipline.
- **Detection works in every mode, for free.** It sits above the split, so it
  is no longer mode-dependent. No separate metadata delivery per transport.
- **Video is encrypted on the network path.** RTSP never was; TLS covered only
  control and update.
- **Argus contention disappears.** One CameraPipeline per sensor feeds every
  transport, so no transport opens the sensor itself. This is what killed the
  old `transports=both`, which propped the USB side up by re-serving the
  camera's own RTSP mount over loopback — costing an RTP round trip and
  silently disabling face detection.
- **Transport choice becomes host-side only.** Both carriers can be live at
  once, so the host just picks. No `set-transport` control method, and none of
  its hazard: that request would tear down the very channel its reply must
  return on, and since usb mode binds no socket, a failed switch leaves only
  the serial console.
- **RTSP is kept** as a separate, optional network path (decision 2026-07-19).
  It is not part of this stack: it stays a plain RTSP server for interop with
  VLC, ffmpeg and NVRs, and for a future network mode that does not need the
  encrypted record protocol. It carries no detection metadata, because that
  rides the Meta channel which only exists inside a session.

## Trade-offs and open points

1. **RTSP is retained for interop.** Kept deliberately so VLC/ffmpeg/NVR
   clients still work and a future network mode has a plain option. It stands
   beside this stack rather than inside it: no encryption, and no detection
   metadata, since Meta only exists within a session. Two network paths is the
   accepted cost.
2. **Discovery stays outside the session.** Over TCP the host needs an address
   first; the UDP responder covers that and is unencrypted by nature.
3. **Auth over a real network.** The CSU handshake pins the device certificate.
   On an untrusted network the CA-signed device cert path should probably be
   mandatory rather than optional.
4. **The USB gadget coupling is separate and still real.** CDC-NCM only exists
   while the composite gadget is bound, and that binding depends on
   camera-streamer owning FunctionFS. That is why stopping the service drops
   ssh, and why switching to network mode once left the device unreachable
   (fixed in 7f5756f). Moving all three functions into usb-gadget.service is
   the proper decoupling and is independent of this design.

## Migration

Already in place:

- `CameraPipeline` + `IFrameTransport` — the shared capture half, with pipeline
  lifecycle, PLAYING verification, live source properties, relaunch and frame
  fanout. Has no callers yet.
- The record protocol, handshake, channel mux and host demux — currently
  reachable only through the USB path.
- `FrameView` on the host draws frames and boxes in one pass.

Remaining, in dependency order:

1. Move `SecureUsbServer`'s capture half onto `CameraPipeline`; it becomes an
   `IFrameTransport` that encrypts and writes to the endpoint.
2. Add `TcpTransport`: the same session/crypto/mux over a TCP socket. This is
   the new carrier, and the smallest piece of genuinely new code.
3. Host: factor `SecureUsbBridge` so the session/crypto/demux half is shared,
   with libusb and TCP as interchangeable carriers underneath.
4. Delete RTSP once (1)–(3) carry every mode, along with `IStreamController`'s
   RTSP assumptions and the remaining network listeners.

Each step is independently testable on hardware; do not stack them.

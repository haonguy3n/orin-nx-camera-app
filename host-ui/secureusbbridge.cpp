#include "secureusbbridge.h"

#include <QPointer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
// GLib's GDBusInterfaceInfo has a member named `signals`, which Qt defines as
// a keyword macro expanding to `public:`. Qt's keywords are not used in this
// file, so drop them before the GStreamer headers pull in gio.
#undef signals
#undef slots
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <libusb-1.0/libusb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "camera/secure/SecureRecord.h"
#include "secure/SecureHandshake.h"
#include "secure/SecureWire.h"

namespace {

// Handshake transfer timeout.
//
// The device answers a ClientHello with one P-256 ECDH plus a signature --
// about a millisecond -- so this bounds a device that is wedged or absent,
// not one that is merely busy. It is spent on the UI thread, so the old 5000
// froze the viewer for five seconds per failed attempt.
constexpr int kHandshakeTimeoutMs = 1500;

// Video no longer has a TCP listener: the device pushes an H.265 elementary
// stream, which is re-served locally as RTSP so mainwindow's
// rtsp://127.0.0.1:8554/camN URLs keep working unchanged.
constexpr int kCameras = 2;
constexpr std::array<quint16, 2> kPorts = {8555, 8557};
constexpr std::array<camera::secure::Channel, 2> kChannels = {
    camera::secure::Channel::Control,
    camera::secure::Channel::Update,
};

// CAMERA_SECURE_USB_DEBUG=1 turns on the periodic counters; the one-time
// milestones (first bytes, first record, first frame) always print, since
// they cost nothing and each one bounds where a failure lives.
bool debugEnabled()
{
    static const bool on = qEnvironmentVariableIntValue("CAMERA_SECURE_USB_DEBUG") != 0;
    return on;
}

int makeListener(quint16 port, QString *error)
{
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        *error = QStringLiteral("cannot create localhost proxy socket: %1")
                     .arg(QString::fromLocal8Bit(strerror(errno)));
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || listen(fd, 4) != 0) {
        *error = QStringLiteral("cannot bind secure USB proxy on 127.0.0.1:%1: %2")
                     .arg(port)
                     .arg(QString::fromLocal8Bit(strerror(errno)));
        close(fd);
        return -1;
    }
    return fd;
}

int channelIndex(camera::secure::Channel channel)
{
    for (size_t i = 0; i < kChannels.size(); ++i) {
        if (kChannels[i] == channel)
            return static_cast<int>(i);
    }
    return -1;
}

bool writeAll(int fd, const uint8_t *data, size_t size)
{
    while (size != 0) {
        const ssize_t written = send(fd, data, size, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

// Decodes one camera's tunnelled H.265 elementary stream and pushes the
// decoded frames straight into the pane's QVideoSink.
//
// This replaces the local RTSP re-serve (gst-rtsp-server on 127.0.0.1:8554
// + QMediaPlayer reconnecting to it). That hop existed only so the player
// code could stay unchanged, and it owned an outsized share of the failure
// modes: the port conflicts with stale viewers, the GMainContext collision
// with Qt, and the RTSP preroll that presented as "connecting" forever.
// Decoding directly means frames either draw or the pipeline error says why.
class VideoDecoder
{
public:
    ~VideoDecoder()
    {
        if (element_) {
            gst_element_set_state(element_, GST_STATE_NULL);
            gst_object_unref(element_);
        }
    }

    void setSink(QVideoSink *sink)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_ = sink;
    }

    void push(int camera, const uint8_t *data, size_t size)
    {
        camera_ = camera;
        if (!element_ && !startPipeline())
            return;
        pushed_ += size;
        if (debugEnabled() && pushed_ - reportedPushed_ >= 4 * 1024 * 1024) {
            reportedPushed_ = pushed_;
            fprintf(stderr, "secure-usb: cam%d %zu KiB into decoder, %zu frames out\n",
                    camera_, pushed_ / 1024, frames_);
        }
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        gst_buffer_fill(buffer, 0, data, size);
        gst_app_src_push_buffer(src_, buffer); // takes ownership
        drainBus();
    }

private:
    bool startPipeline()
    {
        if (failed_)
            return false; // reported once; retrying every record spams stderr
        GError *parseError = nullptr;
        element_ = gst_parse_launch(
            "appsrc name=src is-live=true format=time do-timestamp=true "
            "! h265parse ! avdec_h265 ! videoconvert "
            "! video/x-raw,format=RGBA "
            "! appsink name=sink sync=false max-buffers=4 drop=true",
            &parseError);
        if (!element_) {
            fprintf(stderr,
                    "secure-usb: cam%d decoder failed: %s (is gst-libav "
                    "installed? avdec_h265 comes from it)\n",
                    camera_, parseError ? parseError->message : "unknown");
            if (parseError)
                g_error_free(parseError);
            failed_ = true;
            return false;
        }
        if (parseError)
            g_error_free(parseError);
        src_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(element_), "src"));
        GstElement *sinkElement = gst_bin_get_by_name(GST_BIN(element_), "sink");
        GstCaps *caps = gst_caps_new_simple("video/x-h265", "stream-format",
                                            G_TYPE_STRING, "byte-stream", nullptr);
        gst_app_src_set_caps(src_, caps);
        gst_caps_unref(caps);
        GstAppSinkCallbacks callbacks{};
        callbacks.new_sample = &VideoDecoder::onSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sinkElement), &callbacks, this,
                                   nullptr);
        gst_object_unref(sinkElement);
        if (gst_element_set_state(element_, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
            fprintf(stderr, "secure-usb: cam%d decoder refused to start\n", camera_);
            failed_ = true;
            return false;
        }
        return true;
    }

    static GstFlowReturn onSample(GstAppSink *appsink, gpointer user)
    {
        auto *self = static_cast<VideoDecoder *>(user);
        GstSample *sample = gst_app_sink_pull_sample(appsink);
        if (!sample)
            return GST_FLOW_OK;
        int width = 0, height = 0;
        if (GstCaps *caps = gst_sample_get_caps(sample)) {
            const GstStructure *info = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(info, "width", &width);
            gst_structure_get_int(info, "height", &height);
        }
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (width > 0 && height > 0 && buffer
            && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            QVideoFrame frame(
                QVideoFrameFormat(QSize(width, height),
                                  QVideoFrameFormat::Format_RGBA8888));
            if (frame.map(QVideoFrame::WriteOnly)) {
                // Row-by-row: GStreamer's stride may exceed the frame's.
                const int sourceStride = static_cast<int>(map.size) / height;
                const int rowBytes =
                    std::min(frame.bytesPerLine(0), sourceStride);
                for (int y = 0; y < height; ++y) {
                    memcpy(frame.bits(0) + y * frame.bytesPerLine(0),
                           map.data + y * sourceStride,
                           static_cast<size_t>(rowBytes));
                }
                frame.unmap();
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->sink_) {
                    if (++self->frames_ == 1)
                        fprintf(stderr, "secure-usb: cam%d first decoded frame "
                                        "(%dx%d)\n", self->camera_, width, height);
                    // Safe off the GUI thread: Qt's own decoders deliver
                    // frames to QVideoSink from worker threads.
                    self->sink_->setVideoFrame(frame);
                }
            }
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // A decoder that errors after starting is otherwise silent: appsink just
    // stops producing samples.
    void drainBus()
    {
        GstBus *bus = gst_element_get_bus(element_);
        if (!bus)
            return;
        while (GstMessage *message =
                   gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
            GError *e = nullptr;
            gst_message_parse_error(message, &e, nullptr);
            fprintf(stderr, "secure-usb: cam%d decoder error: %s\n", camera_,
                    e ? e->message : "unknown");
            if (e)
                g_error_free(e);
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    }

    GstElement *element_ = nullptr;
    GstAppSrc *src_ = nullptr;
    std::mutex mutex_;
    QPointer<QVideoSink> sink_;
    int camera_ = 0;
    size_t frames_ = 0;
    size_t pushed_ = 0;
    size_t reportedPushed_ = 0;
    bool failed_ = false;
};

} // namespace

struct SecureUsbBridge::Impl {
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    int interfaceNumber = -1;
    unsigned char bulkIn = 0;
    unsigned char bulkOut = 0;
    std::array<VideoDecoder, kCameras> decoders;
    std::array<int, 2> listeners = {-1, -1};
    std::array<int, 2> clients = {-1, -1};
    std::unique_ptr<camera::secure::SecureRecord> decrypt;
    std::unique_ptr<camera::secure::SecureRecord> encrypt;
    std::atomic<bool> stopping{false};
    std::thread worker;

    // Inbound path: one dedicated thread doing blocking synchronous reads,
    // matching the reference implementation proven on this hardware. The
    // async libusb_submit_transfer pipeline that replaced it never completed
    // a single transfer against the tegra-xudc FunctionFS gadget, while the
    // synchronous ServerHello read on the same endpoint worked every time --
    // so pipelining was an unverified optimization sitting under every later
    // bug. Reintroduce it only against a working baseline, if throughput
    // measurably needs it.
    std::thread reader;
    std::mutex inboundMutex;
    camera::secure::WireBuffer inbound;   // guarded by inboundMutex
    std::atomic<bool> usbFailed{false};
    size_t records = 0;                   // worker thread only
    std::mutex metaMutex;
    std::function<void(int, std::string)> metaHandler;  // guarded by metaMutex

    void readerLoop()
    {
        // 16 KiB per read: WireBuffer reassembles records across reads, so
        // the buffer need not hold a whole record.
        std::vector<uint8_t> buffer(16 * 1024);
        size_t received = 0, reportedReceived = 0;
        while (!stopping) {
            int transferred = 0;
            // Finite timeout so `stopping` is honoured; a timeout with no
            // data is idle, not failure.
            const int rc = libusb_bulk_transfer(handle, bulkIn, buffer.data(),
                                                static_cast<int>(buffer.size()),
                                                &transferred, 250);
            if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
                fprintf(stderr, "secure-usb: bulk IN failed: %s\n",
                        libusb_strerror(libusb_error(rc)));
                usbFailed = true;
                return;
            }
            if (transferred > 0) {
                if (received == 0)
                    fprintf(stderr, "secure-usb: first %d raw bytes from device\n",
                            transferred);
                received += static_cast<size_t>(transferred);
                if (debugEnabled() && received - reportedReceived >= 4 * 1024 * 1024) {
                    reportedReceived = received;
                    fprintf(stderr, "secure-usb: %zu KiB raw received\n",
                            received / 1024);
                }
                std::lock_guard<std::mutex> lock(inboundMutex);
                inbound.append(buffer.data(), static_cast<size_t>(transferred));
            }
        }
    }

    // Returns false when the session must end.
    bool drainInbound()
    {
        std::vector<uint8_t> wire;
        for (;;) {
            auto framed = [&] {
                std::lock_guard<std::mutex> lock(inboundMutex);
                inbound.skip_zero_padding(); // the gadget pads some transfers
                return inbound.next(&wire);
            }();
            if (!framed.hasValue()) {
                // Say why. A silent teardown here is indistinguishable from
                // "the device never sent anything", and they need different
                // fixes.
                fprintf(stderr, "secure-usb: framing failure: %s\n",
                        framed.error().c_str());
                return false;
            }
            if (!framed.value())
                return true; // wait for the rest of the record
            auto message = camera::secure::open_wire_record(wire, *decrypt);
            if (!message.hasValue()) {
                fprintf(stderr, "secure-usb: record rejected: %s\n",
                        message.error().c_str());
                return false;
            }
            if (++records <= 3 || records % 500 == 0) {
                fprintf(stderr, "secure-usb: record %zu: channel %u stream %u, %zu bytes\n",
                        records, static_cast<unsigned>(message.value().channel),
                        static_cast<unsigned>(message.value().stream),
                        message.value().payload.size());
            }
            const auto &decoded = message.value();
            // OTA responses are rare and worth seeing every time.
            if (decoded.channel == camera::secure::Channel::Update)
                fprintf(stderr, "secure-usb: update: %zu bytes from device\n",
                        decoded.payload.size());
            if (decoded.channel == camera::secure::Channel::Video) {
                if (decoded.stream < kCameras)
                    decoders[decoded.stream].push(decoded.stream,
                                                  decoded.payload.data(),
                                                  decoded.payload.size());
                continue;
            }
            if (decoded.channel == camera::secure::Channel::Meta) {
                std::lock_guard<std::mutex> lock(metaMutex);
                if (metaHandler)
                    metaHandler(decoded.stream,
                                std::string(decoded.payload.begin(),
                                            decoded.payload.end()));
                continue;
            }
            const int index = channelIndex(decoded.channel);
            if (index >= 0 && clients[index] >= 0
                && !writeAll(clients[index], decoded.payload.data(), decoded.payload.size()))
                closeClient(index);
        }
    }

    ~Impl()
    {
        stopping = true;
        // Wake a worker blocked applying TCP backpressure before joining it.
        for (int fd : clients) {
            if (fd >= 0)
                shutdown(fd, SHUT_RDWR);
        }
        if (worker.joinable())
            worker.join();
        for (int fd : clients) {
            if (fd >= 0)
                close(fd);
        }
        for (int fd : listeners) {
            if (fd >= 0)
                close(fd);
        }
        if (handle) {
            if (interfaceNumber >= 0)
                libusb_release_interface(handle, interfaceNumber);
            libusb_close(handle);
        }
        if (context)
            libusb_exit(context);
    }

    bool sendRecord(int index, const uint8_t *data, size_t size)
    {
        const std::vector<uint8_t> payload(data, data + size);
        auto wire = camera::secure::make_wire_record(kChannels[index], 0, payload, *encrypt);
        if (!wire.hasValue()) {
            fprintf(stderr, "secure-usb: cannot seal outbound record: %s\n",
                    wire.error().c_str());
            return false;
        }
        int transferred = 0;
        // 60 s, not 5: an .swu upload backpressures through the device's
        // updater socket while swupdate commits to flash, and the device
        // stops reading the OUT endpoint for however long a slow erase
        // block takes. Timing out here killed the session mid-update.
        // Inbound video stalls for the duration of a blocked send (the
        // reader keeps buffering); that is the correct trade during an
        // update.
        const int result = libusb_bulk_transfer(
            handle, bulkOut, wire.value().data(), static_cast<int>(wire.value().size()),
            &transferred, 60000);
        if (result != 0 || transferred != static_cast<int>(wire.value().size())) {
            fprintf(stderr, "secure-usb: OUT transfer failed: %s (%d/%zu bytes)\n",
                    libusb_strerror(libusb_error(result)), transferred,
                    wire.value().size());
            return false;
        }
        return true;
    }

    void closeClient(int index)
    {
        if (clients[index] >= 0) {
            close(clients[index]);
            clients[index] = -1;
        }
    }

    void run()
    {
        std::array<uint8_t, camera::secure::kMaxWireRecord + 4> buffer{};
        reader = std::thread([this] { readerLoop(); });
        while (!stopping) {
            std::array<pollfd, 4> fds{};
            for (int i = 0; i < 2; ++i) {
                fds[i] = {listeners[i], POLLIN, 0};
                fds[i + 2] = {clients[i], POLLIN, 0};
            }
            const int ready = poll(fds.data(), fds.size(), 0);
            if (ready < 0 && errno != EINTR)
                break;
            for (int i = 0; i < 2; ++i) {
                if (fds[i].revents & POLLIN) {
                    const int client = accept4(listeners[i], nullptr, nullptr, SOCK_CLOEXEC);
                    if (client >= 0) {
                        closeClient(i); // one TCP stream per multiplexed channel
                        clients[i] = client;
                    }
                }
                if (clients[i] < 0 || !(fds[i + 2].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                // -2, not -1: the sealed payload carries the channel and
                // stream bytes too. A full 64 KiB read overflowed the seal
                // by one byte and killed the session -- precisely when an
                // update upload first saturated the buffer.
                const ssize_t count =
                    recv(clients[i], buffer.data(), camera::secure::kMaxPayload - 2, 0);
                if (count <= 0) {
                    closeClient(i);
                } else if (!sendRecord(i, buffer.data(), static_cast<size_t>(count))) {
                    // This was a bare `return`: it exited the worker without
                    // a word, skipping the session-ended report AND the
                    // reader join -- the session died with zero output while
                    // the reader kept draining the device into a buffer
                    // nobody read. Fail loudly through the normal exit path.
                    usbFailed = true;
                    break;
                }
            }

            if (usbFailed)
                break;
            if (!drainInbound())
                break;
            // The reader thread blocks in the kernel for up to 250 ms; this
            // thread paces itself on the TCP poll instead of spinning.
            usleep(5000);
        }
        stopping = true;
        if (reader.joinable())
            reader.join();  // before ~Impl frees the libusb handle
        fprintf(stderr, "secure-usb: session ended after %zu records%s\n", records,
                usbFailed ? " (USB transfer failed)" : "");
        for (int i = 0; i < 2; ++i)
            closeClient(i);
    }
};

std::unique_ptr<SecureUsbBridge> SecureUsbBridge::start(QString *error)
{
    auto bridge = std::unique_ptr<SecureUsbBridge>(new SecureUsbBridge);
    bridge->impl_ = std::make_unique<Impl>();
    Impl &impl = *bridge->impl_;
    if (libusb_init(&impl.context) != 0) {
        *error = QStringLiteral("cannot initialize libusb");
        return nullptr;
    }
    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(impl.context, &devices);
    if (count < 0) {
        *error = QStringLiteral("cannot enumerate USB devices");
        return nullptr;
    }
    bool foundSecureInterface = false;
    QString failure;
    for (ssize_t i = 0; i < count && !impl.handle; ++i) {
        libusb_config_descriptor *config = nullptr;
        if (libusb_get_active_config_descriptor(devices[i], &config) != 0)
            continue;
        for (uint8_t n = 0; n < config->bNumInterfaces && !impl.handle; ++n) {
            for (int a = 0; a < config->interface[n].num_altsetting && !impl.handle; ++a) {
                const auto &alt = config->interface[n].altsetting[a];
                if (alt.bInterfaceClass != 0xff || alt.bInterfaceSubClass != 0x53
                    || alt.bInterfaceProtocol != 0x55)
                    continue;
                unsigned char bulkIn = 0, bulkOut = 0;
                for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                    const auto &ep = alt.endpoint[e];
                    if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
                        != LIBUSB_TRANSFER_TYPE_BULK)
                        continue;
                    if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN)
                        bulkIn = ep.bEndpointAddress;
                    else
                        bulkOut = ep.bEndpointAddress;
                }
                if (!bulkIn || !bulkOut)
                    continue;
                foundSecureInterface = true;
                libusb_device_handle *handle = nullptr;
                // Keep the failing step and its libusb code: "cannot be
                // claimed" alone hides the common case, which is an open()
                // denied for want of a udev rule, not a busy interface.
                int rc = libusb_open(devices[i], &handle);
                if (rc != 0) {
                    failure = QStringLiteral("open: %1").arg(libusb_strerror(libusb_error(rc)));
                    continue;
                }
                libusb_set_auto_detach_kernel_driver(handle, 1);
                // SET_INTERFACE is required, not optional: every handshake
                // that ever succeeded against this gadget was preceded by
                // one. Removing it (on the theory it reset endpoint state)
                // made the ClientHello OUT transfer itself fail.
                if ((rc = libusb_claim_interface(handle, alt.bInterfaceNumber)) != 0
                    || (rc = libusb_set_interface_alt_setting(handle, alt.bInterfaceNumber,
                                                              alt.bAlternateSetting)) != 0) {
                    failure = QStringLiteral("claim: %1").arg(libusb_strerror(libusb_error(rc)));
                    libusb_close(handle);
                    continue;
                }
                impl.handle = handle;
                impl.interfaceNumber = alt.bInterfaceNumber;
                impl.bulkIn = bulkIn;
                impl.bulkOut = bulkOut;
            }
        }
        libusb_free_config_descriptor(config);
    }
    libusb_free_device_list(devices, 1);
    if (!impl.handle) {
        *error = foundSecureInterface
            ? QStringLiteral("secure USB camera found but unusable (%1); "
                             "if access is denied, install "
                             "host-ui/70-camera-secure-usb.rules").arg(failure)
            : QStringLiteral("no secure USB camera found (FF/53/55)");
        return nullptr;
    }

    const QByteArray pinned = qgetenv("CAMERA_SECURE_USB_CERT");
    if (pinned.isEmpty()) {
        *error = QStringLiteral("CAMERA_SECURE_USB_CERT is required for secure USB pairing");
        return nullptr;
    }
    // Claim the local ports before opening a session.
    //
    // Doing this after the handshake meant a bind failure (typically a viewer
    // left running from a previous session) tore down a session the device
    // had already authenticated. The UI then retried, and each attempt cost
    // the device a full session teardown: its video pipelines were killed and
    // respawned every couple of seconds, restarting the RTSP mount often
    // enough that the encoder never reached frame rate. Fail here instead,
    // where nothing has been disturbed.
    if (debugEnabled())
        fprintf(stderr, "secure-usb: interface claimed (ep IN 0x%02x, OUT 0x%02x)\n",
                impl.bulkIn, impl.bulkOut);
    gst_init(nullptr, nullptr);
    for (size_t i = 0; i < kPorts.size(); ++i) {
        impl.listeners[i] = makeListener(kPorts[i], error);
        if (impl.listeners[i] < 0)
            return nullptr;
    }

    auto handshake = camera::secure::ClientHandshake::create();
    if (!handshake.hasValue()) {
        *error = QString::fromStdString(handshake.error());
        return nullptr;
    }
    const auto hello = handshake.value()->client_hello();
    if (!hello.hasValue()) {
        *error = QString::fromStdString(hello.error());
        return nullptr;
    }
    int transferred = 0;
    int rc = LIBUSB_ERROR_IO;
    // Two attempts: claiming the interface (SET_INTERFACE) recycles the
    // gadget's endpoints, and a ClientHello sent inside that window can fail
    // with a transient I/O error. One short retry rides it out; persistent
    // failure still surfaces.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt != 0)
            usleep(300000);
        rc = libusb_bulk_transfer(impl.handle, impl.bulkOut,
                                  const_cast<unsigned char *>(hello.value().data()),
                                  static_cast<int>(hello.value().size()), &transferred,
                                  kHandshakeTimeoutMs);
        if (rc == 0 && transferred == static_cast<int>(hello.value().size()))
            break;
        fprintf(stderr, "secure-usb: ClientHello attempt %d: %s\n", attempt + 1,
                libusb_strerror(libusb_error(rc)));
    }
    if (rc != 0 || transferred != static_cast<int>(hello.value().size())) {
        *error = QStringLiteral("secure USB ClientHello transfer failed: %1 (%2/%3 bytes)")
                     .arg(libusb_strerror(libusb_error(rc)))
                     .arg(transferred)
                     .arg(hello.value().size());
        return nullptr;
    }
    std::vector<unsigned char> response(20 * 1024);
    rc = libusb_bulk_transfer(impl.handle, impl.bulkIn, response.data(),
                              static_cast<int>(response.size()), &transferred,
                              kHandshakeTimeoutMs);
    if (rc != 0) {
        *error = QStringLiteral("secure USB ServerHello transfer failed: %1")
                     .arg(libusb_strerror(libusb_error(rc)));
        return nullptr;
    }
    if (debugEnabled())
        fprintf(stderr, "secure-usb: ClientHello sent, ServerHello %d bytes\n",
                transferred);
    response.resize(static_cast<size_t>(transferred));
    const auto keys = handshake.value()->complete(response, pinned.toStdString());
    if (!keys.hasValue()) {
        *error = QString::fromStdString(keys.error());
        return nullptr;
    }
    impl.encrypt = std::make_unique<camera::secure::SecureRecord>(keys.value().host_key,
                                                                  keys.value().host_iv);
    impl.decrypt = std::make_unique<camera::secure::SecureRecord>(keys.value().device_key,
                                                                  keys.value().device_iv);

    fprintf(stderr, "secure-usb: session established (video decoded in-process; "
                    "local proxies: control 8555, update 8557)\n");
    impl.worker = std::thread([pointer = &impl] { pointer->run(); });
    return bridge;
}

void SecureUsbBridge::setVideoSink(int camera, QVideoSink *sink)
{
    if (impl_ && camera >= 0 && camera < kCameras)
        impl_->decoders[camera].setSink(sink);
}

void SecureUsbBridge::setMetaHandler(std::function<void(int, std::string)> handler)
{
    // The worker is already reading, so guard the assignment.
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->metaMutex);
        impl_->metaHandler = std::move(handler);
    }
}

bool SecureUsbBridge::isRunning() const
{
    return impl_ && !impl_->stopping;
}

SecureUsbBridge::~SecureUsbBridge() = default;

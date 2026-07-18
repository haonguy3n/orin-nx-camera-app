#include "camera/secure/SecureUsbServer.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "camera/base/ScopeGuard.h"
#include "camera/base/logging/xlog.h"
#include "camera/detect/FaceDetector.h"
#include "camera/secure/FfsGadget.h"
#include "secure/SecureHandshake.h"
#include "secure/SecureWire.h"
#include "camera/secure/SecureRecord.h"

namespace camera::secure {
namespace {


constexpr uint8_t kCameras = 2;

struct ScopedFd {
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) close(fd_); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
private:
    int fd_;
};

// Endpoint I/O on FunctionFS blocks in an uninterruptible-by-close wait:
// the epfiles have no .poll, O_NONBLOCK does not apply to the data phase,
// and closing the fd from another thread wakes nothing. The ONE portable
// wake-up is a signal -- wait_for_completion_interruptible returns -EINTR.
// Teardown therefore signals blocked endpoint threads with SIGUSR1 (no-op
// handler, installed without SA_RESTART so syscalls are not auto-restarted),
// and the I/O helpers abort on EINTR when their abort flag is set. Without
// this, a host that stops reading left the writer blocked in the kernel
// forever, and joining it hung service shutdown until systemd's SIGKILL.
void wake_signal_handler(int) {}

void install_wake_signal() {
    struct sigaction action {};
    action.sa_handler = wake_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // deliberately no SA_RESTART
    sigaction(SIGUSR1, &action, nullptr);
    // deliver_local() writes to local TCP sockets; writing into one the
    // peer already closed (an updater from a finished upload) raises
    // SIGPIPE, whose default action would kill the whole service.
    signal(SIGPIPE, SIG_IGN);
}

// Signals `thread` until `exited` confirms it unwound. The loop, rather than
// one signal, closes the race where the signal lands between the abort-flag
// check and the blocking syscall.
void wake_until_exited(pthread_t thread, const std::atomic<bool>& exited) {
    while (!exited) {
        pthread_kill(thread, SIGUSR1);
        usleep(10000);
    }
}

// write_all for the endpoint: abortable at teardown, when the peer may never
// drain what is already queued.
bool endpoint_write_all(int fd, const uint8_t* bytes, size_t length,
                        const std::atomic<bool>& abort_flag) {
    while (length != 0) {
        const ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR) {
            if (abort_flag) return false;
            continue;
        }
        if (n <= 0) return false;
        bytes += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

// Video is pulled in-process rather than by forking gst-launch.
//
// Two reasons. A missing gst-launch-1.0 binary fails as _exit(127), which is
// indistinguishable from a pipeline that runs and produces nothing -- and
// camera-streamer already links GStreamer, so the dependency was avoidable.
// Second, the child built its argv *after* fork(): allocating in the child of
// a multi-threaded process deadlocks if any other thread held the malloc lock
// at fork time, which would hang the reader thread forever.
struct Pipeline {
    GstElement* element = nullptr;
    GstAppSink* sink = nullptr;
    GstAppSink* detect = nullptr;  // optional raw-BGR branch for face detection

    ~Pipeline() {
        if (element) {
            gst_element_set_state(element, GST_STATE_NULL);
            gst_object_unref(element);
        }
    }

    bool start(const std::string& description, std::string* error) {
        GError* failure = nullptr;
        element = gst_parse_launch(description.c_str(), &failure);
        if (element == nullptr) {
            *error = failure != nullptr ? failure->message : "unknown parse error";
            if (failure != nullptr) g_error_free(failure);
            return false;
        }
        if (failure != nullptr) g_error_free(failure);
        GstElement* raw = gst_bin_get_by_name(GST_BIN(element), "sink");
        if (raw == nullptr) {
            *error = "pipeline has no appsink";
            return false;
        }
        sink = GST_APP_SINK(raw);
        // The detection branch is optional -- only present when the launch
        // string carries a "detect" appsink (detection enabled).
        if (GstElement* d = gst_bin_get_by_name(GST_BIN(element), "detect"))
            detect = GST_APP_SINK(d);
        if (gst_element_set_state(element, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            *error = "pipeline refused to start";
            return false;
        }
        return true;
    }

    // Reports a pipeline error once, so a mount that never negotiates says so
    // instead of looking like a camera that produces nothing.
    void drain_bus(uint8_t camera) {
        GstBus* bus = gst_element_get_bus(element);
        if (bus == nullptr) return;
        while (GstMessage* message = gst_bus_pop_filtered(
                   bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* failure = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &failure, &debug);
                XLOGF(WARN, "secure-usb: cam%u pipeline error: %s",
                      static_cast<unsigned>(camera),
                      failure != nullptr ? failure->message : "unknown");
                if (failure != nullptr) g_error_free(failure);
                g_free(debug);
            }
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    }
};

// The single thread that writes to the endpoint. Several producers -- two
// video pipelines and the control/update tunnel -- enqueue records; this
// drains them in seal order. Isolated from Session so the queue machinery is
// not tangled with the session's local-socket and watchdog state.
class RecordWriter {
public:
    RecordWriter(int endpoint, const SecureRecord::Key& key,
                 const SecureRecord::Iv& iv, std::atomic<bool>& dead)
        : endpoint_(endpoint), encrypt_(key, iv), dead_(dead) {}

    // Seal `size` bytes and queue them. The ordering lock spans seal+queue so
    // wire order matches seal order: the record nonce is an implicit counter,
    // and reordering (or dropping *after* sealing) desynchronises the peer.
    //
    // `droppable` video is discarded once the queue passes kQueueLimit rather
    // than blocking the producer -- a synchronous stall here would back up
    // through the encoder until the frame watchdog killed the camera. Control
    // and update are never droppable; they are not replaceable. Returns false
    // only if sealing fails.
    bool enqueue(Channel channel, uint8_t stream, const uint8_t* data,
                 size_t size, bool droppable) {
        std::lock_guard<std::mutex> ordering(ordering_);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (droppable && queued_bytes_ >= kQueueLimit) {
                dropped_bytes_ += size;
                return true;
            }
        }
        auto wire = make_wire_record(channel, stream,
                                     std::vector<uint8_t>(data, data + size), encrypt_);
        if (!wire.hasValue()) return false;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queued_bytes_ += wire.value().size();
        queue_.push_back(std::move(wire.value()));
        queue_signal_.notify_one();
        return true;
    }

    // Drains the queue on this thread until `dead`. `should_end` is polled
    // each idle tick (the session's host-silence watchdog); returning true
    // ends the session.
    void run(const std::function<bool()>& should_end) {
        thread_ = pthread_self();
        started_ = true;
        size_t written = 0, reported = 0;
        while (!dead_) {
            if (should_end()) { dead_ = true; break; }
            std::vector<uint8_t> record;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_signal_.wait_for(lock, std::chrono::milliseconds(100),
                                       [this] { return !queue_.empty() || dead_; });
                if (queue_.empty()) continue;
                record = std::move(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= record.size();
            }
            if (!endpoint_write_all(endpoint_, record.data(), record.size(), dead_)) {
                dead_ = true;
                break;
            }
            if (written == 0)
                XLOGF(INFO, "secure-usb: first record (%zu bytes) on the endpoint",
                      record.size());
            written += record.size();
            // "sent" in video_loop counts enqueues; this counts bytes the host
            // actually drained. Diverging numbers mean a stalled host.
            if (written - reported >= 8 * 1024 * 1024) {
                reported = written;
                XLOGF(INFO, "secure-usb: %zu KiB drained by host", written / 1024);
            }
        }
        exited_ = true;
    }

    // Wakes a write blocked in the kernel so run() can observe `dead`.
    void request_stop() {
        queue_signal_.notify_all();
        if (started_)
            wake_until_exited(thread_, exited_);
    }

    size_t dropped_bytes() const { return dropped_bytes_; }

private:
    static constexpr size_t kQueueLimit = 4 * 1024 * 1024;
    int endpoint_;
    SecureRecord encrypt_;  // guarded by ordering_
    std::atomic<bool>& dead_;
    std::mutex ordering_;
    std::mutex queue_mutex_;
    std::condition_variable queue_signal_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t queued_bytes_ = 0;
    size_t dropped_bytes_ = 0;
    pthread_t thread_{};
    std::atomic<bool> started_{false};
    std::atomic<bool> exited_{false};
};

// The local control/update tunnel. Host->device records for these channels
// are handed to loopback TCP servers (deliver); replies produced locally are
// polled and passed back to the writer via the enqueue callback (pump). Video
// never touches this -- it is device-to-host only.
class LocalTunnel {
public:
    using Enqueue = std::function<bool(Channel, const uint8_t*, size_t)>;
    LocalTunnel(std::atomic<bool>& dead, Enqueue enqueue)
        : dead_(dead), enqueue_(std::move(enqueue)) {}

    // Host -> local server. A dead local server drops its channel; it must not
    // end the session, which also carries video. Always returns true.
    bool deliver(const WireMessage& message) {
        if (message.channel == Channel::Video) return true;  // device-to-host only
        std::lock_guard<std::mutex> lock(mutex_);
        int& fd = message.channel == Channel::Control ? control_ : update_;
        const uint16_t port = message.channel == Channel::Control ? 8555 : 8557;
        // No per-record log here: this is the upload data path, so a .swu push
        // is thousands of 64 KiB chunks -- it would flood the journal. OTA
        // progress is tracked via get-update-status instead.
        // Two attempts with a fresh connection between: a held-open fd may be a
        // finished session's socket (EPIPE on write), and dropping this payload
        // on that would eat the first chunk of the next upload.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (fd < 0)
                fd = connect_local(port);
            if (fd < 0)
                break;
            if (endpoint_write_all(fd, message.payload.data(),
                                   message.payload.size(), dead_))
                return true;
            close(fd);
            fd = -1;
        }
        XLOGF(WARN, "secure-usb: cannot deliver a %s request to 127.0.0.1:%u; dropped",
              message.channel == Channel::Control ? "control" : "update", port);
        return true;
    }

    // True while an updater connection is open: an upload can stall all tunnel
    // traffic for as long as a flash commit takes, and the host-silence
    // watchdog must not read that as a departed viewer.
    bool has_active_update() {
        std::lock_guard<std::mutex> lock(mutex_);
        return update_ >= 0;
    }

    // Local servers -> host. poll() is valid here: these are TCP sockets, not
    // FunctionFS endpoint files. Runs on its own thread until `dead`.
    void pump() {
        std::vector<uint8_t> chunk(32 * 1024);
        while (!dead_) {
            int snapshot[2];
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot[0] = control_;
                snapshot[1] = update_;
            }
            pollfd fds[] = {{snapshot[0], POLLIN, 0}, {snapshot[1], POLLIN, 0}};
            if (poll(fds, 2, 100) <= 0) continue;
            for (int i = 0; i != 2; ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                const Channel channel = i == 0 ? Channel::Control : Channel::Update;
                const ssize_t n = read(fds[i].fd, chunk.data(), chunk.size());
                if (n <= 0) { close_channel(channel); continue; }
                if (!enqueue_(channel, chunk.data(), static_cast<size_t>(n))) {
                    dead_ = true;
                    return;
                }
            }
        }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Shut down before closing so a thread blocked in read() wakes.
        for (int* fd : {&control_, &update_}) {
            if (*fd >= 0) { ::shutdown(*fd, SHUT_RDWR); close(*fd); *fd = -1; }
        }
    }

private:
    static int connect_local(uint16_t port) {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    void close_channel(Channel channel) {
        std::lock_guard<std::mutex> lock(mutex_);
        int& fd = channel == Channel::Control ? control_ : update_;
        if (fd >= 0) { close(fd); fd = -1; }
    }

    std::atomic<bool>& dead_;
    Enqueue enqueue_;
    std::mutex mutex_;
    int control_ = -1;
    int update_ = -1;
};

// One authenticated session: a thin coordinator over the outbound writer, the
// local control/update tunnel, and the host-silence watchdog.
struct Session {
    Session(int endpoint, const SecureRecord::Key& key, const SecureRecord::Iv& iv)
        : writer_(endpoint, key, iv, dead),
          tunnel_(dead, [this](Channel c, const uint8_t* d, size_t n) {
              return writer_.enqueue(c, 0, d, n, /*droppable=*/false);
          }) {}

    std::atomic<bool> dead{false};

    // Last time anything arrived from the host. The UI polls status every few
    // seconds over the tunnel, so a live viewer keeps this fresh; a host that
    // exits just goes silent -- a gadget gets no disconnect signal -- and
    // without this the cameras kept capturing forever afterwards.
    std::atomic<int64_t> last_inbound_ms{0};

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool enqueue(Channel channel, uint8_t stream, const uint8_t* data, size_t size,
                 bool droppable) {
        return writer_.enqueue(channel, stream, data, size, droppable);
    }
    size_t dropped_bytes() const { return writer_.dropped_bytes(); }
    bool deliver_local(const WireMessage& message) { return tunnel_.deliver(message); }
    void pump_tunnel() { tunnel_.pump(); }

    // Writer-thread entry: runs the record writer with the host-silence
    // watchdog as its end condition. Silence well beyond the UI's poll cadence
    // means the viewer is gone -- ending the session stops the video pipelines,
    // releasing the cameras.
    void writer_loop() {
        writer_.run([this] {
            // The UI polls control every ~2 s, so silence past a couple of
            // polls means the viewer is gone. A soft disconnect (releasing
            // the USB handle) does not error the endpoint, so this watchdog
            // -- not an I/O failure -- is what stops the cameras; keep it
            // tight so a disconnect releases the sensors promptly. An active
            // OTA upload legitimately stalls the tunnel and is exempt.
            constexpr int64_t kHostSilenceMs = 5000;
            if (now_ms() - last_inbound_ms >= kHostSilenceMs
                && !tunnel_.has_active_update()) {
                XLOGF(INFO, "secure-usb: host silent for %llds; ending session "
                            "(cameras stop)",
                      static_cast<long long>(kHostSilenceMs / 1000));
                return true;
            }
            return false;
        });
    }

    void shutdown() {
        dead = true;
        // The writer may be blocked in a kernel write on the endpoint; only a
        // signal wakes it (see endpoint I/O comment above).
        writer_.request_stop();
        tunnel_.shutdown();
    }

private:
    RecordWriter writer_;
    LocalTunnel tunnel_;
};

// Pulls raw BGR frames from the pipeline's optional detection branch, runs the
// face detector, and pushes the boxes over Channel::Meta. Runs on its own
// thread so inference never blocks the video path; boxes are droppable.
void detection_loop(Session& session, uint8_t camera, GstAppSink* detect,
                    detect::IFaceDetector& detector,
                    const std::atomic<bool>& stop) {
    while (!session.dead && !stop) {
        GstSample* sample = gst_app_sink_try_pull_sample(detect, 200 * GST_MSECOND);
        if (sample == nullptr) continue;
        int w = 0, h = 0;
        if (GstCaps* caps = gst_sample_get_caps(sample)) {
            const GstStructure* s = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(s, "width", &w);
            gst_structure_get_int(s, "height", &h);
        }
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (w > 0 && h > 0 && buffer != nullptr
            && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const int stride = static_cast<int>(map.size) / h;
            const auto boxes = detector.detect(map.data, w, h, stride);
            gst_buffer_unmap(buffer, &map);
            const std::string json = detect::to_meta_json(camera, w, h, boxes);
            session.enqueue(Channel::Meta, camera,
                            reinterpret_cast<const uint8_t*>(json.data()),
                            json.size(), /*droppable=*/true);
        }
        gst_sample_unref(sample);
    }
}

// Pulls one camera's encoded H.265 elementary stream and pushes it over the
// session. When `detect_model` is set the pipeline also carries a raw branch,
// and a detection_loop thread runs face detection alongside.
void video_loop(Session& session, uint8_t camera, const std::string& description,
                const std::atomic<bool>& enabled, const std::string& detect_model,
                int detect_w, int detect_h) {
    // Load the detector once for the camera's lifetime (model load is heavy).
    std::unique_ptr<detect::IFaceDetector> detector;
    if (!detect_model.empty()) {
        auto d = detect::create_face_detector(detect_model, detect_w, detect_h);
        if (d.hasValue())
            detector = std::move(d.value());
        else
            XLOGF(WARN, "secure-usb: cam%u face detection disabled: %s",
                  static_cast<unsigned>(camera), d.error().c_str());
    }
    // A camera that is absent never produces a byte, and retrying it forever
    // recreates the device's RTSP mount every few seconds, which restarts its
    // frame watchdog and buries the log. Give up on a barren camera for the
    // rest of the session; the other camera is unaffected either way.
    constexpr int kBarrenLimit = 3;
    int barren = 0;
    while (!session.dead) {
        if (!enabled) {
            // set-stream stopped this camera: pipeline stays down (sensor
            // released) until it is enabled again.
            barren = 0;
            usleep(200000);
            continue;
        }
        Pipeline pipeline;
        std::string error;
        bool produced = false;
        if (!pipeline.start(description, &error)) {
            XLOGF(WARN, "secure-usb: cam%u pipeline did not start: %s",
                  static_cast<unsigned>(camera), error.c_str());
        } else {
            // Detection runs alongside the video pull, on the raw branch, for
            // this pipeline's lifetime. Joined before ~Pipeline tears the
            // branch down.
            std::atomic<bool> detect_stop{false};
            std::thread detect_thread;
            if (detector && pipeline.detect != nullptr) {
                detect_thread = std::thread([&] {
                    detection_loop(session, camera, pipeline.detect, *detector,
                                   detect_stop);
                });
            }
            SCOPE_EXIT {
                detect_stop = true;
                if (detect_thread.joinable()) detect_thread.join();
            };
            size_t total = 0, reported = 0;
            while (!session.dead) {
                // Bounded pull so session teardown is noticed promptly even
                // when the camera has gone quiet.
                if (!enabled) {
                    XLOGF(INFO, "secure-usb: cam%u stream stopped by set-stream",
                          static_cast<unsigned>(camera));
                    break;  // ~Pipeline releases the sensor
                }
                GstSample* sample = gst_app_sink_try_pull_sample(
                    pipeline.sink, 200 * GST_MSECOND);
                if (sample == nullptr) {
                    pipeline.drain_bus(camera);
                    if (gst_app_sink_is_eos(pipeline.sink)) break;
                    continue;
                }
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                GstMapInfo map;
                bool ok = true;
                if (buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    if (!produced) {
                        XLOGF(INFO, "secure-usb: cam%u streaming (first %zu bytes)",
                              static_cast<unsigned>(camera), map.size);
                    }
                    produced = true;
                    total += map.size;
                    if (total - reported >= 8 * 1024 * 1024) {
                        reported = total;
                        XLOGF(INFO, "secure-usb: cam%u %zu KiB sent",
                              static_cast<unsigned>(camera), total / 1024);
                    }
                    // Split across records: one access unit is not one
                    // record. An IDR frame at this resolution and bitrate
                    // routinely exceeds kMaxPayload, and handing an
                    // oversized payload to make_wire_record fails the
                    // enqueue, which used to kill the session on the first
                    // keyframe -- after the tiny VPS/SPS/PPS buffer had
                    // already gone through.
                    constexpr size_t kChunk = kMaxPayload - 2;  // channel+stream
                    for (size_t offset = 0; offset < map.size && ok;
                         offset += kChunk) {
                        const size_t piece = std::min(kChunk, map.size - offset);
                        ok = session.enqueue(Channel::Video, camera,
                                             map.data + offset, piece, true);
                    }
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
                if (!ok) {
                    session.dead = true;
                    break;
                }
            }
        }
        if (session.dead)
            return;

        // A pipeline that streamed and then stopped is a restart worth
        // retrying; one that never produced a byte is a camera that is not
        // there.
        barren = produced ? 0 : barren + 1;
        if (barren >= kBarrenLimit) {
            XLOGF(WARN,
                  "secure-usb: cam%u produced no video in %d attempts; leaving it "
                  "alone for this session (other cameras keep streaming)",
                  static_cast<unsigned>(camera), barren);
            return;
        }
        const int delay_ms = 3000 * barren;
        for (int waited = 0; waited < delay_ms && !session.dead; waited += 100)
            usleep(100000);
    }
}

// Local control/update servers -> host.  poll() is valid here: these are TCP
// sockets, not FunctionFS endpoint files.
}  // namespace

SecureUsbServer::SecureUsbServer(std::string certificate, std::string private_key,
                                 std::vector<std::string> video_launch)
    : certificate_(std::move(certificate)),
      private_key_(std::move(private_key)),
      video_launch_(std::move(video_launch)) {}
SecureUsbServer::~SecureUsbServer() { stop(); }

bool SecureUsbServer::start(std::string* error) {
    if (certificate_.empty() || private_key_.empty()) {
        *error = "secure USB needs device certificate and private key";
        return false;
    }
    if (access(certificate_.c_str(), R_OK) != 0 || access(private_key_.c_str(), R_OK) != 0) {
        *error = "secure USB certificate/private key is not readable";
        return false;
    }
    worker_ = std::thread(&SecureUsbServer::run, this);
    return true;
}
void SecureUsbServer::set_stream_enabled(uint8_t camera, bool enabled) {
    if (camera < kCameras)
        stream_enabled_[camera] = enabled;
}

void SecureUsbServer::stop() {
    stopping_ = true;
    if (worker_.joinable()) {
        // The worker is usually blocked in a FunctionFS read (waiting for a
        // ClientHello, or for the next record); only a signal wakes that.
        // A bare join here hung service shutdown until systemd's SIGKILL.
        wake_until_exited(worker_.native_handle(), worker_exited_);
        worker_.join();
    }
}

void SecureUsbServer::set_face_detection(std::string model, int input_width,
                                         int input_height) {
    detect_model_ = std::move(model);
    detect_width_ = input_width;
    detect_height_ = input_height;
}

std::string SecureUsbServer::video_description(uint8_t camera) const {
    if (camera < video_launch_.size() && !video_launch_[camera].empty())
        return video_launch_[camera];
    // Fallback: the RTSP server owns the sensor, so take the frames back off
    // its local mount. Costs an RTP payload/depayload round trip through
    // loopback, which is why it is not used when we can tap the encoder.
    return "rtspsrc location=rtsp://127.0.0.1:8554/cam" + std::to_string(camera) +
           " protocols=tcp latency=0"
           " ! rtph265depay ! h265parse config-interval=-1"
           " ! video/x-h265,stream-format=byte-stream"
           " ! appsink name=sink sync=false max-buffers=8 drop=true";
}

void SecureUsbServer::run() {
    // Arm SIGUSR1 before any endpoint I/O: without a handler the wake signal
    // used by teardown would terminate the whole process.
    install_wake_signal();
    // Every exit path must publish this, including the setup failure that
    // returns early -- stop() waits on it before joining.
    SCOPE_EXIT { worker_exited_ = true; };

    // The gadget owns the FunctionFS lifecycle: its destructor removes the
    // function (by closing ep0) when this scope exits.
    auto gadget = FfsGadget::create();
    if (!gadget.hasValue()) {
        XLOGF(ERR, "secure-usb: %s", gadget.error().c_str());
        return;
    }
    const int ep0 = gadget.value()->control_endpoint();

    while (!stopping_) {
        serve_session(ep0);
        if (!stopping_)
            usleep(100000);  // let the host notice the drop before retrying
    }
}

// One authenticated session lifetime of the endpoint pair.
//
// Threads rather than an event loop: FunctionFS endpoint files implement no
// .poll, so poll() cannot indicate readiness. The endpoint is read in LARGE
// chunks into a WireBuffer and records are framed from that stream -- never
// with small exact-size endpoint reads. A read request smaller than the
// packet that arrives (a 4-byte header read meeting a 53-byte control
// record) is exactly the case a UDC may reject until the host times out;
// large reads also restore mid-session ClientHello detection, so a
// restarting host re-handshakes instead of desyncing the stream.
void SecureUsbServer::serve_session(int ep0) {
    const int in = open("/dev/ffs-secure/ep1", O_WRONLY | O_CLOEXEC);
    const int out = open("/dev/ffs-secure/ep2", O_RDONLY | O_CLOEXEC);
    if (in < 0 || out < 0) {
        if (in >= 0) close(in);
        if (out >= 0) close(out);
        usleep(200000);
        return;
    }
    ScopedFd in_guard(in), out_guard(out);

    WireBuffer stream;
    std::vector<uint8_t> chunk(16 * 1024);
    size_t inbound_records = 0;
    std::unique_ptr<SecureRecord> decrypt;
    std::unique_ptr<Session> session;
    std::vector<std::thread> workers;

    auto end_session = [&] {
        if (!session)
            return;
        session->shutdown();
        for (auto& worker : workers)
            worker.join();
        workers.clear();
        if (session->dropped_bytes() != 0) {
            XLOGF(INFO, "secure-usb: session ended (dropped %zu KiB of video)",
                  session->dropped_bytes() / 1024);
        } else {
            XLOGF(INFO, "secure-usb: session ended");
        }
        session.reset();
        decrypt.reset();
    };

    bool endpoint_failed = false;
    while (!stopping_ && !endpoint_failed) {
        // Consume FunctionFS events, by name: endpoint state transitions are
        // exactly what a stalled-transfer investigation needs.
        std::array<uint8_t, 12> event{};
        while (read(ep0, event.data(), event.size()) > 0) {
            static const char* kNames[] = {"BIND", "UNBIND", "ENABLE", "DISABLE",
                                           "SETUP", "SUSPEND", "RESUME"};
            const uint8_t type = event[8];  // struct usb_functionfs_event.type
            XLOGF(INFO, "secure-usb: ep0 event %s",
                  type < 7 ? kNames[type] : "UNKNOWN");
        }

        const ssize_t received = read(out, chunk.data(), chunk.size());
        if (received < 0 && errno == EINTR) {
            if (stopping_) break;
            continue;
        }
        if (received <= 0)
            break;  // endpoint recycled or host gone
        stream.append(chunk.data(), static_cast<size_t>(received));

        while (!endpoint_failed) {
            if (stream.starts_with(kClientHelloMagic, sizeof(kClientHelloMagic))) {
                if (stream.size() < kClientHelloSize)
                    break;  // await the rest of the handshake
                const std::vector<uint8_t> hello(stream.data(),
                                                 stream.data() + kClientHelloSize);
                stream.consume(kClientHelloSize);
                end_session();  // a new handshake supersedes any live session
                auto response =
                    DeviceHandshake::respond(hello, certificate_, private_key_);
                if (!response.hasValue()) {
                    XLOGF(WARN, "secure-usb: handshake rejected: %s",
                          response.error().c_str());
                    continue;
                }
                const auto& wire = response.value().first;
                if (!endpoint_write_all(in, wire.data(), wire.size(), stopping_)) {
                    endpoint_failed = true;
                    break;
                }
                decrypt = std::make_unique<SecureRecord>(
                    response.value().second.host_key, response.value().second.host_iv);
                session = std::make_unique<Session>(in,
                                                    response.value().second.device_key,
                                                    response.value().second.device_iv);
                session->last_inbound_ms = Session::now_ms();
                XLOGF(INFO, "secure-usb: authenticated session established");
                Session* raw = session.get();
                for (uint8_t camera = 0; camera < kCameras; ++camera) {
                    workers.emplace_back([this, raw, camera] {
                        video_loop(*raw, camera, video_description(camera),
                                   stream_enabled_[camera], detect_model_,
                                   detect_width_, detect_height_);
                    });
                }
                workers.emplace_back([raw] { raw->pump_tunnel(); });
                workers.emplace_back([raw] { raw->writer_loop(); });
                continue;
            }
            if (!decrypt) {
                // Bytes before any handshake cannot be interpreted; keeping
                // them would desynchronise the handshake to come.
                if (stream.size() != 0) {
                    XLOGF(WARN,
                          "secure-usb: discarding %zu bytes received before handshake",
                          stream.size());
                    stream.clear();
                }
                break;
            }
            std::vector<uint8_t> wire;
            auto framed = stream.next(&wire);
            if (!framed.hasValue()) {
                XLOGF(WARN, "secure-usb: %s", framed.error().c_str());
                end_session();
                stream.clear();
                break;
            }
            if (!framed.value())
                break;  // await the remainder of the record
            auto message = open_wire_record(wire, *decrypt);
            if (!message.hasValue()) {
                XLOGF(WARN, "secure-usb: %s", message.error().c_str());
                end_session();
                stream.clear();
                break;
            }
            session->last_inbound_ms = Session::now_ms();
            // Mirrors the host's "record N" lines so the two logs can be
            // laid side by side when tracing a transfer.
            ++inbound_records;
            if (inbound_records <= 3 || inbound_records % 500 == 0) {
                XLOGF(INFO, "secure-usb: record %zu from host: channel %u, %zu bytes",
                      inbound_records,
                      static_cast<unsigned>(message.value().channel),
                      message.value().payload.size());
            }
            session->deliver_local(message.value());
        }
        if (session && session->dead)
            break;  // the writer saw the endpoint fail
    }
    end_session();
}
}  // namespace camera::secure

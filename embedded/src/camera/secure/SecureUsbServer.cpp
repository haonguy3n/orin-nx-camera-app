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
#include "camera/detect/MetaSink.h"
#include "camera/media/CameraPipeline.h"
#include "camera/detect/Snapshot.h"
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

// Video is pulled in-process rather than by forking gst-launch: a missing
// gst-launch-1.0 fails as _exit(127), indistinguishable from a pipeline that
// runs and produces nothing, and building argv after fork() in a
// multi-threaded process can deadlock on the malloc lock. The pipeline itself
// now lives in media::CameraPipeline, shared with every other transport.

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
    using Dispatch = std::function<std::string(const std::string&)>;
    using OpenUpdate = std::function<int()>;
    LocalTunnel(std::atomic<bool>& dead, Enqueue enqueue, Dispatch dispatch,
                OpenUpdate open_update)
        : dead_(dead), enqueue_(std::move(enqueue)),
          dispatch_(std::move(dispatch)),
          open_update_(std::move(open_update)) {}

    // Host -> local server. A dead local server drops its channel; it must not
    // end the session, which also carries video. Always returns true.
    bool deliver(const WireMessage& message) {
        if (message.channel == Channel::Video) return true;  // device-to-host only
        // Control in-process: no loopback socket, so nothing to connect, drop
        // or reconnect. Requests are newline-delimited and a record may carry
        // a partial or several lines, so they are reassembled here -- the TCP
        // path got that for free from GDataInputStream.
        if (message.channel == Channel::Control && dispatch_) {
            control_buffer_.append(
                reinterpret_cast<const char*>(message.payload.data()),
                message.payload.size());
            size_t nl;
            while ((nl = control_buffer_.find('\n')) != std::string::npos) {
                const std::string line = control_buffer_.substr(0, nl);
                control_buffer_.erase(0, nl + 1);
                const std::string reply = dispatch_(line);
                if (!reply.empty())
                    enqueue_(Channel::Control,
                             reinterpret_cast<const uint8_t*>(reply.data()),
                             reply.size());
            }
            return true;
        }
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
                fd = (message.channel == Channel::Update && open_update_)
                         ? open_update_()      // in-process socketpair
                         : connect_local(port);  // legacy loopback
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
    Dispatch dispatch_;
    OpenUpdate open_update_;
    std::string control_buffer_;  // reassembles newline-delimited requests
    std::mutex mutex_;
    int control_ = -1;
    int update_ = -1;
};

// One authenticated session: a thin coordinator over the outbound writer, the
// local control/update tunnel, and the host-silence watchdog.
struct Session {
    Session(int endpoint, const SecureRecord::Key& key, const SecureRecord::Iv& iv,
            LocalTunnel::Dispatch dispatch, LocalTunnel::OpenUpdate open_update)
        : writer_(endpoint, key, iv, dead),
          tunnel_(dead,
                  [this](Channel c, const uint8_t* d, size_t n) {
                      return writer_.enqueue(c, 0, d, n, /*droppable=*/false);
                  },
                  std::move(dispatch), std::move(open_update)) {}

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

// Encoded frames -> encrypted records. The transport half of the capture/
// transport split: CameraPipeline will drive this via on_frame once video_loop
// moves onto it, and today video_loop calls it directly, so the behaviour is
// identical either way.
class VideoSink : public media::IFrameTransport {
public:
    VideoSink(Session& session, uint8_t camera,
              const std::function<void(uint8_t, bool, size_t)>& report)
        : session_(session), camera_(camera), report_(report) {}

    void on_frame(uint8_t camera, const media::Frame& frame) override {
        if (!first_) {
            XLOGF(INFO, "secure-usb: cam%u streaming (first %zu bytes)",
                  static_cast<unsigned>(camera), frame.size);
            first_ = true;
        }
        total_ += frame.size;
        if (report_) report_(camera, true, frame.size);
        if (total_ - reported_ >= 8 * 1024 * 1024) {
            reported_ = total_;
            XLOGF(INFO, "secure-usb: cam%u %zu KiB sent",
                  static_cast<unsigned>(camera), total_ / 1024);
        }
        // Split across records: one access unit is not one record. An IDR
        // frame at this resolution and bitrate routinely exceeds kMaxPayload,
        // and handing an oversized payload to make_wire_record fails the
        // enqueue -- which used to kill the session on the first keyframe,
        // after the tiny VPS/SPS/PPS buffer had already gone through.
        constexpr size_t kChunk = kMaxPayload - 2;  // channel + stream bytes
        bool ok = true;
        for (size_t offset = 0; offset < frame.size && ok; offset += kChunk) {
            const size_t piece = std::min(kChunk, frame.size - offset);
            ok = session_.enqueue(Channel::Video, camera, frame.data + offset,
                                  piece, /*droppable=*/true);
        }
        if (!ok) session_.dead = true;  // sealing failed; the peer is desynced
    }

    [[nodiscard]] bool produced() const { return first_; }

private:
    Session& session_;
    uint8_t camera_;
    const std::function<void(uint8_t, bool, size_t)>& report_;
    bool first_ = false;
    size_t total_ = 0;
    size_t reported_ = 0;
};

// Session's delivery: boxes ride the encrypted Meta channel.
class SessionMetaSink : public detect::IMetaSink {
public:
    explicit SessionMetaSink(Session& session) : session_(session) {}
    void on_meta(uint8_t camera, const std::string& json) override {
        session_.enqueue(Channel::Meta, camera,
                         reinterpret_cast<const uint8_t*>(json.data()),
                         json.size(), /*droppable=*/true);
    }

private:
    Session& session_;
};

// Raw frames -> detector -> boxes via an IMetaSink. Driven by pump_raw() on a
// dedicated thread, because inference (~25 ms) must never sit in front of the
// video pull (~16 ms/frame).
class DetectSink : public media::IFrameTransport {
public:
    DetectSink(detect::IMetaSink& meta, uint8_t camera,
               detect::IFaceDetector& detector)
        : meta_(meta), camera_(camera), detector_(detector) {}

    void on_frame(uint8_t, const media::Frame&) override {}  // video is not ours

    void on_raw(uint8_t camera, const uint8_t* bgrx, int width, int height,
                int stride) override {
        const auto boxes = detector_.detect(bgrx, width, height, stride);
        // Serve a pending ISP snapshot from this same frame: Argus allows one
        // consumer per camera, so nothing outside this process can open the
        // sensor while the service runs.
        if (const std::string path = detect::take_snapshot_request(camera);
            !path.empty()) {
            const std::string failure =
                detect::write_bgr_ppm(path, bgrx, width, height, stride,
                                      /*bytes_per_pixel=*/4, boxes);
            if (failure.empty())
                XLOGF(INFO, "secure-usb: cam%u snapshot written to %s "
                            "(%zu face(s) in this frame)",
                      static_cast<unsigned>(camera), path.c_str(), boxes.size());
            else
                XLOGF(WARN, "secure-usb: cam%u snapshot failed: %s",
                      static_cast<unsigned>(camera), failure.c_str());
        }
        meta_.on_meta(camera,
                      detect::to_meta_json(camera, width, height, boxes));
    }

private:
    detect::IMetaSink& meta_;
    uint8_t camera_;
    detect::IFaceDetector& detector_;
};

// Pulls one camera's encoded H.265 elementary stream and pushes it over the
// session. When `detect_model` is set the pipeline also carries a raw branch,
// and a DetectSink pumps it on its own thread.
void video_loop(Session& session, uint8_t camera,
                const std::function<std::string()>& describe,
                const std::atomic<bool>& enabled, const std::string& detect_model,
                int detect_w, int detect_h, double detect_score, int detect_fps,
                std::atomic<bool>& relaunch,
                const std::function<void(uint8_t, void*)>& publish,
                const std::function<void(uint8_t, bool, size_t)>& report) {
    // Load the detector once for the camera's lifetime (model load is heavy).
    std::unique_ptr<detect::IFaceDetector> detector;
    if (!detect_model.empty()) {
        auto d = detect::create_face_detector(detect_model, detect_w, detect_h,
                                              static_cast<float>(detect_score));
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
        // Re-read each time: refresh_launch can have replaced it (zoom, and
        // any setting the live element cannot take).
        const std::string description = describe();
        relaunch = false;
        media::CameraPipeline pipeline(camera, description);
        bool produced = false;
        if (auto started = pipeline.start(); !started.hasValue()) {
            XLOGF(WARN, "secure-usb: cam%u pipeline did not start: %s",
                  static_cast<unsigned>(camera), started.error().c_str());
        } else {
            // Declared before the detection thread on purpose. pump_raw() fans
            // out to EVERY registered transport, so a sink must outlive that
            // thread; declaring it later would let it be destroyed while the
            // detector is still pumping, and the thread would call into freed
            // memory. Reverse destruction order gives the guarantee for free.
            VideoSink video_sink(session, camera, report);
            pipeline.add_transport(&video_sink);

            // Detection runs alongside the video pull, on the raw branch, for
            // this pipeline's lifetime. Joined before the pipeline tears the
            // branch down.
            std::atomic<bool> detect_stop{false};
            std::thread detect_thread;
            // Say which of the two preconditions failed. Both used to be
            // silent, so "detection is on" in the config could sit next to a
            // pipeline that had no detect branch at all -- video streaming
            // perfectly the whole time, and nothing in the log to say why no
            // boxes (or snapshots) ever appeared.
            if (detector == nullptr)
                XLOGF(WARN, "secure-usb: cam%u no detector; detection off",
                      static_cast<unsigned>(camera));
            else if (!pipeline.has_detect_branch())
                XLOGF(WARN,
                      "secure-usb: cam%u pipeline has no 'detect' appsink; "
                      "detection off (launch: %s)",
                      static_cast<unsigned>(camera), description.c_str());
            // Detection pumps the raw branch on its own thread: inference must
            // never sit in front of the video pull.
            std::unique_ptr<SessionMetaSink> meta_sink;
            std::unique_ptr<DetectSink> detect_sink;
            if (detector && pipeline.has_detect_branch()) {
                meta_sink = std::make_unique<SessionMetaSink>(session);
                detect_sink =
                    std::make_unique<DetectSink>(*meta_sink, camera, *detector);
                pipeline.add_transport(detect_sink.get());
                const int interval = detect_fps > 0 ? 1000 / detect_fps : 0;
                detect_thread = std::thread([&pipeline, &detect_stop, &session,
                                             interval] {
                    while (!session.dead && !detect_stop) {
                        if (interval > 0)
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(interval));
                        if (session.dead || detect_stop) break;
                        pipeline.pump_raw(200);
                    }
                });
            }
            SCOPE_EXIT {
                detect_stop = true;
                if (detect_thread.joinable()) detect_thread.join();
            };
            // Expose the source while this pipeline lives, so the control
            // server can set exposure/gain on it.
            publish(camera, &pipeline);
            SCOPE_EXIT {
                publish(camera, nullptr);
                report(camera, false, 0);  // pipeline down: not streaming
            };
            while (!session.dead) {
                // Bounded pull so session teardown is noticed promptly even
                // when the camera has gone quiet.
                if (!enabled) {
                    XLOGF(INFO, "secure-usb: cam%u stream stopped by set-stream",
                          static_cast<unsigned>(camera));
                    break;  // ~Pipeline releases the sensor
                }
                if (relaunch) {
                    XLOGF(INFO, "secure-usb: cam%u rebuilding pipeline "
                                "(launch changed)", static_cast<unsigned>(camera));
                    break;  // outer loop re-reads the description
                }
                // CameraPipeline pulls and fans out to VideoSink.
                if (!pipeline.pump(200)) {
                    pipeline.drain_bus();
                    if (pipeline.is_eos()) break;
                    continue;
                }
                produced = video_sink.produced();
                // VideoSink marks the session dead if sealing failed: the
                // record nonce is an implicit counter, so a dropped record
                // desyncs the peer and the session cannot continue.
                if (session.dead) break;
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

void SecureUsbServer::set_control_dispatcher(ControlDispatcher dispatcher) {
    control_dispatcher_ = std::move(dispatcher);
}

void SecureUsbServer::set_update_channel_factory(UpdateChannelFactory factory) {
    update_channel_factory_ = std::move(factory);
}

void SecureUsbServer::report_frame(uint8_t camera, bool streaming, size_t bytes) {
    if (camera >= kCameras) return;
    streaming_[camera] = streaming;
    if (!streaming) {
        fps_milli_[camera] = 0;
        return;
    }
    frames_[camera].fetch_add(1, std::memory_order_relaxed);
    bytes_[camera].fetch_add(bytes, std::memory_order_relaxed);
}

SecureUsbServer::CameraStats SecureUsbServer::stats(uint8_t camera) const {
    CameraStats out;
    if (camera >= kCameras) return out;
    out.streaming = streaming_[camera].load(std::memory_order_relaxed);
    out.frames = frames_[camera].load(std::memory_order_relaxed);
    out.bytes = bytes_[camera].load(std::memory_order_relaxed);
    out.fps = static_cast<double>(fps_milli_[camera].load(
                  std::memory_order_relaxed)) / 1000.0;
    return out;
}

void SecureUsbServer::publish_source(uint8_t camera, void* pipeline) {
    if (camera >= kCameras) return;
    std::lock_guard<std::mutex> lock(live_mutex_);
    // A media::CameraPipeline*, not a GstElement: no refcounting, because the
    // video loop clears this (publishes nullptr) before the pipeline is
    // destroyed, under the same mutex the control server takes.
    live_source_[camera] = pipeline;
}

bool SecureUsbServer::set_source_property(uint8_t camera, const char* property,
                                          const char* value) {
    if (camera >= kCameras) return false;
    std::lock_guard<std::mutex> lock(live_mutex_);
    auto* pipeline = static_cast<media::CameraPipeline*>(live_source_[camera]);
    if (pipeline == nullptr)
        return false;  // no pipeline up; the launch string carries it instead
    return pipeline->set_source_property(property, value);
}

void SecureUsbServer::refresh_launch(uint8_t camera, std::string launch) {
    if (camera >= kCameras || launch.empty()) return;
    if (camera < video_launch_.size())
        video_launch_[camera] = std::move(launch);
    // Ask the video loop to rebuild. A live session picks it up within one
    // pull timeout; with no session the next one reads the new string.
    relaunch_[camera] = true;
}

void SecureUsbServer::set_face_detection(std::string model, int input_width,
                                         int input_height,
                                         double score_threshold,
                                         int detect_fps) {
    detect_model_ = std::move(model);
    detect_width_ = input_width;
    detect_height_ = input_height;
    detect_score_ = score_threshold;
    detect_fps_ = detect_fps;
}

std::string SecureUsbServer::video_description(uint8_t camera) const {
    if (camera < video_launch_.size() && !video_launch_[camera].empty())
        return video_launch_[camera];
    // No fallback. This used to re-serve the camera's own RTSP mount over
    // loopback when RTSP owned the sensor, which only arose under
    // transports=both -- a mode that no longer exists. That path also had no
    // detect branch, so it disabled face detection without saying so. An empty
    // description now fails the pipeline loudly instead.
    XLOGF(WARN, "secure-usb: cam%u has no launch description; not streaming",
          static_cast<unsigned>(camera));
    return {};
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
                session = std::make_unique<Session>(
                    in, response.value().second.device_key,
                    response.value().second.device_iv, control_dispatcher_,
                    update_channel_factory_);
                session->last_inbound_ms = Session::now_ms();
                XLOGF(INFO, "secure-usb: authenticated session established");
                Session* raw = session.get();
                for (uint8_t camera = 0; camera < kCameras; ++camera) {
                    workers.emplace_back([this, raw, camera] {
                        video_loop(
                            *raw, camera,
                            [this, camera] { return video_description(camera); },
                            stream_enabled_[camera], detect_model_,
                            detect_width_, detect_height_, detect_score_, detect_fps_,
                            relaunch_[camera],
                            [this](uint8_t cam, void* element) {
                                publish_source(cam, element);
                            },
                            [this](uint8_t cam, bool live, size_t bytes) {
                                report_frame(cam, live, bytes);
                            });
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

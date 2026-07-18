#include "camera/secure/SecureUsbServer.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "camera/base/ScopeGuard.h"
#include "camera/base/logging/xlog.h"
#include "secure/SecureHandshake.h"
#include "secure/SecureWire.h"
#include "camera/secure/SecureRecord.h"

namespace camera::secure {
namespace {
constexpr char kFfs[] = "/dev/ffs-secure";
constexpr char kGadget[] = "/sys/kernel/config/usb_gadget/vc-camera";

// Endpoint writes are blocking now, but ep0 and configfs writes are not
// necessarily, and a gadget draining an earlier request can still report
// EAGAIN.  Waiting for writability rather than failing matters because a
// failed ServerHello write is never retried: the host just waits out its
// whole handshake timeout.
bool write_all(int fd, const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (length != 0) {
        const ssize_t n = write(fd, bytes, length);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd writable{fd, POLLOUT, 0};
                if (poll(&writable, 1, 1000) <= 0)
                    return false;  // genuinely stuck, not merely busy
                continue;
            }
            return false;
        }
        if (n == 0)
            return false;
        bytes += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> descriptors() {
    const std::array<uint8_t, 9> interface = {9, 4, 0, 0, 2, 0xff, 0x53, 0x55, 1};
    auto endpoint = [](uint8_t address, uint16_t packet) {
        return std::array<uint8_t, 7>{7, 5, address, 2,
            static_cast<uint8_t>(packet), static_cast<uint8_t>(packet >> 8), 0};
    };
    std::vector<uint8_t> body;
    for (uint16_t packet : {uint16_t(64), uint16_t(512)}) {
        const auto in = endpoint(0x81, packet), out = endpoint(0x02, packet);
        body.insert(body.end(), interface.begin(), interface.end());
        body.insert(body.end(), in.begin(), in.end());
        body.insert(body.end(), out.begin(), out.end());
    }
    auto append32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i != 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    std::vector<uint8_t> result;
    append32(result, FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    append32(result, 20 + body.size());
    append32(result, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    append32(result, 3); append32(result, 3);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

int connect_local(uint16_t port) {
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

bool write_file(const std::string& path, const std::string& value) {
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = write_all(fd, value.data(), value.size());
    close(fd);
    return ok;
}

std::string first_udc() {
    DIR* raw = opendir("/sys/class/udc");
    if (raw == nullptr) return {};
    std::unique_ptr<DIR, decltype(&closedir)> directory(raw, closedir);
    while (dirent* entry = readdir(raw)) {
        if (entry->d_name[0] != '.') return entry->d_name;
    }
    return {};
}

bool prepare_functionfs(std::string* error) {
    // usb-gadget.service creates and binds the recovery NCM/ACM gadget first.
    // FunctionFS cannot be linked into a bound gadget, so publish it here in
    // the owning process and bounce the UDC once, before accepting sessions.
    const std::string udc_path = std::string(kGadget) + "/UDC";
    if (!write_file(udc_path, "\n")) {
        // ENODEV means the gadget is already unbound, which is exactly the
        // state left behind when a previous instance of this process died
        // owning the function. Treating it as fatal made secure USB
        // unrecoverable until a reboot: every restart failed here.
        if (errno != ENODEV) {
            *error = std::string("unbind USB gadget: ") + strerror(errno);
            return false;
        }
    }
    // Writing UDC completes the unbind before the write() syscall returns, so
    // configfs is ready for the new function immediately after this point.
    //
    // A previous endpoint owner may have left a FunctionFS superblock mounted
    // after its service exited. That mount holds the ffs.* instance and makes
    // configfs report EBUSY when creating it.
    if (umount(kFfs) != 0 && errno != EINVAL && errno != ENOENT && errno != EPERM) {
        *error = std::string("unmount stale FunctionFS: ") + strerror(errno);
        return false;
    }
    const std::string function = std::string(kGadget) + "/functions/ffs.secure";
    if (mkdir(kFfs, 0755) != 0 && errno != EEXIST) {
        *error = std::string("mkdir ") + kFfs + ": " + strerror(errno);
        return false;
    }
    if (mkdir(function.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = std::string("create FunctionFS function ") + function + ": " + strerror(errno)
            + (errno == ENOENT
                   ? " (kernel registers no \"ffs\" configfs function type -- "
                     "CONFIG_USB_CONFIGFS_F_FS is not enabled in this kernel)"
                   : "");
        return false;
    }
    if (mount("secure", kFfs, "functionfs", 0, nullptr) != 0) {
        *error = std::string("mount FunctionFS: ") + strerror(errno);
        return false;
    }
    const std::string link = std::string(kGadget) + "/configs/c.1/ffs.secure";
    if (symlink(function.c_str(), link.c_str()) != 0 && errno != EEXIST) {
        *error = std::string("link FunctionFS function: ") + strerror(errno);
        return false;
    }
    return true;
}

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

// Shared session state.  The endpoint has one writer mutex because several
// threads push into it: two video pipelines plus the control/update tunnel.
struct Session {
    Session(int endpoint, const SecureRecord::Key& key, const SecureRecord::Iv& iv)
        : endpoint(endpoint), encrypt(key, iv) {}

    int endpoint;
    SecureRecord encrypt;  // guarded by write_mutex
    std::mutex write_mutex;
    std::atomic<bool> dead{false};

    // Outbound queue. ~4 MiB is roughly a second of both streams at the
    // configured bitrate: enough to ride out a brief host stall, small enough
    // that dropping beats accumulating latency nobody wants to watch.
    static constexpr size_t kQueueLimit = 4 * 1024 * 1024;
    std::mutex queue_mutex;
    std::condition_variable queue_signal;
    std::deque<std::vector<uint8_t>> queue;
    size_t queued_bytes = 0;
    size_t dropped_bytes = 0;

    std::mutex local_mutex;
    int control = -1;
    int update = -1;

    // Last time anything arrived from the host. The UI polls status every
    // few seconds over the tunnel, so a live viewer keeps this fresh; a host
    // that exits just goes silent -- a gadget gets no disconnect signal --
    // and without this the cameras kept capturing forever afterwards.
    std::atomic<int64_t> last_inbound_ms{0};

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // True while a local updater connection exists: an upload can stall all
    // tunnel traffic for as long as a flash commit takes, and the host-
    // silence watchdog must not read that as a departed viewer.
    bool has_active_update() {
        std::lock_guard<std::mutex> lock(local_mutex);
        return update >= 0;
    }

    // For waking a writer blocked in a kernel endpoint write at teardown.
    pthread_t writer_thread{};
    std::atomic<bool> writer_started{false};
    std::atomic<bool> writer_exited{false};

    // Queues a record for the writer thread.
    //
    // Video is `droppable`: when the host stops draining the endpoint, a
    // synchronous write here blocks the camera reader, which fills the
    // pipeline's stdout pipe, which stalls its RTSP session, which starves
    // the encoder until the frame watchdog declares the camera dead. Dropping
    // frames is the correct response to a slow link; blocking the camera is
    // not. Control and update are never dropped -- they are not replaceable.
    bool enqueue(Channel channel, uint8_t stream, const uint8_t* data, size_t size,
                 bool droppable) {
        // Held across encrypt+queue so records reach the wire in the order
        // they were sealed: the record nonce is an implicit counter, and any
        // reordering (or dropping *after* sealing) desynchronises the peer.
        std::lock_guard<std::mutex> ordering(write_mutex);
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (droppable && queued_bytes >= kQueueLimit) {
                dropped_bytes += size;
                return true;
            }
        }
        auto wire = make_wire_record(channel, stream,
                                     std::vector<uint8_t>(data, data + size), encrypt);
        if (!wire.hasValue()) return false;
        std::lock_guard<std::mutex> lock(queue_mutex);
        queued_bytes += wire.value().size();
        queue.push_back(std::move(wire.value()));
        queue_signal.notify_one();
        return true;
    }

    // Sole owner of endpoint writes, so a stalled host blocks only this
    // thread.
    void writer_loop() {
        writer_thread = pthread_self();
        writer_started = true;
        // The host polls control every few seconds; well beyond that means
        // the viewer is gone. Ending the session stops the video pipelines,
        // which releases the cameras.
        constexpr int64_t kHostSilenceMs = 15000;
        size_t written = 0, reported = 0;
        while (!dead) {
            if (now_ms() - last_inbound_ms >= kHostSilenceMs && !has_active_update()) {
                XLOGF(INFO, "secure-usb: host silent for %llds; ending session "
                            "(cameras stop)",
                      static_cast<long long>(kHostSilenceMs / 1000));
                dead = true;
                break;
            }
            std::vector<uint8_t> record;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_signal.wait_for(lock, std::chrono::milliseconds(100),
                                      [this] { return !queue.empty() || dead; });
                if (queue.empty()) continue;
                record = std::move(queue.front());
                queue.pop_front();
                queued_bytes -= record.size();
            }
            if (!endpoint_write_all(endpoint, record.data(), record.size(), dead)) {
                dead = true;
                break;
            }
            if (written == 0)
                XLOGF(INFO, "secure-usb: first record (%zu bytes) on the endpoint",
                      record.size());
            written += record.size();
            // "sent" in video_loop counts enqueues; this counts bytes the
            // host actually drained. Diverging numbers mean a stalled host.
            if (written - reported >= 8 * 1024 * 1024) {
                reported = written;
                XLOGF(INFO, "secure-usb: %zu KiB drained by host", written / 1024);
            }
        }
        writer_exited = true;
    }

    // Host -> local server.  A dead local server drops its channel; it must
    // not end the session, which also carries video.
    bool deliver_local(const WireMessage& message) {
        if (message.channel == Channel::Video) return true;  // device-to-host only
        std::lock_guard<std::mutex> lock(local_mutex);
        int& fd = message.channel == Channel::Control ? control : update;
        const uint16_t port = message.channel == Channel::Control ? 8555 : 8557;
        if (message.channel == Channel::Update)
            XLOGF(INFO, "secure-usb: update: %zu bytes to local updater",
                  message.payload.size());
        // Two attempts with a fresh connection in between: a held-open fd may
        // be a finished session's socket (EPIPE on write), and dropping this
        // payload on that would eat the first chunk of the next upload.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (fd < 0)
                fd = connect_local(port);
            if (fd < 0)
                break;
            if (write_all(fd, message.payload.data(), message.payload.size()))
                return true;
            close(fd);
            fd = -1;
        }
        XLOGF(WARN, "secure-usb: cannot deliver a %s request to 127.0.0.1:%u; dropped",
              message.channel == Channel::Control ? "control" : "update", port);
        return true;
    }

    void snapshot_local(int* control_fd, int* update_fd) {
        std::lock_guard<std::mutex> lock(local_mutex);
        *control_fd = control;
        *update_fd = update;
    }

    void close_local(Channel channel) {
        std::lock_guard<std::mutex> lock(local_mutex);
        int& fd = channel == Channel::Control ? control : update;
        if (fd >= 0) { close(fd); fd = -1; }
    }

    void shutdown() {
        dead = true;
        queue_signal.notify_all();
        // The writer may be blocked in a kernel write on the endpoint; only
        // a signal wakes it (see endpoint I/O comment above).
        if (writer_started)
            wake_until_exited(writer_thread, writer_exited);
        std::lock_guard<std::mutex> lock(local_mutex);
        // Shut down before closing so a thread blocked in read() wakes.
        for (int* fd : {&control, &update}) {
            if (*fd >= 0) { ::shutdown(*fd, SHUT_RDWR); close(*fd); *fd = -1; }
        }
    }
};

// Pulls one camera's RTSP mount, strips it to an H.265 elementary stream and
// pushes that over the session.  RTSP therefore terminates on the device
// instead of being tunnelled, so its round trips never cross USB.
void video_loop(Session& session, uint8_t camera, const std::string& description,
                const std::atomic<bool>& enabled) {
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
void local_to_usb_loop(Session& session) {
    std::vector<uint8_t> chunk(32 * 1024);
    while (!session.dead) {
        int control_fd = -1, update_fd = -1;
        session.snapshot_local(&control_fd, &update_fd);
        pollfd fds[] = {{control_fd, POLLIN, 0}, {update_fd, POLLIN, 0}};
        if (poll(fds, 2, 100) <= 0) continue;
        for (int i = 0; i != 2; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const Channel channel = i == 0 ? Channel::Control : Channel::Update;
            const ssize_t n = read(fds[i].fd, chunk.data(), chunk.size());
            if (n <= 0) { session.close_local(channel); continue; }
            if (!session.enqueue(channel, 0, chunk.data(), static_cast<size_t>(n), false)) {
                session.dead = true;
                return;
            }
        }
    }
}

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
    // Every exit path must publish this, including the setup failures that
    // return early -- stop() waits on it before joining.
    SCOPE_EXIT { worker_exited_ = true; };
    // ep0 remains open for this process lifetime; FunctionFS removes the
    // function when it closes. usb-gadget.sh has already mounted the fs.
    const std::string base_udc = first_udc();
    auto restore_gadget = [&] {
        if (!base_udc.empty())
            write_file(std::string(kGadget) + "/UDC", base_udc);
    };
    std::string setup_error;
    if (!prepare_functionfs(&setup_error)) {
        XLOGF(ERR, "secure-usb: %s", setup_error.c_str());
        restore_gadget();
        return;
    }
    int ep0 = open("/dev/ffs-secure/ep0", O_RDWR | O_NONBLOCK);
    if (ep0 < 0) { XLOGF(ERR, "secure-usb: open ep0: %s", strerror(errno)); restore_gadget(); return; }
    const auto desc = descriptors();
    const uint8_t strings[] = {2,0,0,0,25,0,0,0,1,0,0,0,1,0,0,0,9,4,'s','e','c','u','r','e',0};
    if (!write_all(ep0, desc.data(), desc.size()) || !write_all(ep0, strings, sizeof(strings))) {
        XLOGF(ERR, "secure-usb: publishing descriptors failed"); close(ep0); restore_gadget(); return;
    }
    const std::string udc = base_udc.empty() ? first_udc() : base_udc;
    if (udc.empty() || !write_file(std::string(kGadget) + "/UDC", udc)) {
        XLOGF(ERR, "secure-usb: rebinding UDC failed");
        close(ep0); restore_gadget();
        return;
    }
    while (!stopping_) {
        serve_session(ep0);
        if (!stopping_)
            usleep(100000);  // let the host notice the drop before retrying
    }
    close(ep0);
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
        if (session->dropped_bytes != 0) {
            XLOGF(INFO, "secure-usb: session ended (dropped %zu KiB of video)",
                  session->dropped_bytes / 1024);
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
                                   stream_enabled_[camera]);
                    });
                }
                workers.emplace_back([raw] { local_to_usb_loop(*raw); });
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

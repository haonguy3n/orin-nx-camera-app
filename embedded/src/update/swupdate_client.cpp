#include "update/swupdate_client.h"

#include <glib.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

/// Path to the swupdate IPC socket. swupdate's socket path depends on
/// its build configuration and whether it uses systemd socket activation.
/// We try the common paths in order. See:
///   https://sbabic.github.io/swupdate/swupdate-ipc-interface.html
const char* kSwupdateSocketPaths[] = {
    "/run/swupdate/control.sock",  // systemd socket activation (meta-swupdate)
    "/tmp/sockinstctrl",           // legacy default
};

/// Number of socket paths to try.
constexpr int kNumSocketPaths =
    sizeof(kSwupdateSocketPaths) / sizeof(kSwupdateSocketPaths[0]);

/// Progress socket paths (separate from control socket).
/// The progress socket runs in its own thread and pushes progress updates
/// to connected clients — it's never blocked by the install process.
const char* kProgressSocketPaths[] = {
    "/run/swupdate/progress.sock",  // systemd socket activation
    "/tmp/swupdateprog",            // legacy default
};
constexpr int kNumProgressSocketPaths =
    sizeof(kProgressSocketPaths) / sizeof(kProgressSocketPaths[0]);

/// swupdate API version (from network_ipc.h: SWUPDATE_API_VERSION).
constexpr unsigned int kSwupdateApiVersion = 0x1;

/// swupdate IPC magic number (from network_ipc.h).
constexpr int kIpcMagic = 0x14052001;

/// swupdate IPC message types (from network_ipc.h).
enum IpcType {
    kReqInstall = 0,
    kAck = 1,
    kNack = 2,
    kGetStatus = 3,
};

/// swupdate recovery status (from swupdate_status.h).
enum RecoveryStatus {
    kStatusIdle = 0,
    kStatusStart = 1,
    kStatusRun = 2,
    kStatusSuccess = 3,
    kStatusFailure = 4,
    kStatusDownload = 5,
    kStatusDone = 6,
    kStatusSubprocess = 7,
    kStatusProgress = 8,
};

/// Progress socket ACK (from progress_ipc.h).
/// Sent by swupdate when a client connects to the progress socket.
struct progress_connect_ack {
    uint32_t apiversion;
    char magic[4];  // "ACK"
};

/// Progress message (from progress_ipc.h).
/// Pushed by swupdate to all connected progress clients whenever
/// installation progress changes. Packed struct — no padding.
struct progress_msg {
    uint32_t apiversion;
    uint32_t status;           // RECOVERY_STATUS enum
    uint32_t dwl_percent;      // download percent
    unsigned long long dwl_bytes;
    uint32_t nsteps;           // total number of steps
    uint32_t cur_step;         // current step (1-based after first inc)
    uint32_t cur_percent;      // percent within current step (0-100)
    char cur_image[256];       // name of image being installed
    char hnd_name[64];         // handler name
    uint32_t source;           // interface that triggered the update
    uint32_t infolen;          // length of info field
    char info[2048];           // additional info
} __attribute__((packed));

/// swupdate_request (from network_ipc.h, part of the instmsg union member).
/// We define this locally to get the correct union size without depending
/// on swupdate headers at compile time.
struct swupdate_request {
    unsigned int apiversion;
    int source;          // sourcetype enum
    int dry_run;         // run_type enum
    size_t len;          // 8 bytes on 64-bit — drives the union size
    char info[512];
    char software_set[256];
    char running_mode[256];
    bool disable_store_swu;
};

/// Minimum ipc_message struct compatible with swupdate's network_ipc.h.
/// The msgdata union's largest member is instmsg (swupdate_request + uint +
/// char[2048] ≈ 3112 bytes on 64-bit). We define the full union to get the
/// correct size; only the status member is used for reading responses.
/// The layout is part of swupdate's stable ABI for a given architecture.
struct ipc_message {
    int magic;
    int type;
    union msgdata {
        struct {
            int current;       // RECOVERY_STATUS (on response)
            int last_result;
            int error;
            char desc[2048];   // status text / error message
        } status;
        struct {
            swupdate_request req;
            unsigned int len;
            char buf[2048];
        } instmsg;
        char raw[3120 - sizeof(int) * 2];  // ensure correct total size
    } data;
};

/// Connects to the swupdate IPC Unix domain socket. Tries known socket
/// paths in order. Returns fd on success, -1 on failure (with g_warning).
/// |quiet| suppresses the per-connect log (used for status polling).
int connect_swupdate_socket(bool quiet = false) {
    for (int i = 0; i < kNumSocketPaths; ++i) {
        const char* path = kSwupdateSocketPaths[i];
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            g_warning("swupdate: socket(): %s", strerror(errno));
            continue;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)) < 0) {
            if (!quiet)
                g_message("swupdate: connect(%s): %s, trying next", path,
                          strerror(errno));
            close(fd);
            continue;  // try next path
        }
        if (!quiet)
            g_message("swupdate: connected to %s (fd=%d)", path, fd);
        return fd;
    }
    g_warning("swupdate: cannot connect to any IPC socket (tried %d paths)",
              kNumSocketPaths);
    return -1;
}

/// Writes exactly |len| bytes to fd. Returns true on success, false on
/// error (with g_warning). Handles partial writes and EINTR.
bool write_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_warning("swupdate: write: %s", strerror(errno));
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// Reads exactly |len| bytes from fd. Returns true on success, false on
/// error or EOF (with g_warning). Handles partial reads and EINTR.
bool read_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_warning("swupdate: read: %s", strerror(errno));
            return false;
        }
        if (n == 0) {
            g_warning("swupdate: read: EOF after %zu/%zu bytes", got, len);
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

/// Sends REQ_INSTALL and waits for ACK/NACK. Returns fd on ACK, -1 on
/// NACK or error. After ACK, the caller streams the image on this fd.
int request_install() {
    int fd = connect_swupdate_socket();
    if (fd < 0) {
        g_warning("swupdate: request_install: no socket connection");
        return -1;
    }

    g_message("swupdate: sending REQ_INSTALL (magic=0x%08x, sizeof(ipc_message)=%zu)",
              kIpcMagic, sizeof(ipc_message));

    ipc_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = kIpcMagic;
    msg.type = kReqInstall;
    // swupdate checks apiversion == SWUPDATE_API_VERSION (1) and NACKs if
    // it doesn't match. The req is in msg.data.instmsg.req.
    msg.data.instmsg.req.apiversion = kSwupdateApiVersion;

    if (!write_all(fd, &msg, sizeof(msg))) {
        g_warning("swupdate: request_install: write failed");
        close(fd);
        return -1;
    }

    g_message("swupdate: waiting for ACK...");
    if (!read_all(fd, &msg, sizeof(msg))) {
        g_warning("swupdate: request_install: read ACK failed");
        close(fd);
        return -1;
    }

    g_message("swupdate: response type=%d, magic=0x%08x", msg.type, msg.magic);

    if (msg.type == kNack) {
        // swupdate writes the error reason into msg.data.msg (char[128]).
        // For "busy" it says "Installation in progress"; for apiversion
        // mismatch it's empty (memset to 0).
        char nack_msg[129];
        memcpy(nack_msg, msg.data.raw, 128);
        nack_msg[128] = '\0';
        g_warning("swupdate: install request NACK: %s",
                  nack_msg[0] ? nack_msg : "rejected (check apiversion)");
        close(fd);
        return -1;
    }
    if (msg.type != kAck) {
        g_warning("swupdate: unexpected response type %d (expected ACK=%d)",
                  msg.type, kAck);
        close(fd);
        return -1;
    }

    g_message("swupdate: REQ_INSTALL ACKed, ready to stream (fd=%d)", fd);
    return fd;
}

/// Polls swupdate for status. Returns true if the status was read
/// successfully (fills |out|), false on communication error.
bool poll_status(UpdateStatus& out) {
    int fd = connect_swupdate_socket(true);  // quiet — called every second
    if (fd < 0)
        return false;

    ipc_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = kIpcMagic;
    msg.type = kGetStatus;

    if (!write_all(fd, &msg, sizeof(msg))) {
        close(fd);
        return false;
    }
    if (!read_all(fd, &msg, sizeof(msg))) {
        close(fd);
        return false;
    }
    close(fd);

    out.percent = 0;
    out.step = 0;
    out.total_steps = 0;
    out.current_name.clear();
    out.error.clear();

    // The status response has:
    //   data.status.current     — RECOVERY_STATUS enum (overwritten by
    //                             queued notification if one exists)
    //   data.status.last_result — result of the last install (persists
    //                             after swupdate goes back to IDLE)
    //   data.status.error       — error code
    //   data.status.desc        — notification message text (may contain
    //                          progress info like "10%" or "Step 2 of 7")
    const int raw_status = msg.data.status.current;
    const int raw_last_result = msg.data.status.last_result;
    const char* desc = msg.data.status.desc;

    auto to_state = [](int s) -> UpdateState {
        switch (s) {
        case kStatusIdle:       return UpdateState::Idle;
        case kStatusStart:      return UpdateState::Installing;
        case kStatusRun:        return UpdateState::Installing;
        case kStatusSuccess:    return UpdateState::Success;
        case kStatusFailure:    return UpdateState::Failure;
        case kStatusDownload:   return UpdateState::Installing;
        case kStatusDone:       return UpdateState::Done;
        case kStatusSubprocess: return UpdateState::Installing;
        case kStatusProgress:   return UpdateState::Installing;
        default:               return UpdateState::Installing;
        }
    };

    out.state = to_state(raw_status);
    out.last_result = to_state(raw_last_result);

    // Parse progress percentage from the desc text.
    // swupdate sends PROGRESS notifications with text like "10%" or
    // "Step 2 of 7: rootfs.ext4". The GET_STATUS response dequeues one
    // notification at a time and puts its text in desc.
    if (desc[0] != '\0') {
        out.current_name = desc;

        // Try to parse "N%" from the desc text
        const char* pct = strchr(desc, '%');
        if (pct) {
            // Walk backwards to find the start of the number
            const char* start = pct;
            while (start > desc && isdigit(static_cast<unsigned char>(*(start - 1))))
                --start;
            if (start < pct) {
                out.percent = atoi(start);
                if (out.percent < 0) out.percent = 0;
                if (out.percent > 100) out.percent = 100;
            }
        }

        // Try to parse "Step N of M" pattern
        int step = 0, total = 0;
        if (sscanf(desc, "Step %d of %d", &step, &total) >= 2) {
            out.step = step;
            out.total_steps = total;
        }
    }

    if (msg.data.status.error != 0)
        out.error = desc;

    g_message("swupdate: status: raw=%d state=%d last_result=%d percent=%d desc=%.80s",
              raw_status, static_cast<int>(out.state),
              static_cast<int>(out.last_result), out.percent,
              desc[0] ? desc : "(empty)");
    return true;
}

/// Fallback: runs `swupdate -i <path>` as a subprocess and waits for
/// completion. Used when the IPC socket is not available (e.g. swupdate
/// not running in daemon mode). Returns the exit status.
int run_swupdate_subprocess(const std::string& path) {
    g_message("swupdate: IPC socket unavailable, falling back to subprocess");
    pid_t pid = fork();
    if (pid < 0) {
        g_critical("swupdate: fork(): %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // Child: exec swupdate -i <path>
        execlp("swupdate", "swupdate", "-i", path.c_str(), nullptr);
        // exec failed
        _exit(127);
    }
    // Parent: wait for completion
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        g_critical("swupdate: waitpid(): %s", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

}  // namespace

SwupdateClient::SwupdateClient() {}

SwupdateClient::~SwupdateClient() {
    if (install_thread_.joinable())
        install_thread_.detach();
}

bool SwupdateClient::install_from_file(const std::string& path) {
    if (is_busy()) {
        g_warning("swupdate: install requested but an update is in progress");
        return false;
    }

    // Verify the file exists and is readable
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        g_warning("swupdate: %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    set_state(UpdateState::Installing);
    percent_.store(0);
    step_.store(0);
    total_steps_.store(0);
    current_name_.clear();
    error_.clear();

    install_thread_ = std::thread(&SwupdateClient::run_install, this, path);
    install_thread_.detach();
    return true;
}

void SwupdateClient::run_install(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        g_message("swupdate: installing %s (%lld bytes)", path.c_str(),
                  static_cast<long long>(st.st_size));
    else
        g_message("swupdate: installing %s", path.c_str());

    // Try the IPC socket approach first (preferred: streams directly,
    // gives us status feedback).
    int fd = request_install();
    if (fd >= 0) {
        // Stream the file to swupdate
        int file_fd = open(path.c_str(), O_RDONLY);
        if (file_fd < 0) {
            g_critical("swupdate: open(%s): %s", path.c_str(),
                       strerror(errno));
            close(fd);
            set_error(std::string("open: ") + strerror(errno));
            set_state(UpdateState::Failure);
            return;
        }

        char buf[65536];
        ssize_t n;
        ssize_t total = 0;
        while ((n = read(file_fd, buf, sizeof(buf))) > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(fd, buf + written, n - written);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    g_critical("swupdate: write to IPC: %s",
                              strerror(errno));
                    break;
                }
                written += w;
            }
            if (written < n) {
                close(file_fd);
                close(fd);
                set_error("write to swupdate IPC failed");
                set_state(UpdateState::Failure);
                return;
            }
            total += n;
        }
        close(file_fd);
        close(fd);
        g_message("swupdate: streamed %lld bytes to swupdate",
                  static_cast<long long>(total));

        poll_completion();
        return;
    }

    // Fallback: subprocess (swupdate -i <path>)
    int rc = run_swupdate_subprocess(path);
    if (rc == 0) {
        g_message("swupdate: subprocess installation successful");
        set_state(UpdateState::Success);
    } else {
        g_critical("swupdate: subprocess failed (exit %d)", rc);
        set_error("swupdate subprocess exited with code " +
                  std::to_string(rc));
        set_state(UpdateState::Failure);
    }
}

void SwupdateClient::poll_completion() {
    // Connect to the progress socket. This is a separate thread in swupdate
    // that pushes progress_msg updates to connected clients. Unlike GET_STATUS
    // on the control socket (which blocks during CPIO streaming), the progress
    // socket is always responsive.
    int fd = -1;
    for (int i = 0; i < kNumProgressSocketPaths; ++i) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            continue;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, kProgressSocketPaths[i],
                sizeof(addr.sun_path) - 1);

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        g_message("swupdate: connected to progress socket %s (fd=%d)",
                  kProgressSocketPaths[i], fd);
        break;
    }
    if (fd < 0) {
        g_critical("swupdate: cannot connect to progress socket, "
                   "falling back to GET_STATUS polling");
        poll_completion_via_control_socket();
        return;
    }

    // Read the connect ACK (8 bytes: uint32 apiversion + char[4] magic)
    progress_connect_ack ack;
    if (!read_all(fd, &ack, sizeof(ack))) {
        g_critical("swupdate: failed to read progress ACK");
        close(fd);
        set_error("progress socket connect failed");
        set_state(UpdateState::Failure);
        return;
    }
    g_message("swupdate: progress ACK received, apiversion=0x%x magic=%.3s",
              ack.apiversion, ack.magic);

    // Loop: receive progress_msg updates until completion or timeout.
    // The progress socket pushes updates automatically — no polling needed.
    // Use select() with a timeout so we don't block forever if swupdate
    // crashes or hangs.
    bool saw_start = false;
    for (int idle_count = 0; idle_count < 600; ++idle_count) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv;
        tv.tv_sec = 60;  // 60s timeout between messages
        tv.tv_usec = 0;

        int ret = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            g_critical("swupdate: progress select(): %s", strerror(errno));
            break;
        }
        if (ret == 0) {
            // No message in 60 seconds. If we've seen START, the install
            // may have finished without sending a terminal status (e.g.
            // swupdate exited). Check via GET_STATUS as a fallback.
            if (saw_start) {
                g_message("swupdate: progress timeout, checking status");
                UpdateStatus s;
                if (poll_status(s)) {
                    if (s.state == UpdateState::Success ||
                        s.state == UpdateState::Done ||
                        (s.state == UpdateState::Idle &&
                         s.last_result != UpdateState::Failure)) {
                        g_message("swupdate: installation successful (via fallback)");
                        set_state(UpdateState::Success);
                        close(fd);
                        return;
                    }
                    if (s.state == UpdateState::Failure ||
                        (s.state == UpdateState::Idle &&
                         s.last_result == UpdateState::Failure)) {
                        g_critical("swupdate: installation failed (via fallback)");
                        set_error(s.error);
                        set_state(UpdateState::Failure);
                        close(fd);
                        return;
                    }
                }
            }
            continue;
        }

        progress_msg msg;
        if (!read_all(fd, &msg, sizeof(msg))) {
            // Connection closed by swupdate — install likely finished.
            // Check final status via control socket.
            g_message("swupdate: progress socket closed, checking final status");
            if (saw_start) {
                UpdateStatus s;
                if (poll_status(s)) {
                    if (s.last_result == UpdateState::Failure) {
                        set_error(s.error);
                        set_state(UpdateState::Failure);
                        close(fd);
                        return;
                    }
                }
                set_state(UpdateState::Success);
            }
            close(fd);
            return;
        }

        // Update atomic state from progress message
        percent_.store(msg.cur_percent);
        step_.store(msg.cur_step);
        total_steps_.store(msg.nsteps);
        if (msg.cur_image[0] != '\0')
            current_name_ = msg.cur_image;

        // Map status to UpdateState
        UpdateState new_state = UpdateState::Installing;
        switch (msg.status) {
        case kStatusIdle:
            new_state = UpdateState::Idle;
            break;
        case kStatusStart:
            new_state = UpdateState::Installing;
            saw_start = true;
            break;
        case kStatusRun:
        case kStatusProgress:
        case kStatusDownload:
        case kStatusSubprocess:
            new_state = UpdateState::Installing;
            saw_start = true;
            break;
        case kStatusSuccess:
            g_message("swupdate: installation successful "
                      "(step %u/%u, %u%%)",
                      msg.cur_step, msg.nsteps, msg.cur_percent);
            set_state(UpdateState::Success);
            close(fd);
            return;
        case kStatusFailure:
            g_critical("swupdate: installation failed");
            set_error("installation failed");
            set_state(UpdateState::Failure);
            close(fd);
            return;
        case kStatusDone:
            g_message("swupdate: installation done");
            set_state(UpdateState::Success);
            close(fd);
            return;
        }

        // Transition from Uploading to Installing when swupdate starts
        if (new_state == UpdateState::Installing &&
            state_.load() == UpdateState::Uploading) {
            g_message("swupdate: install started (step %u/%u: %s)",
                      msg.cur_step, msg.nsteps, msg.cur_image);
            set_state(UpdateState::Installing);
        }

        g_message("swupdate: progress: status=%u step=%u/%u percent=%u "
                  "image=%.32s",
                  msg.status, msg.cur_step, msg.nsteps, msg.cur_percent,
                  msg.cur_image);
    }

    g_critical("swupdate: timed out waiting for completion");
    set_error("timed out waiting for swupdate completion");
    set_state(UpdateState::Failure);
    close(fd);
}

/// Fallback: poll GET_STATUS on the control socket. Used when the progress
/// socket is not available. Less reliable because the control socket may
/// be blocked during CPIO streaming.
void SwupdateClient::poll_completion_via_control_socket() {
    UpdateState last_logged_state = UpdateState::Idle;

    for (int i = 0; i < 600; ++i) {
        UpdateStatus s;
        if (poll_status(s)) {
            percent_.store(s.percent);
            step_.store(s.step);
            total_steps_.store(s.total_steps);
            if (!s.current_name.empty())
                current_name_ = s.current_name;

            if (s.state == UpdateState::Success ||
                s.state == UpdateState::Done) {
                g_message("swupdate: installation successful");
                set_state(UpdateState::Success);
                return;
            }
            if (s.state == UpdateState::Failure) {
                g_critical("swupdate: installation failed: %s",
                          s.error.c_str());
                set_error(s.error);
                set_state(UpdateState::Failure);
                return;
            }

            if (s.state == UpdateState::Idle) {
                if (i < 5 && last_logged_state == UpdateState::Idle)
                    continue;
                if (s.last_result == UpdateState::Failure) {
                    set_error(s.error);
                    set_state(UpdateState::Failure);
                } else {
                    set_state(UpdateState::Success);
                }
                return;
            }

            if (s.state == UpdateState::Installing &&
                state_.load() == UpdateState::Uploading) {
                set_state(UpdateState::Installing);
            }
            last_logged_state = s.state;
        }
        sleep(1);
    }
    g_critical("swupdate: timed out waiting for completion");
    set_error("timed out waiting for swupdate completion");
    set_state(UpdateState::Failure);
}

int SwupdateClient::begin_stream_install() {
    if (is_busy()) {
        g_warning("swupdate: stream install requested but busy");
        return -1;
    }

    set_state(UpdateState::Uploading);
    percent_.store(0);
    step_.store(0);
    total_steps_.store(0);
    current_name_.clear();
    error_.clear();

    int fd = request_install();
    if (fd < 0) {
        set_error("cannot connect to swupdate IPC");
        set_state(UpdateState::Failure);
        return -1;
    }

    g_message("swupdate: streaming install started (fd=%d)", fd);

    // Start polling immediately — swupdate processes the CPIO stream as it
    // arrives, so installation runs concurrently with upload. The poll
    // thread will transition state from Uploading to Installing when
    // swupdate starts processing, and detect completion via last_result.
    if (install_thread_.joinable())
        install_thread_.detach();
    install_thread_ = std::thread(&SwupdateClient::poll_completion, this);
    install_thread_.detach();

    return fd;
}

void SwupdateClient::end_stream_install() {
    g_message("swupdate: stream upload complete");
    // Don't force state to Installing — the poll thread may have already
    // transitioned to Installing (or even Success/Failure) while upload
    // was ongoing. Only set Installing if we're still in Uploading state.
    if (state_.load() == UpdateState::Uploading) {
        set_state(UpdateState::Installing);
    }
    // The poll thread is already running and will detect completion.
}

UpdateStatus SwupdateClient::get_status() const {
    UpdateStatus s;
    s.state = state_.load();
    s.percent = percent_.load();
    s.step = step_.load();
    s.total_steps = total_steps_.load();
    s.current_name = current_name_;
    s.error = error_;
    return s;
}

bool SwupdateClient::is_busy() const {
    UpdateState s = state_.load();
    return s == UpdateState::Uploading || s == UpdateState::Installing;
}

void SwupdateClient::set_state(UpdateState state) {
    state_.store(state);
}

void SwupdateClient::set_error(const std::string& error) {
    error_ = error;
}

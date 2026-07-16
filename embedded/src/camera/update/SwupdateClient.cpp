#include "camera/update/SwupdateClient.h"

#include <fcntl.h>
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
#include <thread>

#include "camera/folly/File.h"
#include "camera/folly/FileUtil.h"
#include "camera/folly/logging/xlog.h"

namespace camera {

ssize_t write_full_nosigpipe(int fd, const void* buf, size_t count) {
    const char* p = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < count) {
        ssize_t n = ::send(fd, p + sent, count - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(count);
}

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

/// Connects to a Unix domain socket, trying |paths| in order. Returns an
/// owning File on success, an empty File on failure. |quiet| suppresses
/// the per-connect log (used for status polling).
folly::File connect_unix_socket(const char* const* paths, int num_paths,
                                bool quiet = false) {
    for (int i = 0; i < num_paths; ++i) {
        const char* path = paths[i];
        folly::File fd(socket(AF_UNIX, SOCK_STREAM, 0), /*ownsFd=*/true);
        if (!fd) {
            XLOGF(WARN, "swupdate: socket(): %s", strerror(errno));
            continue;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (connect(fd.fd(), reinterpret_cast<struct sockaddr*>(&addr),
                    sizeof(addr)) < 0) {
            if (!quiet)
                XLOGF(INFO, "swupdate: connect(%s): %s, trying next", path,
                          strerror(errno));
            continue;  // File dtor closes; try next path
        }
        if (!quiet)
            XLOGF(INFO, "swupdate: connected to %s (fd=%d)", path, fd.fd());
        return fd;
    }
    XLOGF(WARN, "swupdate: cannot connect to any socket (tried %d paths)",
              num_paths);
    return folly::File();
}

/// Connects to the swupdate IPC control socket (empty File on failure).
folly::File connect_swupdate_socket(bool quiet = false) {
    return connect_unix_socket(kSwupdateSocketPaths, kNumSocketPaths, quiet);
}

/// Writes exactly |len| bytes to a socket fd. Returns true on success,
/// false on error (logged). SIGPIPE-safe.
bool write_all(int fd, const void* buf, size_t len) {
    if (write_full_nosigpipe(fd, buf, len) < 0) {
        XLOGF(WARN, "swupdate: write: %s", strerror(errno));
        return false;
    }
    return true;
}

/// Reads exactly |len| bytes from fd. Returns true on success, false on
/// error or EOF (logged).
bool read_all(int fd, void* buf, size_t len) {
    ssize_t n = folly::readFull(fd, buf, len);
    if (n < 0) {
        XLOGF(WARN, "swupdate: read: %s", strerror(errno));
        return false;
    }
    if (static_cast<size_t>(n) < len) {
        XLOGF(WARN, "swupdate: read: EOF after %zd/%zu bytes", n, len);
        return false;
    }
    return true;
}

/// Sends REQ_INSTALL and waits for ACK/NACK. Returns the connected fd
/// (owning) on ACK, an empty File on NACK or error. After ACK, the
/// caller streams the image on this fd.
folly::File request_install() {
    folly::File fd = connect_swupdate_socket();
    if (!fd) {
        XLOGF(WARN, "swupdate: request_install: no socket connection");
        return folly::File();
    }

    XLOGF(INFO, "swupdate: sending REQ_INSTALL (magic=0x%08x, sizeof(ipc_message)=%zu)",
              kIpcMagic, sizeof(ipc_message));

    ipc_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = kIpcMagic;
    msg.type = kReqInstall;
    // swupdate checks apiversion == SWUPDATE_API_VERSION (1) and NACKs if
    // it doesn't match. The req is in msg.data.instmsg.req.
    msg.data.instmsg.req.apiversion = kSwupdateApiVersion;

    if (!write_all(fd.fd(), &msg, sizeof(msg))) {
        XLOGF(WARN, "swupdate: request_install: write failed");
        return folly::File();
    }

    XLOGF(INFO, "swupdate: waiting for ACK...");
    if (!read_all(fd.fd(), &msg, sizeof(msg))) {
        XLOGF(WARN, "swupdate: request_install: read ACK failed");
        return folly::File();
    }

    XLOGF(INFO, "swupdate: response type=%d, magic=0x%08x", msg.type, msg.magic);

    if (msg.type == kNack) {
        // swupdate writes the error reason into msg.data.msg (char[128]).
        // For "busy" it says "Installation in progress"; for apiversion
        // mismatch it's empty (memset to 0).
        char nack_msg[129];
        memcpy(nack_msg, msg.data.raw, 128);
        nack_msg[128] = '\0';
        XLOGF(WARN, "swupdate: install request NACK: %s",
                  nack_msg[0] ? nack_msg : "rejected (check apiversion)");
        return folly::File();
    }
    if (msg.type != kAck) {
        XLOGF(WARN, "swupdate: unexpected response type %d (expected ACK=%d)",
                  msg.type, kAck);
        return folly::File();
    }

    XLOGF(INFO, "swupdate: REQ_INSTALL ACKed, ready to stream (fd=%d)", fd.fd());
    return fd;
}

/// Polls swupdate for status via GET_STATUS on the control socket.
folly::Expected<UpdateStatus, std::string> poll_status() {
    // quiet connect — called every second
    folly::File fd = connect_swupdate_socket(true);
    if (!fd)
        return folly::makeUnexpected(
            std::string("cannot connect to swupdate IPC"));

    ipc_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic = kIpcMagic;
    msg.type = kGetStatus;

    if (!write_all(fd.fd(), &msg, sizeof(msg)))
        return folly::makeUnexpected(std::string("GET_STATUS write failed"));
    if (!read_all(fd.fd(), &msg, sizeof(msg)))
        return folly::makeUnexpected(std::string("GET_STATUS read failed"));
    fd.close();

    UpdateStatus out;

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

    XLOGF(INFO, "swupdate: status: raw=%d state=%d last_result=%d percent=%d desc=%.80s",
              raw_status, static_cast<int>(out.state),
              static_cast<int>(out.last_result), out.percent,
              desc[0] ? desc : "(empty)");
    return out;
}

/// Fallback: runs `swupdate -i <path>` as a subprocess and waits for
/// completion. Used when the IPC socket is not available (e.g. swupdate
/// not running in daemon mode). Returns the exit status.
int run_swupdate_subprocess(const std::string& path) {
    XLOGF(INFO, "swupdate: IPC socket unavailable, falling back to subprocess");
    pid_t pid = fork();
    if (pid < 0) {
        XLOGF(ERR, "swupdate: fork(): %s", strerror(errno));
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
        XLOGF(ERR, "swupdate: waitpid(): %s", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

}  // namespace

bool SwupdateClient::install_from_file(const std::string& path) {
    // Verify the file exists and is readable
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        XLOGF(WARN, "swupdate: %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    if (!try_begin(UpdateState::Installing)) {
        XLOGF(WARN, "swupdate: install requested but an update is in progress");
        return false;
    }

    std::thread(&SwupdateClient::run_install, this, path).detach();
    return true;
}

void SwupdateClient::run_install(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        XLOGF(INFO, "swupdate: installing %s (%lld bytes)", path.c_str(),
                  static_cast<long long>(st.st_size));
    else
        XLOGF(INFO, "swupdate: installing %s", path.c_str());

    // Try the IPC socket approach first (preferred: streams directly,
    // gives us status feedback).
    folly::File ipc = request_install();
    if (ipc) {
        // Stream the file to swupdate. Both Files must be closed before
        // poll_completion(): swupdate needs the write side closed to
        // finish reading the CPIO — hence the inner scope.
        {
            folly::File swu = std::move(ipc);
            folly::File src(open(path.c_str(), O_RDONLY), /*ownsFd=*/true);
            if (!src) {
                XLOGF(ERR, "swupdate: open(%s): %s", path.c_str(),
                           strerror(errno));
                fail(std::string("open: ") + strerror(errno));
                return;
            }

            char buf[65536];
            ssize_t n;
            ssize_t total = 0;
            while ((n = folly::readFull(src.fd(), buf, sizeof(buf))) > 0) {
                if (write_full_nosigpipe(swu.fd(), buf, n) < 0) {
                    XLOGF(ERR, "swupdate: write to IPC: %s",
                               strerror(errno));
                    fail("write to swupdate IPC failed");
                    return;
                }
                total += n;
            }
            if (n < 0) {
                XLOGF(ERR, "swupdate: read(%s): %s", path.c_str(),
                           strerror(errno));
                fail(std::string("read: ") + strerror(errno));
                return;
            }
            XLOGF(INFO, "swupdate: streamed %lld bytes to swupdate",
                      static_cast<long long>(total));
        }

        poll_completion();
        return;
    }

    // Fallback: subprocess (swupdate -i <path>)
    int rc = run_swupdate_subprocess(path);
    if (rc == 0) {
        XLOGF(INFO, "swupdate: subprocess installation successful");
        set_state(UpdateState::Success);
    } else {
        XLOGF(ERR, "swupdate: subprocess failed (exit %d)", rc);
        fail("swupdate subprocess exited with code " + std::to_string(rc));
    }
}

void SwupdateClient::poll_completion() {
    // Connect to the progress socket. This is a separate thread in swupdate
    // that pushes progress_msg updates to connected clients. Unlike GET_STATUS
    // on the control socket (which blocks during CPIO streaming), the progress
    // socket is always responsive.
    folly::File progress =
        connect_unix_socket(kProgressSocketPaths, kNumProgressSocketPaths);
    if (!progress) {
        XLOGF(ERR, "swupdate: cannot connect to progress socket, "
                   "falling back to GET_STATUS polling");
        poll_completion_via_control_socket();
        return;
    }
    const int fd = progress.fd();  // File closes it on every exit below

    // Read the connect ACK (8 bytes: uint32 apiversion + char[4] magic)
    progress_connect_ack ack;
    if (!read_all(fd, &ack, sizeof(ack))) {
        XLOGF(ERR, "swupdate: failed to read progress ACK");
        fail("progress socket connect failed");
        return;
    }
    XLOGF(INFO, "swupdate: progress ACK received, apiversion=0x%x magic=%.3s",
              ack.apiversion, ack.magic);

    // Loop: receive progress_msg updates until completion or timeout.
    // The progress socket pushes updates automatically — no polling needed.
    // Use select() with a timeout so we don't block forever if swupdate
    // crashes or hangs.
    // Only consecutive 60s silences count toward the bail-out; receiving a
    // message resets the counter. (Counting every message would make a
    // large multi-step update — hundreds of progress messages — spuriously
    // "time out".)
    bool saw_start = false;
    for (int idle_count = 0; idle_count < 600;) {
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
            XLOGF(ERR, "swupdate: progress select(): %s", strerror(errno));
            break;
        }
        if (ret == 0) {
            ++idle_count;
            // No message in 60 seconds. If we've seen START, the install
            // may have finished without sending a terminal status (e.g.
            // swupdate exited). Check via GET_STATUS as a fallback.
            if (saw_start) {
                XLOGF(INFO, "swupdate: progress timeout, checking status");
                if (auto s = poll_status()) {
                    if (s->state == UpdateState::Success ||
                        s->state == UpdateState::Done ||
                        (s->state == UpdateState::Idle &&
                         s->last_result != UpdateState::Failure)) {
                        XLOGF(INFO, "swupdate: installation successful (via fallback)");
                        set_state(UpdateState::Success);
                        return;
                    }
                    if (s->state == UpdateState::Failure ||
                        (s->state == UpdateState::Idle &&
                         s->last_result == UpdateState::Failure)) {
                        XLOGF(ERR, "swupdate: installation failed (via fallback)");
                        fail(s->error);
                        return;
                    }
                }
            }
            continue;
        }
        idle_count = 0;

        progress_msg msg;
        if (!read_all(fd, &msg, sizeof(msg))) {
            // Connection closed by swupdate. After START that usually means
            // the install finished — check final status via control socket.
            // Before START it means swupdate died or rejected us; without a
            // terminal state here the client would stay busy forever.
            XLOGF(INFO, "swupdate: progress socket closed, checking final status");
            if (!saw_start) {
                fail("swupdate closed progress socket before install started");
                return;
            }
            if (auto s = poll_status()) {
                if (s->last_result == UpdateState::Failure) {
                    fail(s->error);
                    return;
                }
            }
            set_state(UpdateState::Success);
            return;
        }

        // Update shared status from progress message
        status_.withWLock([&](UpdateStatus& st) {
            st.percent = msg.cur_percent;
            st.step = msg.cur_step;
            st.total_steps = msg.nsteps;
            if (msg.cur_image[0] != '\0')
                st.current_name = msg.cur_image;
        });

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
            XLOGF(INFO, "swupdate: installation successful "
                      "(step %u/%u, %u%%)",
                      msg.cur_step, msg.nsteps, msg.cur_percent);
            set_state(UpdateState::Success);
            return;
        case kStatusFailure:
            XLOGF(ERR, "swupdate: installation failed");
            fail("installation failed");
            return;
        case kStatusDone:
            XLOGF(INFO, "swupdate: installation done");
            set_state(UpdateState::Success);
            return;
        }

        // Transition from Uploading to Installing when swupdate starts
        if (new_state == UpdateState::Installing) {
            status_.withWLock([&](UpdateStatus& st) {
                if (st.state == UpdateState::Uploading) {
                    XLOGF(INFO, "swupdate: install started (step %u/%u: %s)",
                              msg.cur_step, msg.nsteps, msg.cur_image);
                    st.state = UpdateState::Installing;
                }
            });
        }

        XLOGF(INFO, "swupdate: progress: status=%u step=%u/%u percent=%u "
                  "image=%.32s",
                  msg.status, msg.cur_step, msg.nsteps, msg.cur_percent,
                  msg.cur_image);
    }

    XLOGF(ERR, "swupdate: timed out waiting for completion");
    fail("timed out waiting for swupdate completion");
}

/// Fallback: poll GET_STATUS on the control socket. Used when the progress
/// socket is not available. Less reliable because the control socket may
/// be blocked during CPIO streaming.
void SwupdateClient::poll_completion_via_control_socket() {
    UpdateState last_logged_state = UpdateState::Idle;

    for (int i = 0; i < 600; ++i) {
        if (auto r = poll_status()) {
            const UpdateStatus& s = *r;
            status_.withWLock([&](UpdateStatus& st) {
                st.percent = s.percent;
                st.step = s.step;
                st.total_steps = s.total_steps;
                if (!s.current_name.empty())
                    st.current_name = s.current_name;
            });

            if (s.state == UpdateState::Success ||
                s.state == UpdateState::Done) {
                XLOGF(INFO, "swupdate: installation successful");
                set_state(UpdateState::Success);
                return;
            }
            if (s.state == UpdateState::Failure) {
                XLOGF(ERR, "swupdate: installation failed: %s",
                          s.error.c_str());
                fail(s.error);
                return;
            }

            if (s.state == UpdateState::Idle) {
                if (i < 5 && last_logged_state == UpdateState::Idle)
                    continue;
                if (s.last_result == UpdateState::Failure) {
                    fail(s.error);
                } else {
                    set_state(UpdateState::Success);
                }
                return;
            }

            if (s.state == UpdateState::Installing) {
                status_.withWLock([](UpdateStatus& st) {
                    if (st.state == UpdateState::Uploading)
                        st.state = UpdateState::Installing;
                });
            }
            last_logged_state = s.state;
        }
        sleep(1);
    }
    XLOGF(ERR, "swupdate: timed out waiting for completion");
    fail("timed out waiting for swupdate completion");
}

folly::Expected<folly::File, std::string> SwupdateClient::begin_stream_install() {
    if (!try_begin(UpdateState::Uploading)) {
        XLOGF(WARN, "swupdate: stream install requested but busy");
        return folly::makeUnexpected(
            std::string("an update is already in progress"));
    }

    folly::File fd = request_install();
    if (!fd) {
        fail("cannot connect to swupdate IPC");
        return folly::makeUnexpected(
            std::string("cannot connect to swupdate IPC"));
    }

    XLOGF(INFO, "swupdate: streaming install started (fd=%d)", fd.fd());

    // Start polling immediately — swupdate processes the CPIO stream as it
    // arrives, so installation runs concurrently with upload. The poll
    // thread will transition state from Uploading to Installing when
    // swupdate starts processing, and detect completion via last_result.
    std::thread(&SwupdateClient::poll_completion, this).detach();

    return fd;
}

void SwupdateClient::end_stream_install() {
    XLOGF(INFO, "swupdate: stream upload complete");
    // Don't force state to Installing — the poll thread may have already
    // transitioned to Installing (or even Success/Failure) while upload
    // was ongoing. Only set Installing if we're still in Uploading state.
    status_.withWLock([](UpdateStatus& st) {
        if (st.state == UpdateState::Uploading)
            st.state = UpdateState::Installing;
    });
    // The poll thread is already running and will detect completion.
}

UpdateStatus SwupdateClient::get_status() const {
    return status_.copy();
}

bool SwupdateClient::is_busy() const {
    UpdateState s = status_.rlock()->state;
    return s == UpdateState::Uploading || s == UpdateState::Installing;
}

bool SwupdateClient::try_begin(UpdateState initial) {
    return status_.withWLock([&](UpdateStatus& st) {
        if (st.state == UpdateState::Uploading ||
            st.state == UpdateState::Installing)
            return false;
        st = UpdateStatus{};
        st.state = initial;
        return true;
    });
}

void SwupdateClient::set_state(UpdateState state) {
    status_.wlock()->state = state;
}

void SwupdateClient::fail(const std::string& error) {
    auto st = status_.wlock();
    st->error = error;
    st->state = UpdateState::Failure;
}

}  // namespace camera

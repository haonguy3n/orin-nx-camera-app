// SWUpdate IPC client: installs .swu packages via the swupdate daemon.
//
// Uses the swupdate IPC API (Unix domain socket at /run/swupdate/control.sock
// or /tmp/sockinstctrl) to stream the update image to swupdate and poll
// installation status.
// See: https://sbabic.github.io/swupdate/swupdate-ipc-interface.html
//
// The swupdate IPC protocol:
//  1. Client connects to the control socket
//  2. Client sends ipc_message with type=REQ_INSTALL, receives ACK/NACK
//  3. Client streams the raw .swu image (swupdate detects size from CPIO)
//  4. Client closes the write side
//  5. Client polls GET_STATUS for progress until SUCCESS/FAILURE/DONE
//
// IMPORTANT: swupdate processes the CPIO stream as it arrives, so
// installation runs CONCURRENTLY with upload. The poll thread starts
// immediately after REQ_INSTALL ACK (in begin_stream_install), not
// after upload completes. The state transitions from Uploading to
// Installing as soon as swupdate reports START/RUN/PROGRESS, even
// while the upload thread is still streaming data.
//
// This class encapsulates that protocol so the rest of the application
// only sees install_from_file() + get_status().
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <string>

#include "camera/base/Expected.h"
#include "camera/base/File.h"
#include "camera/base/Synchronized.h"

namespace camera {

/// camera::base::writeFull semantics for sockets: send() with MSG_NOSIGNAL so a
/// peer that dies mid-write (e.g. swupdate crashing) yields EPIPE instead
/// of a process-killing SIGPIPE (this app installs no SIGPIPE handler).
/// ponytail: lives here, not folly/FileUtil.h, to keep the vendored mimic
/// untouched; move there if a caller outside update/ appears.
ssize_t write_full_nosigpipe(int fd, const void* buf, size_t count);

/// SWUpdate installation status (matches swupdate's RECOVERY_STATUS enum).
enum class UpdateState {
    Idle,       ///< No update in progress
    Uploading,  ///< File is being uploaded to the device
    Installing, ///< swupdate is processing the image
    Success,    ///< Installation completed successfully
    Failure,    ///< Installation failed
    Done,       ///< Installation finished (post-update hooks ran)
};

/// Progress information for a running update.
struct UpdateStatus {
    UpdateState state = UpdateState::Idle;
    UpdateState last_result = UpdateState::Idle;  ///< Result of last install (persists after IDLE)
    int percent = 0;            ///< 0-100, current step progress
    int step = 0;               ///< Current step number (1-based)
    int total_steps = 0;        ///< Total number of steps in this update
    std::string current_name;   ///< Filename of the artifact being installed
    std::string error;          ///< Error message on failure
};

/// SWUpdate IPC client. Thread-safe: install runs on a detached background
/// thread, status lives in a camera::base::Synchronized and is readable from any
/// thread. Threads are deliberately detached: this object must outlive any
/// in-flight install (it lives in Application for the process lifetime).
class SwupdateClient {
public:
    SwupdateClient() = default;

    SwupdateClient(const SwupdateClient&) = delete;
    SwupdateClient& operator=(const SwupdateClient&) = delete;

    /// Installs the .swu file at |path| asynchronously. Returns false if
    /// an update is already in progress or swupdate is unreachable.
    /// Status is available via get_status() immediately after.
    bool install_from_file(const std::string& path);

    /// Begins a streaming install: connects to the swupdate IPC socket,
    /// sends REQ_INSTALL, waits for ACK. Returns an owning File that the
    /// caller writes the raw .swu image to, or an error message. After
    /// all data is written, the caller must close the File and call
    /// end_stream_install() to start status polling. No temp file
    /// needed — data goes directly to swupdate.
    camera::base::Expected<camera::base::File, std::string> begin_stream_install();

    /// Called after the caller has written all data and closed the fd
    /// from begin_stream_install(). Starts a background thread that polls
    /// swupdate for installation progress until completion.
    void end_stream_install();

    /// Current installation status (thread-safe).
    UpdateStatus get_status() const;

    /// True if an update is in progress (Uploading/Installing).
    bool is_busy() const;

private:
    void run_install(const std::string& path);
    void poll_completion();
    void poll_completion_via_control_socket();  ///< Fallback if progress socket unavailable

    /// Atomically checks not-busy and resets status to |initial|.
    /// Returns false (and changes nothing) if an update is in progress.
    bool try_begin(UpdateState initial);
    void set_state(UpdateState state);
    void fail(const std::string& error);  ///< error + state=Failure, one lock

    camera::base::Synchronized<UpdateStatus> status_;
};

}  // namespace camera

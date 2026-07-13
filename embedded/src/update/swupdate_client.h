// SWUpdate IPC client: installs .swu packages via the swupdate daemon.
//
// Uses the swupdate IPC API (Unix domain socket at /tmp/sockinstctrl) to
// stream the update image to swupdate and poll installation status.
// See: https://sbabic.github.io/swupdate/swupdate-ipc-interface.html
//
// The swupdate IPC protocol:
//  1. Client connects to /tmp/sockinstctrl
//  2. Client sends ipc_message with type=REQ_INSTALL, receives ACK/NACK
//  3. Client streams the raw .swu image (swupdate detects size from CPIO)
//  4. Client closes the write side
//  5. Client polls GET_STATUS for progress until SUCCESS/FAILURE/DONE
//
// This class encapsulates that protocol so the rest of the application
// only sees install_from_file() + get_status().
#pragma once

#include <atomic>
#include <string>
#include <thread>

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
    int percent = 0;            ///< 0-100, current step progress
    int step = 0;               ///< Current step number (1-based)
    int total_steps = 0;        ///< Total number of steps in this update
    std::string current_name;   ///< Filename of the artifact being installed
    std::string error;          ///< Error message on failure
};

/// SWUpdate IPC client. Thread-safe: install runs on a background thread,
/// status is readable from any thread via atomic state.
class SwupdateClient {
public:
    SwupdateClient();
    ~SwupdateClient();

    SwupdateClient(const SwupdateClient&) = delete;
    SwupdateClient& operator=(const SwupdateClient&) = delete;

    /// Installs the .swu file at |path| asynchronously. Returns false if
    /// an update is already in progress or swupdate is unreachable.
    /// Status is available via get_status() immediately after.
    bool install_from_file(const std::string& path);

    /// Begins a streaming install: connects to the swupdate IPC socket,
    /// sends REQ_INSTALL, waits for ACK. Returns a fd that the caller
    /// writes the raw .swu image to, or -1 on error. After all data is
    /// written, the caller must close the fd and call end_stream_install()
    /// to start status polling. No temp file needed — data goes directly
    /// to swupdate.
    int begin_stream_install();

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
    void set_state(UpdateState state);
    void set_error(const std::string& error);

    std::atomic<UpdateState> state_{UpdateState::Idle};
    std::atomic<int> percent_{0};
    std::atomic<int> step_{0};
    std::atomic<int> total_steps_{0};
    std::string current_name_;  // written by install thread only
    std::string error_;         // written by install thread only
    std::thread install_thread_;
};

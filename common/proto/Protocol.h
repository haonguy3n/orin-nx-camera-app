// Single source of truth for the host <-> device protocol constants
// (documented in proto/PROTOCOL.md). Shared by embedded/ (camera-streamer,
// GLib) and host-ui/ (Qt): pure C++17, no framework dependencies — keep it
// that way so both sides can include it.
#pragma once

namespace proto {

// Default ports ([server] section of camera-streamer.conf).
constexpr int kRtspPort = 8554;
constexpr int kControlPort = 8555;
constexpr int kDiscoveryPort = 8556;
constexpr int kUpdatePort = 8557;

// JSON-RPC-flavored error codes carried in the response "error" object.
enum ErrorCode {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kUnknownMethod = -32601,
    kInvalidParams = -32602,
    kFailed = 1,
};

// Control-channel methods (newline-delimited JSON over kControlPort).
namespace methods {
constexpr const char* kPing = "ping";
constexpr const char* kReload = "reload";
constexpr const char* kReboot = "reboot";
constexpr const char* kGetStatus = "get-status";
constexpr const char* kGetConfig = "get-config";
constexpr const char* kGetUpdateStatus = "get-update-status";
constexpr const char* kSetExposure = "set-exposure";
constexpr const char* kSetGain = "set-gain";
constexpr const char* kSetTrigger = "set-trigger";
constexpr const char* kFireTrigger = "fire-trigger";
constexpr const char* kSetSync = "set-sync";
constexpr const char* kSetZoom = "set-zoom";
constexpr const char* kSetIsp = "set-isp";
constexpr const char* kListControls = "list-controls";
constexpr const char* kGetControl = "get-control";
constexpr const char* kSetControl = "set-control";
// Sent by host-ui's white-balance calibrator but NOT implemented by the
// device yet (answers kUnknownMethod) — drift found when this header was
// created. Implement device-side or drop the host call.
constexpr const char* kSetTuning = "set-tuning";
// UDP discovery request method (broadcast to kDiscoveryPort).
constexpr const char* kDiscover = "discover";
}  // namespace methods

// State strings reported by get-update-status ("state" member).
namespace update_states {
constexpr const char* kIdle = "idle";
constexpr const char* kUploading = "uploading";
constexpr const char* kInstalling = "installing";
constexpr const char* kSuccess = "success";
constexpr const char* kFailure = "failure";
constexpr const char* kDone = "done";
}  // namespace update_states

}  // namespace proto

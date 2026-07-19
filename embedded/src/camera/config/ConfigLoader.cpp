#include "camera/config/ConfigLoader.h"

#include <glib.h>

#include <cstring>

#include "camera/base/ScopeGuard.h"
#include "camera/base/logging/xlog.h"
#include "proto/Protocol.h"

namespace camera {

namespace {

int get_int(GKeyFile* kf, const char* group, const char* key, int def) {
    GError* err = nullptr;
    int v = g_key_file_get_integer(kf, group, key, &err);
    if (err) {
        if (!g_error_matches(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
            !g_error_matches(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND))
            XLOGF(WARN, "config: [%s] %s: %s (using %d)", group, key, err->message, def);
        g_error_free(err);
        return def;
    }
    return v;
}

bool get_bool(GKeyFile* kf, const char* group, const char* key, bool def) {
    GError* err = nullptr;
    gboolean v = g_key_file_get_boolean(kf, group, key, &err);
    if (err) {
        if (!g_error_matches(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
            !g_error_matches(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND))
            XLOGF(WARN, "config: [%s] %s: %s (using %s)", group, key, err->message,
                      def ? "true" : "false");
        g_error_free(err);
        return def;
    }
    return v != FALSE;
}

double get_double(GKeyFile* kf, const char* group, const char* key, double def) {
    GError* err = nullptr;
    double v = g_key_file_get_double(kf, group, key, &err);
    if (err) {
        if (!g_error_matches(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND) &&
            !g_error_matches(err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_GROUP_NOT_FOUND))
            XLOGF(WARN, "config: [%s] %s: %s (using %g)", group, key, err->message, def);
        g_error_free(err);
        return def;
    }
    return v;
}

std::string get_string(GKeyFile* kf, const char* group, const char* key,
                       const std::string& def) {
    gchar* v = g_key_file_get_string(kf, group, key, nullptr);
    if (!v)
        return def;
    std::string s(v);
    g_free(v);
    return s;
}

// Restrict enum-like keys to known values.
std::string get_choice(GKeyFile* kf, const char* group, const char* key,
                       const std::string& def,
                       std::initializer_list<const char*> allowed) {
    std::string v = get_string(kf, group, key, def);
    for (const char* a : allowed)
        if (v == a)
            return v;
    XLOGF(WARN, "config: [%s] %s: invalid value '%s' (using '%s')", group, key,
              v.c_str(), def.c_str());
    return def;
}

}  // namespace

FileConfigLoader::FileConfigLoader(std::string path) : path_(std::move(path)) {}

Config FileConfigLoader::load() {
    Config cfg;
    // Built-in per-index defaults: cam0 -> sensor 0, cam1 -> sensor 1.
    for (int i = 0; i < Config::kNumCameras; ++i) {
        cfg.cameras[i].sensor_id = i;
        cfg.cameras[i].device = "/dev/video" + std::to_string(i);
    }

    GKeyFile* kf = g_key_file_new();
    SCOPE_EXIT { g_key_file_free(kf); };
    GError* err = nullptr;
    if (!g_key_file_load_from_file(kf, path_.c_str(), G_KEY_FILE_NONE, &err)) {
        if (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            XLOGF(INFO, "config: %s not found, using built-in defaults", path_.c_str());
        else
            XLOGF(WARN, "config: failed to load %s: %s (using built-in defaults)",
                      path_.c_str(), err->message);
        g_error_free(err);
        return cfg;
    }

    cfg.port = get_int(kf, "server", "port", cfg.port);
    if (cfg.port < 1 || cfg.port > 65535) {
        XLOGF(WARN, "config: [server] port %d out of range (using %d)", cfg.port,
                  proto::kRtspPort);
        cfg.port = proto::kRtspPort;
    }
    // Free-form on purpose: besides all/usb/ethernet it accepts an explicit
    // IPv4 address or interface name (validated at bind time).
    cfg.listen = get_string(kf, "server", "listen", cfg.listen);
    if (cfg.listen.empty())
        cfg.listen = "all";
    cfg.transports = get_choice(kf, "server", "transports", cfg.transports,
                                {"both", "network", "usb"});
    cfg.recovery_update =
        get_bool(kf, "server", "recovery-update", cfg.recovery_update);
    cfg.control_port = get_int(kf, "server", "control-port", cfg.control_port);
    if (cfg.control_port < 0 || cfg.control_port > 65535) {
        XLOGF(WARN, "config: [server] control-port %d out of range (using %d)",
                  cfg.control_port, proto::kControlPort);
        cfg.control_port = proto::kControlPort;
    }
    cfg.transport = get_choice(kf, "server", "transport", cfg.transport,
                               {"tcp", "udp", "all"});
    cfg.discovery_port =
        get_int(kf, "server", "discovery-port", cfg.discovery_port);
    if (cfg.discovery_port < 0 || cfg.discovery_port > 65535) {
        XLOGF(WARN, "config: [server] discovery-port %d out of range (using %d)",
                  cfg.discovery_port, proto::kDiscoveryPort);
        cfg.discovery_port = proto::kDiscoveryPort;
    }
    cfg.tls_cert = get_string(kf, "server", "tls-cert", cfg.tls_cert);
    cfg.tls_key = get_string(kf, "server", "tls-key", cfg.tls_key);
    cfg.tls_ca = get_string(kf, "server", "tls-ca", cfg.tls_ca);

    cfg.detect_model = get_string(kf, "detect", "model", cfg.detect_model);
    cfg.detect_width = get_int(kf, "detect", "width", cfg.detect_width);

    cfg.update_port = get_int(kf, "server", "update-port", cfg.update_port);
    if (cfg.update_port < 0 || cfg.update_port > 65535) {
        XLOGF(WARN, "config: [server] update-port %d out of range (using %d)",
                  cfg.update_port, proto::kUpdatePort);
        cfg.update_port = proto::kUpdatePort;
    }

    for (int i = 0; i < Config::kNumCameras; ++i) {
        char group[8];
        g_snprintf(group, sizeof(group), "cam%d", i);
        CameraConfig& cam = cfg.cameras[i];

        cam.enabled = get_bool(kf, group, "enabled", cam.enabled);
        cam.source = get_choice(kf, group, "source", cam.source,
                                {"argus", "v4l2", "test"});
        cam.sensor_id = get_int(kf, group, "sensor-id", cam.sensor_id);
        cam.device = get_string(kf, group, "device", cam.device);
        cam.width = get_int(kf, group, "width", cam.width);
        cam.height = get_int(kf, group, "height", cam.height);
        cam.framerate = get_int(kf, group, "framerate", cam.framerate);
        cam.codec = get_choice(kf, group, "codec", cam.codec, {"h265", "h264"});
        cam.bitrate = get_int(kf, group, "bitrate", cam.bitrate);
        cam.exposure = get_int(kf, group, "exposure", cam.exposure);
        cam.gain = get_double(kf, group, "gain", cam.gain);
        cam.trigger = get_int(kf, group, "trigger", cam.trigger);
        cam.zoom = get_double(kf, group, "zoom", cam.zoom);
        if (cam.zoom < 1.0 || cam.zoom > 8.0) {
            XLOGF(WARN, "config: [%s] zoom %g out of range 1-8 (using 1)",
                      group, cam.zoom);
            cam.zoom = 1.0;
        }
        cam.zoom_x = CLAMP(get_double(kf, group, "zoom-x", cam.zoom_x), 0.0, 1.0);
        cam.zoom_y = CLAMP(get_double(kf, group, "zoom-y", cam.zoom_y), 0.0, 1.0);

        // isp-<property> keys become nvarguscamerasrc property overrides
        // (validated against the whitelist where set-isp is served too).
        gchar** keys = g_key_file_get_keys(kf, group, nullptr, nullptr);
        for (gchar** k = keys; k != nullptr && *k != nullptr; ++k) {
            if (!g_str_has_prefix(*k, "isp-"))
                continue;
            const std::string value = get_string(kf, group, *k, "");
            if (!value.empty())
                cam.isp[*k + strlen("isp-")] = value;
        }
        g_strfreev(keys);
    }

    XLOGF(INFO, "config: loaded %s", path_.c_str());
    return cfg;
}

}  // namespace camera

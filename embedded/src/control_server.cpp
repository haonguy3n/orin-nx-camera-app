#include "control_server.h"

#include <json-glib/json-glib.h>

#include "isp_file.h"
#include "rtsp_server.h"
#include "v4l2_ctrl.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace {

// JSON-RPC-flavored error codes (proto/PROTOCOL.md).
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kUnknownMethod = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kFailed = 1;

// nvarguscamerasrc properties settable via set-isp (proto/PROTOCOL.md).
// Everything else on the element (sensor-id, ranges, ...) is owned by the
// launch string / dedicated methods and must not be poked freely.
const char* const kIspParams[] = {
    "wbmode",         "saturation",  "tnr-mode",
    "tnr-strength",   "ee-mode",     "ee-strength",
    "aeantibanding",  "exposurecompensation",
    "aelock",         "awblock",     "ispdigitalgainrange",
};

bool is_isp_param(const std::string& name) {
    for (const char* p : kIspParams)
        if (name == p)
            return true;
    return false;
}

// "Auto" ranges handed to nvarguscamerasrc when exposure/gain is set back
// to 0: wide enough to give the 3A loop its freedom back, clamped by Argus
// to the sensor's real limits (there is no property to truly unset a range).
constexpr const char* kArgusAutoExposure = "13000 100000000";  // 13 µs..100 ms
constexpr const char* kArgusAutoGain = "1 16";

std::string node_to_string(JsonNode* node) {
    JsonGenerator* gen = json_generator_new();
    json_generator_set_root(gen, node);
    gchar* data = json_generator_to_data(gen, nullptr);
    std::string out(data);
    g_free(data);
    g_object_unref(gen);
    return out;
}

// --- params access ----------------------------------------------------------

bool param_int(JsonObject* params, const char* name, int64_t* out) {
    if (params == nullptr || !json_object_has_member(params, name))
        return false;
    JsonNode* n = json_object_get_member(params, name);
    if (!JSON_NODE_HOLDS_VALUE(n))
        return false;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64)
        *out = json_node_get_int(n);
    else if (t == G_TYPE_DOUBLE)
        *out = static_cast<int64_t>(json_node_get_double(n));
    else
        return false;
    return true;
}

bool param_bool(JsonObject* params, const char* name, bool* out) {
    if (params == nullptr || !json_object_has_member(params, name))
        return false;
    JsonNode* n = json_object_get_member(params, name);
    if (!JSON_NODE_HOLDS_VALUE(n) ||
        json_node_get_value_type(n) != G_TYPE_BOOLEAN)
        return false;
    *out = json_node_get_boolean(n) != FALSE;
    return true;
}

bool param_double(JsonObject* params, const char* name, double* out) {
    int64_t i;
    if (params == nullptr || !json_object_has_member(params, name))
        return false;
    JsonNode* n = json_object_get_member(params, name);
    if (JSON_NODE_HOLDS_VALUE(n) &&
        json_node_get_value_type(n) == G_TYPE_DOUBLE) {
        *out = json_node_get_double(n);
        return true;
    }
    if (param_int(params, name, &i)) {
        *out = static_cast<double>(i);
        return true;
    }
    return false;
}

// "control": name string or numeric id.
bool param_control(JsonObject* params, std::string* out) {
    if (params == nullptr || !json_object_has_member(params, "control"))
        return false;
    JsonNode* n = json_object_get_member(params, "control");
    if (!JSON_NODE_HOLDS_VALUE(n))
        return false;
    if (json_node_get_value_type(n) == G_TYPE_STRING) {
        *out = json_node_get_string(n);
        return !out->empty();
    }
    int64_t id;
    if (!param_int(params, "control", &id))
        return false;
    *out = std::to_string(id);
    return true;
}

bool param_camera(JsonObject* params, int* out) {
    int64_t v;
    if (!param_int(params, "camera", &v) || v < 0 || v >= Config::kNumCameras)
        return false;
    *out = static_cast<int>(v);
    return true;
}

// --- result builders --------------------------------------------------------

void add_camera_config(JsonBuilder* b, int index, const CameraConfig& cam) {
    json_builder_set_member_name(b, "index");
    json_builder_add_int_value(b, index);
    json_builder_set_member_name(b, "mount");
    json_builder_add_string_value(b, ("/cam" + std::to_string(index)).c_str());
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, cam.enabled);
    json_builder_set_member_name(b, "source");
    json_builder_add_string_value(b, cam.source.c_str());
    json_builder_set_member_name(b, "device");
    json_builder_add_string_value(b, cam.device.c_str());
    json_builder_set_member_name(b, "sensor_id");
    json_builder_add_int_value(b, cam.sensor_id);
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, cam.width);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, cam.height);
    json_builder_set_member_name(b, "framerate");
    json_builder_add_int_value(b, cam.framerate);
    json_builder_set_member_name(b, "codec");
    json_builder_add_string_value(b, cam.codec.c_str());
    json_builder_set_member_name(b, "bitrate");
    json_builder_add_int_value(b, cam.bitrate);
    json_builder_set_member_name(b, "exposure");
    json_builder_add_int_value(b, cam.exposure);
    json_builder_set_member_name(b, "gain");
    json_builder_add_double_value(b, cam.gain);
    json_builder_set_member_name(b, "trigger");
    json_builder_add_int_value(b, cam.trigger);
    json_builder_set_member_name(b, "isp");
    json_builder_begin_object(b);
    for (const auto& [property, value] : cam.isp) {
        json_builder_set_member_name(b, property.c_str());
        json_builder_add_string_value(b, value.c_str());
    }
    json_builder_end_object(b);
    json_builder_set_member_name(b, "zoom");
    json_builder_add_double_value(b, cam.zoom);
    json_builder_set_member_name(b, "zoom_x");
    json_builder_add_double_value(b, cam.zoom_x);
    json_builder_set_member_name(b, "zoom_y");
    json_builder_add_double_value(b, cam.zoom_y);
}

void add_v4l2_control(JsonBuilder* b, const V4l2Control& c) {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, c.id);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, c.name.c_str());
    json_builder_set_member_name(b, "type");
    json_builder_add_int_value(b, c.type);
    json_builder_set_member_name(b, "min");
    json_builder_add_int_value(b, c.minimum);
    json_builder_set_member_name(b, "max");
    json_builder_add_int_value(b, c.maximum);
    json_builder_set_member_name(b, "step");
    json_builder_add_int_value(b, c.step);
    json_builder_set_member_name(b, "default");
    json_builder_add_int_value(b, c.default_value);
    json_builder_set_member_name(b, "value");
    json_builder_add_int_value(b, c.value);
    json_builder_set_member_name(b, "flags");
    json_builder_add_int_value(b, c.flags);
    json_builder_end_object(b);
}

JsonNode* take_root(JsonBuilder* b) {
    JsonNode* root = json_builder_get_root(b);
    g_object_unref(b);
    return root;
}

JsonNode* empty_result() {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_end_object(b);
    return take_root(b);
}

std::string peer_name(GSocketConnection* connection) {
    GSocketAddress* addr =
        g_socket_connection_get_remote_address(connection, nullptr);
    std::string out = "unknown";
    if (addr != nullptr && G_IS_INET_SOCKET_ADDRESS(addr)) {
        auto* isa = G_INET_SOCKET_ADDRESS(addr);
        gchar* ip = g_inet_address_to_string(g_inet_socket_address_get_address(isa));
        out = std::string(ip) + ":" +
              std::to_string(g_inet_socket_address_get_port(isa));
        g_free(ip);
    }
    if (addr != nullptr)
        g_object_unref(addr);
    return out;
}

}  // namespace

// One accepted client connection. Owns refs on the socket/streams; exactly
// one read_line_async is always pending, and its completion callback frees
// the Conn once the connection is done (or the server is gone).
struct ControlServer::Conn {
    ControlServer* server;  // nulled by ~ControlServer
    GSocketConnection* socket;
    GDataInputStream* in;
    GCancellable* cancellable;
    std::string peer;
};

ControlServer::ControlServer(ControlHooks hooks) : hooks_(std::move(hooks)) {}

ControlServer::~ControlServer() {
    // Orphan live connections; their pending reads complete (cancelled) and
    // free them. Single-threaded: no completion can run during this loop.
    for (Conn* conn : conns_) {
        conn->server = nullptr;
        g_cancellable_cancel(conn->cancellable);
        g_io_stream_close(G_IO_STREAM(conn->socket), nullptr, nullptr);
    }
    conns_.clear();
    if (service_ != nullptr) {
        g_socket_service_stop(service_);
        g_socket_listener_close(G_SOCKET_LISTENER(service_));
        g_object_unref(service_);
    }
}

bool ControlServer::start(const std::string& address, int port) {
    service_ = g_socket_service_new();

    GInetAddress* inet = g_inet_address_new_from_string(address.c_str());
    if (inet == nullptr) {
        g_printerr("control: invalid address %s\n", address.c_str());
        return false;
    }
    GSocketAddress* sockaddr = g_inet_socket_address_new(inet, port);
    g_object_unref(inet);

    GError* err = nullptr;
    gboolean ok = g_socket_listener_add_address(
        G_SOCKET_LISTENER(service_), sockaddr, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, nullptr, nullptr, &err);
    g_object_unref(sockaddr);
    if (!ok) {
        g_printerr("control: bind %s:%d failed: %s\n", address.c_str(), port,
                   err->message);
        g_error_free(err);
        return false;
    }

    g_signal_connect(service_, "incoming", G_CALLBACK(on_incoming), this);
    g_socket_service_start(service_);
    g_message("control server listening on %s:%d", address.c_str(), port);
    return true;
}

gboolean ControlServer::on_incoming(GSocketService* /*service*/,
                                    GSocketConnection* connection,
                                    GObject* /*source*/, gpointer user_data) {
    auto* self = static_cast<ControlServer*>(user_data);

    auto* conn = new Conn;
    conn->server = self;
    conn->socket = static_cast<GSocketConnection*>(g_object_ref(connection));
    conn->in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_set_newline_type(conn->in,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    conn->cancellable = g_cancellable_new();
    conn->peer = peer_name(connection);
    self->conns_.insert(conn);

    g_message("control: %s connected", conn->peer.c_str());
    g_data_input_stream_read_line_async(conn->in, G_PRIORITY_DEFAULT,
                                        conn->cancellable, on_line, conn);
    return FALSE;
}

void ControlServer::close_conn(Conn* conn) {
    if (conn->server != nullptr) {
        conn->server->conns_.erase(conn);
        g_message("control: %s disconnected", conn->peer.c_str());
    }
    g_io_stream_close(G_IO_STREAM(conn->socket), nullptr, nullptr);
    g_object_unref(conn->in);
    g_object_unref(conn->cancellable);
    g_object_unref(conn->socket);
    delete conn;
}

void ControlServer::on_line(GObject* source, GAsyncResult* result,
                            gpointer user_data) {
    auto* conn = static_cast<Conn*>(user_data);
    GError* err = nullptr;
    char* line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, nullptr, &err);
    if (err != nullptr)
        g_error_free(err);

    if (conn->server == nullptr || line == nullptr) {  // gone / EOF / error
        g_free(line);
        close_conn(conn);
        return;
    }

    conn->server->process_line(conn, line);
    g_free(line);
    g_data_input_stream_read_line_async(conn->in, G_PRIORITY_DEFAULT,
                                        conn->cancellable, on_line, conn);
}

void ControlServer::process_line(Conn* conn, const char* line) {
    if (*line == '\0')
        return;  // ignore blank lines (nc users)

    JsonNode* id = nullptr;      // borrowed from the parsed tree
    JsonNode* res = nullptr;     // transfer full
    int err_code = 0;
    std::string err_msg;

    JsonParser* parser = json_parser_new();
    GError* perr = nullptr;
    if (!json_parser_load_from_data(parser, line, -1, &perr)) {
        err_code = kParseError;
        err_msg = perr->message;
        g_error_free(perr);
    } else {
        JsonNode* root = json_parser_get_root(parser);
        if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
            err_code = kInvalidRequest;
            err_msg = "request must be a JSON object";
        } else {
            JsonObject* req = json_node_get_object(root);
            if (json_object_has_member(req, "id"))
                id = json_object_get_member(req, "id");
            JsonObject* params = nullptr;
            if (json_object_has_member(req, "params")) {
                JsonNode* p = json_object_get_member(req, "params");
                if (JSON_NODE_HOLDS_OBJECT(p))
                    params = json_node_get_object(p);
            }
            const char* method =
                json_object_has_member(req, "method")
                    ? json_object_get_string_member_with_default(req, "method",
                                                                 nullptr)
                    : nullptr;
            if (method == nullptr) {
                err_code = kInvalidRequest;
                err_msg = "missing method";
            } else {
                res = dispatch(method, params, &err_code, &err_msg);
            }
        }
    }

    // Envelope: {"id": ..., "result": ...} or {"id": ..., "error": {...}}.
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_value(b, id != nullptr ? json_node_copy(id)
                                            : json_node_new(JSON_NODE_NULL));
    if (res != nullptr) {
        json_builder_set_member_name(b, "result");
        json_builder_add_value(b, res);  // ownership transferred
    } else {
        json_builder_set_member_name(b, "error");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "code");
        json_builder_add_int_value(b, err_code);
        json_builder_set_member_name(b, "message");
        json_builder_add_string_value(b, err_msg.c_str());
        json_builder_end_object(b);
    }
    json_builder_end_object(b);

    JsonNode* reply = take_root(b);
    const std::string out = node_to_string(reply) + "\n";
    json_node_unref(reply);
    g_object_unref(parser);

    g_output_stream_write_all(
        g_io_stream_get_output_stream(G_IO_STREAM(conn->socket)), out.data(),
        out.size(), nullptr, nullptr, nullptr);
}

JsonNode* ControlServer::dispatch(const char* method, JsonObject* params,
                                  int* err_code, std::string* err_msg) {
    auto invalid = [&](const char* msg) -> JsonNode* {
        *err_code = kInvalidParams;
        *err_msg = msg;
        return nullptr;
    };
    auto failed = [&](const std::string& msg) -> JsonNode* {
        *err_code = kFailed;
        *err_msg = msg;
        return nullptr;
    };
    const std::string m = method;
    Config& cfg = hooks_.config();
    RtspServer* rtsp = hooks_.rtsp();

    if (m == "ping") {
        JsonBuilder* b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "pong");
        json_builder_add_boolean_value(b, TRUE);
        json_builder_set_member_name(b, "version");
        json_builder_add_string_value(b, APP_VERSION);
        json_builder_end_object(b);
        return take_root(b);
    }

    if (m == "get-status" || m == "get-config") {
        const bool status = m == "get-status";
        JsonBuilder* b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "version");
        json_builder_add_string_value(b, APP_VERSION);
        json_builder_set_member_name(b, "listen");
        json_builder_add_string_value(b, cfg.listen.c_str());
        json_builder_set_member_name(b, "port");
        json_builder_add_int_value(b, cfg.port);
        json_builder_set_member_name(b, "control_port");
        json_builder_add_int_value(b, cfg.control_port);
        if (status) {
            json_builder_set_member_name(b, "address");
            json_builder_add_string_value(
                b, rtsp != nullptr ? rtsp->bound_address().c_str() : "");
            json_builder_set_member_name(b, "clients");
            json_builder_add_int_value(
                b, rtsp != nullptr ? rtsp->client_count() : 0);
        }
        json_builder_set_member_name(b, "tuning");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "black_level");
        json_builder_add_int_value(b, cfg.tuning.black_level);
        json_builder_set_member_name(b, "wb_trim_r");
        json_builder_add_double_value(b, cfg.tuning.wb_trim_r);
        json_builder_set_member_name(b, "wb_trim_b");
        json_builder_add_double_value(b, cfg.tuning.wb_trim_b);
        json_builder_end_object(b);
        json_builder_set_member_name(b, "cameras");
        json_builder_begin_array(b);
        for (int i = 0; i < Config::kNumCameras; ++i) {
            json_builder_begin_object(b);
            add_camera_config(b, i, cfg.cameras[i]);
            if (status && rtsp != nullptr) {
                const StreamStatus s = rtsp->stream_status(i);
                json_builder_set_member_name(b, "streaming");
                json_builder_add_boolean_value(b, s.streaming);
                json_builder_set_member_name(b, "frames");
                json_builder_add_int_value(b, s.frames);
                // Live values programmed into the sensor right now — when
                // exposure/gain are 0 (auto), this is what Argus AE chose.
                // Read from the driver's V4L2 controls; omitted if the
                // device node can't be queried.
                V4l2Control live;
                std::string lerr;
                if (v4l2_get_control(cfg.cameras[i].device, "exposure",
                                     &live, &lerr)) {
                    json_builder_set_member_name(b, "exposure_current");
                    json_builder_add_int_value(b, live.value);
                }
                if (v4l2_get_control(cfg.cameras[i].device, "gain", &live,
                                     &lerr)) {
                    json_builder_set_member_name(b, "gain_current");
                    json_builder_add_int_value(b, live.value);
                }
                if (s.frames > 0) {
                    json_builder_set_member_name(b, "last_frame");
                    json_builder_begin_object(b);
                    json_builder_set_member_name(b, "sequence");
                    json_builder_add_int_value(b, s.sequence);
                    json_builder_set_member_name(b, "pts");
                    json_builder_add_int_value(b, s.pts);
                    json_builder_set_member_name(b, "wallclock");
                    json_builder_add_int_value(b, s.wallclock);
                    json_builder_end_object(b);
                }
            }
            json_builder_end_object(b);
        }
        json_builder_end_array(b);
        json_builder_end_object(b);
        return take_root(b);
    }

    if (m == "set-exposure" || m == "set-gain") {
        int cam_idx;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        CameraConfig& cam = cfg.cameras[cam_idx];
        if (cam.source == "test")
            return failed("not supported for source 'test'");

        const bool exposure = m == "set-exposure";
        int64_t us = 0;
        double gain = 0;
        if (exposure) {
            if (!param_int(params, "us", &us) || us < 0)
                return invalid("us must be an integer >= 0");
        } else {
            if (!param_double(params, "gain", &gain) || gain < 0)
                return invalid("gain must be a number >= 0");
        }

        if (cam.source == "v4l2") {
            const char* name = exposure ? "exposure" : "gain";
            const bool use_default = exposure ? us == 0 : gain == 0;
            int64_t value = exposure ? us : static_cast<int64_t>(gain);
            std::string err;
            if (use_default) {  // 0 = back to the driver default
                V4l2Control c;
                if (!v4l2_get_control(cam.device, name, &c, &err))
                    return failed(err);
                value = c.default_value;
            }
            if (!v4l2_set_control(cam.device, name, value, &err))
                return failed(err);
        } else {  // argus: pin (or widen) the matching 3A range
            std::string range;
            if (exposure) {
                if (us > 0) {
                    const std::string ns = std::to_string(us * 1000);
                    range = ns + " " + ns;
                } else {
                    range = kArgusAutoExposure;
                }
            } else {
                if (gain > 0) {
                    char buf[32];
                    g_snprintf(buf, sizeof(buf), "%g %g", gain, gain);
                    range = buf;
                } else {
                    range = kArgusAutoGain;
                }
            }
            // No live pipeline is fine: the config below seeds the launch
            // string of the next pipeline.
            if (rtsp != nullptr)
                rtsp->set_source_property(
                    cam_idx, exposure ? "exposuretimerange" : "gainrange",
                    range.c_str());
        }

        if (exposure)
            cam.exposure = static_cast<int>(us);
        else
            cam.gain = gain;
        if (cam.source == "argus" && rtsp != nullptr)
            rtsp->refresh_launch(cam_idx);  // future sessions inherit it
        g_message("control: cam%d %s = %s", cam_idx,
                  exposure ? "exposure" : "gain",
                  exposure ? (std::to_string(us) + " us").c_str()
                           : std::to_string(gain).c_str());
        return empty_result();
    }

    if (m == "set-trigger") {
        int cam_idx;
        int64_t mode;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        if (!param_int(params, "mode", &mode) || mode < 0)
            return invalid("mode must be an integer >= 0");
        CameraConfig& cam = cfg.cameras[cam_idx];
        if (cam.source != "v4l2")
            return failed("hardware trigger requires the v4l2 source "
                          "(current source '" + cam.source + "')");
        std::string err;
        if (!v4l2_set_trigger_mode(cam.device, static_cast<int>(mode), &err))
            return failed(err);
        cam.trigger = static_cast<int>(mode);
        g_message("control: cam%d trigger mode = %d", cam_idx, cam.trigger);
        return empty_result();
    }

    if (m == "set-isp") {
        int cam_idx;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        CameraConfig& cam = cfg.cameras[cam_idx];
        if (cam.source != "argus")
            return failed("ISP controls require the argus source "
                          "(current source '" + cam.source + "')");

        std::string name;
        if (params != nullptr && json_object_has_member(params, "param")) {
            JsonNode* n = json_object_get_member(params, "param");
            if (JSON_NODE_HOLDS_VALUE(n) &&
                json_node_get_value_type(n) == G_TYPE_STRING)
                name = json_node_get_string(n);
        }
        if (!is_isp_param(name))
            return invalid("param must be one of the nvarguscamerasrc ISP "
                           "properties (see PROTOCOL.md)");

        if (params == nullptr || !json_object_has_member(params, "value"))
            return invalid("missing value");
        JsonNode* vn = json_object_get_member(params, "value");
        if (JSON_NODE_HOLDS_NULL(vn)) {  // forget the override
            cam.isp.erase(name);
            if (rtsp != nullptr)
                rtsp->refresh_launch(cam_idx);
            g_message("control: cam%d isp %s reset", cam_idx, name.c_str());
            return empty_result();
        }
        if (!JSON_NODE_HOLDS_VALUE(vn))
            return invalid("value must be a string, number, bool or null");
        std::string value;
        const GType t = json_node_get_value_type(vn);
        if (t == G_TYPE_STRING) {
            value = json_node_get_string(vn);
        } else if (t == G_TYPE_BOOLEAN) {
            value = json_node_get_boolean(vn) ? "true" : "false";
        } else if (t == G_TYPE_INT64) {
            value = std::to_string(json_node_get_int(vn));
        } else {
            char buf[32];
            g_snprintf(buf, sizeof(buf), "%g", json_node_get_double(vn));
            value = buf;
        }

        cam.isp[name] = value;
        // Live pipeline picks it up now; the refreshed factory launch
        // string covers every session created afterwards.
        if (rtsp != nullptr) {
            rtsp->set_source_property(cam_idx, name.c_str(), value.c_str());
            rtsp->refresh_launch(cam_idx);
        }
        g_message("control: cam%d isp %s = %s", cam_idx, name.c_str(),
                  value.c_str());
        return empty_result();
    }

    if (m == "set-zoom") {
        int cam_idx;
        double factor;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        if (!param_double(params, "factor", &factor) || factor < 1.0 ||
            factor > 8.0)
            return invalid("factor must be a number in 1.0-8.0");
        CameraConfig& cam = cfg.cameras[cam_idx];
        double v;
        if (param_double(params, "x", &v))
            cam.zoom_x = CLAMP(v, 0.0, 1.0);
        if (param_double(params, "y", &v))
            cam.zoom_y = CLAMP(v, 0.0, 1.0);
        cam.zoom = factor;
        // The re-armed launch string is authoritative (clients reconnect to
        // pick it up); there is no reliable live crop update on nvvidconv.
        if (rtsp != nullptr)
            rtsp->refresh_launch(cam_idx);
        g_message("control: cam%d zoom = %.2fx @ (%.2f, %.2f)", cam_idx,
                  cam.zoom, cam.zoom_x, cam.zoom_y);
        return empty_result();
    }

    if (m == "set-sync") {
        bool enabled;
        if (!param_bool(params, "enabled", &enabled))
            return invalid("enabled must be a boolean");
        // All-or-nothing precheck: hardware sync only makes sense when every
        // enabled camera is on the v4l2 path.
        for (int i = 0; i < Config::kNumCameras; ++i) {
            const CameraConfig& cam = cfg.cameras[i];
            if (cam.enabled && cam.source != "v4l2")
                return failed("cam" + std::to_string(i) + " is source '" +
                              cam.source + "'; sync requires v4l2");
        }
        const int mode = enabled ? 1 : 0;  // external / free running
        for (int i = 0; i < Config::kNumCameras; ++i) {
            CameraConfig& cam = cfg.cameras[i];
            if (!cam.enabled)
                continue;
            std::string err;
            if (!v4l2_set_trigger_mode(cam.device, mode, &err))
                return failed(err);
            cam.trigger = mode;
        }
        g_message("control: sync trigger %s", enabled ? "on" : "off");
        return empty_result();
    }

    if (m == "fire-trigger") {
        int cam_idx;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        const CameraConfig& cam = cfg.cameras[cam_idx];
        if (cam.source != "v4l2")
            return failed("software trigger requires the v4l2 source "
                          "(current source '" + cam.source + "')");
        std::string err;
        if (!v4l2_fire_single_trigger(cam.device, &err))
            return failed(err);
        return empty_result();
    }

    if (m == "list-controls") {
        int cam_idx;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        std::string err;
        std::vector<V4l2Control> ctrls =
            v4l2_list_controls(cfg.cameras[cam_idx].device, &err);
        if (ctrls.empty())
            return failed(err);
        JsonBuilder* b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "controls");
        json_builder_begin_array(b);
        for (const V4l2Control& c : ctrls)
            add_v4l2_control(b, c);
        json_builder_end_array(b);
        json_builder_end_object(b);
        return take_root(b);
    }

    if (m == "get-control" || m == "set-control") {
        int cam_idx;
        std::string control;
        if (!param_camera(params, &cam_idx))
            return invalid("camera must be 0 or 1");
        if (!param_control(params, &control))
            return invalid("control must be a name or numeric id");
        const std::string& device = cfg.cameras[cam_idx].device;
        std::string err;
        if (m == "set-control") {
            int64_t value;
            if (!param_int(params, "value", &value))
                return invalid("value must be an integer");
            if (!v4l2_set_control(device, control, value, &err))
                return failed(err);
            return empty_result();
        }
        V4l2Control c;
        if (!v4l2_get_control(device, control, &c, &err))
            return failed(err);
        JsonBuilder* b = json_builder_new();
        add_v4l2_control(b, c);
        return take_root(b);
    }

    if (m == "set-tuning") {
        TuningConfig t = cfg.tuning;
        int64_t bl;
        double v;
        if (param_int(params, "black_level", &bl)) {
            if (bl < 0 || bl > 1023)
                return invalid("black_level must be 0-1023");
            t.black_level = static_cast<int>(bl);
        }
        if (param_double(params, "wb_trim_r", &v)) {
            if (v < 0.5 || v > 2.0)
                return invalid("wb_trim_r must be 0.5-2.0");
            t.wb_trim_r = v;
        }
        if (param_double(params, "wb_trim_b", &v)) {
            if (v < 0.5 || v > 2.0)
                return invalid("wb_trim_b must be 0.5-2.0");
            t.wb_trim_b = v;
        }
        if (t == cfg.tuning)
            return empty_result();  // nothing changed, no outage

        cfg.tuning = t;
        isp_file_sync(cfg.tuning);
        // Reply first: applying restarts the Argus daemon and the RTSP
        // servers (~5 s outage; clients reconnect). Same deferred-idle
        // pattern as reload — the apply replaces this very server's peers.
        auto* fn = new std::function<void()>(hooks_.apply_tuning);
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* f = static_cast<std::function<void()>*>(data);
                if (*f)
                    (*f)();
                delete f;
                return G_SOURCE_REMOVE;
            },
            fn);
        return empty_result();
    }

    if (m == "reload") {
        // Reply first, restart after: the reload replaces this very server,
        // so run it from an idle callback that owns its own copy of the hook.
        auto* fn = new std::function<void()>(hooks_.reload);
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* f = static_cast<std::function<void()>*>(data);
                (*f)();
                delete f;
                return G_SOURCE_REMOVE;
            },
            fn);
        return empty_result();
    }

    *err_code = kUnknownMethod;
    *err_msg = "unknown method '" + m + "'";
    return nullptr;
}

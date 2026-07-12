#include "control_server.h"

#include <json-glib/json-glib.h>

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

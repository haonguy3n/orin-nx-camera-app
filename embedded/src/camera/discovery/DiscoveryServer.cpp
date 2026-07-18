#include "camera/discovery/DiscoveryServer.h"

#include <json-glib/json-glib.h>

#include <string>

#include "camera/base/logging/xlog.h"
#include "proto/Protocol.h"

namespace camera {

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace {

// A discovery request is exactly a JSON object with "method": proto::methods::kDiscover;
// everything else (stray packets, port scans) is silently ignored.
bool is_discover(const char* data) {
    JsonParser* parser = json_parser_new();
    bool ok = false;
    if (json_parser_load_from_data(parser, data, -1, nullptr)) {
        JsonNode* root = json_parser_get_root(parser);
        if (root != nullptr && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject* obj = json_node_get_object(root);
            const char* method = json_object_get_string_member_with_default(
                obj, "method", nullptr);
            ok = method != nullptr && g_strcmp0(method, proto::methods::kDiscover) == 0;
        }
    }
    g_object_unref(parser);
    return ok;
}

std::string build_reply(const Config& cfg) {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "device");
    json_builder_add_string_value(b, "camera-streamer");
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, APP_VERSION);
    json_builder_set_member_name(b, "rtsp_port");
    json_builder_add_int_value(b, cfg.port);
    json_builder_set_member_name(b, "control_port");
    json_builder_add_int_value(b, cfg.control_port);
    json_builder_set_member_name(b, "cameras");
    json_builder_begin_array(b);
    for (int i = 0; i < Config::kNumCameras; ++i) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "index");
        json_builder_add_int_value(b, i);
        json_builder_set_member_name(b, "mount");
        json_builder_add_string_value(b, ("/cam" + std::to_string(i)).c_str());
        json_builder_set_member_name(b, "enabled");
        json_builder_add_boolean_value(b, cfg.cameras[i].enabled);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonNode* root = json_builder_get_root(b);
    JsonGenerator* gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar* data = json_generator_to_data(gen, nullptr);
    std::string out(data);
    g_free(data);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(b);
    return out;
}

}  // namespace

DiscoveryServer::DiscoveryServer(const Config& config) : config_(config) {}

DiscoveryServer::~DiscoveryServer() {
    if (source_ != nullptr) {
        g_source_destroy(source_);
        g_source_unref(source_);
    }
    if (socket_ != nullptr) {
        g_socket_close(socket_, nullptr);
        g_object_unref(socket_);
    }
}

camera::base::Expected<camera::base::Unit, std::string> DiscoveryServer::start() {
    GError* err = nullptr;
    socket_ = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                           G_SOCKET_PROTOCOL_UDP, &err);
    if (socket_ == nullptr) {
        std::string msg = std::string("discovery: socket: ") + err->message;
        g_error_free(err);
        return camera::base::makeUnexpected(std::move(msg));
    }

    GInetAddress* any = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
    GSocketAddress* addr =
        g_inet_socket_address_new(any, config_.discovery_port);
    g_object_unref(any);
    const gboolean ok = g_socket_bind(socket_, addr, TRUE, &err);
    g_object_unref(addr);
    if (!ok) {
        std::string msg = "discovery: bind 0.0.0.0:" +
                          std::to_string(config_.discovery_port) +
                          "/udp failed: " + err->message;
        g_error_free(err);
        return camera::base::makeUnexpected(std::move(msg));
    }

    source_ = g_socket_create_source(socket_, G_IO_IN, nullptr);
    g_source_set_callback(source_, G_SOURCE_FUNC(on_datagram), this, nullptr);
    g_source_attach(source_, nullptr);

    XLOGF(INFO, "discovery responder on 0.0.0.0:%d/udp", config_.discovery_port);
    return camera::base::unit;
}

gboolean DiscoveryServer::on_datagram(GSocket* socket, GIOCondition /*cond*/,
                                      gpointer user_data) {
    auto* self = static_cast<DiscoveryServer*>(user_data);

    char buf[1024];
    GSocketAddress* sender = nullptr;
    const gssize n = g_socket_receive_from(socket, &sender, buf,
                                           sizeof(buf) - 1, nullptr, nullptr);
    if (n > 0) {
        buf[n] = '\0';
        if (is_discover(buf)) {
            const std::string reply = build_reply(self->config_);
            g_socket_send_to(socket, sender, reply.data(), reply.size(),
                             nullptr, nullptr);
        }
    }
    if (sender != nullptr)
        g_object_unref(sender);
    return G_SOURCE_CONTINUE;
}

}  // namespace camera

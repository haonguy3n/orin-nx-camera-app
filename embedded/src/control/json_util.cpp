#include "control/json_util.h"

#include <string>

/// @name Typed parameter extraction
/// @{

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

/// @}

/// @name Result builders
/// @{

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

std::string node_to_string(JsonNode* node) {
    JsonGenerator* gen = json_generator_new();
    json_generator_set_root(gen, node);
    gchar* data = json_generator_to_data(gen, nullptr);
    std::string out(data);
    g_free(data);
    g_object_unref(gen);
    return out;
}

/// @}

/// @name Domain object serializers
/// @{

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

/// @}

// JSON utilities for the control protocol.
//
// Extracts the JSON parameter parsing, result building, and JSON-RPC
// envelope helpers from the original control_server.cpp's anonymous
// namespace into a reusable module. This is shared by all control
// handlers and the control server's request/response framing.
#pragma once

#include <json-glib/json-glib.h>

#include <string>

#include "config/config.h"
#include "v4l2/v4l2_device.h"

// JSON-RPC-flavored error codes (proto/PROTOCOL.md).
enum {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kUnknownMethod = -32601,
    kInvalidParams = -32602,
    kFailed = 1,
};

/// @name Typed parameter extraction from JsonObject*
/// @{

bool param_int(JsonObject* params, const char* name, int64_t* out);
bool param_bool(JsonObject* params, const char* name, bool* out);
bool param_double(JsonObject* params, const char* name, double* out);

// "control": name string or numeric id.
bool param_control(JsonObject* params, std::string* out);

// "camera": validates 0..kNumCameras-1.
bool param_camera(JsonObject* params, int* out);

/// @}

/// @name Result builders (return transfer-full JsonNode*)
/// @{

// Takes the root from a builder and unrefs the builder.
JsonNode* take_root(JsonBuilder* b);

// Empty result object: {}
JsonNode* empty_result();

// Serializes a JsonNode to a string.
std::string node_to_string(JsonNode* node);

/// @}

/// @name Domain object serializers (append to a JsonBuilder*)
/// @{

void add_camera_config(JsonBuilder* b, int index, const CameraConfig& cam);
void add_v4l2_control(JsonBuilder* b, const V4l2Control& c);

/// @}

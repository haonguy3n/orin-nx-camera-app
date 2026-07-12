// Thin V4L2 control access for the sensor device nodes (/dev/videoN).
// This is how exposure/gain/trigger reach the VC MIPI driver on the v4l2
// capture path — and the generic escape hatch for every other control the
// driver exposes (black level, IO/flash modes, ...). Controls can be set
// from a separate fd while the pipeline is streaming.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct V4l2Control {
    uint32_t id = 0;
    std::string name;
    uint32_t type = 0;      // raw V4L2_CTRL_TYPE_* value
    int64_t minimum = 0;
    int64_t maximum = 0;
    int64_t step = 0;
    int64_t default_value = 0;
    int64_t value = 0;      // current value (0 if write-only/non-scalar)
    uint32_t flags = 0;
};

// Every non-disabled scalar control the device exposes. Empty + |error| set
// on failure (device missing, not a V4L2 device, ...).
std::vector<V4l2Control> v4l2_list_controls(const std::string& device,
                                            std::string* error);

// |control| is a control name (matched case-insensitively, with space, '_'
// and '-' treated as equal — "trigger_mode" matches "Trigger Mode") or a
// numeric id (decimal or 0x-prefixed hex).
bool v4l2_get_control(const std::string& device, const std::string& control,
                      V4l2Control* out, std::string* error);
bool v4l2_set_control(const std::string& device, const std::string& control,
                      int64_t value, std::string* error);

// Sets the VC MIPI hardware trigger mode (0 = disabled .. 7 = stream level).
// Finds the driver's control by name ("trigger_mode" on the VC driver;
// falls back to the first non-button control containing "trigger").
bool v4l2_set_trigger_mode(const std::string& device, int mode,
                           std::string* error);

// Presses the VC driver's software "single trigger" button control (exposes
// one frame when the sensor is in a software-triggerable trigger mode).
bool v4l2_fire_single_trigger(const std::string& device, std::string* error);

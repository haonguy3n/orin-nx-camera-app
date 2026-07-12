#include "v4l2_ctrl.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

// Retry ioctls interrupted by signals (SIGHUP reload is routine here).
int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

// Lowercase, with space/'_'/'-' collapsed to one separator, so config and
// protocol values match driver names loosely ("trigger_mode" == "Trigger Mode").
std::string normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '-')
            c = ' ';
        out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Numeric control spec -> id. Returns 0 if |s| is not a number.
uint32_t parse_id(const std::string& s) {
    if (s.empty())
        return 0;
    char* end = nullptr;
    unsigned long v = strtoul(s.c_str(), &end, 0);  // handles decimal and 0x...
    if (end == nullptr || *end != '\0')
        return 0;
    return static_cast<uint32_t>(v);
}

bool is_scalar(uint32_t type) {
    switch (type) {
    case V4L2_CTRL_TYPE_INTEGER:
    case V4L2_CTRL_TYPE_BOOLEAN:
    case V4L2_CTRL_TYPE_MENU:
    case V4L2_CTRL_TYPE_INTEGER_MENU:
    case V4L2_CTRL_TYPE_BITMASK:
    case V4L2_CTRL_TYPE_BUTTON:
    case V4L2_CTRL_TYPE_INTEGER64:
        return true;
    default:
        return false;
    }
}

int open_device(const std::string& device, std::string* error) {
    int fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && error)
        *error = device + ": " + strerror(errno);
    return fd;
}

bool get_value(int fd, const V4l2Control& ctrl, int64_t* out) {
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;
    memset(&c, 0, sizeof(c));
    memset(&cs, 0, sizeof(cs));
    c.id = ctrl.id;
    cs.count = 1;
    cs.controls = &c;
    if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &cs) != 0)
        return false;
    *out = ctrl.type == V4L2_CTRL_TYPE_INTEGER64 ? c.value64 : c.value;
    return true;
}

// Enumerate scalar controls; if |match| is non-empty, stop at the first one
// whose normalized name or id matches.
std::vector<V4l2Control> enumerate(int fd, const std::string& match,
                                   bool* found) {
    const std::string want_name = normalize(match);
    const uint32_t want_id = parse_id(match);
    if (found)
        *found = false;

    std::vector<V4l2Control> out;
    struct v4l2_query_ext_ctrl q;
    memset(&q, 0, sizeof(q));
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    while (xioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        const uint32_t id = q.id;
        if (!(q.flags & V4L2_CTRL_FLAG_DISABLED) &&
            q.type != V4L2_CTRL_TYPE_CTRL_CLASS && is_scalar(q.type)) {
            V4l2Control c;
            c.id = id;
            c.name = q.name;
            c.type = q.type;
            c.minimum = q.minimum;
            c.maximum = q.maximum;
            c.step = q.step;
            c.default_value = q.default_value;
            c.flags = q.flags;
            if (!(q.flags & V4L2_CTRL_FLAG_WRITE_ONLY))
                get_value(fd, c, &c.value);

            const bool hit = !match.empty() &&
                             (id == want_id || normalize(c.name) == want_name);
            out.push_back(std::move(c));
            if (hit) {
                if (found)
                    *found = true;
                break;
            }
        }
        q.id = id | V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
    return out;
}

bool find_control(int fd, const std::string& device, const std::string& control,
                  V4l2Control* out, std::string* error) {
    bool found = false;
    std::vector<V4l2Control> ctrls = enumerate(fd, control, &found);
    if (!found) {
        if (error)
            *error = device + ": no control matching '" + control + "'";
        return false;
    }
    *out = ctrls.back();
    return true;
}

}  // namespace

std::vector<V4l2Control> v4l2_list_controls(const std::string& device,
                                            std::string* error) {
    int fd = open_device(device, error);
    if (fd < 0)
        return {};
    std::vector<V4l2Control> out = enumerate(fd, "", nullptr);
    close(fd);
    if (out.empty() && error)
        *error = device + ": no controls";
    return out;
}

bool v4l2_get_control(const std::string& device, const std::string& control,
                      V4l2Control* out, std::string* error) {
    int fd = open_device(device, error);
    if (fd < 0)
        return false;
    bool ok = find_control(fd, device, control, out, error);
    close(fd);
    return ok;
}

bool v4l2_set_trigger_mode(const std::string& device, int mode,
                           std::string* error) {
    std::vector<V4l2Control> ctrls = v4l2_list_controls(device, error);
    const V4l2Control* found = nullptr;
    for (const V4l2Control& c : ctrls) {
        const std::string name = normalize(c.name);
        if (name == "trigger mode") {
            found = &c;
            break;
        }
        if (!found && c.type != V4L2_CTRL_TYPE_BUTTON &&
            name.find("trigger") != std::string::npos)
            found = &c;
    }
    if (found == nullptr) {
        if (error)
            *error = device + ": no trigger control (not a VC MIPI sensor?)";
        return false;
    }
    return v4l2_set_control(device, std::to_string(found->id), mode, error);
}

bool v4l2_fire_single_trigger(const std::string& device, std::string* error) {
    std::vector<V4l2Control> ctrls = v4l2_list_controls(device, error);
    for (const V4l2Control& c : ctrls) {
        const std::string name = normalize(c.name);
        if (name.find("single") != std::string::npos &&
            name.find("trigger") != std::string::npos)
            return v4l2_set_control(device, std::to_string(c.id), 1, error);
    }
    if (error)
        *error = device + ": no single-trigger control (not a VC MIPI sensor?)";
    return false;
}

bool v4l2_set_control(const std::string& device, const std::string& control,
                      int64_t value, std::string* error) {
    int fd = open_device(device, error);
    if (fd < 0)
        return false;

    V4l2Control ctrl;
    if (!find_control(fd, device, control, &ctrl, error)) {
        close(fd);
        return false;
    }

    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;
    memset(&c, 0, sizeof(c));
    memset(&cs, 0, sizeof(cs));
    c.id = ctrl.id;
    if (ctrl.type == V4L2_CTRL_TYPE_INTEGER64)
        c.value64 = value;
    else
        c.value = static_cast<int32_t>(value);
    cs.count = 1;
    cs.controls = &c;

    bool ok = xioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) == 0;
    if (!ok && error)
        *error = device + ": set '" + ctrl.name + "' = " +
                 std::to_string(value) + ": " + strerror(errno);
    close(fd);
    return ok;
}

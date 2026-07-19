#include "camera/lib/v4l2/V4l2Device.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "camera/base/File.h"

namespace camera {

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

camera::base::Expected<V4l2Control, std::string> find_control(
    int fd, const std::string& device, const std::string& control) {
    bool found = false;
    std::vector<V4l2Control> ctrls = enumerate(fd, control, &found);
    if (!found)
        return camera::base::makeUnexpected(device + ": no control matching '" +
                                     control + "'");
    return ctrls.back();
}

}  // namespace

camera::base::Expected<std::vector<V4l2Control>, std::string>
V4l2Device::list_controls() {
    auto fd = open_device();
    if (!fd)
        return camera::base::makeUnexpected(std::move(fd.error()));
    std::vector<V4l2Control> out = enumerate(fd->fd(), "", nullptr);
    if (out.empty())
        return camera::base::makeUnexpected(device_path_ + ": no controls");
    return out;
}

camera::base::Expected<V4l2Control, std::string> V4l2Device::get_control(
    const std::string& control) {
    auto fd = open_device();
    if (!fd)
        return camera::base::makeUnexpected(std::move(fd.error()));
    return find_control(fd->fd(), device_path_, control);
}

camera::base::Expected<camera::base::Unit, std::string> V4l2Device::set_control(
    const std::string& control, int64_t value) {
    auto fd = open_device();
    if (!fd)
        return camera::base::makeUnexpected(std::move(fd.error()));

    auto ctrl = find_control(fd->fd(), device_path_, control);
        if (!ctrl)
            return camera::base::makeUnexpected(std::move(ctrl.error()));

        struct v4l2_ext_control c;
        struct v4l2_ext_controls cs;
        memset(&c, 0, sizeof(c));
        memset(&cs, 0, sizeof(cs));
        c.id = ctrl->id;
        if (ctrl->type == V4L2_CTRL_TYPE_INTEGER64)
            c.value64 = value;
        else
            c.value = static_cast<int32_t>(value);
        cs.count = 1;
        cs.controls = &c;

    if (xioctl(fd->fd(), VIDIOC_S_EXT_CTRLS, &cs) != 0)
        return camera::base::makeUnexpected(
            device_path_ + ": set '" + ctrl->name + "' = " +
            std::to_string(value) + ": " + strerror(errno));
    return camera::base::unit;
}

camera::base::Expected<camera::base::Unit, std::string>
V4l2Device::set_trigger_mode(int mode) {
    auto ctrls = list_controls();
    if (!ctrls)
        return camera::base::makeUnexpected(std::move(ctrls.error()));
    const V4l2Control* found = nullptr;
    for (const V4l2Control& c : *ctrls) {
        const std::string name = normalize(c.name);
        if (name == "trigger mode") {
            found = &c;
            break;
        }
        if (!found && c.type != V4L2_CTRL_TYPE_BUTTON &&
            name.find("trigger") != std::string::npos)
            found = &c;
    }
    if (found == nullptr)
        return camera::base::makeUnexpected(
            device_path_ + ": no trigger control (not a VC MIPI sensor?)");
    return set_control(std::to_string(found->id), mode);
}

camera::base::Expected<camera::base::Unit, std::string>
V4l2Device::fire_single_trigger() {
    auto ctrls = list_controls();
    if (!ctrls)
        return camera::base::makeUnexpected(std::move(ctrls.error()));
    for (const V4l2Control& c : *ctrls) {
        const std::string name = normalize(c.name);
        if (name.find("single") != std::string::npos &&
            name.find("trigger") != std::string::npos)
            return set_control(std::to_string(c.id), 1);
    }
    return camera::base::makeUnexpected(
        device_path_ +
        ": no single-trigger control (not a VC MIPI sensor?)");
}

camera::base::Expected<camera::base::File, std::string>
V4l2Device::open_device() const {
    camera::base::File fd(
        ::open(device_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC),
        /*ownsFd=*/true);
    if (!fd)
        return camera::base::makeUnexpected(device_path_ + ": " +
                                     strerror(errno));
    return fd;
}

}  // namespace camera

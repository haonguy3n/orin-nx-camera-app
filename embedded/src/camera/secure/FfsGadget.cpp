#include "camera/secure/FfsGadget.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "camera/base/logging/xlog.h"

namespace camera::secure {
namespace {

constexpr char kFfs[] = "/dev/ffs-secure";
constexpr char kGadget[] = "/sys/kernel/config/usb_gadget/vc-camera";

// Configfs/ep0 writes can report EAGAIN while a gadget drains an earlier
// request; wait for writability rather than failing. A failed descriptor
// write is never retried by the host, which would just wait out its timeout.
bool write_all(int fd, const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (length != 0) {
        const ssize_t n = write(fd, bytes, length);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd writable{fd, POLLOUT, 0};
                if (poll(&writable, 1, 1000) <= 0)
                    return false;
                continue;
            }
            return false;
        }
        if (n == 0)
            return false;
        bytes += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

bool write_file(const std::string& path, const std::string& value) {
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = write_all(fd, value.data(), value.size());
    close(fd);
    return ok;
}

std::string first_udc() {
    DIR* raw = opendir("/sys/class/udc");
    if (raw == nullptr)
        return {};
    std::unique_ptr<DIR, decltype(&closedir)> directory(raw, closedir);
    while (dirent* entry = readdir(raw)) {
        if (entry->d_name[0] != '.')
            return entry->d_name;
    }
    return {};
}

// The FunctionFS descriptor blob: one vendor interface (FF/53/55) with a
// bulk IN/OUT pair, in both full- and high-speed variants.
std::vector<uint8_t> descriptors() {
    const std::array<uint8_t, 9> interface = {9, 4, 0, 0, 2, 0xff, 0x53, 0x55, 1};
    auto endpoint = [](uint8_t address, uint16_t packet) {
        return std::array<uint8_t, 7>{7, 5, address, 2,
            static_cast<uint8_t>(packet), static_cast<uint8_t>(packet >> 8), 0};
    };
    std::vector<uint8_t> body;
    for (uint16_t packet : {uint16_t(64), uint16_t(512)}) {
        const auto in = endpoint(0x81, packet), out = endpoint(0x02, packet);
        body.insert(body.end(), interface.begin(), interface.end());
        body.insert(body.end(), in.begin(), in.end());
        body.insert(body.end(), out.begin(), out.end());
    }
    auto append32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i != 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    std::vector<uint8_t> result;
    append32(result, FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    append32(result, 20 + body.size());
    append32(result, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    append32(result, 3); append32(result, 3);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

// Publish the ffs.secure configfs function and mount it. usb-gadget.service
// creates and binds the recovery NCM/ACM gadget first; FunctionFS cannot be
// linked into a bound gadget, so this unbinds the UDC, adds the function, and
// leaves the caller to republish descriptors and rebind.
bool prepare_functionfs(std::string* error) {
    const std::string udc_path = std::string(kGadget) + "/UDC";
    if (!write_file(udc_path, "\n")) {
        // ENODEV: already unbound, exactly the state a previous instance that
        // died owning the function leaves. Treating it as fatal made secure
        // USB unrecoverable until reboot.
        if (errno != ENODEV) {
            *error = std::string("unbind USB gadget: ") + strerror(errno);
            return false;
        }
    }
    // A previous owner may have left a FunctionFS superblock mounted; it holds
    // the ffs.* instance and makes configfs report EBUSY when creating it.
    if (umount(kFfs) != 0 && errno != EINVAL && errno != ENOENT && errno != EPERM) {
        *error = std::string("unmount stale FunctionFS: ") + strerror(errno);
        return false;
    }
    const std::string function = std::string(kGadget) + "/functions/ffs.secure";
    if (mkdir(kFfs, 0755) != 0 && errno != EEXIST) {
        *error = std::string("mkdir ") + kFfs + ": " + strerror(errno);
        return false;
    }
    if (mkdir(function.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = std::string("create FunctionFS function ") + function + ": " + strerror(errno)
            + (errno == ENOENT
                   ? " (kernel registers no \"ffs\" configfs function type -- "
                     "CONFIG_USB_CONFIGFS_F_FS is not enabled in this kernel)"
                   : "");
        return false;
    }
    if (mount("secure", kFfs, "functionfs", 0, nullptr) != 0) {
        *error = std::string("mount FunctionFS: ") + strerror(errno);
        return false;
    }
    const std::string link = std::string(kGadget) + "/configs/c.1/ffs.secure";
    if (symlink(function.c_str(), link.c_str()) != 0 && errno != EEXIST) {
        *error = std::string("link FunctionFS function: ") + strerror(errno);
        return false;
    }
    return true;
}

}  // namespace

base::Expected<std::unique_ptr<FfsGadget>, std::string> FfsGadget::create() {
    const std::string base_udc = first_udc();
    auto restore = [&] {
        if (!base_udc.empty())
            write_file(std::string(kGadget) + "/UDC", base_udc);
    };

    std::string setup_error;
    if (!prepare_functionfs(&setup_error)) {
        restore();
        return base::makeUnexpected(std::move(setup_error));
    }
    const int ep0 = open("/dev/ffs-secure/ep0", O_RDWR | O_NONBLOCK);
    if (ep0 < 0) {
        auto err = std::string("open ep0: ") + strerror(errno);
        restore();
        return base::makeUnexpected(std::move(err));
    }
    const auto desc = descriptors();
    const uint8_t strings[] = {2,0,0,0,25,0,0,0,1,0,0,0,1,0,0,0,9,4,'s','e','c','u','r','e',0};
    if (!write_all(ep0, desc.data(), desc.size()) || !write_all(ep0, strings, sizeof(strings))) {
        close(ep0);
        restore();
        return base::makeUnexpected(std::string("publishing descriptors failed"));
    }
    const std::string udc = base_udc.empty() ? first_udc() : base_udc;
    if (udc.empty() || !write_file(std::string(kGadget) + "/UDC", udc)) {
        close(ep0);
        restore();
        return base::makeUnexpected(std::string("rebinding UDC failed"));
    }
    return std::unique_ptr<FfsGadget>(new FfsGadget(ep0));
}

FfsGadget::~FfsGadget() {
    // Closing ep0 removes the FunctionFS function, which tears the interface
    // down. The UDC stays bound to the base gadget for usb-gadget.service to
    // manage, matching the pre-refactor teardown.
    if (ep0_ >= 0)
        close(ep0_);
}

}  // namespace camera::secure

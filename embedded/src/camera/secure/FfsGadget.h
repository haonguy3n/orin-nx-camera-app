#pragma once

#include <memory>
#include <string>

#include "camera/base/Expected.h"

namespace camera::secure {

// RAII owner of the FunctionFS vendor-bulk function.
//
// create() does everything that must be true before a host can enumerate the
// secure interface: publish the ffs.secure configfs function, mount it, open
// ep0, write the interface/endpoint descriptors, and bind the UDC. ep0 stays
// open for the object's lifetime -- FunctionFS removes the function when ep0
// closes, so the destructor closing it is what tears the interface down.
//
// This is the sole owner of that lifecycle: the configfs/UDC dance and its
// many failure modes (EBUSY, ENODEV, stale mounts) live here, not scattered
// through the session loop.
class FfsGadget {
public:
    [[nodiscard]] static base::Expected<std::unique_ptr<FfsGadget>, std::string>
    create();

    // Call when this process will NOT own FunctionFS (transports=network, or
    // secure USB failed to start). Removes a stale ffs.secure left by an
    // earlier run in this boot and rebinds the UDC, so the NCM/ACM gadget --
    // and with it the 192.168.55.x link the host reaches the device on --
    // comes back. A no-op when nothing of ours is in the config, so it never
    // disturbs a healthy gadget.
    static void release_base_gadget();
    ~FfsGadget();

    FfsGadget(const FfsGadget&) = delete;
    FfsGadget& operator=(const FfsGadget&) = delete;

    // The FunctionFS control endpoint; the session reads its bind/enable
    // events. Data endpoints (ep1/ep2) are opened per session, not here.
    [[nodiscard]] int control_endpoint() const { return ep0_; }

private:
    explicit FfsGadget(int ep0) : ep0_(ep0) {}
    int ep0_;
};

}  // namespace camera::secure

#include "camera/control/handlers/SystemHandlers.h"

#include <glib.h>

#include <unistd.h>
#include <sys/reboot.h>

#include "camera/control/JsonUtil.h"

#include "camera/folly/logging/xlog.h"

namespace camera {

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

HandlerResult PingHandler::handle(JsonObject* /*params*/, ControlContext& /*ctx*/) {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pong");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, APP_VERSION);
    json_builder_end_object(b);
    return take_root(b);
}

HandlerResult ReloadHandler::handle(JsonObject* /*params*/, ControlContext& ctx) {
    // Reply first, restart after: the reload replaces this very server,
    // so run it from an idle callback that owns its own copy of the hook.
    auto* fn = new std::function<void()>(ctx.reload);
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

HandlerResult RebootHandler::handle(JsonObject* /*params*/, ControlContext& /*ctx*/) {
    // Reply first, then reboot after a short delay so the response can be
    // sent and the control connection can flush. Use g_timeout_add so the
    // main loop runs the reboot after the response is written.
    g_timeout_add_seconds(2, [](gpointer) -> gboolean {
        XLOGF(INFO, "reboot: requested via control channel, rebooting now");
        sync();  // flush filesystem buffers before reboot
        execlp("systemctl", "systemctl", "reboot", nullptr);
        // If systemctl is not available, try the direct syscall
        execlp("reboot", "reboot", nullptr);
        // Last resort: reboot(2) syscall
        reboot(RB_AUTOBOOT);
        _exit(1);
        return G_SOURCE_REMOVE;
    }, nullptr);
    return empty_result();
}

}  // namespace camera

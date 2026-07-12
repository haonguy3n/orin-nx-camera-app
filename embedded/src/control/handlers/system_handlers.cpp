#include "control/handlers/system_handlers.h"

#include <glib.h>

#include "control/json_util.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

JsonNode* PingHandler::handle(JsonObject* /*params*/, ControlContext& /*ctx*/,
                              int* /*err_code*/, std::string* /*err_msg*/) {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "pong");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, APP_VERSION);
    json_builder_end_object(b);
    return take_root(b);
}

JsonNode* ReloadHandler::handle(JsonObject* /*params*/, ControlContext& ctx,
                                int* /*err_code*/, std::string* /*err_msg*/) {
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

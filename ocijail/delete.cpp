#include <signal.h>
#include <unistd.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "delete.h"
#include "hook.h"
#include "jail.h"
#include "mount.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void delete_::init(main_app& app) {
    static delete_ instance{app};
}

delete_::delete_(main_app& app) : app_(app) {
    auto sub =
        app.add_subcommand("delete", "Delete the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_flag("--force", force_, "Delete even if running");
    sub->final_callback([this] { run(); });
}

void delete_::run() {
    auto state = app_.get_runtime_state(id_);

    // If some other process has already deleted the state, just return.
    if (!state.exists()) {
        return;
    }

    auto lk = state.lock();
    state.load();

    // update state
    if (::kill(state["pid"], 0) < 0) {
        state["status"] = "stopped";
    }

    if (state["status"] != "stopped") {
        throw std::runtime_error("start: container not in 'stopped' state");
    }

    auto j = jail::find(int(state["jid"]));
    j.remove();

    if (state["config"].contains("mounts") && !state["config"]["mounts"].is_null()) {
        unmount_volumes(state, state["root_path"], state["config"]["mounts"]);
    }

    hook::run_hooks(app_, state["config"]["hooks"], "poststop", state);

    state.remove_all();
}

}  // namespace ocijail

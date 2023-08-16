#include <signal.h>
#include <sys/mount.h>
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

    // The specification limits delete to just containers in "stopped" state. In
    // practice, both runc and crun relax this requirement:
    //
    // - If the container is "stopped" then just delete it.
    // - If the container is "created", send it a KILL signal and delete it.
    // - If the container is "running" (or for crun, any other state) and the
    //   force flag is set, send it a KILL signal and delete it.
    //
    // We follow the more restrictive crun behaviour.
    if (state["status"] == "stopped") {
        // Nothing to do here
    } else if (state["status"] == "created") {
        ::kill(state["pid"], SIGKILL);
    } else if (state["status"] == "running" && force_) {
        ::kill(state["pid"], SIGKILL);
    } else {
        std::stringstream ss;
        ss << "delete: container not in \"stopped\" or \"created\" state "
              "(currently "
           << state["status"] << ")";
        throw std::runtime_error(ss.str());
    }

    auto j = jail::find(int(state["jid"]));
    j.remove();

    bool root_readonly = false;
    if (state.contains("root_readonly")) {
        root_readonly = state["root_readonly"];
    }
    fs::path root_path = state["root_path"];
    if (root_readonly) {
        root_path = fs::path{state["readonly_root_path"]};
    }
    if (state["config"].contains("mounts") &&
        !state["config"]["mounts"].is_null()) {
        unmount_volumes(app_, state, root_path, state["config"]["mounts"]);
    }
    if (root_readonly) {
        if (::unmount(root_path.c_str(), MNT_FORCE) > 0) {
            throw std::system_error{errno,
                                    std::system_category(),
                                    "unmounting " + root_path.native()};
        }
    }

    hook::run_hooks(app_, state["config"]["hooks"], "poststop", state);

    state.remove_all();
}

}  // namespace ocijail

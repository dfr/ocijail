#include <signal.h>
#include <unistd.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "delete.h"
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
    auto state_dir = app_.state_db_ / id_;
    auto state_path = state_dir / "state.json";

    if (!fs::is_directory(state_dir)) {
        throw std::runtime_error("start: container " + id_ + " not found");
    }

    json state;
    std::ifstream{state_path} >> state;

    // update state
    if (::kill(state["pid"], 0) < 0) {
        state["status"] = "stopped";
    }

    if (state["status"] != "stopped") {
        throw std::runtime_error("start: container not in 'stopped' state");
    }

    auto j = jail::find(int(state["jid"]));
    j.remove();

    if (state["config"].contains("mounts")) {
        unmount_volumes(state, state["root_path"], state["config"]["mounts"]);
    }

    fs::remove_all(state_dir);
}

}  // namespace ocijail

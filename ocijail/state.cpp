#include <signal.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "state.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void state::init(main_app& app) {
    static state instance{app};
}

state::state(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "state", "Get the state of the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->final_callback([this] { run(); });
}

void state::run() {
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

    json res;
    res["ociVersion"] = "1.0.2";
    res["id"] = id_;
    res["status"] = state["status"];
    if (state["status"] != "stopped") {
        res["pid"] = state["pid"];
    }
    res["bundle"] = state["bundle"];
    if (state["config"].contains("annotations")) {
        res["annotations"] = state["config"]["annotations"];
    }
    std::cout << res;
}

}  // namespace ocijail

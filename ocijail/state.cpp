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
    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();

    // update state
    if (::kill(state["pid"], 0) < 0) {
        state["status"] = "stopped";
        state.save();
    }

    std::cout << state.report();
}

}  // namespace ocijail

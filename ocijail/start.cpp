#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include "CLI/CLI.hpp"
#include "nlohmann/json.hpp"

#include "hook.h"
#include "start.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void start::init(main_app& app) {
    static start instance{app};
}

start::start(main_app& app) : app_(app) {
    auto sub =
        app.add_subcommand("start", "Start the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->final_callback([this] { run(); });
}

void start::run() {
    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();

    if (state["status"] != "created") {
        std::stringstream ss;
        ss << "start: container not in \"created\" state (currently "
           << state["status"] << ")";
        throw std::runtime_error(ss.str());
    }
    state["status"] = "running";
    state.save();

    auto& config_hooks = state["config"]["hooks"];
    hook::run_hooks(app_, config_hooks, "prestart", state);

    auto start_wait = state.get_state_dir() / "start_wait";
    auto fd = ::open(start_wait.c_str(), O_RDWR);
    char ch = 0;
    if (fd < 0) {
        throw std::system_error{
            errno, std::system_category(), "open start fifo"};
    }
    auto n = ::write(fd, &ch, 1);
    if (n < 0) {
        throw std::system_error{
            errno, std::system_category(), "write to start fifo"};
    }
    ::close(fd);

    // Somehow sync with executing the container process before
    // running poststart hooks?
    hook::run_hooks(app_, config_hooks, "poststart", state);
}

}  // namespace ocijail

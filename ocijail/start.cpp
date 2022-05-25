#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include "CLI/CLI.hpp"
#include "nlohmann/json.hpp"

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
    state.load();

    if (state["status"] != "created") {
        throw std::runtime_error("start: container not in 'created' state");
    }
    state["status"] = "running";
    state.save();

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
}

}  // namespace ocijail

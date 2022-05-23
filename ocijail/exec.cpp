#include <signal.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "exec.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void exec::init(main_app& app) {
    static exec instance{app};
}

exec::exec(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "exec", "Execute a command in the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the exec")
        ->required();
    sub->add_option(
           "--process", process_, "Path to a file containing the process json")
        ->required()
        ->check(CLI::ExistingPath);
    sub->add_option(
           "--console-socket",
           console_socket_,
           "Path to a socket which will receive the console pty descriptor")
        ->check(CLI::ExistingPath);
    sub->add_option(
        "--pid-file",
        pid_file_,
        "Path to a file where the container process id will be written");
    sub->add_flag("--detach,-d",
                  detach_,
                  "Detach the command and execute in the background");
    sub->final_callback([this] { run(); });
}

void exec::run() {
    auto state_dir = app_.state_db_ / id_;
    auto state_path = state_dir / "state.json";

    if (!fs::is_directory(state_dir)) {
        throw std::runtime_error("exec: container " + id_ + " not found");
    }

    json state;
    std::ifstream{state_path} >> state;

    json process;
    std::ifstream{process_} >> process;
    std::cerr << process << "\n";
}

}  // namespace ocijail

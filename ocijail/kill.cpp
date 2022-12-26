#include <signal.h>
#include <charconv>
#include <iostream>

#include "nlohmann/json.hpp"

#include "kill.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void kill::init(main_app& app) {
    static kill instance{app};
}

kill::kill(main_app& app) : app_(app) {
    auto sub = app.add_subcommand("kill", "Send a signal to a container");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_option("signal", signame_, "Signal to send, defaults to TERM");
    auto all_opt = sub->add_flag(
        "--all,-a", all_, "Send the signal to all processes in the container");
    auto pid_opt = sub->add_option(
        "--pid,-p", pid_, "Send the signal to the given process");
    all_opt->excludes(pid_opt);
    sub->final_callback([this] { run(); });
}

void kill::run() {
    int signum = 0;
    if (signame_) {
        // This can be either the signal number or its name. Try the
        // number first.
        size_t len = 0;
        try {
            signum = std::stoi(*signame_, &len, 10);
        } catch (...) {
            // if we get an exception, try matching it as a signal
            // name.
            len = 0;
        }
        if (len != signame_->size()) {
            for (int i = 0; i < sys_nsig; i++) {
                if (sys_signame[i] && *signame_ == sys_signame[i]) {
                    signum = i;
                    break;
                }
            }
        }
        if (signum == 0) {
            throw std::runtime_error("Unknown signal name " + *signame_);
        }
    } else {
        signum = SIGTERM;
    }

    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();

    if (state["status"] == "created" || state["status"] == "running") {
        if (::kill(state["pid"], signum) < 0) {
            throw std::system_error(
                errno,
                std::system_category(),
                "sending signal to pid " + state["pid"].get<std::string>());
        }
    }
}

}  // namespace ocijail

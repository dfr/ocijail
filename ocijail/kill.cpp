#include <signal.h>
#include <charconv>
#include <iostream>
#include <set>

#include "nlohmann/json.hpp"

#include "jail.h"
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

    if (all_) {
        // Signal every process in the container's jail, not just the
        // container process. This works in any state since processes may
        // outlive the container process. Keep sweeping until we see no
        // new processes so that children forked while we were signalling
        // are not missed.
        auto j = jail::find(int(state["jid"]));
        std::set<pid_t> signalled;
        bool found = true;
        while (found) {
            found = false;
            for (auto pid : j.processes()) {
                if (!signalled.insert(pid).second) {
                    continue;
                }
                found = true;
                if (::kill(pid, signum) < 0 && errno != ESRCH) {
                    throw std::system_error(
                        errno,
                        std::system_category(),
                        "sending signal to pid " + std::to_string(pid));
                }
            }
        }
        return;
    }

    if (state["status"] == "created" || state["status"] == "running") {
        if (::kill(state["pid"], signum) < 0 && errno != ESRCH) {
            throw std::system_error(
                errno,
                std::system_category(),
                "sending signal to pid " +
                    std::to_string(state["pid"].get<int>()));
        }
    }
}

}  // namespace ocijail

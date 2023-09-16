#include <signal.h>
#include <charconv>
#include <iostream>

#include "nlohmann/json.hpp"

#include "list.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void list::init(main_app& app) {
    static list instance{app};
}

list::list(main_app& app) : app_(app) {
    std::map<std::string, list_format> formats{
        {"table", list_format::LIST_TABLE},
        {"json", list_format::LIST_JSON},
    };

    auto sub = app.add_subcommand("list", "List containers");
    sub->add_option("--quiet,-q", quiet_, "show only IDs");
    sub->add_option("--format,-f",
                    format_,
                    "output format: either table or json (default: table)")
        ->transform(CLI::CheckedTransformer(formats, CLI::ignore_case));
    sub->final_callback([this] { run(); });
}

void list::run() {
    std::map<std::string, runtime_state> states;
    int max_id_width = 1;

    for (const auto& it : fs::directory_iterator{app_.get_state_db()}) {
        auto id = it.path().filename().native();
        if (id.size() > max_id_width) {
            max_id_width = id.size();
        }
        auto state = app_.get_runtime_state(id);
        if (state.exists()) {
            auto lk = state.lock();
            state.load();
            state.check_status();
            if (state["status"] == "stopped") {
                state["pid"] = 0;
            }
            states.emplace(id, state);
        }
    }

    if (format_ == list_format::LIST_TABLE) {
        std::cout << std::left << std::setw(max_id_width) << "ID"
                  << " " << std::setw(10) << "PID"
                  << " " << std::setw(8) << "STATUS"
                  << " " << std::setw(40) << "BUNDLE"
                  << "\n";
        for (const auto& [id, state] : states) {
            std::cout << std::left << std::setw(max_id_width) << id << " "
                      << std::setw(10) << state["pid"].get<int>() << " "
                      << std::setw(8) << state["status"].get<std::string>()
                      << " " << std::setw(40)
                      << state["bundle"].get<std::string>() << "\n";
        }
    } else {
        json res;
        for (const auto& [id, state] : states) {
            json entry;
            entry["id"] = id;
            entry["pid"] = state["pid"];
            entry["status"] = state["status"];
            entry["bundle"] = state["bundle"];
            res.push_back(entry);
        }
        std::cout << res;
    }
}

}  // namespace ocijail

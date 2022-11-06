#pragma once

#include "nlohmann/json.hpp"

#include "ocijail/main.h"

namespace ocijail {

class main_app;

struct hook {
    // initialise with a json describing the hook from the config
    hook(const nlohmann::json& hook_config);

    // Given a hooks object, validate that each hook in the given
    // phase is well-formed
    static void validate_hooks(main_app& app,
                               const nlohmann::json& hooks,
                               const char* phase);

    // Run all the hooks for a phase
    static void run_hooks(main_app& app,
                          const nlohmann::json& hooks,
                          const char* phase,
                          const runtime_state& state);

    // Run this hook
    int run(main_app& app, const runtime_state& state);

   private:
    // Copied out from the json during parsing
    std::string path_;
    std::optional<std::vector<std::string>> args_;
    std::optional<std::vector<std::string>> env_;
    std::optional<int> timeout_;
};

}  // namespace ocijail

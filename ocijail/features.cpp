#include <signal.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "features.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void features::init(main_app& app) {
    static features instance{app};
}

features::features(main_app& app) {
    auto sub = app.add_subcommand("features",
                                  "Get the enabled feature set of the runtime");
    sub->final_callback([this] { run(); });
}

void features::run() {
    json features;
    static const char* hooks[] = {"prestart",
                                  "createRuntime",
                                  "createContainer",
                                  "startContainer",
                                  "poststart",
                                  "poststop"};
    static const char* mountOptions[] = {
        // Feature options
        "async",
        "atime",
        "exec",
        "suid",
        "symfollow",
        "rdonly",
        "sync",
        "union",
        "userquota",
        "groupquota",
        "clusterr",
        "clusterw",
        "suiddir",
        "snapshot",
        "multilabel",
        "acls",
        "nfsv4acls",
        "automounted",
        "untrusted",

        // Pseudo options
        "tmpcopyup",  // copy image data into a tmpfs
        "rule",       // apply a devfs rule

        // Control options
        "force",
        "update",
        "ro",
        "rw",
        "cover",
        "emptydir",

        // Ignored options
        "private",
        "rprivate",
        "rbind",
        "nodev",
        "bind",
    };

    features["ociVersionMin"] = "1.0.0";
    features["ociVersionMax"] = "1.2.0";
    for (auto hook : hooks) {
        features["hooks"].push_back(hook);
    }
    for (auto opt : mountOptions) {
        features["mountOptions"].push_back(opt);
    }

    std::cout << features;
}

}  // namespace ocijail

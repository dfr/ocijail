#pragma once

#include <filesystem>
#include <optional>

#include "nlohmann/json.hpp"

#include "ocijail/main.h"
#include "ocijail/process.h"

namespace ocijail {

struct create {
    static void init(main_app& app);

   private:
    create(main_app& app);

    void run();

    main_app& app_;
    std::filesystem::path bundle_path_{"."};
    std::string id_;
    std::optional<std::filesystem::path> console_socket_;
    std::optional<std::filesystem::path> pid_file_;
    int preserve_fds_{0};
};

}  // namespace ocijail

#pragma once

#include <filesystem>
#include <optional>

#include "main.h"
#include "nlohmann/json.hpp"

namespace ocijail {

struct create {
    static void init(main_app& app);

   private:
    create(main_app& app);

    void malformed_config(std::string_view message);
    void run();
    void reset_signals();
    void set_uid_gid(const nlohmann::json& user);

    main_app& app_;
    std::filesystem::path bundle_path_{"."};
    std::string id_;
    std::optional<std::filesystem::path> console_socket_;
    std::optional<std::filesystem::path> pid_file_;
};

}  // namespace ocijail

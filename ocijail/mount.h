#pragma once

#include <filesystem>

#include "nlohmann/json.hpp"

namespace ocijail {

class runtime_state;

int do_mount(
    const std::vector<std::tuple<std::string, std::string>>& mount_opts,
    int mount_flags);

void mount_volumes(main_app& app,
                   runtime_state& state,
                   const std::filesystem::path& root_path,
                   bool root_read_only,
                   const nlohmann::json& mounts);

void unmount_volumes(main_app& app,
                     runtime_state& state,
                     const std::filesystem::path& root_path,
                     const nlohmann::json& mounts);

}  // namespace ocijail

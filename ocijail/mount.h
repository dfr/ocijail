#pragma once

#include <filesystem>

#include "nlohmann/json.hpp"

namespace ocijail {

class runtime_state;

void mount_volumes(runtime_state& state,
                   const std::filesystem::path& root_path,
                   const nlohmann::json& mounts);

void unmount_volumes(runtime_state& state,
                     const std::filesystem::path& root_path,
                     const nlohmann::json& mounts);

}  // namespace ocijail

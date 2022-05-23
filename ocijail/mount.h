#pragma once

#include <filesystem>

#include "nlohmann/json.hpp"

namespace ocijail {

void mount_volumes(nlohmann::json& state,
                   const std::filesystem::path& root_path,
                   const nlohmann::json& mounts);

void unmount_volumes(nlohmann::json& state,
                     const std::filesystem::path& root_path,
                     const nlohmann::json& mounts);

}  // namespace ocijail

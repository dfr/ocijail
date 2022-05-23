#pragma once

#include <filesystem>

namespace ocijail {

std::tuple<int, int> open_pty();
void send_pty_control_fd(std::filesystem::path socket_name, int control_fd);

}  // namespace ocijail

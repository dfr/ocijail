#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct exec {
    static void init(main_app& app);

   private:
    exec(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
    std::filesystem::path process_;
    std::optional<std::filesystem::path> console_socket_;
    std::optional<std::filesystem::path> pid_file_;
    std::optional<bool> tty_;
    bool detach_{false};
};

}  // namespace ocijail

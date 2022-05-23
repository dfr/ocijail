#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct kill {
    static void init(main_app& app);

   private:
    kill(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
    std::optional<std::string> signame_;
    std::optional<int> pid_;
    bool all_;
};

}  // namespace ocijail

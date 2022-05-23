#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct state {
    static void init(main_app& app);

   private:
    state(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
};

}  // namespace ocijail

#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct features {
    static void init(main_app& app);

   private:
    features(main_app& app);
    void run();
};

}  // namespace ocijail

#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

enum list_format {
    LIST_TABLE,
    LIST_JSON,
};

struct list {
    static void init(main_app& app);

   private:
    list(main_app& app);
    void run();

    main_app& app_;
    bool quiet_{false};
    list_format format_{LIST_TABLE};
};

}  // namespace ocijail

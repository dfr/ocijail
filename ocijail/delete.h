#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct delete_ {
    static void init(main_app& app);

   private:
    delete_(main_app& app);
    void run();

    main_app& app_;
    bool force_{false};
    std::string id_;
};

}  // namespace ocijail

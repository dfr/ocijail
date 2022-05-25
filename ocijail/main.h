#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>

#include "CLI/CLI.hpp"

namespace ocijail {

enum class log_format {
    TEXT,
    JSON,
};

enum class test_mode {
    NONE,        // not testing
    VALIDATION,  // test config validation
};

struct main_app : public CLI::App {
    main_app(const std::string& title) : CLI::App(title) {}
    void log_error(const std::system_error& e);
    void log_error(const std::exception& e);

    std::filesystem::path state_db_{"/var/db/ocijail"};
    test_mode test_mode_{test_mode::NONE};
    log_format log_format_{log_format::TEXT};
    std::optional<std::filesystem::path> log_file_;
    int log_fd_{2};
};

void malformed_config(std::string_view message);

}  // namespace ocijail

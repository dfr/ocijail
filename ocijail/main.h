#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "CLI/CLI.hpp"
#include "nlohmann/json.hpp"

namespace ocijail {

enum class log_format {
    TEXT,
    JSON,
};

enum class test_mode {
    NONE,        // not testing
    VALIDATION,  // test config validation
};

class runtime_state {
    struct locked_state {
        ~locked_state();
        void unlock();
        void lock();
        bool locked_;
        int fd_;
    };

   public:
    runtime_state(const std::filesystem::path& dir, std::string_view id)
        : id_(id),
          state_dir_(dir),
          state_json_(dir / "state.json"),
          state_lock_(dir / "state.lock") {}

    auto& operator[](auto&& key) { return state_[key]; }
    const auto& operator[](auto&& key) const { return state_[key]; }
    auto get_id() const { return id_; }
    auto exists() const { return std::filesystem::is_directory(state_dir_); }
    auto& get_state_dir() const { return state_dir_; }

    locked_state create();
    void remove_all();
    void load();
    void save();
    locked_state lock();

   private:
    std::string_view id_;
    nlohmann::json state_;
    std::filesystem::path state_dir_;
    std::filesystem::path state_json_;
    std::filesystem::path state_lock_;
    // int lock_fd_{-1};
};

class main_app : public CLI::App {
   public:
    main_app(const std::string& title);
    runtime_state get_runtime_state(std::string_view id) {
        return {state_db_ / id, id};
    }
    auto get_test_mode() const { return test_mode_; }
    void log_error(const std::system_error& e);
    void log_error(const std::exception& e);

   private:
    std::filesystem::path state_db_{"/var/db/ocijail"};
    test_mode test_mode_{test_mode::NONE};
    log_format log_format_{log_format::TEXT};
    std::optional<std::filesystem::path> log_file_;
    int log_fd_{2};
};

void malformed_config(std::string_view message);

}  // namespace ocijail

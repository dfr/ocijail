#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctime>
#include <iomanip>

#include "ocijail/create.h"
#include "ocijail/delete.h"
#include "ocijail/exec.h"
#include "ocijail/kill.h"
#include "ocijail/main.h"
#include "ocijail/start.h"
#include "ocijail/state.h"

using namespace ocijail;
using nlohmann::json;

static const char* version = "0.1.2-dev";

int main(int argc, char** argv) {
    main_app app{"ocijail: Yet another OCI runtime"};

    create::init(app);
    start::init(app);
    delete_::init(app);
    exec::init(app);
    kill::init(app);
    state::init(app);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    } catch (const std::exception& e) {
        app.log_error(e);
        return 1;
    }

    return 0;
}

namespace ocijail {

void malformed_config(std::string_view message) {
    std::stringstream ss;
    ss << "create: malformed config: " << message;
    throw std::runtime_error(ss.str());
}

runtime_state::locked_state::~locked_state() {
    if (locked_) {
        unlock();
    }
}

void runtime_state::locked_state::unlock() {
    assert(locked_);
    locked_ = false;
    if (::flock(fd_, LOCK_UN) < 0) {
        throw std::system_error(
            errno, std::system_category(), "unlocking state lock");
    }
}

void runtime_state::locked_state::lock() {
    assert(!locked_);
    if (::flock(fd_, LOCK_EX) < 0) {
        throw std::system_error(
            errno, std::system_category(), "locking state lock");
    }
    locked_ = true;
}

runtime_state::locked_state runtime_state::create() {
    std::filesystem::remove_all(state_dir_);
    std::filesystem::create_directories(state_dir_);
    auto fd = ::open(state_lock_.c_str(), O_RDWR | O_CREAT | O_EXLOCK);
    if (fd < 0) {
        throw std::system_error(
            errno, std::system_category(), "opening state lock");
    }
    return {true, fd};
}

void runtime_state::remove_all() {
    std::filesystem::remove_all(state_dir_);
}

void runtime_state::load() {
    if (!std::filesystem::is_directory(state_dir_)) {
        std::stringstream ss;
        ss << "container " << id_ << " not found";
        throw std::runtime_error(ss.str());
    }
    std::ifstream{state_json_} >> state_;
}

void runtime_state::save() {
    std::ofstream{state_json_} << state_;
}

json runtime_state::report() const {
    json res;
    res["ociVersion"] = "1.0.2";
    res["id"] = id_;
    res["status"] = state_["status"];
    if (state_["status"] != "stopped") {
        res["pid"] = state_["pid"];
    }
    res["bundle"] = state_["bundle"];
    if (state_["config"].contains("annotations")) {
        res["annotations"] = state_["config"]["annotations"];
    }
    return res;
}

runtime_state::locked_state runtime_state::lock() {
    auto fd = ::open(state_lock_.c_str(), O_RDWR | O_CREAT);
    if (fd < 0) {
        throw std::system_error(
            errno, std::system_category(), "opening state lock");
    }
    if (::flock(fd, LOCK_EX) < 0) {
        throw std::system_error(
            errno, std::system_category(), "locking state lock");
    }
    return {true, fd};
}

main_app::main_app(const std::string& title) : CLI::App(title) {
    add_option("--root",
               state_db_,
               "Override default location for state database");
    add_flag(
        "--version",
        [](size_t) {
            std::cout << "ocijail version " << ::version << "\n";
            std::exit(0);
        },
        "Print runtime version");

    std::map<std::string, test_mode> test_modes{
        {"none", test_mode::NONE},
        {"validation", test_mode::VALIDATION},
    };
    add_option("--testing", test_mode_, "Unit test mode")
        ->group("")
        ->transform(CLI::CheckedTransformer(test_modes, CLI::ignore_case));

    std::map<std::string, log_format> log_formats{
        {"text", log_format::TEXT},
        {"json", log_format::JSON},
    };
    add_option("--log-format", log_format_, "Log format")
        ->transform(CLI::CheckedTransformer(log_formats, CLI::ignore_case));
    add_option("--log", log_file_, "Log file");

    require_subcommand(1);

    parse_complete_callback([this] {
        if (log_file_) {
            log_fd_ =
                ::open(log_file_->c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        }
    });
}

static std::string log_timestamp() {
    struct ::timeval tv;
    struct std::tm now;
    gettimeofday(&tv, nullptr);
    gmtime_r(&tv.tv_sec, &now);
    std::stringstream ss;
    ss << std::put_time(&now, "%Y-%m-%dT%H:%M:%S");
    ss << std::setw(9) << std::setfill('0') << tv.tv_usec << "Z";
    return ss.str();
}

log_entry::~log_entry() {
    app_.log_message(ss_.str());
}

void main_app::log_error(const std::exception& e) {
    log_message(e.what());
}

void main_app::log_message(const std::string& msg) {
    std::stringstream ss;
    switch (log_format_) {
    case log_format::TEXT:
        ss << log_timestamp() << ": " << msg << "\n";
        break;
    case log_format::JSON: {
        json err;
        err["msg"] = msg;
        err["level"] = "error";
        err["time"] = log_timestamp();
        ss << err << "\n";
        break;
    }
    }
    auto s = ss.str();
    ::write(log_fd_, s.data(), s.size());

    if (log_fd_ != 2) {
        // Copy to stderr
        std::cerr << "Error: " << msg << "\n";
    }
}

}  // namespace ocijail

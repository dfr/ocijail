#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctime>
#include <iomanip>

#include "ocijail/main.h"

#include "nlohmann/json.hpp"

#include "ocijail/create.h"
#include "ocijail/delete.h"
#include "ocijail/exec.h"
#include "ocijail/kill.h"
#include "ocijail/start.h"
#include "ocijail/state.h"

using namespace ocijail;
using nlohmann::json;

int main(int argc, char** argv) {
    main_app app{"ocijail: Yet another OCI runtime"};
    app.add_option("--state-db",
                   app.state_db_,
                   "Override default location for state database");

    std::map<std::string, test_mode> test_modes{
        {"none", test_mode::NONE},
        {"validation", test_mode::VALIDATION},
    };
    app.add_option("--testing", app.test_mode_, "Unit test mode")
        ->group("")
        ->transform(CLI::CheckedTransformer(test_modes, CLI::ignore_case));

    std::map<std::string, log_format> log_formats{
        {"text", log_format::TEXT},
        {"json", log_format::JSON},
    };
    app.add_option("--log-format", app.log_format_, "Log format")
        ->transform(CLI::CheckedTransformer(log_formats, CLI::ignore_case));
    app.add_option("--log", app.log_file_, "Log file");

    app.require_subcommand(1);

    app.parse_complete_callback([&app] {
        if (app.log_file_) {
            app.log_fd_ = ::open(
                app.log_file_->c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        }
    });

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

void main_app::log_error(const std::exception& e) {
    std::stringstream ss;
    switch (log_format_) {
    case log_format::TEXT:
        ss << log_timestamp() << ": " << e.what() << "\n";
        break;
    case log_format::JSON: {
        json err;
        err["msg"] = e.what();
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
        std::cerr << "Error: " << e.what() << "\n";
    }
}

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <sstream>

#include "ocijail/create.h"
#include "ocijail/jail.h"
#include "ocijail/mount.h"
#include "ocijail/process.h"
#include "ocijail/tty.h"

namespace fs = std::filesystem;

using nlohmann::json;

extern "C" char** environ;

namespace {

struct oci_version {
    std::string major;
    std::string minor;
    std::string patch;
};

oci_version parse_version(std::string ociver) {
    std::vector<std::string_view> parts;
    auto tmp = std::string_view{ociver};
    while (tmp.size() > 0) {
        auto i = tmp.find_first_of(".");
        if (i != std::string_view::npos) {
            parts.push_back(tmp.substr(0, i));
            tmp = tmp.substr(i + 1);
        } else {
            parts.push_back(tmp);
            tmp = "";
        }
    }
    if (parts.size() != 3) {
        throw std::runtime_error("malformed ociVersion " + std::string(ociver));
    }
    return oci_version{
        std::string{parts[0]}, std::string{parts[1]}, std::string{parts[2]}};
}

}  // namespace

namespace ocijail {

void create::init(main_app& app) {
    static create instance{app};
}

create::create(main_app& app) : app_(app) {
    auto sub = app.add_subcommand("create",
                                  "Create a jail instance for the container "
                                  "described by the given bundle directory.");
    sub->add_option("--bundle,-b",
                    bundle_path_,
                    "Path to the OCI runtime bundle directory")
        ->check(CLI::ExistingDirectory);
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_option(
           "--console-socket",
           console_socket_,
           "Path to a socket which will receive the console pty descriptor")
        ->check(CLI::ExistingPath);
    sub->add_option(
        "--pid-file",
        pid_file_,
        "Path to a file where the container process id will be written");

    sub->final_callback([this] { run(); });
}

void create::run() {
    auto state_dir = app_.state_db_ / id_;
    if (app_.test_mode_ == test_mode::NONE && fs::is_directory(state_dir)) {
        throw std::runtime_error{"container " + id_ + " exists"};
    }

    auto config_path = bundle_path_ / "config.json";
    if (!fs::is_regular_file(config_path)) {
        throw std::runtime_error{
            "create: bundle directory must contain config.json"};
    }
    json config;
    std::ifstream{config_path} >> config;

    if (!config.contains("ociVersion")) {
        malformed_config("no ociVersion");
    }
    if (!config["ociVersion"].is_string()) {
        malformed_config("ociVersion must be a string");
    }
    auto ver = parse_version(config["ociVersion"]);
    if (ver.major != "1" || ver.minor != "0") {
        throw std::runtime_error{"create: unsupported OCI version " +
                                 std::string{config["ociVersion"]}};
    }

    if (!config.contains("process")) {
        malformed_config("no process");
    }

    auto& config_process = config["process"];
    process proc{config_process, console_socket_};

    // If the config contains a root path, use that, otherwise the
    // bundle directory must have a subdirectory named "root"
    fs::path root_path;
    if (config.contains("root") && config["root"].contains("path")) {
        auto& config_root = config["root"];
        root_path = fs::path{config_root["path"]};
    } else {
        root_path = bundle_path_ / "root";
    }
    if (!fs::is_directory(root_path)) {
        std::stringstream ss;
        ss << "root directory " << root_path << " must be a directory";
        throw std::runtime_error{ss.str()};
    }

    // Validate mounts if present
    auto& config_mounts = config["mounts"];
    if (!config_mounts.is_null()) {
        if (!config_mounts.is_array()) {
            malformed_config("mounts must be an array");
        }
        for (auto& mount : config_mounts) {
            if (!mount.is_object()) {
                malformed_config("mounts must be an array of objects");
            }
            if (!mount["destination"].is_string()) {
                malformed_config("mount destination must be a string");
            }
            if (mount.contains("source")) {
                if (!mount["source"].is_string()) {
                    malformed_config(
                        "if present, mount source must be a string");
                }
            }
            if (mount.contains("type")) {
                if (!mount["type"].is_string()) {
                    malformed_config("if present, mount type must be a string");
                }
            }
            if (mount.contains("options")) {
                if (!mount["options"].is_array()) {
                    malformed_config(
                        "if present, mount options must be an array");
                }
                for (auto& opt : mount["options"]) {
                    if (!opt.is_string()) {
                        malformed_config(
                            "if present, mount options must be an array of "
                            "strings");
                    }
                }
            }
        }
    }

    // Get the parent jail name (if any).
    std::optional<std::string> parent_jail;
    if (config.contains("annotations")) {
        auto config_annotations = config["annotations"];
        if (config_annotations.contains("org.freebsd.parentJail")) {
            parent_jail = config_annotations["org.freebsd.parentJail"];
        }
    }

    // Create a jail config from the OCI config
    jail::config jconf;
    jconf.set("name", id_);
    jconf.set("persist");
    jconf.set("enforce_statfs", 1u);
    jconf.set("allow.raw_sockets");
    jconf.set("path", root_path);
    jconf.set("ip4", jail::INHERIT);
    jconf.set("ip6", jail::INHERIT);
    if (config.contains("hostname")) {
        jconf.set("host.hostname", config["hostname"]);
        jconf.set("host", jail::NEW);
    } else {
        jconf.set("host", jail::INHERIT);
    }

    // Unit tests for config validation stop here.
    if (app_.test_mode_ == test_mode::VALIDATION) {
        return;
    }

    // Create a state object with initial fields from the config
    json state;
    state["id"] = id_;
    state["root_path"] = root_path;
    state["bundle"] = bundle_path_;
    state["config"] = config;
    state["status"] = "created";
    if (parent_jail) {
        state["parent_jail"] = *parent_jail;
    }

    // Mount filesystems, if requested and record unmount actions in the
    // state
    if (config_mounts.is_array()) {
        mount_volumes(state, root_path, config_mounts);
    }

    // Create the jail for our container. If we have a parent, attach
    // to that first.
    if (parent_jail) {
        jail::find(*parent_jail).attach();
    }
    auto j = jail::create(jconf);

    // We record the container state including the bundle config. We
    // need to create the start fifo before forking - this will be
    // used to pause the container until start is called.
    umask(077);
    fs::create_directories(state_dir);
    auto start_wait = state_dir / "start_wait";
    if (mkfifo(start_wait.c_str(), 0600) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error creating start fifo"};
    }

    auto pid = ::fork();
    auto state_path = state_dir / "state.json";
    if (pid) {
        // Parent process - write to pid file if requested
        if (pid_file_) {
            std::ofstream{*pid_file_} << pid;
        }
        state["jid"] = j.jid();
        state["pid"] = pid;
        std::ofstream{state_path} << state;
    } else {
        // Perform the console-socket hand off if process.terminal is true.
        auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();

        // Wait for start to signal us via the fifo
        auto fd = open(start_wait.c_str(), O_RDWR);
        char ch;
        if (fd < 0) {
            throw std::system_error{
                errno, std::system_category(), "error opening start fifo"};
        }
        assert(fd >= 0);
        auto n = read(fd, &ch, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "error reading from start fifo"};
        }
        close(fd);

        // If start was called, we should be in state 'running'. TODO:
        // figure out the right semantics for kill and delete on
        // containers in 'created' state.
        std::ifstream{state_path} >> state;
        if (state["status"] != "running") {
            exit(0);
        }

        // Enter the jail and set the requested working directory.
        j.attach();

        // Execute the requested process inside the jail
        proc.exec(stdin_fd, stdout_fd, stderr_fd);
    }
}

}  // namespace ocijail

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <sstream>

#include "ocijail/create.h"
#include "ocijail/hook.h"
#include "ocijail/jail.h"
#include "ocijail/mount.h"
#include "ocijail/process.h"
#include "ocijail/tty.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace {

struct oci_version {
    std::string major;
    std::string minor;
    std::string patch;
};

oci_version parse_version(std::string ociver) {
    std::vector<std::string_view> parts;
    auto tmp = std::string_view{ociver};
    // Trim off any -rc.x or -dev suffix first
    auto i = tmp.find_first_of("-");
    if (i != std::string_view::npos) {
        auto suffix = tmp.substr(i + 1);
        if (suffix.substr(0, 3) != "rc." && suffix != "dev") {
            throw std::runtime_error("malformed ociVersion " +
                                     std::string(ociver));
        }
        tmp = tmp.substr(0, i);
    }
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
    sub->add_option("--preserve-fds",
                    preserve_fds_,
                    "Number of additional file descriptors for the container");

    sub->final_callback([this] { run(); });
}

void create::run() {
    auto state = app_.get_runtime_state(id_);

    if (app_.get_test_mode() == test_mode::NONE && state.exists()) {
        throw std::runtime_error{"container " + id_ + " exists"};
    }

    if (chdir(bundle_path_.c_str()) < 0) {
        throw std::system_error{
            errno,
            std::system_category(),
            "error changing directory to" + bundle_path_.string()};
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
    // Allow 1.0.x and 1.1.x
    auto ver = parse_version(config["ociVersion"]);
    if (ver.major != "1" || !(ver.minor == "0" || ver.minor == "1")) {
        throw std::runtime_error{"create: unsupported OCI version " +
                                 std::string{config["ociVersion"]}};
    }

    if (!config.contains("process")) {
        malformed_config("no process");
    }

    auto& config_process = config["process"];
    process proc{config_process, console_socket_, true, preserve_fds_};

    // If the config contains a root path, use that, otherwise the
    // bundle directory must have a subdirectory named "root"
    bool root_readonly = false;
    auto root_path = bundle_path_ / "root";
    auto readonly_root_path = state.get_state_dir() / "readonly_root";
    if (config.contains("root")) {
        auto& config_root = config["root"];
        if (config["root"].contains("path")) {
            root_path = fs::path{config_root["path"]};
        }
        if (config_root.contains("readonly") && config_root["readonly"]) {
            root_readonly = true;
        }
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

    // Validate hooks, if present
    auto& config_hooks = config["hooks"];
    if (!config_hooks.is_null()) {
        if (!config_hooks.is_object()) {
            malformed_config("hooks must be an object");
        }

        hook::validate_hooks(app_, config_hooks, "prestart");
        hook::validate_hooks(app_, config_hooks, "createRuntime");
        hook::validate_hooks(app_, config_hooks, "createContainer");
        hook::validate_hooks(app_, config_hooks, "startContainer");
        hook::validate_hooks(app_, config_hooks, "poststart");
        hook::validate_hooks(app_, config_hooks, "poststop");
    }

    // Default to setting allow.chflags but disable if we have a
    // parent jail where this is not set.
    bool allow_chflags = true;

    // Get the parent jail name (if any).
    std::optional<std::string> parent_jail;
    if (config.contains("annotations")) {
        auto config_annotations = config["annotations"];
        if (config_annotations.contains("org.freebsd.parentJail")) {
            parent_jail = config_annotations["org.freebsd.parentJail"];
            auto pj = jail::find(*parent_jail);
            allow_chflags = pj.get<bool>("allow.chflags");
        }
    }

    // Create a jail config from the OCI config
    jail::config jconf;
    if (parent_jail) {
        jconf.set("name", *parent_jail + "." + id_);
    } else {
        jconf.set("name", id_);
    }
    jconf.set("persist");
    jconf.set("enforce_statfs", 1u);
    jconf.set("allow.raw_sockets");
    if (allow_chflags) {
        jconf.set("allow.chflags");
    }
    if (root_readonly) {
        jconf.set("path", readonly_root_path);
    } else {
        jconf.set("path", root_path);
    }
    jconf.set("ip4", jail::INHERIT);
    jconf.set("ip6", jail::INHERIT);
    if (config.contains("hostname")) {
        jconf.set("host.hostname", config["hostname"]);
        jconf.set("host", jail::NEW);
    } else {
        jconf.set("host", jail::INHERIT);
    }

    // Unit tests for config validation stop here.
    if (app_.get_test_mode() == test_mode::VALIDATION) {
        return;
    }

    // Create a state object with initial fields from the config
    state["root_path"] = root_path;
    state["bundle"] = bundle_path_;
    state["config"] = config;
    state["status"] = "created";
    if (parent_jail) {
        state["parent_jail"] = *parent_jail;
    }

    // Create the state here in case we have a readonly root
    auto lk = state.create();

    // Mount filesystems if requested and record unmount actions in the
    // state.
    //
    // If rootfs needs to be remounted read-only, we make two passes. The first
    // prepares mount points and the second completes the mounts in our
    // read-only alias.
    state["root_readonly"] = false;
    if (root_readonly) {
        if (config_mounts.is_array()) {
            mount_volumes(app_, state, root_path, true, config_mounts);
        }
        fs::create_directory(readonly_root_path);
        std::vector<std::tuple<std::string, std::string>> mount_opts;
        mount_opts.emplace_back("fstype", "nullfs");
        mount_opts.emplace_back("fspath", readonly_root_path);
        mount_opts.emplace_back("target", root_path);
        if (do_mount(mount_opts, MNT_RDONLY) < 0) {
            throw std::system_error(errno,
                                    std::system_category(),
                                    "mounting " + readonly_root_path.native());
        }
        root_path = readonly_root_path;
        state["root_readonly"] = true;
        state["readonly_root_path"] = readonly_root_path;
    }
    if (config_mounts.is_array()) {
        mount_volumes(app_, state, root_path, false, config_mounts);
    }

    // Create the jail for our container. If we have a parent, attach
    // to that first.
    if (parent_jail) {
        auto pj = jail::find(*parent_jail);
        auto current_child_count = pj.get<uint32_t>("children.cur");
        auto max_child_count = pj.get<uint32_t>("children.max");
        if (current_child_count >= max_child_count) {
            pj.set("children.max", current_child_count + 1);
        }
    }

    // Create a socket pair for coordinating create activities with
    // our child process.
    int create_sock[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, create_sock) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error creating socket pair"};
    }

    auto j = jail::create(jconf);

    // We record the container state including the bundle config. We
    // need to create the start fifo before forking - this will be
    // used to pause the container until start is called.
    umask(077);
    auto start_wait = state.get_state_dir() / "start_wait";
    if (mkfifo(start_wait.c_str(), 0600) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error creating start fifo"};
    }

    auto pid = ::fork();
    if (pid) {
        // Parent process - write to pid file if requested
        if (pid_file_) {
            std::ofstream{*pid_file_} << pid;
        }
        state["jid"] = j.jid();
        state["pid"] = pid;
        state.save();

        hook::run_hooks(app_, config_hooks, "createRuntime", state);

        lk.unlock();

        // Signal the child to execute any hooks and validate that the
        // container process can be found.
        char ch = 1;
        auto n = ::write(create_sock[0], &ch, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "write to create socket"};
        }

        // Read back the child's status - this is our exit status. The
        // child will have already written to stderr if necessary.
        char status;
        n = ::read(create_sock[0], &status, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "read from create socket"};
        }
        if (status != 0) {
            state["status"] = "stopped";
            state.save();
        }
        ::exit(status);
    } else {
        // Perform the console-socket hand off if process.terminal is true.
        auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();

        auto start_wait_fd = ::open(start_wait.c_str(), O_RDWR);
        if (start_wait_fd < 0) {
            throw std::system_error{
                errno, std::system_category(), "open start fifo"};
        }

        // Wait for our parent to signal us via the socket
        char ch;
        auto n = read(create_sock[1], &ch, 1);
        if (n < 0) {
            throw std::system_error{errno,
                                    std::system_category(),
                                    "error reading from create socket"};
        }

        char status = 0;
        try {
            // Our part of create: execute any hooks, enter the jail and
            // validate process args.

            // The specification states that for createContainer hooks:
            //
            // - The value of path MUST resolve in the container namespace.
            // - The startContainer hooks MUST be executed in the container
            //   namespace.
            //
            // This doesn't make a lot of sense but looking at other
            // implementations, runc interprets this as changing directory
            // to the container root (but not chrooting).
            if (chdir(root_path.c_str()) < 0) {
                throw std::system_error{
                    errno,
                    std::system_category(),
                    "error changing directory to" + root_path.string()};
            }
            hook::run_hooks(app_, config_hooks, "createContainer", state);

            // Enter the jail and set the requested working directory.
            j.attach();

            // Validate the process executable exists and can be executed
            proc.validate();
        } catch (const std::exception& e) {
            std::string_view msg{e.what()};
            ::write(2, msg.data(), msg.size());
            status = 1;
        }

        n = write(create_sock[1], &status, 1);
        if (n < 0) {
            throw std::system_error{errno,
                                    std::system_category(),
                                    "error writing to create socket"};
        }
        ::close(create_sock[1]);

        // Finished coordinating with parent - now we wait until
        // signalled by start.
        n = ::read(start_wait_fd, &ch, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "read from start fifo"};
        }
        ::close(start_wait_fd);

        // If validate failed, don't try to run hooks or execve, just stop here.
        if (status != 0) {
            ::exit(status);
        }

        // Run startContainer hooks inside the jail.
        hook::run_hooks(app_, config_hooks, "startContainer", state);

        // Execute the requested process inside the jail.
        proc.exec(stdin_fd, stdout_fd, stderr_fd);
    }
}

}  // namespace ocijail

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <sstream>

#include "ocijail/create.h"
#include "ocijail/jail.h"
#include "ocijail/mount.h"
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

void create::malformed_config(std::string_view message) {
    std::stringstream ss;
    ss << "create: malformed config " << bundle_path_ / "config.json"
       << ": " << message;
    throw std::runtime_error(ss.str());
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
    if (!config_process.is_object()) {
        malformed_config("process must be an object");
    }

    if (!config_process.contains("cwd")) {
        malformed_config("no process.cwd");
    }
    auto& config_process_cwd = config_process["cwd"];
    if (!config_process_cwd.is_string()) {
        malformed_config("process.cwd must be a string");
    }

    if (!config_process.contains("args")) {
        malformed_config("no process.args");
    }
    auto& config_args = config_process["args"];
    if (!config_args.is_array()) {
        malformed_config("process.args must be an array");
    }
    if (config_args.size() == 0) {
        malformed_config("process.args must have at least one element");
    }
    std::vector<std::string> args;
    for (auto& arg : config_args) {
        if (!arg.is_string()) {
            malformed_config("process.args must be an array of strings");
        }
        args.push_back(arg.get<std::string>());
    }

    auto config_process_user = config_process["user"];
    if (!config_process_user.is_null()) {
        if (!config_process_user.is_object()) {
            malformed_config("process.user must be an object");
        }
        if (!config_process_user["uid"].is_number()) {
            malformed_config("process.user.uid must be a number");
        }
        if (!config_process_user["gid"].is_number()) {
            malformed_config("process.user.gid must be a number");
        }
        if (config_process_user.contains("umask") &&
            !config_process_user["umask"].is_number()) {
            malformed_config("process.user.umask must be a number");
        }
        if (config_process_user.contains("additionalGids")) {
            auto gids = config_process_user["additionalGids"];
            if (!gids.is_array()) {
                malformed_config(
                    "process.user.additionalGids must be an array");
            }
            for (auto& gid : gids) {
                if (!gid.is_number()) {
                    malformed_config(
                        "process.user.additionalGids must be an array of "
                        "numbers");
                }
            }
        }
    }

    std::vector<std::string> env;
    if (config_process.contains("env")) {
        auto& config_env = config_process["env"];
        if (!config_env.is_array()) {
            malformed_config("process.env must be an array");
        }
        for (auto& arg : config_env) {
            if (!arg.is_string()) {
                malformed_config("process.env must be an array of strings");
            }
            env.push_back(arg.get<std::string>());
        }
    }

    bool config_process_terminal = false;
    if (config_process.contains("terminal")) {
        if (!config_process["terminal"].is_boolean()) {
            malformed_config("process.terminal must be a boolean");
        }
        config_process_terminal = config_process["terminal"];
    }
    if (config_process_terminal) {
        if (!console_socket_) {
            throw std::runtime_error{
                "create: --console-socket is required when "
                "process.terminal is true"};
        }
        if (!fs::is_socket(*console_socket_)) {
            throw std::runtime_error{
                "create: --console-socket must be a path to a local domain "
                "socket"};
        }
    } else if (console_socket_) {
        throw std::runtime_error(
            "create: --console-socket provided but process.terminal is "
            "false");
    }

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

    auto pid = fork();
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
        int stdin_fd, stdout_fd, stderr_fd;
        if (config_process_terminal) {
            auto [control_fd, tty_fd] = open_pty();
            stdin_fd = stdout_fd = stderr_fd = tty_fd;
            send_pty_control_fd(*console_socket_, control_fd);
        } else {
            stdin_fd = 0;
            stdout_fd = 1;
            stderr_fd = 2;
        }

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

        // Prepare the environment and arguments for execvp.
        std::vector<char*> envv, argv;
        for (auto& s : env) {
            envv.push_back(const_cast<char*>(s.c_str()));
        }
        envv.push_back(nullptr);
        for (auto& s : args) {
            argv.push_back(const_cast<char*>(s.c_str()));
        }
        argv.push_back(nullptr);
        environ = &envv[0];

        // Enter the jail and set the requested working directory.
        j.attach();
        if (chdir(config_process_cwd.get<std::string>().c_str()) < 0) {
            throw std::system_error{errno,
                                    std::system_category(),
                                    "error changing directory to" +
                                        config_process_cwd.get<std::string>()};
        }

        // Unblock signals
        reset_signals();

        // Set the uid, gid etc.
        set_uid_gid(config_process_user);

        // Setup stdin, stdout and stderr. Close everything else.
        if (stdin_fd != 0) {
            ::dup2(stdin_fd, 0);
        }
        if (stdout_fd != 1) {
            ::dup2(stdout_fd, 1);
        }
        if (stderr_fd != 2) {
            ::dup2(stderr_fd, 2);
        }
        ::close_range(3, INT_MAX, CLOSE_RANGE_CLOEXEC);

        // exec the requested command.
        ::execvp(argv[0], &argv[0]);
        throw std::system_error{errno,
                                std::system_category(),
                                "error executing container command " + args[0]};
    }
}

void create::reset_signals() {
    ::sigset_t mask;
    ::sigfillset(&mask);
    if (::sigprocmask(SIG_UNBLOCK, &mask, nullptr) < 0) {
        throw std::system_error{
            errno, std::system_category(), "setting signal mask"};
    }
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = 0;
    ::sigemptyset(&sa.sa_mask);
    for (int sig = 0; sig < NSIG; sig++) {
        if (::sigaction(sig, &sa, nullptr) < 0 && errno != EINVAL) {
            throw std::system_error{
                errno, std::system_category(), "setting signal handler"};
        }
    }
}

void create::set_uid_gid(const json& user) {
    std::vector<gid_t> gids;
    uid_t uid;
    gid_t gid;
    mode_t umask = 077;

    if (user.is_object()) {
        uid = user["uid"];
        gid = user["gid"];
        gids.push_back(gid);
        if (user.contains("additionalGids")) {
            for (auto& gid : user["additionalGids"]) {
                gids.push_back(gid);
            }
        }
        if (user.contains("umask")) {
            umask = user["umask"];
        }
    } else {
        uid = 0;
        gid = 0;
        gids.push_back(gid);
    }

    if (::setgroups(gids.size(), &gids[0]) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling setgroups"};
    }
    if (::setgid(gid) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling getgid"};
    }
    if (::setuid(uid) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling setuid"};
    }
    if (::umask(umask) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling umask"};
    }
}

}  // namespace ocijail

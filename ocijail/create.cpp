#include <fcntl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
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

static ocijail::jail::sharing parse_sharing(const std::string& ns,
                                            std::string val) {
    if (val == "disable") {
        return ocijail::jail::DISABLE;
    } else if (val == "new") {
        return ocijail::jail::NEW;
    } else if (val == "inherit") {
        return ocijail::jail::INHERIT;
    } else {
        std::stringstream ss;
        ss << "bad value for sharing " << ns << ": " << val;
        throw std::runtime_error(ss.str());
    }
}

static void set_sharing(ocijail::jail::config& jconf,
                        const json& params,
                        const std::string& subsys) {
    if (params.contains(subsys)) {
        jconf.set(subsys, parse_sharing(subsys, params[subsys]));
    }
}

static void set_addresses(ocijail::jail::config& jconf,
                          const json& params,
                          const std::string& addr,
                          const std::string& jaddr,
                          sa_family_t af) {
    using ocijail::malformed_config;
    if (params.contains(addr)) {
        auto& addr_list = params[addr];
        if (!addr_list.is_array()) {
            malformed_config("ip4.addr and ip6.addr must be arrays");
        }
        addrinfo hints{
            .ai_flags = AI_NUMERICHOST,
            .ai_family = af,
            .ai_socktype = SOCK_STREAM,
            .ai_protocol = 0,
            .ai_addrlen = 0,
            .ai_canonname = nullptr,
            .ai_addr = nullptr,
            .ai_next = nullptr
        };
        std::vector<std::byte> addrs;
        for (auto& ip : addr_list) {
            if (!ip.is_string()) {
                malformed_config("IP addresses must be strings");
            }
            addrinfo* ai0{nullptr};
            int error = getaddrinfo(ip.get<std::string>().c_str(), nullptr, &hints, &ai0);
            if (error) {
                throw std::runtime_error{std::string{"error from getaddrinfo: "}
                                         + std::string{gai_strerror(error)}};
            }
            for (auto ai = ai0; ai; ai = ai->ai_next) {
                if (af == AF_INET) {
                    auto sin4 = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
                    std::copy_n(reinterpret_cast<std::byte*>(&sin4->sin_addr), sizeof(in_addr), std::back_inserter(addrs));
                } else {
                    auto sin6 = reinterpret_cast<sockaddr_in6*>(ai->ai_addr);
                    std::copy_n(reinterpret_cast<std::byte*>(&sin6->sin6_addr), sizeof(in6_addr), std::back_inserter(addrs));
                }
            }
            freeaddrinfo(ai0);
        }
        jconf.set(jaddr, addrs);
    }
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
    // Allow 1.0.x, 1.1.x, 1.2.x and 1.3.x
    auto ver = parse_version(config["ociVersion"]);
    if (ver.major != "1" || ver.minor > "3") {
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

    // Create a jail config from the OCI config. If we have a freebsd config,
    // jail parameters are taken from that, otherwise we fall back to using
    // org.freebsd annotations to get the parent jail (if any) and network
    // settings.
    jail::config jconf;
    std::optional<std::string> parent_jail;

    // Set name to the container id (possible prefixed with parent name, see
    // below).
    jconf.set("name", id_);
    auto& config_freebsd = config["freebsd"];
    std::vector<device> devices;
    if (!config_freebsd.is_null()) {
        if (config_freebsd.contains("devices")) {
            auto& freebsd_devices = config_freebsd["devices"];
            if (!freebsd_devices.is_array()) {
                malformed_config("if present, freebsd.devices must be an array");
            }
            for (const auto& device : freebsd_devices) {
                devices.emplace_back(device["path"].get<std::string>(),
                                     device["mode"].get<uint32_t>());
            }
        }
        if (config_freebsd.contains("jail")) {
            auto& freebsd_jail = config_freebsd["jail"];
            if (!freebsd_jail.is_object()) {
                malformed_config("if present, freebsd.jail must be an object");
            }
            if (freebsd_jail.contains("parent")) {
                parent_jail = freebsd_jail["parent"];
                state["parent_jail"] = *parent_jail;
                jconf.set("name", *parent_jail + "." + id_);
            }
            set_sharing(jconf, freebsd_jail, "vnet");
            set_sharing(jconf, freebsd_jail, "ip4");
            set_sharing(jconf, freebsd_jail, "ip6");
            set_sharing(jconf, freebsd_jail, "host");
            set_sharing(jconf, freebsd_jail, "sysvmsg");
            set_sharing(jconf, freebsd_jail, "sysvsem");
            set_sharing(jconf, freebsd_jail, "sysvshm");
            set_addresses(jconf, freebsd_jail, "ip4Addr", "ip4.addr", AF_INET);
            set_addresses(jconf, freebsd_jail, "ip6Addr", "ip6.addr", AF_INET6);
            if (freebsd_jail.contains("enforceStatfs")) {
                jconf.set("enforce_statfs", freebsd_jail["enforceStatfs"].get<uint32_t>());
            }
            if (freebsd_jail.contains("allow")) {
                auto& jail_allow = freebsd_jail["allow"];
                if (!jail_allow.is_object()) {
                    malformed_config("if present, freebsd.jail.allow must be an object");
                }
                if (jail_allow["setHostname"]) {
                    jconf.set("allow.set_hostname");
                }
                if (jail_allow["rawSockets"]) {
                    jconf.set("allow.raw_sockets");
                }
                if (jail_allow["chflags"]) {
                    jconf.set("allow.chflags");
                }
                if (jail_allow["quotas"]) {
                    jconf.set("allow.quotas");
                }
                if (jail_allow["socketAf"]) {
                    jconf.set("allow.socket_af");
                }
                if (jail_allow["mlock"]) {
                    jconf.set("allow.mlock");
                }
                if (jail_allow["reservedPorts"]) {
                    jconf.set("allow.reserved_ports");
                }
                if (jail_allow["suser"]) {
                    jconf.set("allow.suser");
                }
            }
        }
    } else if (config.contains("annotations")) {
        // Get the parent jail name and requested vnet type (if any)
        auto config_annotations = config["annotations"];
        jconf.set("host", jail::NEW);
        if (config_annotations.contains("org.freebsd.parentJail")) {
            parent_jail = config_annotations["org.freebsd.parentJail"];
            state["parent_jail"] = *parent_jail;
            jconf.set("name", *parent_jail + "." + id_);
            jconf.set("ip4", jail::INHERIT);
            jconf.set("ip6", jail::INHERIT);
        }
        if (config_annotations.contains("org.freebsd.jail.vnet")) {
            auto vnet = parse_sharing(
                "vnet", config_annotations["org.freebsd.jail.vnet"]);
            if (vnet == jail::NEW) {
                jconf.set("vnet", vnet);
            } else {
                jconf.set("ip4", jail::INHERIT);
                jconf.set("ip6", jail::INHERIT);
            }
        }
        jconf.set("enforce_statfs", 1u);
        jconf.set("allow.raw_sockets");
        // Default to setting allow.chflags but disable if we have a
        // parent jail where this is not set.
        if (parent_jail) {
            auto pj = jail::find(*parent_jail);
            if (pj.get<bool>("allow.chflags")) {
                jconf.set("allow.chflags");
            }
        } else {
            jconf.set("allow.chflags");
        }
    }
    jconf.set("persist");
    if (root_readonly) {
        jconf.set("path", readonly_root_path);
    } else {
        jconf.set("path", root_path);
    }
    if (config.contains("hostname") && jconf.contains("host") &&
        jconf.get<jail::sharing>("host") == jail::NEW) {
        jconf.set("host.hostname", config["hostname"].get<std::string>());
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
            mount_volumes(app_, state, root_path, true, config_mounts, devices);
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
        mount_volumes(app_, state, root_path, false, config_mounts, devices);
    }

    // Create the jail for our container. If we have a parent, adjust its
    // children.max if necessary.
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
            // If the create failed, we need to clean up: unmount the volumes and
            // delete the state.
            j.remove();
            if (config_mounts.is_array()) {
                unmount_volumes(app_, state, root_path, config_mounts);
            }
            if (root_readonly) {
                if (::unmount(root_path.c_str(), MNT_FORCE) > 0) {
                    throw std::system_error{errno,
                                            std::system_category(),
                                            "unmounting " + root_path.native()};
                }
            }
            state.remove_all();
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

        // If validate failed, don't wait for a start signal, just stop here.
        if (status != 0) {
            ::exit(status);
        }

        // Finished coordinating with parent - now we wait until
        // signalled by start.
        n = ::read(start_wait_fd, &ch, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "read from start fifo"};
        }
        ::close(start_wait_fd);

        // Run startContainer hooks inside the jail.
        hook::run_hooks(app_, config_hooks, "startContainer", state);

        // Execute the requested process inside the jail.
        proc.exec(stdin_fd, stdout_fd, stderr_fd);
    }
}

}  // namespace ocijail

#include <sys/param.h>

#include <spawn.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <iostream>

#include "ocijail/main.h"
#include "ocijail/mount.h"

extern "C" char** environ;

using namespace std::literals::string_literals;
namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

static std::map<std::string, int, std::less<>> name_to_flag = {
    {"async", MNT_ASYNC},
    {"atime", -MNT_NOATIME},
    {"exec", -MNT_NOEXEC},
    {"suid", -MNT_NOSUID},
    {"symfollow", -MNT_NOSYMFOLLOW},
    {"rdonly", MNT_RDONLY},
    {"sync", MNT_SYNCHRONOUS},
    {"union", MNT_UNION},
    {"userquota", 0},
    {"groupquota", 0},
    {"clusterr", -MNT_NOCLUSTERR},
    {"clusterw", -MNT_NOCLUSTERW},
    {"suiddir", MNT_SUIDDIR},
    {"snapshot", MNT_SNAPSHOT},
    {"multilabel", MNT_MULTILABEL},
    {"acls", MNT_ACLS},
    {"nfsv4acls", MNT_NFS4ACLS},
    {"automounted", MNT_AUTOMOUNTED},
    {"untrusted", MNT_UNTRUSTED},

    /* Control flags. */
    {"force", MNT_FORCE},
    {"update", MNT_UPDATE},
    {"ro", MNT_RDONLY},
    {"rw", -MNT_RDONLY},
    {"cover", -MNT_NOCOVER},
    {"emptydir", MNT_EMPTYDIR},

    // ignore these
    {"private", 0},
    {"rprivate", 0},
    {"rbind", 0},
    {"nodev", 0},
    {"bind", 0},
};

struct pseudo_option {
    pseudo_option(std::string_view type_, std::string_view optkey_)
        : type(type_), optkey(optkey_) {
        handlers_.push_back(this);
    }
    virtual ~pseudo_option() = default;

    static pseudo_option* lookup(std::string_view type,
                                 std::string_view optkey) {
        for (auto h : handlers_) {
            if (h->type == type && h->optkey == optkey) {
                return h;
            }
        }
        return nullptr;
    }

    std::string_view type;
    std::string_view optkey;
    virtual void before_mount(const fs::path& destination,
                              std::string_view optval) = 0;
    virtual void after_mount(const fs::path& destination,
                             std::string_view optval) = 0;

   private:
    static std::vector<pseudo_option*> handlers_;
};

std::vector<pseudo_option*> pseudo_option::handlers_;

struct tmpcopyup_option : pseudo_option {
    using pseudo_option::pseudo_option;

    void before_mount(const fs::path& destination,
                      std::string_view optval) override {
        char dir_template[] = "/tmp/tmpcopyup.XXXXXXXX";
        tmp_copy = mkdtemp(dir_template);
        fs::copy(destination,
                 tmp_copy,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    }

    void after_mount(const fs::path& destination,
                     std::string_view optval) override {
        fs::copy(tmp_copy,
                 destination,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    }

    fs::path tmp_copy;
} tmpcopyup_handler{"tmpfs", "tmpcopyup"};

struct devfs_rule_option : pseudo_option {
    using pseudo_option::pseudo_option;

    void before_mount(const fs::path& destination,
                      std::string_view optval) override {}

    void after_mount(const fs::path& destination,
                     std::string_view rule) override {
        std::vector<std::string> args;
        std::vector<char*> argv;

        args.emplace_back("devfs");
        args.emplace_back("-m");
        args.emplace_back(destination);
        args.emplace_back("rule");
        args.emplace_back("apply");
        while (rule.size() > 0) {
            auto sep = rule.find(' ');
            args.emplace_back(rule.substr(0, sep));
            if (sep != std::string_view::npos) {
                rule = rule.substr(sep);
                while (rule.size() > 0 && rule[0] == ' ') {
                    rule = rule.substr(1);
                }
            } else {
                rule = "";
            }
        }
        for (auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        pid_t pid;
        auto res = ::posix_spawn(
            &pid, "/sbin/devfs", nullptr, nullptr, &argv[0], environ);
        if (res != 0) {
            throw std::system_error{res, std::system_category(), "posix_spawn"};
        }

        int status;
        if (::waitpid(pid, &status, 0) < 0) {
            throw std::system_error{errno, std::system_category(), "waitpid"};
        }

        if (status != 0) {
            throw std::runtime_error{"devfs exited with error " +
                                     std::to_string(status)};
        }
    }

    fs::path tmp_copy;
} devfs_rule_handler{"devfs", "rule"};

static std::tuple<std::string_view, std::string_view> split_option(
    std::string_view option) {
    auto sep = option.find_first_of("=");
    if (sep == std::string::npos) {
        return std::make_tuple(option, "");
    }
    return {option.substr(0, sep), option.substr(sep + 1)};
}

static std::tuple<fs::path, fs::path> get_save_path(
    const runtime_state& state,
    const fs::path& destination) {
    auto save_dir =
        destination.parent_path() / (".save-"s + std::string{state.get_id()});
    auto save_path = save_dir / destination.filename();
    return std::make_tuple(save_dir, save_path);
}

static fs::path resolve_container_path(const fs::path& root_path,
                                       const json& mount) {
    // Strip off the leading '/' from destination if it has one
    std::string path{mount["destination"]};
    if (path[0] == '/') {
        path = path.substr(1);
    }
    return root_path / path;
}

void apply_devfs_rule(const fs::path& destination, std::string_view rule) {
    std::vector<std::string> args;
    std::vector<char*> argv;

    args.emplace_back("devfs");
    args.emplace_back("-m");
    args.emplace_back(destination);
    args.emplace_back("rule");
    args.emplace_back("apply");
    while (rule.size() > 0) {
        auto sep = rule.find(' ');
        args.emplace_back(rule.substr(0, sep));
        if (sep != std::string_view::npos) {
            rule = rule.substr(sep);
            while (rule.size() > 0 && rule[0] == ' ') {
                rule = rule.substr(1);
            }
        } else {
            rule = "";
        }
    }
    for (auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid;
    auto res =
        ::posix_spawn(&pid, "/sbin/devfs", nullptr, nullptr, &argv[0], environ);
    if (res != 0) {
        throw std::system_error{res, std::system_category(), "posix_spawn"};
    }

    int status;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::system_error{errno, std::system_category(), "waitpid"};
    }

    if (status != 0) {
        throw std::runtime_error{"devfs exited with error " +
                                 std::to_string(status)};
    }
}

static void mount_volume(runtime_state& state,
                         const fs::path& root_path,
                         const json& mount) {
    auto destination = resolve_container_path(root_path, mount);

    std::string type = mount.contains("type") ? mount["type"] : "nullfs";
    if (type == "bind") {
        // TODO: remove this when podman syncs with buildah fixes to
        // avoid using "bind" on FreeBSD.
        type = "nullfs";
    }
    bool is_file_mount =
        type == "nullfs" && fs::is_regular_file(mount["source"]);

    // Validate mount options before we perform any actions
    std::vector<std::tuple<pseudo_option*, std::string>> pseudo_opts;
    std::vector<std::tuple<std::string, std::string>> mount_opts;
    int mount_flags = 0;
    if (!is_file_mount) {
        mount_opts.emplace_back("fstype", type);
        mount_opts.emplace_back("fspath", destination);
        if (type == "nullfs") {
            mount_opts.emplace_back("target", mount["source"]);
        }
        if (mount.contains("options")) {
            for (auto& opt : mount["options"]) {
                // Copy the string out of json to make life simpler
                std::string optstring{opt};
                auto [key, val] = split_option(optstring);

                auto it = name_to_flag.find(key);
                if (it != name_to_flag.end()) {
                    auto flag = it->second;
                    if (flag > 0) {
                        mount_flags |= flag;
                    } else if (flag < 0) {
                        mount_flags &= ~(-flag);
                    }
                    continue;
                } else {
                    auto h = pseudo_option::lookup(type, key);
                    if (h != nullptr) {
                        pseudo_opts.emplace_back(h, val);
                        continue;
                    }
                    mount_opts.emplace_back(key, val);
                }
            }
        }
    }

    if (fs::exists(destination)) {
        if (is_file_mount) {
            if (!fs::is_regular_file(destination)) {
                throw std::runtime_error(
                    "destination for file mount exists and is not a file");
            }
            // Mimic real file mounts by moving the original to a subdirectory
            auto [save_dir, save_path] = get_save_path(state, destination);
            if (!fs::exists(save_dir)) {
                fs::create_directories(save_dir);
                state["remove_on_unmount"].push_back(save_dir);
            }
            fs::rename(destination, save_path);
        } else {
            if (!fs::is_directory(destination)) {
                throw std::runtime_error(
                    "destination for non-file mount exists and is not a "
                    "directory");
            }
        }
    } else {
        state["remove_on_unmount"].push_back(destination);
        if (is_file_mount) {
            // Create parent directories if necessary.
            fs::create_directories(destination.parent_path());
        } else {
            fs::create_directories(destination);
        }
    }

    for (auto& entry : pseudo_opts) {
        std::get<0>(entry)->before_mount(destination, std::get<1>(entry));
    }

    if (is_file_mount) {
        // Mimic real file mounts by copying the source
        fs::copy_file(mount["source"], destination);
    } else {
        // Otherwise perform the actual mount.
        std::vector<iovec> iov;
        iov.reserve(2 * mount_opts.size());
        for (auto& [key, val] : mount_opts) {
            iov.emplace_back(
                iovec{reinterpret_cast<void*>(const_cast<char*>(key.c_str())),
                      key.size() + 1});
            iov.emplace_back(
                iovec{reinterpret_cast<void*>(const_cast<char*>(val.c_str())),
                      val.size() + 1});
        }
        if (nmount(&iov[0], iov.size(), mount_flags) < 0) {
            std::stringstream ss;
            ss << mount;
            throw std::system_error(
                errno, std::system_category(), "mounting " + ss.str());
        }
    }

    for (auto& entry : pseudo_opts) {
        std::get<0>(entry)->after_mount(destination, std::get<1>(entry));
    }
}

static void unmount_volume(runtime_state& state,
                           const fs::path& root_path,
                           const json& mount) {
    auto destination = resolve_container_path(root_path, mount);

    std::string type = mount.contains("type") ? mount["type"] : "nullfs";
    bool is_file_mount =
        type == "nullfs" && fs::is_regular_file(mount["source"]);

    if (is_file_mount) {
        // Restore the saved path if it exists
        auto [_, save_path] = get_save_path(state, destination);
        if (fs::exists(save_path)) {
            fs::rename(save_path, destination);
        }
    } else {
        if (::unmount(destination.c_str(), MNT_FORCE) < 0) {
            throw std::system_error{
                errno,
                std::system_category(),
                "mounting " + mount["destination"].get<std::string>()};
        }
    }
}

void mount_volumes(runtime_state& state,
                   const fs::path& root_path,
                   const json& mounts) {
    try {
        for (auto& mount : mounts) {
            mount_volume(state, root_path, mount);
        }
    } catch (...) {
        // Attempt to clean up in case we mounted something
        try {
            unmount_volume(state, root_path, mounts);
        } catch (...) {
        }
        throw;
    }
}

void unmount_volumes(runtime_state& state,
                     const fs::path& root_path,
                     const json& mounts) {
    // Remember the first exception (if any) but try to unmount
    // everything
    std::exception_ptr eptr{nullptr};
    for (auto& mount : mounts) {
        try {
            unmount_volume(state, root_path, mount);
        } catch (...) {
            if (!eptr) {
                eptr = std::current_exception();
            }
        }
    }
    try {
        for (auto& dir : state["remove_on_unmount"]) {
            if (fs::exists(dir)) {
                fs::remove(dir);
            }
        }
    } catch (...) {
        if (!eptr) {
            eptr = std::current_exception();
        }
    }
    if (eptr) {
        std::rethrow_exception(eptr);
    }
}

}  // namespace ocijail

#include <sys/param.h>

#include <sys/mount.h>
#include <sys/uio.h>
#include <iostream>

#include "ocijail/main.h"
#include "ocijail/mount.h"

using namespace std::literals::string_literals;
namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

static std::map<std::string, int> name_to_flag = {
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
};

static std::tuple<fs::path, fs::path> get_save_path(
    const runtime_state& state,
    const fs::path& destination) {
    auto save_dir =
        destination.parent_path() / (".save-"s + std::string{state.get_id()});
    auto save_path = save_dir / destination.filename();
    return std::make_tuple(save_dir, save_path);
}

static void mount_volume(runtime_state& state,
                         const fs::path& root_path,
                         const json& mount) {
    // We use fs::relative to strip off the leading '/' from destination
    auto destination = root_path / fs::relative(mount["destination"], "/");

    std::string type = mount.contains("type") ? mount["type"] : "nullfs";
    bool is_file_mount =
        type == "nullfs" && fs::is_regular_file(mount["source"]);

    // For tmpfs with tmpcopyup, we make a copy of the original
    // directory and use it to initialize the tmpfs
    std::optional<fs::path> tmp_copy;

    // Validate mount options before we perform any actions
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
                if (type == "tmpfs" && opt == "tmpcopyup") {
                    char dir_template[] = "/tmp/tmpcopyup.XXXXXXXX";
                    tmp_copy = mkdtemp(dir_template);
                    continue;
                }
                auto it = name_to_flag.find(opt);
                if (it != name_to_flag.end()) {
                    auto flag = it->second;
                    if (flag > 0) {
                        mount_flags |= flag;
                    } else if (flag < 0) {
                        mount_flags &= ~(-flag);
                    }
                    continue;
                } else {
                    auto s = opt.get<std::string>();
                    auto sep = s.find_first_of("=");
                    if (sep == std::string::npos) {
                        throw std::runtime_error("malformed mount option: " +
                                                 s);
                    }
                    mount_opts.emplace_back(s.substr(0, sep),
                                            s.substr(sep + 1));
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

    if (tmp_copy) {
        fs::copy(destination,
                 *tmp_copy,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks);
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
            throw std::system_error(
                errno,
                std::system_category(),
                "mounting " + mount["destination"].get<std::string>());
        }
    }
    if (tmp_copy) {
        fs::copy(*tmp_copy,
                 destination,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        fs::remove_all(*tmp_copy);
    }
}

static void unmount_volume(runtime_state& state,
                           const fs::path& root_path,
                           const json& mount) {
    // We use fs::relative to strip off the leading '/' from destination
    auto destination = root_path / fs::relative(mount["destination"], "/");

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
            throw std::system_error(
                errno,
                std::system_category(),
                "mounting " + mount["destination"].get<std::string>());
        }
    }
}

void mount_volumes(runtime_state& state,
                   const fs::path& root_path,
                   const json& mounts) {
    for (auto& mount : mounts) {
        mount_volume(state, root_path, mount);
    }
}

void unmount_volumes(runtime_state& state,
                     const fs::path& root_path,
                     const json& mounts) {
    for (auto& mount : mounts) {
        unmount_volume(state, root_path, mount);
    }
    for (auto& dir : state["remove_on_unmount"]) {
        if (fs::exists(dir)) {
            fs::remove(dir);
        }
    }
}

}  // namespace ocijail

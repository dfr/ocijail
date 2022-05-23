#include <sys/param.h>

#include <sys/jail.h>
#include <cassert>
#include <string>
#include <system_error>
#include <vector>

#include "ocijail/jail.h"

namespace ocijail {

void jail::config::set(const std::string& key, const value& val) {
    // Validate parameter types
    if (key == "jid" || key == "devfs_ruleset" || key == "enforce_statfs") {
        assert(std::holds_alternative<uint32_t>(val));
    } else if (key == "ip4" || key == "ip6") {
        assert(std::holds_alternative<ns>(val));
    } else if (key == "host" || key == "vnet") {
        assert(std::holds_alternative<ns>(val) &&
               std::get<ns>(val) != DISABLED);
    } else if (key == "persist" || key == "sysvmsg" || key == "sysvsem" ||
               key == "sysvshm" || key.starts_with("allow.")) {
        assert(std::holds_alternative<std::monostate>(val));
    } else {
        assert(std::holds_alternative<std::string>(val));
    }
    params_[key] = val;
}

jail jail::create(config& jconf) {
    auto jiov = get_iovec(jconf);
    int32_t jid = jail_set(&jiov[0], jiov.size(), JAIL_CREATE);
    if (jid < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_set"};
    }
    return jail{jid};
}

jail jail::find(const std::string& name) {
    config jconf;
    jconf.set("name", name);
    auto jiov = get_iovec(jconf);
    int32_t jid = jail_get(&jiov[0], jiov.size(), 0);
    if (jid < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_get"};
    }
    return jail{jid};
}

void jail::attach() {
    if (jail_attach(jid_) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_attach"};
    }
}

void jail::remove() {
    if (jail_remove(jid_) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_remove"};
    }
}

std::vector<iovec> jail::get_iovec(config& jconf) {
    std::vector<iovec> jiov;
    jiov.reserve(2 * jconf.params_.size());
    for (auto& [key, val] : jconf.params_) {
        jiov.emplace_back(
            iovec{reinterpret_cast<void*>(const_cast<char*>(key.c_str())),
                  key.size() + 1});
        if (auto p = std::get_if<std::string>(&val)) {
            jiov.emplace_back(
                iovec{reinterpret_cast<void*>(const_cast<char*>(p->c_str())),
                      p->size() + 1});
        } else if (auto p = std::get_if<uint32_t>(&val)) {
            jiov.emplace_back(
                iovec{reinterpret_cast<void*>(p), sizeof(uint32_t)});
        } else if (auto p = std::get_if<std::monostate>(&val)) {
            jiov.emplace_back(iovec{nullptr, 0});
        } else if (auto p = std::get_if<ns>(&val)) {
            jiov.emplace_back(
                iovec{reinterpret_cast<void*>(p), sizeof(uint32_t)});
        }
    }
    return jiov;
}

}  // namespace ocijail

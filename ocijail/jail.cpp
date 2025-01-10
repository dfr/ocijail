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
    std::array<char, 1024> errbuf;
    auto jiov = get_iovec(jconf, errbuf);
    int32_t jid = jail_set(&jiov[0], jiov.size(), JAIL_CREATE);
    if (jid < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_set: " + get_errmsg(jiov)};
    }
    return jail{jid};
}

jail jail::find(const std::string& name) {
    config jconf;
    jconf.set("name", name);
    std::array<char, 1024> errbuf;
    auto jiov = get_iovec(jconf, errbuf);
    int32_t jid = jail_get(&jiov[0], jiov.size(), 0);
    if (jid < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_get: " + get_errmsg(jiov)};
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
        // If errno is EINVAL, jail is already removed
        if (errno != EINVAL) {
            throw std::system_error{
                errno, std::system_category(), "error calling jail_remove"};
        }
    }
}

void jail::_get(config& jconf) {
    std::array<char, 1024> errbuf;
    auto jiov = get_iovec(jconf, errbuf);
    if (jail_get(&jiov[0], jiov.size(), 0) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_get: " + get_errmsg(jiov)};
    }
}

void jail::_set(config& jconf) {
    std::array<char, 1024> errbuf;
    auto jiov = get_iovec(jconf, errbuf);
    if (jail_set(&jiov[0], jiov.size(), JAIL_UPDATE) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error calling jail_set: " + get_errmsg(jiov)};
    }
}

static iovec string_to_iovec(const char *s) {
    return {reinterpret_cast<void*>(const_cast<char*>(s)),
            strlen(s) + 1};
}

static iovec string_to_iovec(const std::string& s) {
    return {reinterpret_cast<void*>(const_cast<char*>(s.c_str())),
            s.size() + 1};
}

std::vector<iovec> jail::get_iovec(config& jconf, std::array<char, 1024>& errbuf) {
    std::vector<iovec> jiov;
    jiov.reserve(2 * jconf.params_.size() + 2);
    for (auto& [key, val] : jconf.params_) {
        jiov.emplace_back(string_to_iovec(key));
        if (auto p = std::get_if<std::string>(&val)) {
            jiov.emplace_back(string_to_iovec(*p));
        } else if (auto p = std::get_if<uint32_t>(&val)) {
            jiov.emplace_back(
                iovec{reinterpret_cast<void*>(p), sizeof(uint32_t)});
        } else if (auto p = std::get_if<int32_t>(&val)) {
            jiov.emplace_back(
                iovec{reinterpret_cast<void*>(p), sizeof(int32_t)});
        } else if (auto p = std::get_if<std::monostate>(&val)) {
            jiov.emplace_back(iovec{nullptr, 0});
        } else if (auto p = std::get_if<ns>(&val)) {
            jiov.emplace_back(
                iovec{reinterpret_cast<void*>(p), sizeof(uint32_t)});
        }
    }
    jiov.emplace_back(string_to_iovec("errmsg"));
    jiov.emplace_back(reinterpret_cast<void*>(errbuf.data()), errbuf.size());
    
    return jiov;
}

}  // namespace ocijail

#include "ocijail/rctl.h"

#include <sys/types.h>
#include <sys/rctl.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

using nlohmann::json;

namespace ocijail::rctl {

namespace {

// Returns the value of an integer field if present, non-null and > 0.
std::optional<std::int64_t> positive(const json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number_integer()) {
        return std::nullopt;
    }
    auto v = obj[key].get<std::int64_t>();
    if (v <= 0) {
        return std::nullopt;
    }
    return v;
}

}  // namespace

translation from_oci_resources(const json& resources) {
    translation t;
    if (!resources.is_object()) {
        return t;
    }

    if (resources.contains("memory")) {
        if (auto limit = positive(resources["memory"], "limit")) {
            t.rules.push_back("memoryuse:sigkill=" + std::to_string(*limit));
        }
    }

    if (resources.contains("cpu")) {
        const auto& cpu = resources["cpu"];
        auto quota = positive(cpu, "quota");
        auto period = positive(cpu, "period");
        if (quota && period) {
            // percent of one CPU, rounded up, at least 1
            auto pct = (*quota * 100 + *period - 1) / *period;
            t.rules.push_back("pcpu:deny=" + std::to_string(pct < 1 ? 1 : pct));
        }
        if (positive(cpu, "shares")) {
            t.ignored.push_back("cpu.shares");
        }
    }

    if (resources.contains("pids")) {
        if (auto limit = positive(resources["pids"], "limit")) {
            t.rules.push_back("maxproc:deny=" + std::to_string(*limit));
        }
    }

    return t;
}

bool apply(const std::string& jail_name, const std::vector<std::string>& rules) {
    for (const auto& r : rules) {
        auto rule = "jail:" + jail_name + ":" + r;
        if (::rctl_add_rule(rule.c_str(), rule.size() + 1, nullptr, 0) < 0) {
            if (errno == ENOSYS || errno == EPERM) {
                // ENOSYS: RACCT disabled (kern.racct.enable=0).
                // EPERM: not allowed here, e.g. ocijail running in a jail.
                return false;
            }
            throw std::system_error(
                errno, std::system_category(), "rctl_add_rule " + rule);
        }
    }
    return true;
}

void remove(const std::string& jail_name) {
    auto filter = "jail:" + jail_name;
    if (::rctl_remove_rule(filter.c_str(), filter.size() + 1, nullptr, 0) < 0) {
        if (errno == ENOSYS || errno == ESRCH || errno == EPERM) {
            return;  // no RACCT, no matching rules, or not permitted here
        }
        throw std::system_error(
            errno, std::system_category(), "rctl_remove_rule " + filter);
    }
}

}  // namespace ocijail::rctl

#pragma once

#include <sys/uio.h>
#include <array>
#include <cstring>
#include <map>
#include <map>
#include <variant>

namespace ocijail {

struct jail {
    enum ns : uint32_t {
        DISABLED = 0,
        NEW = 1,
        INHERIT = 2,
    };

    struct config {
        using value =
            std::variant<std::monostate, std::string, uint32_t, int32_t, ns>;
        void set(const std::string& key, const value& value = std::monostate{});
        value& at(const std::string& key) { return params_.at(key); }

        std::map<std::string, value> params_;
    };

    static jail create(config& jconf);
    static jail find(const std::string& name);
    static jail find(int jid) { return jail{jid}; }

    auto jid() const { return jid_; }

    void attach();

    void remove();

    template <typename T>
    T get(const std::string& key) {
        config jconf;
        jconf.set("jid", jid_);
        jconf.set(key, T{});
        _get(jconf);
        return std::get<T>(jconf.at(key));
    }

    template <>
    bool get<bool>(const std::string& key) {
        auto val = get<uint32_t>(key);
        return !!val;
    }

    template <typename T>
    void set(const std::string& key, T val) {
        config jconf;
        jconf.set("jid", jid_);
        jconf.set(key, val);
        _set(jconf);
    }

   private:
    jail(int jid) : jid_(jid) {}
    void _get(config& jconf);
    void _set(config& jconf);
    static std::vector<iovec> get_iovec(config& jconf, std::array<char, 1024>& errbuf);
    static std::string get_errmsg(const std::vector<iovec>& jiov) {
        const auto& err = jiov.back();
        auto msg = reinterpret_cast<const char*>(err.iov_base);
        return std::string{msg, strnlen(msg, 1024)};
    }

    int32_t jid_;
};

}  // namespace ocijail

#pragma once

#include <sys/uio.h>
#include <map>

namespace ocijail {

struct jail {
    enum ns : uint32_t {
        DISABLED = 0,
        NEW = 1,
        INHERIT = 2,
    };

    struct config {
        using value = std::variant<std::monostate, std::string, uint32_t, ns>;
        void set(const std::string& key, const value& value = std::monostate{});
        std::map<std::string, value> params_;
    };

    static jail create(config& jconf);
    static jail find(const std::string& name);

    jail(int jid) : jid_(jid) {}
    auto jid() const { return jid_; }
    void attach();
    void remove();

   private:
    static std::vector<iovec> get_iovec(config& jconf);

    int32_t jid_;
};

}  // namespace ocijail

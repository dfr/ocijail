#include <sys/wait.h>
#include <unistd.h>

#include "ocijail/hook.h"

extern "C" char** environ;

using nlohmann::json;

namespace ocijail {

hook::hook(const json& hook_config) {
    // We can assume that validate_hooks has ensured that this is well-formed
    path_ = hook_config["path"];

    if (hook_config.contains("args")) {
        auto& config_args = hook_config["args"];
        args_ = std::vector<std::string>();
        for (auto& arg : config_args) {
            args_->push_back(arg.get<std::string>());
        }
    }

    if (hook_config.contains("env")) {
        auto& config_env = hook_config["env"];
        env_ = std::vector<std::string>();
        for (auto& arg : config_env) {
            env_->push_back(arg.get<std::string>());
        }
    }

    if (hook_config.contains("timeout")) {
        auto& config_timeout = hook_config["timeout"];
        if (!config_timeout.is_number()) {
            malformed_config("process.env must be a number");
        }
        timeout_ = config_timeout;
    }
}

void hook::validate_hooks(main_app& app,
                          const nlohmann::json& hooks,
                          const char* phase) {
    if (hooks.is_null() || !hooks.contains(phase)) {
        return;
    }
    auto& a = hooks[phase];
    if (!a.is_array()) {
        malformed_config("hook lists must be arrays");
    }
    for (auto& hook : a) {
        if (!hook.contains("path")) {
            malformed_config("hook must have a path property");
        }
        if (hook.contains("args")) {
            auto& args = hook["args"];
            if (!args.is_array()) {
                malformed_config("hook.args must be an array");
            }
            for (auto& s : args) {
                if (!s.is_string()) {
                    malformed_config("hook.args elements must be strings");
                }
            }
        }
        if (hook.contains("env")) {
            auto& env = hook["env"];
            if (!env.is_array()) {
                malformed_config("hook.env must be an array");
            }
            for (auto& s : env) {
                if (!s.is_string()) {
                    malformed_config("hook.env elements must be strings");
                }
            }
        }
        if (hook.contains("timeout") && !hook["timeout"].is_number()) {
            malformed_config("hook.timeout must be a number");
        }
    }
}

void hook::run_hooks(main_app& app,
                     const nlohmann::json& hooks,
                     const char* phase,
                     const runtime_state& state) {
    if (hooks.is_null() || !hooks.contains(phase)) {
        return;
    }

    for (auto& hook_config : hooks[phase]) {
        hook(hook_config).run(app, state);
    }
}

int hook::run(main_app& app, const runtime_state& state) {
    std::vector<char*> argv;
    std::vector<char*> envv;
    if (env_) {
        for (auto& s : *env_) {
            envv.push_back(const_cast<char*>(s.c_str()));
        }
        envv.push_back(nullptr);
    }

    argv.push_back(const_cast<char*>(path_.c_str()));
    if (args_) {
        for (auto& s : *args_) {
            argv.push_back(const_cast<char*>(s.c_str()));
        }
    }
    argv.push_back(nullptr);

    int stdin[2];
    if (::pipe(stdin) < 0) {
        throw std::system_error{errno,
                                std::system_category(),
                                "error creating pipe for executing hook"};
    }

    std::stringstream ss;
    ss << state.report();
    auto pid = ::fork();
    if (pid) {
        // Parent process - write the state json
        auto s = ss.str();
        auto len = s.size();
        auto p = s.data();
        while (len > 0) {
            auto n = ::write(stdin[1], p, len);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "error writing state to hook"};
            }
            len -= n;
            p += n;
        }
        if (::close(stdin[0]) < 0 || ::close(stdin[1]) < 0) {
            throw std::system_error{
                errno, std::system_category(), "error closing hook stdin pipe"};
        }
        int status;
        // TODO: timeout
        auto res = ::waitpid(pid, &status, 0);
        if (res < 0) {
            throw std::system_error{
                errno, std::system_category(), "error waiting for hook"};
        }
        int ret =
            WIFEXITED(status) ? WEXITSTATUS(status) : 127 + WTERMSIG(status);
        return ret;
    } else {
        // Child process - setup descriptors and go
        ::dup2(stdin[0], 0);
        ::close(stdin[1]);
        ::close_range(3, INT_MAX, CLOSE_RANGE_CLOEXEC);
        // Don't override environment unless it was in the config
        char** envp;
        if (env_) {
            envp = &envv[0];
        } else {
            envp = environ;
        }
        // The path should be absolute - no PATH lookup is needed
        ::execve(argv[0], &argv[0], envp);
        throw std::system_error{
            errno, std::system_category(), "error executing hook" + path_};
    }
}

}  // namespace ocijail

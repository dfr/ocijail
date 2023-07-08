#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "ocijail/exec.h"
#include "ocijail/jail.h"
#include "ocijail/process.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void exec::init(main_app& app) {
    static exec instance{app};
}

exec::exec(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "exec", "Execute a command in the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the exec")
        ->required();
    sub->add_option(
           "--process", process_, "Path to a file containing the process json")
        ->required()
        ->check(CLI::ExistingPath);
    sub->add_option(
           "--console-socket",
           console_socket_,
           "Path to a socket which will receive the console pty descriptor")
        ->check(CLI::ExistingPath);
    sub->add_option(
        "--pid-file",
        pid_file_,
        "Path to a file where the container process id will be written");
    sub->add_flag("--tty,-t", tty_, "Allocate a pty for the exec process");
    sub->add_flag("--detach,-d",
                  detach_,
                  "Detach the command and execute in the background");
    sub->add_option("--preserve-fds",
                    preserve_fds_,
                    "Number of additional file descriptors for the container");

    sub->final_callback([this] { run(); });
}

void exec::run() {
    json process_json;
    std::ifstream{process_} >> process_json;
    if (tty_) {
        process_json["terminal"] = *tty_;
    }
    process proc{process_json, console_socket_, detach_, preserve_fds_};

    // Unit tests for config validation stop here.
    if (app_.get_test_mode() == test_mode::VALIDATION) {
        return;
    }

    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();

    auto j = jail::find(int(state["jid"]));

    if (detach_) {
        // Create a socket pair for coordinating create activities with
        // our child process.
        int create_sock[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, create_sock) < 0) {
            throw std::system_error{
                errno, std::system_category(), "error creating socket pair"};
        }

        // Detach from parent and send the pid which will perform the
        // exec (if requested).
        auto pid = ::fork();
        if (pid) {
            // Parent process - write to pid file if requested
            if (pid_file_) {
                std::ofstream{*pid_file_} << pid;
            }

            // Signal the child to validate that the container process can be
            // found.
            char ch = 1;
            auto n = ::write(create_sock[0], &ch, 1);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "write to exec create socket"};
            }

            // Read back the child's status - this is our exit status. The
            // child will have already written to stderr if necessary.
            char status;
            n = ::read(create_sock[0], &status, 1);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "read from exec create socket"};
            }
            ::exit(status);
        } else {
            // Setup the tty if requested
            auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();

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
                // Our part of exec: validate process args.

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
                                        "error writing to exec create socket"};
            }
            ::close(create_sock[1]);

            // If validate failed, don't try to exec - it will fail and double
            // the error message reported to the user in a confusing way.
            if (status != 0) {
                ::exit(status);
            }

            // Run the process inside the jail
            proc.exec(stdin_fd, stdout_fd, stderr_fd);
        }
    } else {
        // Otherwise, just exec in this process
        auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();
        j.attach();
        proc.validate();
        proc.exec(stdin_fd, stdout_fd, stderr_fd);
    }
}

}  // namespace ocijail

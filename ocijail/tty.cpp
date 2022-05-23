#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <array>
#include <iostream>
#include <sstream>
#include <system_error>

#include "ocijail/tty.h"

namespace ocijail {

std::tuple<int, int> open_pty() {
    auto control_fd = ::posix_openpt(O_RDWR | O_CLOEXEC);
    if (control_fd < 0) {
        throw std::system_error{
            errno, std::system_category(), "error from posix_openpt"};
    }
    if (::grantpt(control_fd) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error from grantpt"};
    }
    auto tty_path = ::ptsname(control_fd);
    if (tty_path == nullptr) {
        throw std::system_error{
            errno, std::system_category(), "error from ptsname"};
    }
    auto tty_fd = ::open(tty_path, O_RDWR, 0);
    if (tty_fd < 0) {
        throw std::system_error{errno, std::system_category(), tty_path};
    }

    // Make the pty our control terminal
    if (::setsid() < 0) {
        throw std::system_error{errno, std::system_category(), "setsid"};
    }
    if (::ioctl(tty_fd, TIOCSCTTY, nullptr) < 0) {
        throw std::system_error{errno, std::system_category(), "TIOCSCTTY"};
    }
    return {control_fd, tty_fd};
}

void send_pty_control_fd(std::filesystem::path socket_name, int control_fd) {
    // Connect to the console socket. The socket path may be
    // too long to fit into sockaddr_un - split it and use
    // connectat to work around the limitation
    auto sock_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock_fd < 0) {
        throw std::system_error{errno, std::system_category(), "socket"};
    }
    auto dir = socket_name.parent_path();
    auto sock = socket_name.filename();
    auto dir_fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC, 0);
    if (dir_fd < 0) {
        throw std::system_error{
            errno, std::system_category(), "open " + dir.native()};
    }
    sockaddr_un sun;
    sun.sun_len = sock.native().size() + 1;
    sun.sun_family = AF_UNIX;
    ::strlcpy(sun.sun_path, sock.c_str(), SUNPATHLEN);
    if (::connectat(
            dir_fd, sock_fd, reinterpret_cast<sockaddr*>(&sun), sizeof(sun)) <
        0) {
        throw std::system_error{
            errno, std::system_category(), "connectat " + sock.native()};
    }
    ::close(dir_fd);

    // Send over our pty descriptor using a CMSG
    char zero = 0;
    ::iovec iov{.iov_base = &zero, .iov_len = 1};
    std::array<char, CMSG_SPACE(sizeof(int))> cmsg;
    std::fill_n(cmsg.begin(), cmsg.size(), 0);
    ::msghdr hdr{
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg.data(),
        .msg_controllen = cmsg.size(),
    };
    auto m = CMSG_FIRSTHDR(&hdr);
    *m = cmsghdr{
        .cmsg_len = CMSG_LEN(sizeof(int)),
        .cmsg_level = SOL_SOCKET,
        .cmsg_type = SCM_RIGHTS,
    };
    *reinterpret_cast<int*>(CMSG_DATA(m)) = control_fd;
    auto n = ::sendmsg(sock_fd, &hdr, 0);
    if (n < 0) {
        throw std::system_error{errno, std::system_category(), "sendmsg"};
    }
    if (n != 1) {
        std::stringstream ss;
        ss << "unexpected return value from sendmsg: " << n;
        throw std::runtime_error(ss.str());
    }
    ::close(control_fd);
    ::close(sock_fd);
}

}  // namespace ocijail

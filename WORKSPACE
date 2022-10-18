load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "com_github_google_rules_install",
    url = "https://github.com/google/bazel_rules_install/archive/refs/heads/main.zip",
    strip_prefix = "bazel_rules_install-main",
    sha256 = "9e8d52e18e9a39a594e1ab4fb44eacf0139ad3710b56e85baa0a8d2c933b2d63",
)

load("@com_github_google_rules_install//:deps.bzl", "install_rules_dependencies")

install_rules_dependencies()

load("@com_github_google_rules_install//:setup.bzl", "install_rules_setup")

install_rules_setup()

http_archive(
    name = "cliutils_cli11",
    strip_prefix = "CLI11-2.2.0",
    build_file = "//third_party:BUILD.cli11",
    url = "https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.2.0.zip",
    sha256 = "1cb6906a2a9c8136cd6da547dc01c6cb348d787138199b2c40025a1fc9f5d81e",
)

http_archive(
    name = "nlohmann_json",
    build_file = "//third_party:BUILD.json",
    url = "https://github.com/nlohmann/json/releases/download/v3.10.5/include.zip",
    sha256 = "b94997df68856753b72f0d7a3703b7d484d4745c567f3584ef97c96c25a5798e",
)

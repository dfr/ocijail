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
    strip_prefix = "CLI11-2.3.2",
    build_file = "//third_party:BUILD.cli11",
    url = "https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.3.2.zip",
    sha256 = "562c4be7507dc6fb4997ecd648bf935d84efe17b54715fa5cfbddac05279f668",
)

http_archive(
    name = "nlohmann_json",
    build_file = "//third_party:BUILD.json",
    url = "https://github.com/nlohmann/json/releases/download/v3.11.2/include.zip",
    sha256 = "e5c7a9f49a16814be27e4ed0ee900ecd0092bfb7dbfca65b5a421b774dccaaed",
)

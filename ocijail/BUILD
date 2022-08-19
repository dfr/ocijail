cc_binary(
    name = "ocijail",
    copts = [
        "-std=c++20",
    ],
    srcs = glob([
        "*.cpp",
        "*.h",
    ]) + [
        ":version.h",
    ],
    deps = [
        "@cliutils_cli11//:cli11",
        "@nlohmann_json//:json",
    ],
    visibility = ["//visibility:public"],
)

genrule(
    name = "gen_version",
    srcs = ["version.h.in"],
    outs = ["version.h"],
    cmd_bash = """
        h=$$(grep ^STABLE_GIT_COMMIT bazel-out/stable-status.txt | cut -d' ' -f2); \
        sed -e 's/%%GIT_COMMIT%%/'$$h'/' < $< > $@
    """,
)

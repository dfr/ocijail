cc_binary(
    name = "ocijail",
    copts = [
        "-std=c++20",
    ],
    srcs = glob([
        "*.cpp",
        "*.h",
    ]),
    deps = [
        "@cliutils_cli11//:cli11",
        "@nlohmann_json//:json",
    ],
    visibility = ["//visibility:public"],
)

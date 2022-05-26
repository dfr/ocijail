test_suite(
    name = "user",
    tests = [
        ":create_test",
        ":exec_test",
    ],
)

test_suite(
    name = "root",
    tests = [
        ":run_test_wrapper",
    ],
)

py_test(
    name = "create_test",
    srcs = ["create_test.py"],
    data = ["//ocijail:ocijail"],
)

py_test(
    name = "exec_test",
    srcs = ["exec_test.py"],
    data = ["//ocijail:ocijail"],
)

py_binary(
    name = "run_test",
    srcs = ["run_test.py"],
    data = ["//ocijail:ocijail"],
)

cc_binary(
    name = "with_subreaper",
    srcs = ["with_subreaper.c"],
)

sh_test(
    name = "run_test_wrapper",
    srcs = ["run_test_wrapper.sh"],
    data = [
        ":run_test",
        ":with_subreaper",
    ],
)

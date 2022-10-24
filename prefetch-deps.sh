#! /bin/sh

cd distfiles
fetch https://github.com/google/bazel_rules_install/archive/refs/heads/main.zip
fetch https://mirror.bazel.build/github.com/bazelbuild/bazel-skylib/releases/download/1.3.0/bazel-skylib-1.3.0.tar.gz
fetch https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.2.0.zip
fetch https://github.com/nlohmann/json/releases/download/v3.10.5/include.zip

# Taken from bazel-5.3.0/distdir_deps.bzl
fetch https://mirror.bazel.build/github.com/bazelbuild/rules_cc/archive/b1c40e1de81913a3c40e5948f78719c28152486d.zip
fetch https://mirror.bazel.build/github.com/bazelbuild/rules_java/archive/7cf3cefd652008d0a64a419c34c13bdca6c8f178.zip

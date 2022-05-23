OCI Runtime using FreeBSD Jails
===============================

This repository contains an experimental OCI runtime intended to be
used with [buildah](https://buildah.io) and/or [podman](https://podman.io).

Build and install using:
```
bazel run //:install -- -s /usr/local/bin
```

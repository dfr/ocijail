#! /bin/sh

# Run the test as root and make it a sub-reaper so that it can wait
# for the container pid
exec sudo PYTHONTRACEMALLOC=1 ./test/with_subreaper ./test/run_test $*

#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))

# we can add some actions to clean up files which no need anymore, such as share memory overflow...
rm -f  /dev/shm/*trace.pid*
rm -f  /dev/shm/spdk_*_conns.*

#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))

sudo $rootdir/scripts/pkgdep.sh
exit 0

conf=~/autorun-spdk.conf

# Runs agent scripts
$rootdir/autobuild.sh "$conf"
sudo WITH_DPDK_DIR="$WITH_DPDK_DIR" $rootdir/autotest.sh "$conf"
$rootdir/autopackage.sh "$conf"

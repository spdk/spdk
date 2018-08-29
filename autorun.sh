#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))

conf=~/autorun-spdk.conf

# Running pkgdep to install the intel-ipsec module
sudo $rootdir/scripts/pkgdep.sh

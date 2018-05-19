#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))

conf=~/autorun-spdk.conf

# Runs agent scripts
$rootdir/autobuild.sh "$conf"
sudo -E $rootdir/autotest.sh "$conf"
$rootdir/autopackage.sh "$conf"

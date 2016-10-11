#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))

# Runs agent scripts
$rootdir/autobuild.sh
sudo $rootdir/autotest.sh
$rootdir/autopackage.sh

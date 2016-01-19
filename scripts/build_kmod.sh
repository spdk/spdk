#!/usr/bin/env bash

readonly BASEDIR=$(readlink -f $(dirname $0))/..

set -e

function build_ioat_kmod() {
	if [ -d $BASEDIR/examples/ioat/kperf/kmod ]; then
		echo "Build Linux Ioat Test Module ..."
		cd $BASEDIR/examples/ioat/kperf/kmod
		make
	fi
}

function clean_ioat_kmod() {
	# remove dmaperf test module
	grep -q "^dmaperf" /proc/modules && rmmod dmaperf
	# cleanup build
	if [ -d $BASEDIR/examples/ioat/kperf/kmod ]; then
		echo "Cleanup Linux Ioat Test Module ..."
		cd $BASEDIR/examples/ioat/kperf/kmod
		make clean
	fi
}

if [ `uname` = Linux ]; then
	if [ "$1" = "build" ]; then
		build_ioat_kmod
	fi
	if [ "$1" = "clean" ]; then
		clean_ioat_kmod
	fi
fi

#!/usr/bin/env bash

set -xe

if [ $EUID -ne 0 ]; then
	echo "$0 must be run as root"
	exit 1
fi

if [ ! -f /var/run/iscsi.pid.0 ]; then
	echo "ids is not running"
	exit 1
fi

# delete any existing temporary iscsi.conf files
rm -f /tmp/iscsi.conf.*

kill -USR1 `cat /var/run/iscsi.pid.0`

if [ ! -f `ls /tmp/iscsi.conf.*` ]; then
	echo "ids did not generate config file"
	exit 1
fi

mv `ls /tmp/iscsi.conf.*` /tmp/iscsi.conf

#!/usr/bin/env bash

set -xe

pid="$1"

if [[ -z "$pid" ]]; then
	echo "usage: $0 pid"
	exit 1
fi

# delete any existing temporary iscsi.conf files
rm -f /tmp/iscsi.conf.*

kill -USR1 "$pid"

if [ ! -f `ls /tmp/iscsi.conf.*` ]; then
	echo "iscsi_tgt did not generate config file"
	exit 1
fi

mv `ls /tmp/iscsi.conf.*` /tmp/iscsi.conf

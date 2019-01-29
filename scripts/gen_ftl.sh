#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function usage {
	echo "Usage: $0 -a TRANSPORT_ADDR -n BDEV_NAME -l PUNITS [-u UUID]"
	echo "UUID is required when restoring device state"
	echo
	echo "Replaces FTL_* variables in config files inside the test/ftl/config directory."
	echo "The following varaibles are replaced:"
	echo "	- FTL_CONF_DIR - config directory"
	echo "	- FTL_TRANSPORT_ADDR - SSD's PCIe address (defaults to first lnvm device)"
	echo "	- FTL_BDEV_NAME - name of the bdev"
	echo "	- FTL_BDEV_PUNITS - bdev's parallel unit range (e.g. 0-3)"
	echo "	- FTL_BDEV_UUID - bdev's uuid (used when in restore mode)"
}

function generate_config {
	echo "[Ftl]"
	echo "  TransportID \"trtype:PCIe traddr:${addr}\" $name $punits $uuid"
}

while getopts ":a:n:l:m:u:" arg; do
	case "$arg" in
		a)	addr=$OPTARG	;;
		n)	name=$OPTARG	;;
		l)	punits=$OPTARG	;;
		u)	uuid=$OPTARG	;;
		h)	usage
			exit 0		;;
		*)	usage
			exit 1		;;
	esac
done

if [[ -z "$addr" || -z "$name" || -z "$punits" ]]; then
	usage
	exit 1
fi

generate_config $file

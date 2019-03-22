#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function usage {
	echo "Usage: $0 -a TRANSPORT_ADDR -n BDEV_NAME -l PUNITS [-u UUID]"
	echo "UUID is required when restoring device state"
	echo
	echo "TRANSPORT_ADDR - SSD's PCIe address"
	echo "BDEV_NAME - name of the bdev"
	echo "PUNITS - bdev's parallel unit range (e.g. 0-3)"
	echo "UUID - bdev's uuid (used when in restore mode)"
}

function generate_config {
	echo "[Ftl]"
	echo "  TransportID \"trtype:PCIe traddr:$1\" $2 $3 $4"
}

uuid=00000000-0000-0000-0000-000000000000

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

generate_config $addr $name $punits $uuid

#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function usage {
	echo "Usage: $0 [-j] -a TRANSPORT_ADDR -n BDEV_NAME -l PUNITS [-u UUID]"
	echo "UUID is required when restoring device state"
	echo
	echo "-j json format"
	echo "TRANSPORT_ADDR - SSD's PCIe address"
	echo "BDEV_NAME - name of the bdev"
	echo "PUNITS - bdev's parallel unit range (e.g. 0-3)"
	echo "UUID - bdev's uuid (used when in restore mode)"
}

function create_classic_config {
	echo "[Ftl]"
	echo "  TransportID \"trtype:PCIe traddr:$1\" $2 $3 $4"
}

function create_json_config()
{
	echo "{"
	echo '"subsystem": "bdev",'
	echo '"config": ['
	echo '{'
	echo '"method": "construct_ftl_bdev",'
	echo '"params": {'
	echo "\"name\": \"$2\","
	echo '"trtype": "PCIe",'
	echo "\"traddr\": \"$1\","
	if [ -n "$4" ]; then
		echo "\"punits\": \"$3\","
		echo "\"uuid\": \"$4\""
	else
		echo "\"punits\": \"$3\""
	fi
	echo '}'
	echo '}'
	echo ']'
	echo '}'
}

while getopts "ja:n:l:m:u:" arg; do
	case "$arg" in
		j)	json=1		;;
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

if [ -n "$json" ]; then
	create_json_config $addr $name $punits $uuid
else
	create_classic_config $addr $name $punits $uuid
fi

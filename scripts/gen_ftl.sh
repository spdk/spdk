#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function usage {
	echo "Usage: [-j] $0 -a TRANSPORT_ADDR -n BDEV_NAME [-u UUID] [-c CACHE]"
	echo "UUID is required when restoring device state"
	echo
	echo "-j json format"
	echo "TRANSPORT_ADDR - SSD's PCIe address"
	echo "BDEV_NAME - name of the bdev"
	echo "UUID - bdev's uuid (used when in restore mode)"
	echo "CACHE - name of the bdev to be used as write buffer cache"
}

function create_classic_config {
	echo "[Ftl]"
	echo "  TransportID \"trtype:PCIe traddr:$1\" $2 $3 $4 $5"
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
	if [ -n "$5" ]; then
		echo "\"uuid\": \"$3\","
		echo "\"cache\": \"$4\""
	else
		echo "\"uuid\": \"$3\""
	fi
	echo '}'
	echo '}'
	echo ']'
	echo '}'
}

uuid=00000000-0000-0000-0000-000000000000

while getopts "ja:n:l:m:u:c:" arg; do
	case "$arg" in
		j)	json=1		;;
		a)	addr=$OPTARG	;;
		n)	name=$OPTARG	;;
		u)	uuid=$OPTARG	;;
		c)	cache=$OPTARG	;;
		h)	usage
			exit 0		;;
		*)	usage
			exit 1		;;
	esac
done

if [[ -z "$addr" || -z "$name" ]]; then
	usage
	exit 1
fi

if [ -n "$json" ]; then
	create_json_config $addr $name $uuid $cache
else
	create_classic_config $addr $name $uuid $cache
fi

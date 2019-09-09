#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/..

function usage {
	echo "Usage: [-j] $0 -n BDEV_NAME -d DISK_BDEV [-u UUID] [-c CACHE]"
	echo "UUID is required when restoring device state"
	echo
	echo "BDEV_NAME - name of the bdev"
	echo "DISK BDEV - name of the bdev to be used as underlying device"
	echo "UUID - bdev's uuid (used when in restore mode)"
	echo "CACHE - name of the bdev to be used as write buffer cache"
}

function create_json_config()
{
	echo "{"
	echo '"subsystem": "bdev",'
	echo '"config": ['
	echo '{'
	echo '"method": "construct_ftl_bdev",'
	echo '"params": {'
	echo "\"name\": \"$1\","
	echo "\"disk_bdev\": \"$2\","
	if [ -n "$6" ]; then
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

while getopts ":n:d:u:c:" arg; do
	case "$arg" in
		n)	name=$OPTARG	;;
		d)	disk_bdev=$OPTARG	;;
		u)	uuid=$OPTARG		;;
		c)	cache=$OPTARG		;;
		h)	usage
			exit 0			;;
		*)	usage
			exit 1			;;
	esac
done

if [[ -z "$name" || -z "$disk_bdev" ]]; then
	usage
	exit 1
fi

create_json_config $name $disk_bdev $uuid $cache
